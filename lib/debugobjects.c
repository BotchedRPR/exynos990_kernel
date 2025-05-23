/*
 * Generic infrastructure for lifetime debugging of objects.
 *
 * Started by Thomas Gleixner
 *
 * Copyright (C) 2008, Thomas Gleixner <tglx@linutronix.de>
 *
 * For licencing details see kernel-base/COPYING
 */

#define pr_fmt(fmt) "ODEBUG: " fmt

#include <linux/debugobjects.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/kmemleak.h>

#define ODEBUG_HASH_BITS	14
#define ODEBUG_HASH_SIZE	(1 << ODEBUG_HASH_BITS)

#define ODEBUG_POOL_SIZE	1024
#define ODEBUG_POOL_MIN_LEVEL	256
#define ODEBUG_POOL_PERCPU_SIZE	64

#define ODEBUG_CHUNK_SHIFT	PAGE_SHIFT
#define ODEBUG_CHUNK_SIZE	(1 << ODEBUG_CHUNK_SHIFT)
#define ODEBUG_CHUNK_MASK	(~(ODEBUG_CHUNK_SIZE - 1))

struct debug_bucket {
	struct hlist_head	list;
	raw_spinlock_t		lock;
};

/*
 * Debug object percpu free list
 * Access is protected by disabling irq
 */
struct debug_percpu_free {
	struct hlist_head	free_objs;
	int			obj_free;
};

static DEFINE_PER_CPU(struct debug_percpu_free, percpu_obj_pool);

static struct debug_bucket	obj_hash[ODEBUG_HASH_SIZE];

static struct debug_obj		obj_static_pool[ODEBUG_POOL_SIZE] __initdata;

static DEFINE_RAW_SPINLOCK(pool_lock);

static HLIST_HEAD(obj_pool);
static HLIST_HEAD(obj_to_free);

/*
 * Because of the presence of percpu free pools, obj_pool_free will
 * under-count those in the percpu free pools. Similarly, obj_pool_used
 * will over-count those in the percpu free pools. Adjustments will be
 * made at debug_stats_show(). Both obj_pool_min_free and obj_pool_max_used
 * can be off.
 */
static int			obj_pool_min_free = ODEBUG_POOL_SIZE;
static int			obj_pool_free = ODEBUG_POOL_SIZE;
static int			obj_pool_used;
static int			obj_pool_max_used;
/* The number of objs on the global free list */
static int			obj_nr_tofree;

static int			debug_objects_maxchain __read_mostly;
static int __maybe_unused	debug_objects_maxchecked __read_mostly;
static int			debug_objects_fixups __read_mostly;
static int			debug_objects_warnings __read_mostly;
static int			debug_objects_enabled __read_mostly
				= CONFIG_DEBUG_OBJECTS_ENABLE_DEFAULT;
static int			debug_objects_pool_size __read_mostly
				= ODEBUG_POOL_SIZE;
static int			debug_objects_pool_min_level __read_mostly
				= ODEBUG_POOL_MIN_LEVEL;
static struct debug_obj_descr	*descr_test  __read_mostly;
static struct kmem_cache	*obj_cache __read_mostly;

/*
 * Track numbers of kmem_cache_alloc()/free() calls done.
 */
static int			debug_objects_allocated;
static int			debug_objects_freed;

static void free_obj_work(struct work_struct *work);
static DECLARE_WORK(debug_obj_work, free_obj_work);

static int __init enable_object_debug(char *str)
{
	debug_objects_enabled = 1;
	return 0;
}

static int __init disable_object_debug(char *str)
{
	debug_objects_enabled = 0;
	return 0;
}

early_param("debug_objects", enable_object_debug);
early_param("no_debug_objects", disable_object_debug);

static const char *obj_states[ODEBUG_STATE_MAX] = {
	[ODEBUG_STATE_NONE]		= "none",
	[ODEBUG_STATE_INIT]		= "initialized",
	[ODEBUG_STATE_INACTIVE]		= "inactive",
	[ODEBUG_STATE_ACTIVE]		= "active",
	[ODEBUG_STATE_DESTROYED]	= "destroyed",
	[ODEBUG_STATE_NOTAVAILABLE]	= "not available",
};

static void fill_pool(void)
{
	gfp_t gfp = GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN;
	struct debug_obj *new, *obj;
	unsigned long flags;

	if (likely(obj_pool_free >= debug_objects_pool_min_level))
		return;

	/*
	 * Reuse objs from the global free list; they will be reinitialized
	 * when allocating.
	 */
	while (obj_nr_tofree && (obj_pool_free < obj_pool_min_free)) {
		raw_spin_lock_irqsave(&pool_lock, flags);
		/*
		 * Recheck with the lock held as the worker thread might have
		 * won the race and freed the global free list already.
		 */
		if (obj_nr_tofree) {
			obj = hlist_entry(obj_to_free.first, typeof(*obj), node);
			hlist_del(&obj->node);
			obj_nr_tofree--;
			hlist_add_head(&obj->node, &obj_pool);
			obj_pool_free++;
		}
		raw_spin_unlock_irqrestore(&pool_lock, flags);
	}

	if (unlikely(!obj_cache))
		return;

	while (obj_pool_free < debug_objects_pool_min_level) {

		new = kmem_cache_zalloc(obj_cache, gfp);
		if (!new)
			return;

		raw_spin_lock_irqsave(&pool_lock, flags);
		hlist_add_head(&new->node, &obj_pool);
		debug_objects_allocated++;
		obj_pool_free++;
		raw_spin_unlock_irqrestore(&pool_lock, flags);
	}
}

/*
 * Lookup an object in the hash bucket.
 */
static struct debug_obj *lookup_object(void *addr, struct debug_bucket *b)
{
	struct debug_obj *obj;
	int cnt = 0;

	hlist_for_each_entry(obj, &b->list, node) {
		cnt++;
		if (obj->object == addr)
			return obj;
	}
	if (cnt > debug_objects_maxchain)
		debug_objects_maxchain = cnt;

	return NULL;
}

/*
 * Allocate a new object from the hlist
 */
static struct debug_obj *__alloc_object(struct hlist_head *list)
{
	struct debug_obj *obj = NULL;

	if (list->first) {
		obj = hlist_entry(list->first, typeof(*obj), node);
		hlist_del(&obj->node);
	}

	return obj;
}

static struct debug_obj *
alloc_object(void *addr, struct debug_bucket *b, struct debug_obj_descr *descr)
{
	struct debug_percpu_free *percpu_pool;
	struct debug_obj *obj;

	if (likely(obj_cache)) {
		percpu_pool = this_cpu_ptr(&percpu_obj_pool);
		obj = __alloc_object(&percpu_pool->free_objs);
		if (obj) {
			percpu_pool->obj_free--;
			goto init_obj;
		}
	}

	raw_spin_lock(&pool_lock);
	obj = __alloc_object(&obj_pool);
	if (obj) {
		obj_pool_used++;
		if (obj_pool_used > obj_pool_max_used)
			obj_pool_max_used = obj_pool_used;

		obj_pool_free--;
		if (obj_pool_free < obj_pool_min_free)
			obj_pool_min_free = obj_pool_free;
	}
	raw_spin_unlock(&pool_lock);

init_obj:
	if (obj) {
		obj->object = addr;
		obj->descr  = descr;
		obj->state  = ODEBUG_STATE_NONE;
		obj->astate = 0;
		hlist_add_head(&obj->node, &b->list);
	}
	return obj;
}

/*
 * workqueue function to free objects.
 *
 * To reduce contention on the global pool_lock, the actual freeing of
 * debug objects will be delayed if the pool_lock is busy.
 */
static void free_obj_work(struct work_struct *work)
{
	struct hlist_node *tmp;
	struct debug_obj *obj;
	unsigned long flags;
	HLIST_HEAD(tofree);

	if (!raw_spin_trylock_irqsave(&pool_lock, flags))
		return;

	/*
	 * The objs on the pool list might be allocated before the work is
	 * run, so recheck if pool list it full or not, if not fill pool
	 * list from the global free list
	 */
	while (obj_nr_tofree && obj_pool_free < debug_objects_pool_size) {
		obj = hlist_entry(obj_to_free.first, typeof(*obj), node);
		hlist_del(&obj->node);
		hlist_add_head(&obj->node, &obj_pool);
		obj_pool_free++;
		obj_nr_tofree--;
	}

	/*
	 * Pool list is already full and there are still objs on the free
	 * list. Move remaining free objs to a temporary list to free the
	 * memory outside the pool_lock held region.
	 */
	if (obj_nr_tofree) {
		hlist_move_list(&obj_to_free, &tofree);
		debug_objects_freed += obj_nr_tofree;
		obj_nr_tofree = 0;
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);

	hlist_for_each_entry_safe(obj, tmp, &tofree, node) {
		hlist_del(&obj->node);
		kmem_cache_free(obj_cache, obj);
	}
}

static bool __free_object(struct debug_obj *obj)
{
	unsigned long flags;
	bool work;
	struct debug_percpu_free *percpu_pool;

	local_irq_save(flags);
	/*
	 * Try to free it into the percpu pool first.
	 */
	percpu_pool = this_cpu_ptr(&percpu_obj_pool);
	if (obj_cache && percpu_pool->obj_free < ODEBUG_POOL_PERCPU_SIZE) {
		hlist_add_head(&obj->node, &percpu_pool->free_objs);
		percpu_pool->obj_free++;
		local_irq_restore(flags);
		return false;
	}

	raw_spin_lock(&pool_lock);
	work = (obj_pool_free > debug_objects_pool_size) && obj_cache;
	obj_pool_used--;

	if (work) {
		obj_nr_tofree++;
		hlist_add_head(&obj->node, &obj_to_free);
	} else {
		obj_pool_free++;
		hlist_add_head(&obj->node, &obj_pool);
	}
	raw_spin_unlock(&pool_lock);
	local_irq_restore(flags);
	return work;
}

/*
 * Put the object back into the pool and schedule work to free objects
 * if necessary.
 */
static void free_object(struct debug_obj *obj)
{
	if (__free_object(obj))
		schedule_work(&debug_obj_work);
}

/*
 * We run out of memory. That means we probably have tons of objects
 * allocated.
 */
static void debug_objects_oom(void)
{
	struct debug_bucket *db = obj_hash;
	struct hlist_node *tmp;
	HLIST_HEAD(freelist);
	struct debug_obj *obj;
	unsigned long flags;
	int i;

	pr_warn("Out of memory. ODEBUG disabled\n");

	for (i = 0; i < ODEBUG_HASH_SIZE; i++, db++) {
		raw_spin_lock_irqsave(&db->lock, flags);
		hlist_move_list(&db->list, &freelist);
		raw_spin_unlock_irqrestore(&db->lock, flags);

		/* Now free them */
		hlist_for_each_entry_safe(obj, tmp, &freelist, node) {
			hlist_del(&obj->node);
			free_object(obj);
		}
	}
}

/*
 * We use the pfn of the address for the hash. That way we can check
 * for freed objects simply by checking the affected bucket.
 */
static struct debug_bucket *get_bucket(unsigned long addr)
{
	unsigned long hash;

	hash = hash_long((addr >> ODEBUG_CHUNK_SHIFT), ODEBUG_HASH_BITS);
	return &obj_hash[hash];
}

static void debug_print_object(struct debug_obj *obj, char *msg)
{
	struct debug_obj_descr *descr = obj->descr;
	static int limit;

	/*
	 * Don't report if lookup_object_or_alloc() by the current thread
	 * failed because lookup_object_or_alloc()/debug_objects_oom() by a
	 * concurrent thread turned off debug_objects_enabled and cleared
	 * the hash buckets.
	 */
	if (!debug_objects_enabled)
		return;

	if (limit < 5 && descr != descr_test) {
		void *hint = descr->debug_hint ?
			descr->debug_hint(obj->object) : NULL;
		limit++;
		WARN(1, KERN_ERR "ODEBUG: %s %s (active state %u) "
				 "object type: %s hint: %pS\n",
			msg, obj_states[obj->state], obj->astate,
			descr->name, hint);
	}
	debug_objects_warnings++;
}

#ifdef CONFIG_SEC_DEBUG_OBJECTS_ADDITIONAL_INFO
static void debug_objecte_additional_info(struct debug_obj *obj)
{
#ifdef CONFIG_STACKTRACE
	struct stack_trace trace;
	
	trace.nr_entries = 0;
	trace.max_entries = DEBUG_OBJ_CALLSTACK_MAX;
	trace.entries = (unsigned long *)obj->addrs;
	trace.skip = 0;
	save_stack_trace(&trace);
#endif
	return;
}
#endif

/*
 * Try to repair the damage, so we have a better chance to get useful
 * debug output.
 */
static bool
debug_object_fixup(bool (*fixup)(void *addr, enum debug_obj_state state),
		   void * addr, enum debug_obj_state state)
{
	if (fixup && fixup(addr, state)) {
		debug_objects_fixups++;
		return true;
	}
	return false;
}

static void debug_object_is_on_stack(void *addr, int onstack)
{
	int is_on_stack;
	static int limit;

	if (limit > 4)
		return;

	is_on_stack = object_is_on_stack(addr);
	if (is_on_stack == onstack)
		return;

	limit++;
	if (is_on_stack)
		pr_warn("object %p is on stack %p, but NOT annotated.\n", addr,
			 task_stack_page(current));
	else
		pr_warn("object %p is NOT on stack %p, but annotated.\n", addr,
			 task_stack_page(current));

	WARN_ON(1);
}

static struct debug_obj *lookup_object_or_alloc(void *addr, struct debug_bucket *b,
						struct debug_obj_descr *descr,
						bool onstack, bool alloc_ifstatic)
{
	struct debug_obj *obj = lookup_object(addr, b);
	enum debug_obj_state state = ODEBUG_STATE_NONE;

	if (likely(obj))
		return obj;

	/*
	 * debug_object_init() unconditionally allocates untracked
	 * objects. It does not matter whether it is a static object or
	 * not.
	 *
	 * debug_object_assert_init() and debug_object_activate() allow
	 * allocation only if the descriptor callback confirms that the
	 * object is static and considered initialized. For non-static
	 * objects the allocation needs to be done from the fixup callback.
	 */
	if (unlikely(alloc_ifstatic)) {
		if (!descr->is_static_object || !descr->is_static_object(addr))
			return ERR_PTR(-ENOENT);
		/* Statically allocated objects are considered initialized */
		state = ODEBUG_STATE_INIT;
	}

	obj = alloc_object(addr, b, descr);
	if (likely(obj)) {
		obj->state = state;
		debug_object_is_on_stack(addr, onstack);
		return obj;
	}

	/* Out of memory. Do the cleanup outside of the locked region */
	debug_objects_enabled = 0;
	return NULL;
}

static void debug_objects_fill_pool(void)
{
	/*
	 * On RT enabled kernels the pool refill must happen in preemptible
	 * context:
	 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT) || preemptible())
		fill_pool();
}

static void
__debug_object_init(void *addr, struct debug_obj_descr *descr, int onstack)
{
	enum debug_obj_state state;
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;

	debug_objects_fill_pool();

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object_or_alloc(addr, db, descr, onstack, false);
	if (unlikely(!obj)) {
		raw_spin_unlock_irqrestore(&db->lock, flags);
		debug_objects_oom();
		return;
	}

	switch (obj->state) {
	case ODEBUG_STATE_NONE:
	case ODEBUG_STATE_INIT:
	case ODEBUG_STATE_INACTIVE:
		obj->state = ODEBUG_STATE_INIT;
		break;

	case ODEBUG_STATE_ACTIVE:
		state = obj->state;
		raw_spin_unlock_irqrestore(&db->lock, flags);
		debug_print_object(obj, "init");
		debug_object_fixup(descr->fixup_init, addr, state);
		return;

	case ODEBUG_STATE_DESTROYED:
		raw_spin_unlock_irqrestore(&db->lock, flags);
		debug_print_object(obj, "init");
		return;
	default:
		break;
	}

	raw_spin_unlock_irqrestore(&db->lock, flags);
}

/**
 * debug_object_init - debug checks when an object is initialized
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_init(void *addr, struct debug_obj_descr *descr)
{
	if (!debug_objects_enabled)
		return;

	__debug_object_init(addr, descr, 0);
}
EXPORT_SYMBOL_GPL(debug_object_init);

/**
 * debug_object_init_on_stack - debug checks when an object on stack is
 *				initialized
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_init_on_stack(void *addr, struct debug_obj_descr *descr)
{
	if (!debug_objects_enabled)
		return;

	__debug_object_init(addr, descr, 1);
}
EXPORT_SYMBOL_GPL(debug_object_init_on_stack);

/**
 * debug_object_activate - debug checks when an object is activated
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 * Returns 0 for success, -EINVAL for check failed.
 */
int debug_object_activate(void *addr, struct debug_obj_descr *descr)
{
	struct debug_obj o = { .object = addr, .state = ODEBUG_STATE_NOTAVAILABLE, .descr = descr };
	enum debug_obj_state state;
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;
	int ret;

	if (!debug_objects_enabled)
		return 0;

	debug_objects_fill_pool();

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object_or_alloc(addr, db, descr, false, true);
	if (likely(!IS_ERR_OR_NULL(obj))) {
		bool print_object = false;

		switch (obj->state) {
		case ODEBUG_STATE_INIT:
		case ODEBUG_STATE_INACTIVE:
			obj->state = ODEBUG_STATE_ACTIVE;
#ifdef CONFIG_SEC_DEBUG_OBJECTS_ADDITIONAL_INFO
			debug_objecte_additional_info(obj);
#endif
			ret = 0;
			break;

		case ODEBUG_STATE_ACTIVE:
			state = obj->state;
			raw_spin_unlock_irqrestore(&db->lock, flags);
			debug_print_object(obj, "activate");
			ret = debug_object_fixup(descr->fixup_activate, addr, state);
#ifdef CONFIG_SEC_DEBUG_OBJECTS_FIXUP
			if (descr->fixup_activate)
				panic("%s() descr name : %s", __func__, descr->name);
#endif
			return ret ? 0 : -EINVAL;

		case ODEBUG_STATE_DESTROYED:
			print_object = true;
			ret = -EINVAL;
			break;
		default:
			ret = 0;
			break;
		}
		raw_spin_unlock_irqrestore(&db->lock, flags);
		if (print_object)
			debug_print_object(obj, "activate");
		return ret;
	}

	raw_spin_unlock_irqrestore(&db->lock, flags);

	/* If NULL the allocation has hit OOM */
	if (!obj) {
		debug_objects_oom();
		return 0;
	}

	/* Object is neither static nor tracked. It's not initialized */
	debug_print_object(&o, "activate");
	ret = debug_object_fixup(descr->fixup_activate, addr, ODEBUG_STATE_NOTAVAILABLE);
	return ret ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(debug_object_activate);

/**
 * debug_object_deactivate - debug checks when an object is deactivated
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_deactivate(void *addr, struct debug_obj_descr *descr)
{
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;
	bool print_object = false;

	if (!debug_objects_enabled)
		return;

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object(addr, db);
	if (obj) {
		switch (obj->state) {
		case ODEBUG_STATE_INIT:
		case ODEBUG_STATE_INACTIVE:
		case ODEBUG_STATE_ACTIVE:
			if (!obj->astate)
				obj->state = ODEBUG_STATE_INACTIVE;
			else
				print_object = true;
			break;

		case ODEBUG_STATE_DESTROYED:
			print_object = true;
			break;
		default:
			break;
		}
	}

	raw_spin_unlock_irqrestore(&db->lock, flags);
	if (!obj) {
		struct debug_obj o = { .object = addr,
				       .state = ODEBUG_STATE_NOTAVAILABLE,
				       .descr = descr };

		debug_print_object(&o, "deactivate");
	} else if (print_object) {
		debug_print_object(obj, "deactivate");
	}
}
EXPORT_SYMBOL_GPL(debug_object_deactivate);

/**
 * debug_object_destroy - debug checks when an object is destroyed
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_destroy(void *addr, struct debug_obj_descr *descr)
{
	enum debug_obj_state state;
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;
	bool print_object = false;

	if (!debug_objects_enabled)
		return;

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object(addr, db);
	if (!obj)
		goto out_unlock;

	switch (obj->state) {
	case ODEBUG_STATE_NONE:
	case ODEBUG_STATE_INIT:
	case ODEBUG_STATE_INACTIVE:
		obj->state = ODEBUG_STATE_DESTROYED;
		break;
	case ODEBUG_STATE_ACTIVE:
		state = obj->state;
		raw_spin_unlock_irqrestore(&db->lock, flags);
		debug_print_object(obj, "destroy");
		debug_object_fixup(descr->fixup_destroy, addr, state);
		return;

	case ODEBUG_STATE_DESTROYED:
		print_object = true;
		break;
	default:
		break;
	}
out_unlock:
	raw_spin_unlock_irqrestore(&db->lock, flags);
	if (print_object)
		debug_print_object(obj, "destroy");
}
EXPORT_SYMBOL_GPL(debug_object_destroy);

/**
 * debug_object_free - debug checks when an object is freed
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_free(void *addr, struct debug_obj_descr *descr)
{
	enum debug_obj_state state;
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;

	if (!debug_objects_enabled)
		return;

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object(addr, db);
	if (!obj)
		goto out_unlock;

	switch (obj->state) {
	case ODEBUG_STATE_ACTIVE:
		state = obj->state;
		raw_spin_unlock_irqrestore(&db->lock, flags);
		debug_print_object(obj, "free");
		debug_object_fixup(descr->fixup_free, addr, state);
		return;
	default:
		hlist_del(&obj->node);
		raw_spin_unlock_irqrestore(&db->lock, flags);
		free_object(obj);
		return;
	}
out_unlock:
	raw_spin_unlock_irqrestore(&db->lock, flags);
}
EXPORT_SYMBOL_GPL(debug_object_free);

/**
 * debug_object_assert_init - debug checks when object should be init-ed
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 */
void debug_object_assert_init(void *addr, struct debug_obj_descr *descr)
{
	struct debug_obj o = { .object = addr, .state = ODEBUG_STATE_NOTAVAILABLE, .descr = descr };
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;

	if (!debug_objects_enabled)
		return;

	debug_objects_fill_pool();

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);
	obj = lookup_object_or_alloc(addr, db, descr, false, true);
	raw_spin_unlock_irqrestore(&db->lock, flags);
	if (likely(!IS_ERR_OR_NULL(obj)))
		return;

	/* If NULL the allocation has hit OOM */
	if (!obj) {
		debug_objects_oom();
		return;
	}

	/* Object is neither tracked nor static. It's not initialized. */
	debug_print_object(&o, "assert_init");
	debug_object_fixup(descr->fixup_assert_init, addr, ODEBUG_STATE_NOTAVAILABLE);
}
EXPORT_SYMBOL_GPL(debug_object_assert_init);

/**
 * debug_object_active_state - debug checks object usage state machine
 * @addr:	address of the object
 * @descr:	pointer to an object specific debug description structure
 * @expect:	expected state
 * @next:	state to move to if expected state is found
 */
void
debug_object_active_state(void *addr, struct debug_obj_descr *descr,
			  unsigned int expect, unsigned int next)
{
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;
	bool print_object = false;

	if (!debug_objects_enabled)
		return;

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object(addr, db);
	if (obj) {
		switch (obj->state) {
		case ODEBUG_STATE_ACTIVE:
			if (obj->astate == expect)
				obj->astate = next;
			else
				print_object = true;
			break;

		default:
			print_object = true;
			break;
		}
	}

	raw_spin_unlock_irqrestore(&db->lock, flags);
	if (!obj) {
		struct debug_obj o = { .object = addr,
				       .state = ODEBUG_STATE_NOTAVAILABLE,
				       .descr = descr };

		debug_print_object(&o, "active_state");
	} else if (print_object) {
		debug_print_object(obj, "active_state");
	}
}
EXPORT_SYMBOL_GPL(debug_object_active_state);

#ifdef CONFIG_DEBUG_OBJECTS_FREE
static void __debug_check_no_obj_freed(const void *address, unsigned long size)
{
	unsigned long flags, oaddr, saddr, eaddr, paddr, chunks;
	struct debug_obj_descr *descr;
	enum debug_obj_state state;
	struct debug_bucket *db;
	struct hlist_node *tmp;
	struct debug_obj *obj;
	int cnt, objs_checked = 0;
	bool work = false;
#ifdef CONFIG_SEC_DEBUG_OBJECTS_FREE	
	int fixup_cnt = 0;
#endif	

	saddr = (unsigned long) address;
	eaddr = saddr + size;
	paddr = saddr & ODEBUG_CHUNK_MASK;
	chunks = ((eaddr - paddr) + (ODEBUG_CHUNK_SIZE - 1));
	chunks >>= ODEBUG_CHUNK_SHIFT;

	for (;chunks > 0; chunks--, paddr += ODEBUG_CHUNK_SIZE) {
		db = get_bucket(paddr);

repeat:
		cnt = 0;
		raw_spin_lock_irqsave(&db->lock, flags);
		hlist_for_each_entry_safe(obj, tmp, &db->list, node) {
			cnt++;
			oaddr = (unsigned long) obj->object;
			if (oaddr < saddr || oaddr >= eaddr)
				continue;

			switch (obj->state) {
			case ODEBUG_STATE_ACTIVE:
				descr = obj->descr;
				state = obj->state;
				raw_spin_unlock_irqrestore(&db->lock, flags);
				debug_print_object(obj, "free");
				debug_object_fixup(descr->fixup_free,
						   (void *) oaddr, state);
#ifdef CONFIG_SEC_DEBUG_OBJECTS_FREE
				fixup_cnt++;
#endif
				goto repeat;
			default:
				hlist_del(&obj->node);
				work |= __free_object(obj);
				break;
			}
		}
		raw_spin_unlock_irqrestore(&db->lock, flags);

#ifdef CONFIG_SEC_DEBUG_OBJECTS_FREE
		if (fixup_cnt)
			panic("%s fixup_cnt : %d", __func__, fixup_cnt);
#endif
		if (cnt > debug_objects_maxchain)
			debug_objects_maxchain = cnt;

		objs_checked += cnt;
	}

	if (objs_checked > debug_objects_maxchecked)
		debug_objects_maxchecked = objs_checked;

	/* Schedule work to actually kmem_cache_free() objects */
	if (work)
		schedule_work(&debug_obj_work);
}

void debug_check_no_obj_freed(const void *address, unsigned long size)
{
	if (debug_objects_enabled)
		__debug_check_no_obj_freed(address, size);
}
#endif

#ifdef CONFIG_DEBUG_FS

static int debug_stats_show(struct seq_file *m, void *v)
{
	int cpu, obj_percpu_free = 0;

	for_each_possible_cpu(cpu)
		obj_percpu_free += per_cpu(percpu_obj_pool.obj_free, cpu);

	seq_printf(m, "max_chain     :%d\n", debug_objects_maxchain);
	seq_printf(m, "max_checked   :%d\n", debug_objects_maxchecked);
	seq_printf(m, "warnings      :%d\n", debug_objects_warnings);
	seq_printf(m, "fixups        :%d\n", debug_objects_fixups);
	seq_printf(m, "pool_free     :%d\n", obj_pool_free + obj_percpu_free);
	seq_printf(m, "pool_pcp_free :%d\n", obj_percpu_free);
	seq_printf(m, "pool_min_free :%d\n", obj_pool_min_free);
	seq_printf(m, "pool_used     :%d\n", obj_pool_used - obj_percpu_free);
	seq_printf(m, "pool_max_used :%d\n", obj_pool_max_used);
	seq_printf(m, "on_free_list  :%d\n", obj_nr_tofree);
	seq_printf(m, "objs_allocated:%d\n", debug_objects_allocated);
	seq_printf(m, "objs_freed    :%d\n", debug_objects_freed);
	return 0;
}

static int debug_stats_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, debug_stats_show, NULL);
}

static const struct file_operations debug_stats_fops = {
	.open		= debug_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init debug_objects_init_debugfs(void)
{
	struct dentry *dbgdir, *dbgstats;

	if (!debug_objects_enabled)
		return 0;

	dbgdir = debugfs_create_dir("debug_objects", NULL);
	if (!dbgdir)
		return -ENOMEM;

	dbgstats = debugfs_create_file("stats", 0444, dbgdir, NULL,
				       &debug_stats_fops);
	if (!dbgstats)
		goto err;

	return 0;

err:
	debugfs_remove(dbgdir);

	return -ENOMEM;
}
__initcall(debug_objects_init_debugfs);

#else
static inline void debug_objects_init_debugfs(void) { }
#endif

#ifdef CONFIG_DEBUG_OBJECTS_SELFTEST

/* Random data structure for the self test */
struct self_test {
	unsigned long	dummy1[6];
	int		static_init;
	unsigned long	dummy2[3];
};

static __initdata struct debug_obj_descr descr_type_test;

static bool __init is_static_object(void *addr)
{
	struct self_test *obj = addr;

	return obj->static_init;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static bool __init fixup_init(void *addr, enum debug_obj_state state)
{
	struct self_test *obj = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		debug_object_deactivate(obj, &descr_type_test);
		debug_object_init(obj, &descr_type_test);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown non-static object is activated
 */
static bool __init fixup_activate(void *addr, enum debug_obj_state state)
{
	struct self_test *obj = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		return true;
	case ODEBUG_STATE_ACTIVE:
		debug_object_deactivate(obj, &descr_type_test);
		debug_object_activate(obj, &descr_type_test);
		return true;

	default:
		return false;
	}
}

/*
 * fixup_destroy is called when:
 * - an active object is destroyed
 */
static bool __init fixup_destroy(void *addr, enum debug_obj_state state)
{
	struct self_test *obj = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		debug_object_deactivate(obj, &descr_type_test);
		debug_object_destroy(obj, &descr_type_test);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static bool __init fixup_free(void *addr, enum debug_obj_state state)
{
	struct self_test *obj = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		debug_object_deactivate(obj, &descr_type_test);
		debug_object_free(obj, &descr_type_test);
		return true;
	default:
		return false;
	}
}

static int __init
check_results(void *addr, enum debug_obj_state state, int fixups, int warnings)
{
	struct debug_bucket *db;
	struct debug_obj *obj;
	unsigned long flags;
	int res = -EINVAL;

	db = get_bucket((unsigned long) addr);

	raw_spin_lock_irqsave(&db->lock, flags);

	obj = lookup_object(addr, db);
	if (!obj && state != ODEBUG_STATE_NONE) {
		WARN(1, KERN_ERR "ODEBUG: selftest object not found\n");
		goto out;
	}
	if (obj && obj->state != state) {
		WARN(1, KERN_ERR "ODEBUG: selftest wrong state: %d != %d\n",
		       obj->state, state);
		goto out;
	}
	if (fixups != debug_objects_fixups) {
		WARN(1, KERN_ERR "ODEBUG: selftest fixups failed %d != %d\n",
		       fixups, debug_objects_fixups);
		goto out;
	}
	if (warnings != debug_objects_warnings) {
		WARN(1, KERN_ERR "ODEBUG: selftest warnings failed %d != %d\n",
		       warnings, debug_objects_warnings);
		goto out;
	}
	res = 0;
out:
	raw_spin_unlock_irqrestore(&db->lock, flags);
	if (res)
		debug_objects_enabled = 0;
	return res;
}

static __initdata struct debug_obj_descr descr_type_test = {
	.name			= "selftest",
	.is_static_object	= is_static_object,
	.fixup_init		= fixup_init,
	.fixup_activate		= fixup_activate,
	.fixup_destroy		= fixup_destroy,
	.fixup_free		= fixup_free,
};

static __initdata struct self_test obj = { .static_init = 0 };

static void __init debug_objects_selftest(void)
{
	int fixups, oldfixups, warnings, oldwarnings;
	unsigned long flags;

	local_irq_save(flags);

	fixups = oldfixups = debug_objects_fixups;
	warnings = oldwarnings = debug_objects_warnings;
	descr_test = &descr_type_test;

	debug_object_init(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_INIT, fixups, warnings))
		goto out;
	debug_object_activate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_ACTIVE, fixups, warnings))
		goto out;
	debug_object_activate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_ACTIVE, ++fixups, ++warnings))
		goto out;
	debug_object_deactivate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_INACTIVE, fixups, warnings))
		goto out;
	debug_object_destroy(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_DESTROYED, fixups, warnings))
		goto out;
	debug_object_init(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_DESTROYED, fixups, ++warnings))
		goto out;
	debug_object_activate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_DESTROYED, fixups, ++warnings))
		goto out;
	debug_object_deactivate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_DESTROYED, fixups, ++warnings))
		goto out;
	debug_object_free(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_NONE, fixups, warnings))
		goto out;

	obj.static_init = 1;
	debug_object_activate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_ACTIVE, fixups, warnings))
		goto out;
	debug_object_init(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_INIT, ++fixups, ++warnings))
		goto out;
	debug_object_free(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_NONE, fixups, warnings))
		goto out;

#ifdef CONFIG_DEBUG_OBJECTS_FREE
	debug_object_init(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_INIT, fixups, warnings))
		goto out;
	debug_object_activate(&obj, &descr_type_test);
	if (check_results(&obj, ODEBUG_STATE_ACTIVE, fixups, warnings))
		goto out;
	__debug_check_no_obj_freed(&obj, sizeof(obj));
	if (check_results(&obj, ODEBUG_STATE_NONE, ++fixups, ++warnings))
		goto out;
#endif
	pr_info("selftest passed\n");

out:
	debug_objects_fixups = oldfixups;
	debug_objects_warnings = oldwarnings;
	descr_test = NULL;

	local_irq_restore(flags);
}
#else
static inline void debug_objects_selftest(void) { }
#endif

/*
 * Called during early boot to initialize the hash buckets and link
 * the static object pool objects into the poll list. After this call
 * the object tracker is fully operational.
 */
void __init debug_objects_early_init(void)
{
	int i;

	for (i = 0; i < ODEBUG_HASH_SIZE; i++)
		raw_spin_lock_init(&obj_hash[i].lock);

	for (i = 0; i < ODEBUG_POOL_SIZE; i++)
		hlist_add_head(&obj_static_pool[i].node, &obj_pool);
}

/*
 * Convert the statically allocated objects to dynamic ones:
 */
static int __init debug_objects_replace_static_objects(void)
{
	struct debug_bucket *db = obj_hash;
	struct hlist_node *tmp;
	struct debug_obj *obj, *new;
	HLIST_HEAD(objects);
	int i, cnt = 0;

	for (i = 0; i < ODEBUG_POOL_SIZE; i++) {
		obj = kmem_cache_zalloc(obj_cache, GFP_KERNEL);
		if (!obj)
			goto free;
		hlist_add_head(&obj->node, &objects);
	}

	/*
	 * When debug_objects_mem_init() is called we know that only
	 * one CPU is up, so disabling interrupts is enough
	 * protection. This avoids the lockdep hell of lock ordering.
	 */
	local_irq_disable();

	/* Remove the statically allocated objects from the pool */
	hlist_for_each_entry_safe(obj, tmp, &obj_pool, node)
		hlist_del(&obj->node);
	/* Move the allocated objects to the pool */
	hlist_move_list(&objects, &obj_pool);

	/* Replace the active object references */
	for (i = 0; i < ODEBUG_HASH_SIZE; i++, db++) {
		hlist_move_list(&db->list, &objects);

		hlist_for_each_entry(obj, &objects, node) {
			new = hlist_entry(obj_pool.first, typeof(*obj), node);
			hlist_del(&new->node);
			/* copy object data */
			*new = *obj;
			hlist_add_head(&new->node, &db->list);
			cnt++;
		}
	}
	local_irq_enable();

	pr_debug("%d of %d active objects replaced\n",
		 cnt, obj_pool_used);
	return 0;
free:
	hlist_for_each_entry_safe(obj, tmp, &objects, node) {
		hlist_del(&obj->node);
		kmem_cache_free(obj_cache, obj);
	}
	return -ENOMEM;
}

/*
 * Called after the kmem_caches are functional to setup a dedicated
 * cache pool, which has the SLAB_DEBUG_OBJECTS flag set. This flag
 * prevents that the debug code is called on kmem_cache_free() for the
 * debug tracker objects to avoid recursive calls.
 */
void __init debug_objects_mem_init(void)
{
	int cpu;

	if (!debug_objects_enabled)
		return;

	/*
	 * Initialize the percpu object pools
	 *
	 * Initialization is not strictly necessary, but was done for
	 * completeness.
	 */
	for_each_possible_cpu(cpu)
		INIT_HLIST_HEAD(&per_cpu(percpu_obj_pool.free_objs, cpu));

	obj_cache = kmem_cache_create("debug_objects_cache",
				      sizeof (struct debug_obj), 0,
				      SLAB_DEBUG_OBJECTS | SLAB_NOLEAKTRACE,
				      NULL);

	if (!obj_cache || debug_objects_replace_static_objects()) {
		debug_objects_enabled = 0;
		kmem_cache_destroy(obj_cache);
		pr_warn("out of memory.\n");
	} else
		debug_objects_selftest();
}

/*
 *  Copyright (C) 2003-2005 Pontus Fuchs, Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#include "ntoskernel.h"
#include "ndis.h"
#include "usb.h"
#include "pnp.h"
#include "loader.h"

/* MDLs describe a range of virtual address with an array of physical
 * pages right after the header. For different ranges of virtual
 * addresses, the number of entries of physical pages may be different
 * (depending on number of entries required). If we want to allocate
 * MDLs from a pool, the size has to be constant. So we assume that
 * maximum range used by a driver is CACHE_MDL_PAGES; if a driver
 * requests an MDL for a bigger region, we allocate it with kmalloc;
 * otherwise, we allocate from the pool */
#define CACHE_MDL_PAGES 2
#define CACHE_MDL_SIZE (sizeof(struct mdl) + (sizeof(ULONG) * CACHE_MDL_PAGES))
struct wrap_mdl {
	struct nt_list list;
	char mdl[CACHE_MDL_SIZE];
};

/* everything here is for all drivers/devices - not per driver/device */
static KSPIN_LOCK nt_event_lock;
KSPIN_LOCK ntoskernel_lock;
static kmem_cache_t *mdl_cache;
static struct nt_list wrap_mdl_list;

static KSPIN_LOCK inter_lock;

struct work_struct kdpc_work;
static struct nt_list kdpc_list;
static KSPIN_LOCK kdpc_list_lock;
static void kdpc_worker(void *data);

static struct nt_list callback_objects;

struct nt_list object_list;

struct bus_driver {
	struct nt_list list;
	char name[MAX_DRIVER_NAME_LEN];
	struct driver_object drv_obj;
};

static struct nt_list bus_driver_list;
static void del_bus_drivers(void);

struct work_struct wrap_work_item_work;
struct nt_list wrap_work_item_list;
KSPIN_LOCK wrap_work_item_list_lock;
static void wrap_work_item_worker(void *data);

KSPIN_LOCK irp_cancel_lock;

extern KSPIN_LOCK loader_lock;
extern struct nt_list wrap_drivers;
static struct nt_list wrap_timer_list;
KSPIN_LOCK timer_lock;

/* compute ticks (100ns) since 1601 until when system booted into
 * wrap_ticks_to_boot */
u64 wrap_ticks_to_boot;

#if defined(CONFIG_X86_64)
static struct timer_list shared_data_timer;
struct kuser_shared_data kuser_shared_data;
static void update_user_shared_data_proc(unsigned long data);
#endif

static int add_bus_driver(const char *name);
static void free_all_objects(void);
static BOOLEAN queue_kdpc(struct kdpc *kdpc);

WRAP_EXPORT_MAP("KeTickCount", &jiffies);

WRAP_EXPORT_MAP("NlsMbCodePageTag", FALSE);

int ntoskernel_init(void)
{
	struct timeval now;

	kspin_lock_init(&nt_event_lock);
	kspin_lock_init(&ntoskernel_lock);
	kspin_lock_init(&wrap_work_item_list_lock);
	kspin_lock_init(&kdpc_list_lock);
	kspin_lock_init(&irp_cancel_lock);
	kspin_lock_init(&inter_lock);
	InitializeListHead(&wrap_mdl_list);
	InitializeListHead(&kdpc_list);
	InitializeListHead(&callback_objects);
	InitializeListHead(&bus_driver_list);
	InitializeListHead(&object_list);
	InitializeListHead(&wrap_work_item_list);

	INIT_WORK(&kdpc_work, kdpc_worker, NULL);
	INIT_WORK(&wrap_work_item_work, wrap_work_item_worker, NULL);

	kspin_lock_init(&timer_lock);
	InitializeListHead(&wrap_timer_list);

	do_gettimeofday(&now);
	wrap_ticks_to_boot = (u64)now.tv_sec * TICKSPERSEC;
	wrap_ticks_to_boot += now.tv_usec * 10;
	wrap_ticks_to_boot -= jiffies * TICKSPERSEC / HZ;
	wrap_ticks_to_boot += TICKS_1601_TO_1970;

	if (add_bus_driver("PCI")
#ifdef CONFIG_USB
	    || add_bus_driver("USB")
#endif
		) {
		ntoskernel_exit();
		return -ENOMEM;
	}
	mdl_cache = kmem_cache_create("wrap_mdl", sizeof(struct wrap_mdl),
				      0, 0, NULL, NULL);
	if (!mdl_cache) {
		ERROR("couldn't allocate MDL cache");
		ntoskernel_exit();
		return -ENOMEM;
	}
#if defined(CONFIG_X86_64)
	memset(&kuser_shared_data, 0, sizeof(kuser_shared_data));
	init_timer(&shared_data_timer);
	shared_data_timer.function = &update_user_shared_data_proc;
#endif
	return 0;
}

int ntoskernel_init_device(struct wrap_device *wd)
{
	InitializeListHead(&wd->timer_list);
	kspin_lock_init(&wd->timer_lock);
#if defined(CONFIG_X86_64)
	*((ULONG64 *)&kuser_shared_data.system_time) = ticks_1601();
	shared_data_timer.data = (unsigned long)0;
	/* don't use add_timer - to avoid creating more than one
	 * timer */
	mod_timer(&shared_data_timer, jiffies + 1);
#endif
	return 0;
}

void ntoskernel_exit_device(struct wrap_device *wd)
{
	KIRQL irql;

	TRACEENTER2("");

	KeFlushQueuedDpcs();
	/* cancel any timers left by bugyy windows driver; also free
	 * the memory for timers */
	while (1) {
		struct nt_list *ent;
		struct wrap_timer *wrap_timer;

		irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
		ent = RemoveHeadList(&wd->timer_list);
		kspin_unlock_irql(&timer_lock, irql);
		if (!ent)
			break;
		wrap_timer = container_of(ent, struct wrap_timer, list);
		/* ktimer that this wrap_timer is associated to can't
		 * be touched, as it may have been freed by the driver
		 * already */
		if (del_timer_sync(&wrap_timer->timer))
			WARNING("Buggy Windows driver left timer %p running",
				&wrap_timer->timer);
		memset(wrap_timer, 0, sizeof(*wrap_timer));
		wrap_kfree(wrap_timer);
	}
	TRACEEXIT2(return);
}

void ntoskernel_exit(void)
{
	struct nt_list *cur;
	KIRQL irql;

	TRACEENTER2("");
	/* free kernel (Ke) timers */
	DBGTRACE2("freeing timers");
	while (1) {
		struct wrap_timer *wrap_timer;

		irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
		cur = RemoveTailList(&wrap_timer_list);
		kspin_unlock_irql(&timer_lock, irql);
		if (!cur)
			break;
		wrap_timer = container_of(cur, struct wrap_timer, list);
		if (del_timer_sync(&wrap_timer->timer))
			WARNING("Buggy Windows driver left timer %p running",
				&wrap_timer->timer);
		memset(wrap_timer, 0, sizeof(*wrap_timer));
		wrap_kfree(wrap_timer);
	}

	DBGTRACE2("freeing MDLs");
	if (mdl_cache) {
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		if (!IsListEmpty(&wrap_mdl_list)) {
			ERROR("Windows driver didn't free all MDLs; "
			      "freeing them now");
			while ((cur = RemoveHeadList(&wrap_mdl_list))) {
				struct wrap_mdl *p;
				struct mdl *mdl;
				p = container_of(cur, struct wrap_mdl, list);
				mdl = (struct mdl *)p->mdl;
				if (mdl->flags & MDL_CACHE_ALLOCATED)
					kmem_cache_free(mdl_cache, p);
				else
					kfree(p);
			}
		}
		kspin_unlock_irql(&ntoskernel_lock, irql);
		kmem_cache_destroy(mdl_cache);
		mdl_cache = NULL;
	}

	DBGTRACE2("freeing callbacks");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	while ((cur = RemoveHeadList(&callback_objects))) {
		struct callback_object *object;
		struct nt_list *ent;
		object = container_of(cur, struct callback_object, list);
		while ((ent = RemoveHeadList(&object->callback_funcs))) {
			struct callback_func *f;
			f = container_of(ent, struct callback_func, list);
			kfree(f);
		}
		kfree(object);
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);

	del_bus_drivers();
	free_all_objects();
#if defined(CONFIG_X86_64)
	del_timer_sync(&shared_data_timer);
#endif
	TRACEEXIT2(return);
}

#if defined(CONFIG_X86_64)
static void update_user_shared_data_proc(unsigned long data)
{
	/* timer is supposed to be scheduled every 10ms, but bigger
	 * intervals seem to work (tried upto 50ms) */
	*((ULONG64 *)&kuser_shared_data.system_time) = ticks_1601();
	*((ULONG64 *)&kuser_shared_data.interrupt_time) =
		jiffies * TICKSPERSEC / HZ;
	*((ULONG64 *)&kuser_shared_data.tick) = jiffies;

	shared_data_timer.expires += 30 * HZ / 1000 + 1;
	add_timer(&shared_data_timer);
}
#endif

void *allocate_object(ULONG size, enum common_object_type type, char *name)
{									
	struct common_object_header *hdr;
	KIRQL irql;
	void *body;

	/* we pad header as prefix to body */
	hdr = ExAllocatePoolWithTag(NonPagedPool, OBJECT_SIZE(size), 0);
	if (!hdr) {
		WARNING("couldn't allocate memory");
		return NULL;
	}
	memset(hdr, 0, OBJECT_SIZE(size));
	hdr->type = type;
	hdr->ref_count = 1;
	hdr->name = name;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	/* threads are looked up often (in KeWaitForXXx), so optimize
	 * for fast lookups of threads */
	if (type == OBJECT_TYPE_NT_THREAD)
		InsertHeadList(&object_list, &hdr->list);
	else
		InsertTailList(&object_list, &hdr->list);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	body = HEADER_TO_OBJECT(hdr);
	DBGTRACE3("allocated hdr: %p, body: %p", hdr, body);
	return body;
}

void free_object(void *object)
{									
	struct common_object_header *hdr;
	KIRQL irql;
	hdr = OBJECT_TO_HEADER(object);
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	RemoveEntryList(&hdr->list);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	DBGTRACE3("freed hdr: %p, body: %p", hdr, object);
	ExFreePool(hdr);
}

static void free_all_objects(void)
{
	struct nt_list *cur;
	KIRQL irql;

	TRACEENTER2("freeing objects");
	/* delete all objects */
	while (1) {
		struct common_object_header *hdr;

		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		cur = RemoveHeadList(&object_list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		if (!cur)
			break;
		hdr = container_of(cur, struct common_object_header, list);
		WARNING("object %p type %d was not freed, freeing it now",
			HEADER_TO_OBJECT(hdr), hdr->type);
		ExFreePool(hdr);
	}
	TRACEEXIT2(return);
}

static int add_bus_driver(const char *name)
{
	struct bus_driver *bus_driver;
	KIRQL irql;

	bus_driver = kmalloc(sizeof(*bus_driver), GFP_KERNEL);
	if (!bus_driver) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	memset(bus_driver, 0, sizeof(*bus_driver));
	strncpy(bus_driver->name, name, sizeof(bus_driver->name));
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	InsertTailList(&bus_driver_list, &bus_driver->list);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	DBGTRACE1("bus driver %s is at %p", name, &bus_driver->drv_obj);
	return STATUS_SUCCESS;
}

static void del_bus_drivers(void)
{
	struct nt_list *ent;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	while ((ent = RemoveHeadList(&bus_driver_list))) {
		struct bus_driver *bus_driver;
		bus_driver = container_of(ent, struct bus_driver, list);
		/* TODO: make sure all all drivers are shutdown/removed */
		kfree(bus_driver);
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
}

struct driver_object *find_bus_driver(const char *name)
{
	struct bus_driver *bus_driver;
	struct driver_object *drv_obj;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	drv_obj = NULL;
	nt_list_for_each_entry(bus_driver, &bus_driver_list, list) {
		if (strcmp(bus_driver->name, name) == 0)
			drv_obj = &bus_driver->drv_obj;
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return drv_obj;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedInsertHeadList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	struct nt_list *first;
	KIRQL irql;

	TRACEENTER5("head = %p, entry = %p", head, entry);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	first = InsertHeadList(head, entry);
	kspin_unlock_irql(lock, irql);
	DBGTRACE5("head = %p, old = %p", head, first);
	return first;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedInsertHeadList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExfInterlockedInsertHeadList(FASTCALL_ARGS_3(head, entry,
							    lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedInsertTailList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	struct nt_list *last;
	KIRQL irql;

	TRACEENTER5("head = %p, entry = %p", head, entry);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	last = InsertTailList(head, entry);
	kspin_unlock_irql(lock, irql);
	DBGTRACE5("head = %p, old = %p", head, last);
	return last;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedInsertTailList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExfInterlockedInsertTailList(FASTCALL_ARGS_3(head, entry,
							    lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedRemoveHeadList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	struct nt_list *ret;
	KIRQL irql;

	TRACEENTER5("head = %p", head);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	ret = RemoveHeadList(head);
	kspin_unlock_irql(lock, irql);
	DBGTRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedRemoveHeadList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExfInterlockedRemoveHeadList(FASTCALL_ARGS_2(head, lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedRemoveTailList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	struct nt_list *ret;
	KIRQL irql;

	TRACEENTER5("head = %p", head);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	ret = RemoveTailList(head);
	kspin_unlock_irql(lock, irql);
	DBGTRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedRemoveTailList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExfInterlockedRemoveTailList(FASTCALL_ARGS_2(head, lock));
}

STDCALL struct nt_slist *WRAP_EXPORT(ExpInterlockedPushEntrySList)
	(union nt_slist_head *head, struct nt_slist *entry)
{
	struct nt_slist *ret;
	KIRQL irql;

	TRACEENTER5("head = %p", head);
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	ret = PushEntryList(head, entry);
	kspin_unlock_irql(&inter_lock, irql);
	DBGTRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_slist *WRAP_EXPORT(ExInterlockedPushEntrySList)
	(FASTCALL_DECL_3(union nt_slist_head *head, struct nt_slist *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExpInterlockedPushEntrySList(head, entry);
}

_FASTCALL struct nt_slist *WRAP_EXPORT(InterlockedPushEntrySList)
	(FASTCALL_DECL_2(union nt_slist_head *head, struct nt_slist *entry))
{
	TRACEENTER5("%p", head);
	return ExpInterlockedPushEntrySList(head, entry);
}

STDCALL struct nt_slist *WRAP_EXPORT(ExpInterlockedPopEntrySList)
	(union nt_slist_head *head)
{
	struct nt_slist *ret;
	KIRQL irql;

	TRACEENTER5("head = %p", head);
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	ret = PopEntryList(head);
	kspin_unlock_irql(&inter_lock, irql);
	DBGTRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_slist *WRAP_EXPORT(ExInterlockedPopEntrySList)
	(FASTCALL_DECL_2(union nt_slist_head *head, KSPIN_LOCK *lock))
{
	TRACEENTER5("%p", head);
	return ExpInterlockedPopEntrySList(head);
}

_FASTCALL struct nt_slist *WRAP_EXPORT(InterlockedPopEntrySList)
	(FASTCALL_DECL_1(union nt_slist_head *head))
{
	TRACEENTER5("%p", head);
	return ExpInterlockedPopEntrySList(head);
}

STDCALL USHORT WRAP_EXPORT(ExQueryDepthSList)
	(union nt_slist_head *head)
{
	TRACEENTER5("%p", head);
	return head->list.depth;
}

/* should be called with nt_event_lock held at DISPATCH_LEVEL */
static void initialize_dh(struct dispatch_header *dh, enum event_type type,
			  int state, enum dh_type dh_type)
{
	memset(dh, 0, sizeof(*dh));
	dh->type = type;
	dh->signal_state = state;
	set_dh_type(dh, dh_type);
	InitializeListHead(&dh->wait_blocks);
}

static void timer_proc(unsigned long data)
{
	struct nt_timer *nt_timer = (struct nt_timer *)data;
	struct wrap_timer *wrap_timer;
	struct kdpc *kdpc;
	KIRQL irql;

	wrap_timer = nt_timer->wrap_timer;
	TRACEENTER5("%p(%p), %lu", wrap_timer, nt_timer, jiffies);
#ifdef DEBUG_TIMER
	BUG_ON(wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
	BUG_ON(nt_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
#endif
	kdpc = nt_timer->kdpc;
	KeSetEvent((struct nt_event *)nt_timer, 0, FALSE);
	if (kdpc && kdpc->func) {
		DBGTRACE5("kdpc %p (%p)", kdpc, kdpc->func);
		LIN2WIN4(kdpc->func, kdpc, kdpc->ctx,
			 kdpc->arg1, kdpc->arg2);
	}

	irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
	if (wrap_timer->repeat)
		mod_timer(&wrap_timer->timer, jiffies + wrap_timer->repeat);
	kspin_unlock_irql(&timer_lock, irql);

	TRACEEXIT5(return);
}

void wrap_init_timer(struct nt_timer *nt_timer, enum timer_type type,
		     struct wrap_device *wd)
{
	struct wrap_timer *wrap_timer;
	KIRQL irql;

	/* TODO: if a timer is initialized more than once, we allocate
	 * memory for wrap_timer more than once for the same nt_timer,
	 * wasting memory. We can check if nt_timer->wrap_timer_magic is
	 * set and not allocate, but it is not guaranteed always to be
	 * safe */
	TRACEENTER5("%p", nt_timer);
	/* we allocate memory for wrap_timer behind driver's back
	 * and there is no NDIS/DDK function where this memory can be
	 * freed, so we use wrap_kmalloc so it gets freed when driver
	 * is unloaded */
	wrap_timer = wrap_kmalloc(sizeof(*wrap_timer));
	if (!wrap_timer) {
		ERROR("couldn't allocate memory for timer");
		return;
	}

	memset(wrap_timer, 0, sizeof(*wrap_timer));
	init_timer(&wrap_timer->timer);
	wrap_timer->timer.data = (unsigned long)nt_timer;
	wrap_timer->timer.function = &timer_proc;
	wrap_timer->nt_timer = nt_timer;
#ifdef DEBUG_TIMER
	wrap_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
#endif
	nt_timer->wrap_timer = wrap_timer;
	nt_timer->kdpc = NULL;
	initialize_dh(&nt_timer->dh, type, 0, DH_NT_TIMER);
	nt_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
	if (wd) {
		irql = kspin_lock_irql(&wd->timer_lock, DISPATCH_LEVEL);
		InsertTailList(&wd->timer_list, &wrap_timer->list);
		kspin_unlock_irql(&wd->timer_lock, irql);
	} else {
		irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
		InsertTailList(&wrap_timer_list, &wrap_timer->list);
		kspin_unlock_irql(&timer_lock, irql);
	}
	DBGTRACE5("timer %p (%p)", wrap_timer, nt_timer);
	TRACEEXIT5(return);
}

STDCALL void WRAP_EXPORT(KeInitializeTimerEx)
	(struct nt_timer *nt_timer, enum timer_type type)
{
	TRACEENTER5("%p", nt_timer);
	wrap_init_timer(nt_timer, type, NULL);
}

STDCALL void WRAP_EXPORT(KeInitializeTimer)
	(struct nt_timer *nt_timer)
{
	TRACEENTER5("%p", nt_timer);
	wrap_init_timer(nt_timer, NotificationTimer, NULL);
}

/* expires and repeat are in HZ */
BOOLEAN wrap_set_timer(struct nt_timer *nt_timer, unsigned long expires_hz,
		       unsigned long repeat_hz, struct kdpc *kdpc)
{
	BOOLEAN ret;
	KIRQL irql;
	struct wrap_timer *wrap_timer;

	TRACEENTER4("%p, %lu, %lu, %p, %lu",
		    nt_timer, expires_hz, repeat_hz, kdpc, jiffies);

	KeClearEvent((struct nt_event *)nt_timer);
	wrap_timer = nt_timer->wrap_timer;

#ifdef DEBUG_TIMER
	if (nt_timer->wrap_timer_magic != WRAP_TIMER_MAGIC) {
		WARNING("Buggy Windows timer didn't initialize timer %p",
			nt_timer);
		return FALSE;
	}
	if (wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC) {
		WARNING("timer %p is not initialized (%lx)?",
			wrap_timer, wrap_timer->wrap_timer_magic);
		wrap_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
	}
#endif
	irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
	if (kdpc)
		nt_timer->kdpc = kdpc;
	wrap_timer->repeat = repeat_hz;
	if (mod_timer(&wrap_timer->timer, jiffies + expires_hz))
		ret = TRUE;
	else
		ret = FALSE;
	kspin_unlock_irql(&timer_lock, irql);
	TRACEEXIT5(return ret);
}

STDCALL BOOLEAN WRAP_EXPORT(KeSetTimerEx)
	(struct nt_timer *nt_timer, LARGE_INTEGER duetime_ticks,
	 LONG period_ms, struct kdpc *kdpc)
{
	unsigned long expires_hz, repeat_hz;

	DBGTRACE5("%p, %Ld, %d", nt_timer, duetime_ticks, period_ms);
	expires_hz = SYSTEM_TIME_TO_HZ(duetime_ticks) + 1;
	repeat_hz = MSEC_TO_HZ(period_ms);
	return wrap_set_timer(nt_timer, expires_hz, repeat_hz, kdpc);
}

STDCALL BOOLEAN WRAP_EXPORT(KeSetTimer)
	(struct nt_timer *nt_timer, LARGE_INTEGER duetime_ticks,
	 struct kdpc *kdpc)
{
	TRACEENTER5("%p, %Ld, %p", nt_timer, duetime_ticks, kdpc);
	return KeSetTimerEx(nt_timer, duetime_ticks, 0, kdpc);
}

STDCALL BOOLEAN WRAP_EXPORT(KeCancelTimer)
	(struct nt_timer *nt_timer)
{
	BOOLEAN canceled;
	KIRQL irql;
	struct wrap_timer *wrap_timer;

	TRACEENTER5("%p", nt_timer);
	wrap_timer = nt_timer->wrap_timer;
	if (!wrap_timer) {
		ERROR("invalid wrap_timer");
		return TRUE;
	}
#ifdef DEBUG_TIMER
	DBGTRACE5("canceling timer %p", wrap_timer);
	BUG_ON(wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
#endif
	/* del_timer_sync may not be called here, as this function can
	 * be called at DISPATCH_LEVEL */
	irql = kspin_lock_irql(&timer_lock, DISPATCH_LEVEL);
	DBGTRACE5("deleting timer %p(%p)", wrap_timer, nt_timer);
	/* disable timer before deleting so it won't be re-armed after
	 * deleting */
	wrap_timer->repeat = 0;
	if (del_timer(&wrap_timer->timer))
		canceled = TRUE;
	else
		canceled = FALSE;
	kspin_unlock_irql(&timer_lock, irql);
	DBGTRACE5("canceled (%p): %d", wrap_timer, canceled);
	TRACEEXIT5(return canceled);
}

STDCALL void WRAP_EXPORT(KeInitializeDpc)
	(struct kdpc *kdpc, void *func, void *ctx)
{
	TRACEENTER3("%p, %p, %p", kdpc, func, ctx);
	memset(kdpc, 0, sizeof(*kdpc));
	kdpc->number = 0;
	kdpc->func = func;
	kdpc->ctx  = ctx;
	InitializeListHead(&kdpc->list);
}

static void kdpc_worker(void *data)
{
	struct nt_list *entry;
	struct kdpc *kdpc;
	KIRQL irql;

	while (1) {
		irql = kspin_lock_irql(&kdpc_list_lock, DISPATCH_LEVEL);
		entry = RemoveHeadList(&kdpc_list);
		if (!entry) {
			kspin_unlock_irql(&kdpc_list_lock, irql);
			break;
		}
		kdpc = container_of(entry, struct kdpc, list);
		kdpc->number = 0;
		kspin_unlock_irql(&kdpc_list_lock, irql);
		DBGTRACE5("%p, %p, %p, %p, %p", kdpc, kdpc->func, kdpc->ctx,
			  kdpc->arg1, kdpc->arg2);
		irql = raise_irql(DISPATCH_LEVEL);
		LIN2WIN4(kdpc->func, kdpc, kdpc->ctx, kdpc->arg1, kdpc->arg2);
		lower_irql(irql);
	}
}

STDCALL void KeFlushQueuedDpcs(void)
{
	kdpc_worker(NULL);
}

static BOOLEAN queue_kdpc(struct kdpc *kdpc)
{
	KIRQL irql;
	BOOLEAN ret;

	TRACEENTER5("%p", kdpc);
	if (!kdpc)
		return FALSE;
	irql = kspin_lock_irql(&kdpc_list_lock, DISPATCH_LEVEL);
	if (kdpc->number) {
		if (kdpc->number != 1)
			ERROR("kdpc->number: %d", kdpc->number);
		ret = FALSE;
	} else {
		kdpc->number = 1;
		InsertTailList(&kdpc_list, &kdpc->list);
		ret = TRUE;
	}
	kspin_unlock_irql(&kdpc_list_lock, irql);
	if (ret == TRUE)
		schedule_work(&kdpc_work);
	TRACEEXIT5(return ret);
}

BOOLEAN dequeue_kdpc(struct kdpc *kdpc)
{
	KIRQL irql;
	BOOLEAN ret;

	if (!kdpc)
		return FALSE;
	irql = kspin_lock_irql(&kdpc_list_lock, DISPATCH_LEVEL);
	if (kdpc->number) {
		if (kdpc->number != 1)
			ERROR("kdpc->number: %d", kdpc->number);
		RemoveEntryList(&kdpc->list);
		kdpc->number = 0;
		ret = TRUE;
	} else
		ret = FALSE;
	kspin_unlock_irql(&kdpc_list_lock, irql);
	return ret;
}

STDCALL BOOLEAN WRAP_EXPORT(KeInsertQueueDpc)
	(struct kdpc *kdpc, void *arg1, void *arg2)
{
	BOOLEAN ret;

	TRACEENTER5("%p, %p, %p", kdpc, arg1, arg2);
	kdpc->arg1 = arg1;
	kdpc->arg2 = arg2;
	ret = queue_kdpc(kdpc);
	TRACEEXIT5(return ret);
}

STDCALL BOOLEAN WRAP_EXPORT(KeRemoveQueueDpc)
	(struct kdpc *kdpc)
{
	BOOLEAN ret;

	TRACEENTER3("%p", kdpc);
	ret = dequeue_kdpc(kdpc);
	TRACEEXIT3(return ret);
}

static void wrap_work_item_worker(void *data)
{
	struct wrap_work_item *wrap_work_item;
	struct nt_list *cur;
	void (*func)(void *arg1, void *arg2) STDCALL;
	KIRQL irql;

	while (1) {
		irql = kspin_lock_irql(&wrap_work_item_list_lock,
				       DISPATCH_LEVEL);
		cur = RemoveHeadList(&wrap_work_item_list);
		kspin_unlock_irql(&wrap_work_item_list_lock, irql);
		if (!cur)
			break;
		wrap_work_item = container_of(cur, struct wrap_work_item,
					      list);
		func = wrap_work_item->func;
		DBGTRACE4("%p, %p, %p", func, wrap_work_item->arg1,
			  wrap_work_item->arg2);
		if (wrap_work_item->win_func == TRUE)
			LIN2WIN2(func, wrap_work_item->arg1,
				 wrap_work_item->arg2);
		else
			func(wrap_work_item->arg1, wrap_work_item->arg2);
		kfree(wrap_work_item);
	}
	return;
}

int schedule_wrap_work_item(WRAP_WORK_FUNC func, void *arg1, void *arg2,
			    BOOLEAN win_func)
{
	struct wrap_work_item *wrap_work_item;
	KIRQL irql;

	wrap_work_item = kmalloc(sizeof(*wrap_work_item), GFP_ATOMIC);
	if (!wrap_work_item) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	wrap_work_item->func = func;
	wrap_work_item->arg1 = arg1;
	wrap_work_item->arg2 = arg2;
	wrap_work_item->win_func = win_func;
	irql = kspin_lock_irql(&wrap_work_item_list_lock, DISPATCH_LEVEL);
	InsertTailList(&wrap_work_item_list, &wrap_work_item->list);
	kspin_unlock_irql(&wrap_work_item_list_lock, irql);
	schedule_work(&wrap_work_item_work);
	return 0;
}

STDCALL void WRAP_EXPORT(KeInitializeSpinLock)
	(KSPIN_LOCK *lock)
{
	TRACEENTER6("%p", lock);
	kspin_lock_init(lock);
}

STDCALL void WRAP_EXPORT(KeAcquireSpinLock)
	(KSPIN_LOCK *lock, KIRQL *irql)
{
	TRACEENTER6("%p", lock);
	*irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
}

STDCALL void WRAP_EXPORT(KeReleaseSpinLock)
	(KSPIN_LOCK *lock, KIRQL oldirql)
{
	TRACEENTER6("%p", lock);
	kspin_unlock_irql(lock, oldirql);
}

STDCALL void WRAP_EXPORT(KeAcquireSpinLockAtDpcLevel)
	(KSPIN_LOCK *lock)
{
	TRACEENTER6("%p", lock);
	kspin_lock(lock);
}

STDCALL void WRAP_EXPORT(KeRaiseIrql)
	(KIRQL newirql, KIRQL *oldirql)
{
	TRACEENTER6("%d", newirql);
	*oldirql = raise_irql(newirql);
}

STDCALL KIRQL WRAP_EXPORT(KeRaiseIrqlToDpcLevel)
	(void)
{
	return raise_irql(DISPATCH_LEVEL);
}

STDCALL void WRAP_EXPORT(KeLowerIrql)
	(KIRQL irql)
{
	TRACEENTER6("%d", irql);
	lower_irql(irql);
}

STDCALL KIRQL WRAP_EXPORT(KeAcquireSpinLockRaiseToDpc)
        (KSPIN_LOCK *lock)
{
	TRACEENTER6("%p", lock);
	return kspin_lock_irql(lock, DISPATCH_LEVEL);
}

STDCALL void WRAP_EXPORT(KeAcquireSpinLockdAtDpcLevel)
        (KSPIN_LOCK *lock)
{
	TRACEENTER6("%p", lock);
	kspin_lock(lock);
}

STDCALL void WRAP_EXPORT(KeReleaseSpinLockFromDpcLevel)
	(KSPIN_LOCK *lock)
{
	TRACEENTER6("%p", lock);
	kspin_unlock(lock);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedDecrement)
	(FASTCALL_DECL_1(LONG volatile *val))
{
	LONG x;
	KIRQL irql;

	TRACEENTER5("");
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	(*val)--;
	x = *val;
	kspin_unlock_irql(&inter_lock, irql);
	TRACEEXIT5(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedIncrement)
	(FASTCALL_DECL_1(LONG volatile *val))
{
	LONG x;
	KIRQL irql;

	TRACEENTER5("");
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	(*val)++;
	x = *val;
	kspin_unlock_irql(&inter_lock, irql);
	TRACEEXIT5(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedExchange)
	(FASTCALL_DECL_2(LONG volatile *target, LONG val))
{
	LONG x;
	KIRQL irql;

	TRACEENTER5("");
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	x = *target;
	*target = val;
	kspin_unlock_irql(&inter_lock, irql);
	TRACEEXIT5(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedCompareExchange)
	(FASTCALL_DECL_3(LONG volatile *dest, LONG xchg, LONG comperand))
{
	LONG x;
	KIRQL irql;

	TRACEENTER5("");
	irql = kspin_lock_irql(&inter_lock, DISPATCH_LEVEL);
	x = *dest;
	if (*dest == comperand)
		*dest = xchg;
	kspin_unlock_irql(&inter_lock, irql);
	TRACEEXIT5(return x);
}

_FASTCALL void WRAP_EXPORT(ExInterlockedAddLargeStatistic)
	(FASTCALL_DECL_2(LARGE_INTEGER *plint, ULONG n))
{
	unsigned long flags;

	TRACEENTER5("%p = %llu, n = %u", plint, *plint, n);
	kspin_lock_irqsave(&inter_lock, flags);
	*plint += n;
	kspin_unlock_irqrestore(&inter_lock, flags);
}

STDCALL void *WRAP_EXPORT(ExAllocatePoolWithTag)
	(enum pool_type pool_type, SIZE_T size, ULONG tag)
{
	void *addr;

	TRACEENTER4("pool_type: %d, size: %lu, tag: %u", pool_type,
		    size, tag);

	if (size <= KMALLOC_THRESHOLD) {
		if (current_irql() < DISPATCH_LEVEL)
			addr = kmalloc(size, GFP_KERNEL);
		else
			addr = kmalloc(size, GFP_ATOMIC);
	} else {
		if (current_irql() > PASSIVE_LEVEL)
			ERROR("Windows driver allocating %ld bytes in "
			      "atomic context", size);
		addr = vmalloc(size);
	}
	TRACEEXIT4(return addr);
}

STDCALL void wrap_vfree(void *addr, void *ctx)
{
	vfree(addr);
}

STDCALL void WRAP_EXPORT(ExFreePool)
	(void *addr)
{
	DBGTRACE4("addr: %p", addr);
	if ((unsigned long)addr < VMALLOC_START ||
	    (unsigned long)addr >= VMALLOC_END)
		kfree(addr);
	else {
		if (in_interrupt())
			schedule_wrap_work_item(wrap_vfree, addr, NULL, FALSE);
		else
			vfree(addr);
	}
	return;
}

STDCALL void WRAP_EXPORT(ExFreePoolWithTag)
	(void *addr, ULONG tag)
{
	ExFreePool(addr);
}

WRAP_FUNC_PTR_DECL(ExAllocatePoolWithTag)
WRAP_FUNC_PTR_DECL(ExFreePool)

STDCALL void WRAP_EXPORT(ExInitializeNPagedLookasideList)
	(struct npaged_lookaside_list *lookaside,
	 LOOKASIDE_ALLOC_FUNC *alloc_func, LOOKASIDE_FREE_FUNC *free_func,
	 ULONG flags, SIZE_T size, ULONG tag, USHORT depth)
{
	TRACEENTER3("lookaside: %p, size: %lu, flags: %u, head: %p, "
		    "alloc: %p, free: %p", lookaside, size, flags,
		    lookaside->head.list.next, alloc_func, free_func);

	memset(lookaside, 0, sizeof(*lookaside));

	lookaside->size = size;
	lookaside->tag = tag;
	lookaside->depth = 4;
	lookaside->maxdepth = 256;
	lookaside->pool_type = NonPagedPool;

	if (alloc_func)
		lookaside->alloc_func = alloc_func;
	else
		lookaside->alloc_func = (LOOKASIDE_ALLOC_FUNC *)
			WRAP_FUNC_PTR(ExAllocatePoolWithTag);
	if (free_func)
		lookaside->free_func = free_func;
	else
		lookaside->free_func = (LOOKASIDE_FREE_FUNC *)
			WRAP_FUNC_PTR(ExFreePool);

#ifndef X86_64
	DBGTRACE3("lock: %p", &lookaside->obsolete);
	kspin_lock_init(&lookaside->obsolete);
#endif
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(ExDeleteNPagedLookasideList)
	(struct npaged_lookaside_list *lookaside)
{
	struct nt_slist *entry;
	KIRQL irql;

	TRACEENTER3("lookaside = %p", lookaside);
	irql = raise_irql(DISPATCH_LEVEL);
	while ((entry = ExpInterlockedPopEntrySList(&lookaside->head)))
		ExFreePool(entry);
	lower_irql(irql);
	TRACEEXIT5(return);
}

STDCALL NTSTATUS WRAP_EXPORT(ExCreateCallback)
	(struct callback_object **object, struct object_attributes *attributes,
	 BOOLEAN create, BOOLEAN allow_multiple_callbacks)
{
	struct callback_object *obj;
	KIRQL irql;

	TRACEENTER2("");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(obj, &callback_objects, callback_funcs) {
		if (obj->attributes == attributes) {
			kspin_unlock_irql(&ntoskernel_lock, irql);
			*object = obj;
			return STATUS_SUCCESS;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	obj = allocate_object(sizeof(struct callback_object),
			      OBJECT_TYPE_CALLBACK, NULL);
	if (!obj)
		TRACEEXIT2(return STATUS_INSUFFICIENT_RESOURCES);
	InitializeListHead(&obj->callback_funcs);
	kspin_lock_init(&obj->lock);
	obj->allow_multiple_callbacks = allow_multiple_callbacks;
	obj->attributes = attributes;
	*object = obj;
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL void *WRAP_EXPORT(ExRegisterCallback)
	(struct callback_object *object, PCALLBACK_FUNCTION func,
	 void *context)
{
	struct callback_func *callback;
	KIRQL irql;

	TRACEENTER2("");
	irql = kspin_lock_irql(&object->lock, DISPATCH_LEVEL);
	if (object->allow_multiple_callbacks == FALSE &&
	    !IsListEmpty(&object->callback_funcs)) {
		kspin_unlock_irql(&object->lock, irql);
		TRACEEXIT2(return NULL);
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	callback = kmalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback) {
		ERROR("couldn't allocate memory");
		return NULL;
	}
	callback->func = func;
	callback->context = context;
	callback->object = object;
	irql = kspin_lock_irql(&object->lock, DISPATCH_LEVEL);
	InsertTailList(&object->callback_funcs, &callback->list);
	kspin_unlock_irql(&object->lock, irql);
	TRACEEXIT2(return callback);
}

STDCALL void WRAP_EXPORT(ExUnregisterCallback)
	(struct callback_func *callback)
{
	struct callback_object *object;
	KIRQL irql;

	TRACEENTER3("%p", callback);
	if (!callback)
		return;
	object = callback->object;
	irql = kspin_lock_irql(&object->lock, DISPATCH_LEVEL);
	RemoveEntryList(&callback->list);
	kspin_unlock_irql(&object->lock, irql);
	kfree(callback);
	return;
}

STDCALL void WRAP_EXPORT(ExNotifyCallback)
	(struct callback_object *object, void *arg1, void *arg2)
{
	struct callback_func *callback;
	KIRQL irql;

	TRACEENTER3("%p", object);
	irql = kspin_lock_irql(&object->lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(callback, &object->callback_funcs, list){
		LIN2WIN3(callback->func, callback->context, arg1, arg2);
	}
	kspin_unlock_irql(&object->lock, irql);
	return;
}

/* check and set signaled state; should be called with nt_event_lock held */
/* @reset indicates if the event should be reset to not-signaled state
 * - note that a semaphore may stay in signaled state for multiple
 * 'resets' if the count is > 1 */
static int inline check_reset_signaled_state(void *object,
					     struct nt_thread *thread,
					     int reset)
{
	struct dispatch_header *dh;
	struct nt_mutex *nt_mutex;

	dh = object;
	nt_mutex = container_of(object, struct nt_mutex, dh);

	if (is_mutex_dh(dh)) {
		/* either no thread owns the mutex or this thread owns
		 * it */
		if (nt_mutex->owner_thread == NULL ||
		    nt_mutex->owner_thread == thread) {
			assert(nt_mutex->owner_thread == NULL &&
			       dh->signal_state == 1);
			if (reset) {
				dh->signal_state--;
				nt_mutex->owner_thread = thread;
			}
			return 1;
		}
	} else if (dh->signal_state > 0) {
		/* if resetting, decrement signal_state for
		 * synchronization or semaphore objects */
		if (reset && (dh->type == SynchronizationEvent ||
			      is_semaphore_dh(dh)))
			dh->signal_state--;
		return 1;
	}
	return 0;
}

/* this function should be called holding nt_event_lock spinlock at
 * DISPATCH_LEVEL */
static void wakeup_threads(struct dispatch_header *dh)
{
	struct wait_block *wb;

	EVENTENTER("dh: %p", dh);
	nt_list_for_each_entry(wb, &dh->wait_blocks, list) {
		EVENTTRACE("wait block: %p, thread: %p", wb, wb->thread);
		assert(wb->thread != NULL && wb->object == dh);
		if (wb->thread &&
		    check_reset_signaled_state(dh, wb->thread, 0)) {
			EVENTTRACE("waking up task: %p", wb->thread->task);
			wb->thread->event_wait_done = 1;
			wake_up(&wb->thread->event_wq);
#if 0
			/* DDK says only one thread will be woken up,
			 * but we let each waking thread to check if
			 * the object is in signaled state anyway */
			if (dh->type == SynchronizationEvent)
				break;
#endif
		} else
			EVENTTRACE("not waking up task: %p",
				   wb->thread->task);
	}
	EVENTEXIT(return);
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForMultipleObjects)
	(ULONG count, void *object[], enum wait_type wait_type,
	 KWAIT_REASON wait_reason, KPROCESSOR_MODE wait_mode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout,
	 struct wait_block *wait_block_array)
{
	int i, res = 0, wait_count;
	long wait_jiffies = 0;
	struct wait_block *wb, wb_array[THREAD_WAIT_OBJECTS];
	struct dispatch_header *dh;
	KIRQL irql;
	struct nt_thread *thread;
	struct task_struct *task;

	task = get_current();
	EVENTENTER("task: %p, count = %d, type: %d, reason = %u, "
		   "waitmode = %u, alertable = %u, timeout = %p", task, count,
		   wait_type, wait_reason, wait_mode, alertable, timeout);

	thread = KeGetCurrentThread();
	EVENTTRACE("thread: %p", thread);
	if (thread == NULL)
		EVENTEXIT(return STATUS_RESOURCES);

	if (count > MAX_WAIT_OBJECTS)
		EVENTEXIT(return STATUS_INVALID_PARAMETER);
	if (count > THREAD_WAIT_OBJECTS && wait_block_array == NULL)
		EVENTEXIT(return STATUS_INVALID_PARAMETER);

	if (wait_block_array == NULL)
		wb = &wb_array[0];
	else
		wb = wait_block_array;

	/* TODO: should we allow threads to wait in non-alertable state? */
	alertable = TRUE;
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	/* first check if the wait can be satisfied or not, without
	 * grabbing the objects, except in the case of WaitAny */
	for (i = wait_count = 0; i < count; i++) {
		dh = object[i];
		EVENTTRACE("%p: event %p state: %d",
			   task, dh, dh->signal_state);
		/* wait_type == 1 for WaitAny, 0 for WaitAll */
		if (check_reset_signaled_state(dh, thread, wait_type)) {
			if (wait_type == WaitAny) {
				kspin_unlock_irql(&nt_event_lock, irql);
				if (count > 1)
					EVENTEXIT(return STATUS_WAIT_0 + i);
				else
					EVENTEXIT(return STATUS_SUCCESS);
			}
		} else
			wait_count++;
	}
	if (timeout && *timeout == 0 && wait_count) {
		kspin_unlock_irql(&nt_event_lock, irql);
		EVENTEXIT(return STATUS_TIMEOUT);
	}

	/* get the list of objects the thread (task) needs to wait on
	 * and add the thread on the wait list for each such object */
	/* if *timeout == 0, this step will grab the objects */
	thread->event_wait_done = 0;
	for (i = wait_count = 0; i < count; i++) {
		dh = object[i];
		EVENTTRACE("%p: event %p state: %d",
			   task, dh, dh->signal_state);
		if (check_reset_signaled_state(dh, thread, 1)) {
			EVENTTRACE("%p: event %p already signaled: %d",
				   task, dh, dh->signal_state);
			/* mark that we are not waiting on this object */
			wb[i].thread = NULL;
			wb[i].object = NULL;
		} else {
			assert(timeout == NULL || *timeout != 0);
			wb[i].thread = thread;
			wb[i].object = dh;
			InsertTailList(&dh->wait_blocks, &wb[i].list);
			wait_count++;
			EVENTTRACE("%p: waiting on event %p", task, dh);
		}
	}
	kspin_unlock_irql(&nt_event_lock, irql);

	if (wait_count == 0)
		EVENTEXIT(return STATUS_SUCCESS);

	assert(timeout == NULL || *timeout != 0);
	if (timeout == NULL)
		wait_jiffies = 0;
	else
		wait_jiffies = SYSTEM_TIME_TO_HZ(*timeout) + 1;
	EVENTTRACE("%p: sleeping for %ld", task, wait_jiffies);

	while (wait_count) {
		if (wait_jiffies) {
			if (alertable)
				res = wait_event_interruptible_timeout(
					thread->event_wq,
					(thread->event_wait_done == 1),
					wait_jiffies);
			else
				res = wait_event_timeout(
					thread->event_wq,
					(thread->event_wait_done == 1),
					wait_jiffies);
		} else {
			if (alertable)
				wait_event_interruptible(thread->event_wq,
					   (thread->event_wait_done == 1));
			else
				wait_event(thread->event_wq,
					   (thread->event_wait_done == 1));
			/* mark that it didn't timeout */
			res = 1;
		}
		irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
		thread->event_wait_done = 0;
		if (signal_pending(current))
			res = -ERESTARTSYS;
		EVENTTRACE("%p: woke up, res = %d", task, res);
//		assert(res < 0 && alertable);
		if (res <= 0) {
			/* timed out or interrupted; remove from wait list */
			for (i = 0; i < count; i++) {
				if (!wb[i].thread)
					continue;
				EVENTTRACE("%p: timedout, deq'ing %p",
					   task, wb[i].object);
				RemoveEntryList(&wb[i].list);
				wb[i].thread = NULL;
				wb[i].object = NULL;
			}
			kspin_unlock_irql(&nt_event_lock, irql);
			if (res < 0)
				EVENTEXIT(return STATUS_ALERTED);
			else
				EVENTEXIT(return STATUS_TIMEOUT);
		}
		/* woken up by wakeup_threads */
		for (i = 0; wait_count && i < count; i++) {
			if (!wb[i].thread)
				continue;
			dh = object[i];
			if (!check_reset_signaled_state(dh, thread, 1))
				continue;
			RemoveEntryList(&wb[i].list);
			wait_count--;
			if (wait_type == WaitAny) {
				int j;
				/* done; remove from rest of wait list */
				for (j = i; j < count; j++)
					if (wb[j].thread && wb[j].object)
						RemoveEntryList(&wb[j].list);
				kspin_unlock_irql(&nt_event_lock, irql);
				if (count > 1)
					EVENTEXIT(return STATUS_WAIT_0 + i);
				else
					EVENTEXIT(return STATUS_SUCCESS);
			}
		}
		if (wait_count == 0) {
			kspin_unlock_irql(&nt_event_lock, irql);
			EVENTEXIT(return STATUS_SUCCESS);
		}
		/* this thread is still waiting for more objects, so
		 * let it wait for remaining time and those objects */
		/* we already set res to 1 if timeout was NULL, so
		 * reinitialize wait_jiffies accordingly */
		if (timeout)
			wait_jiffies = res;
		else
			wait_jiffies = 0;
		kspin_unlock_irql(&nt_event_lock, irql);
	}
	/* this should never reach, but compiler wants return value */
	ERROR("%p: wait_jiffies: %ld", task, wait_jiffies);
	EVENTEXIT(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForSingleObject)
	(void *object, KWAIT_REASON wait_reason, KPROCESSOR_MODE wait_mode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout)
{
	return KeWaitForMultipleObjects(1, &object, WaitAny, wait_reason,
					wait_mode, alertable, timeout, NULL);
}

STDCALL void WRAP_EXPORT(KeInitializeEvent)
	(struct nt_event *nt_event, enum event_type type, BOOLEAN state)
{
	KIRQL irql;

	EVENTENTER("event = %p, type = %d, state = %d", nt_event, type, state);
//	dump_bytes(__FUNCTION__, __builtin_return_address(0), 20);
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	initialize_dh(&nt_event->dh, type, state, DH_NT_EVENT);
	kspin_unlock_irql(&nt_event_lock, irql);
	EVENTEXIT(return);
}

STDCALL LONG WRAP_EXPORT(KeSetEvent)
	(struct nt_event *nt_event, KPRIORITY incr, BOOLEAN wait)
{
	LONG old_state;
	KIRQL irql;

	EVENTENTER("event = %p, type = %d, wait = %d",
		   nt_event, nt_event->dh.type, wait);
	if (wait == TRUE)
		WARNING("wait = %d, not yet implemented", wait);

	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	old_state = nt_event->dh.signal_state;
	nt_event->dh.signal_state = 1;
	wakeup_threads(&nt_event->dh);
	kspin_unlock_irql(&nt_event_lock, irql);
	EVENTEXIT(return old_state);
}

STDCALL void WRAP_EXPORT(KeClearEvent)
	(struct nt_event *nt_event)
{
	KIRQL irql;

	EVENTENTER("event = %p", nt_event);
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	nt_event->dh.signal_state = 0;
	kspin_unlock_irql(&nt_event_lock, irql);
	EVENTEXIT(return);
}

STDCALL LONG WRAP_EXPORT(KeResetEvent)
	(struct nt_event *nt_event)
{
	LONG old_state;
	KIRQL irql;

	EVENTENTER("event = %p", nt_event);

	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	old_state = nt_event->dh.signal_state;
	nt_event->dh.signal_state = 0;
	kspin_unlock_irql(&nt_event_lock, irql);

	EVENTEXIT(return old_state);
}

STDCALL void WRAP_EXPORT(KeInitializeMutex)
	(struct nt_mutex *mutex, BOOLEAN wait)
{
	KIRQL irql;

	EVENTENTER("%p", mutex);
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	initialize_dh(&mutex->dh, SynchronizationEvent, 1, DH_NT_MUTEX);
	kspin_unlock_irql(&nt_event_lock, irql);
	mutex->dh.size = sizeof(*mutex);
	InitializeListHead(&mutex->list);
	mutex->abandoned = FALSE;
	mutex->apc_disable = 1;
	mutex->owner_thread = NULL;
	EVENTEXIT(return);
}

STDCALL LONG WRAP_EXPORT(KeReleaseMutex)
	(struct nt_mutex *mutex, BOOLEAN wait)
{
	LONG ret;
	KIRQL irql;

	EVENTENTER("%p", mutex);
	if (wait == TRUE)
		WARNING("wait: %d", wait);
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	ret = mutex->dh.signal_state++;
	if (mutex->dh.signal_state > 0) {
		mutex->owner_thread = NULL;
		wakeup_threads(&mutex->dh);
	}
	kspin_unlock_irql(&nt_event_lock, irql);
	EVENTEXIT(return ret);
}

STDCALL void WRAP_EXPORT(KeInitializeSemaphore)
	(struct nt_semaphore *semaphore, LONG count, LONG limit)
{
	KIRQL irql;

	EVENTENTER("%p: %d", semaphore, count);
	/* if limit > 1, we need to satisfy as many waits (until count
	 * becomes 0); so we keep decrementing count everytime a wait
	 * is satisified */
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	initialize_dh(&semaphore->dh, NotificationEvent, count,
		      DH_NT_SEMAPHORE);
	kspin_unlock_irql(&nt_event_lock, irql);
	semaphore->dh.size = sizeof(*semaphore);
	semaphore->limit = limit;
	EVENTEXIT(return);
}

STDCALL LONG WRAP_EXPORT(KeReleaseSemaphore)
	(struct nt_semaphore *semaphore, KPRIORITY incr, LONG adjustment,
	 BOOLEAN wait)
{
	LONG ret;
	KIRQL irql;

	EVENTENTER("%p", semaphore);
	irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
	ret = semaphore->dh.signal_state;
	assert(ret >= 0);
	if (semaphore->dh.signal_state + adjustment <= semaphore->limit)
		semaphore->dh.signal_state += adjustment;
	/* else raise exception */
	if (semaphore->dh.signal_state > 0)
		wakeup_threads(&semaphore->dh);
	kspin_unlock_irql(&nt_event_lock, irql);
	EVENTEXIT(return ret);
}

STDCALL NTSTATUS WRAP_EXPORT(KeDelayExecutionThread)
	(KPROCESSOR_MODE wait_mode, BOOLEAN alertable, LARGE_INTEGER *interval)
{
	int res;
	long timeout;

	if (wait_mode != 0)
		ERROR("invalid wait_mode %d", wait_mode);

	timeout = SYSTEM_TIME_TO_HZ(*interval) + 1;
	EVENTTRACE("thread: %p, interval: %Ld, timeout: %ld",
		    get_current(), *interval, timeout);
	if (timeout <= 0)
		EVENTEXIT(return STATUS_SUCCESS);

	alertable = TRUE;
	if (alertable)
		set_current_state(TASK_INTERRUPTIBLE);
	else
		set_current_state(TASK_UNINTERRUPTIBLE);

	res = schedule_timeout(timeout);
	EVENTTRACE("thread: %p, res: %d", get_current(), res);
	if (res == 0)
		EVENTEXIT(return STATUS_SUCCESS);
	else
		EVENTEXIT(return STATUS_ALERTED);
}

STDCALL KPRIORITY WRAP_EXPORT(KeQueryPriorityThread)
	(struct nt_thread *thread)
{
	KPRIORITY prio;

	EVENTENTER("thread: %p, task: %p", thread, thread->task);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	prio = 1;
#else
	if (rt_task(thread->task))
		prio = LOW_REALTIME_PRIORITY;
	else
		prio = MAXIMUM_PRIORITY;
#endif
	EVENTEXIT(return prio);
}

STDCALL ULONGLONG WRAP_EXPORT(KeQueryInterruptTime)
	(void)
{
	TRACEEXIT5(return jiffies * TICKSPERSEC / HZ);
}

STDCALL ULONG WRAP_EXPORT(KeQueryTimeIncrement)
	(void)
{
	TRACEEXIT5(return TICKSPERSEC / HZ);
}

STDCALL void WRAP_EXPORT(KeQuerySystemTime)
	(LARGE_INTEGER *time)
{
	*time = ticks_1601();
	return;
}

STDCALL void WRAP_EXPORT(KeQUeryTickCount)
	(LARGE_INTEGER *j)
{
	*j = jiffies;
}

STDCALL LARGE_INTEGER WRAP_EXPORT(KeQueryPerformanceCounter)
	(LARGE_INTEGER *counter)
{
	if (counter)
		*counter = HZ;
	return jiffies;
}

STDCALL struct nt_thread *WRAP_EXPORT(KeGetCurrentThread)
	(void)
{
	KIRQL irql;
	struct task_struct *task = get_current();
	struct nt_thread *ret;
	struct common_object_header *header;

	DBGTRACE5("task: %p", task);
	ret = NULL;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(header, &object_list, list) {
		struct nt_thread *thread;
		DBGTRACE5("header: %p, type: %d", header, header->type);
		if (header->type != OBJECT_TYPE_NT_THREAD)
			continue;
		thread = HEADER_TO_OBJECT(header);
		DBGTRACE5("thread: %p, task: %p", thread, thread->task);
		if (thread->task == task) {
			ret = thread;
			break;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	if (ret == NULL)
		DBGTRACE1("couldn't find thread for task %p", task);
	DBGTRACE5("current thread = %p", ret);
	return ret;
}

STDCALL KPRIORITY WRAP_EXPORT(KeSetPriorityThread)
	(struct nt_thread *thread, KPRIORITY priority)
{
	KPRIORITY old_prio;

	TRACEENTER3("thread: %p, priority = %u", thread, priority);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	/* FIXME: is there a way to set kernel thread prio on 2.4? */
	old_prio = LOW_PRIORITY;
#else
	if (rt_task(thread->task))
		old_prio = LOW_REALTIME_PRIORITY;
	else
		old_prio = MAXIMUM_PRIORITY;
#if 0
	if (priority == LOW_REALTIME_PRIORITY)
		set_user_nice(thread->task, -20);
	else
		set_user_nice(thread->task, 10);
#endif
#endif
	return old_prio;
}

struct trampoline_context {
	void (*start_routine)(void *) STDCALL;
	void *context;
	struct nt_thread *thread;
};

static int thread_trampoline(void *data)
{
	struct trampoline_context ctx;
	struct nt_thread *thread;

	memcpy(&ctx, data, sizeof(ctx));
	kfree(data);
	thread = ctx.thread;
	thread->task = get_current();
	thread->pid = thread->task->pid;

	DBGTRACE2("thread: %p, task: %p (%d)", thread, thread->task,
		  thread->pid);
	ctx.start_routine(ctx.context);

	return 0;
}

struct nt_thread *wrap_create_thread(struct task_struct *task)
{
	struct nt_thread *thread;
	KIRQL irql;

	thread = allocate_object(sizeof(*thread), OBJECT_TYPE_NT_THREAD, NULL);
	if (thread) {
		thread->task = task;
		if (task)
			thread->pid = task->pid;
		else
			thread->pid = 0;
		kspin_lock_init(&thread->lock);
		init_waitqueue_head(&thread->event_wq);
		InitializeListHead(&thread->irps);
		irql = kspin_lock_irql(&nt_event_lock, DISPATCH_LEVEL);
		initialize_dh(&thread->dh, NotificationEvent, 0, DH_NT_THREAD);
		kspin_unlock_irql(&nt_event_lock, irql);
		thread->dh.size = sizeof(*thread);

		DBGTRACE1("thread: %p, task: %p, pid: %d",
			  thread, thread->task, thread->pid);
	} else
		ERROR("couldn't allocate thread object");
	return thread;
}

void wrap_remove_thread(struct nt_thread *thread)
{
	KIRQL irql;
	struct nt_list *ent;

	if (thread) {
		DBGTRACE1("terminating thread: %p, task: %p, pid: %d",
			  thread, thread->task, thread->task->pid);
		/* TODO: make sure waitqueue is empty and destroy it */
		while (1) {
			struct irp *irp;
			irql = kspin_lock_irql(&thread->lock, DISPATCH_LEVEL);
			ent = RemoveHeadList(&thread->irps);
			kspin_unlock_irql(&thread->lock, irql);
			if (!ent)
				break;
			irp = container_of(ent, struct irp, threads);
			IoCancelIrp(irp);
		}
		ObDereferenceObject(thread);
	} else
		ERROR("couldn't find thread for task: %p", get_current());
	return;
}

STDCALL NTSTATUS WRAP_EXPORT(PsCreateSystemThread)
	(void **phandle, ULONG access, void *obj_attr, void *process,
	 void *client_id, void (*start_routine)(void *) STDCALL, void *context)
{
	struct trampoline_context *ctx;
	struct nt_thread *thread;
	struct task_struct *task;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,7)
	int pid;
#endif

	TRACEENTER2("phandle = %p, access = %u, obj_attr = %p, process = %p, "
	            "client_id = %p, start_routine = %p, context = %p",
	            phandle, access, obj_attr, process, client_id,
	            start_routine, context);

	ctx = kmalloc(sizeof(struct trampoline_context), GFP_KERNEL);
	if (!ctx)
		TRACEEXIT2(return STATUS_RESOURCES);
	ctx->start_routine = start_routine;
	ctx->context = context;
	thread = wrap_create_thread(NULL);
	if (!thread) {
		kfree(ctx);
		TRACEEXIT2(return STATUS_RESOURCES);
	}
	ctx->thread = thread;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,7)
	pid = kernel_thread(thread_trampoline, ctx,
		CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	DBGTRACE2("pid = %d", pid);
	if (pid < 0) {
		kfree(ctx);
		free_object(thread);
		TRACEEXIT2(return STATUS_FAILURE);
	}
	task = NULL;
	DBGTRACE2("created task: %p (%d)", find_task_by_pid(pid), pid);
#else
	task = KTHREAD_RUN(thread_trampoline, ctx, DRIVER_NAME);
	if (IS_ERR(task)) {
		kfree(ctx);
		free_object(thread);
		TRACEEXIT2(return STATUS_FAILURE);
	}
	DBGTRACE2("created task: %p (%d)", task, task->pid);
#endif
	*phandle = thread;
	DBGTRACE2("created thread: %p", thread);
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(PsTerminateSystemThread)
	(NTSTATUS status)
{
	struct nt_thread *thread;

	thread = KeGetCurrentThread();
	if (thread) {
		DBGTRACE2("setting event for thread: %p", thread);
		KeSetEvent((struct nt_event *)&thread->dh, 0, FALSE);
		DBGTRACE2("set event for thread: %p", thread);
		wrap_remove_thread(thread);
		complete_and_exit(NULL, status);
		ERROR("oops: %p, %d", thread->task, thread->pid);
	} else
		ERROR("couldn't find thread for task: %p", get_current);
	return STATUS_FAILURE;
}

STDCALL BOOLEAN WRAP_EXPORT(KeRemoveEntryDeviceQueue)
	(struct kdevice_queue *dev_queue, struct kdevice_queue_entry *entry)
{
	struct kdevice_queue_entry *e;
	KIRQL irql;

	irql = kspin_lock_irql(&dev_queue->lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(e, &dev_queue->list, list) {
		if (e == entry) {
			RemoveEntryList(&e->list);
			kspin_unlock_irql(&dev_queue->lock, irql);
			return TRUE;
		}
	}
	kspin_unlock_irql(&dev_queue->lock, irql);
	return FALSE;
}

STDCALL BOOLEAN WRAP_EXPORT(KeSynchronizeExecution)
	(struct kinterrupt *interrupt, PKSYNCHRONIZE_ROUTINE synch_routine,
	 void *synch_context)
{
	KSPIN_LOCK *spinlock;
	BOOLEAN ret;
	KIRQL irql = PASSIVE_LEVEL;

	if (interrupt->actual_lock)
		spinlock = interrupt->actual_lock;
	else
		spinlock = &interrupt->lock;
	if (interrupt->synch_irql == DISPATCH_LEVEL)
		irql = kspin_lock_irql(spinlock, interrupt->synch_irql);
	else
		kspin_lock(spinlock);
	ret = synch_routine(synch_context);
	if (interrupt->synch_irql == DISPATCH_LEVEL)
		kspin_unlock_irql(spinlock, irql);
	else
		kspin_unlock(spinlock);
	return ret;
}

/* Atheros card with pciid 168C:0014 calls this function with 0xf0000
 * and 0xf6ef0 address, and then check for things that seem to be
 * related to ACPI: "_SM_" and "_DMI_". This may be the hack they do
 * to check if this card is installed in IBM thinkpads; we can
 * probably get this device to work if we create a buffer with the
 * strings as required by the driver and return virtual address for
 * that address instead */
STDCALL void *WRAP_EXPORT(MmMapIoSpace)
	(PHYSICAL_ADDRESS phys_addr, SIZE_T size,
	 enum memory_caching_type cache)
{
	void *virt;
	TRACEENTER1("cache type: %d", cache);
	if (cache == MmCached)
		virt = ioremap(phys_addr, size);
	else
		virt = ioremap_nocache(phys_addr, size);
	DBGTRACE1("%Lx, %lu, %p", phys_addr, size, virt);
	return virt;
}

STDCALL void WRAP_EXPORT(MmUnmapIoSpace)
	(void *addr, SIZE_T size)
{
	TRACEENTER1("%p, %lu", addr, size);
	iounmap(addr);
	return;
}

STDCALL ULONG WRAP_EXPORT(MmSizeOfMdl)
	(void *base, ULONG length)
{
	return (sizeof(struct mdl) +
		SPAN_PAGES((ULONG_PTR)base, length) * sizeof(ULONG));
}

struct mdl *allocate_init_mdl(void *virt, ULONG length)
{
	struct wrap_mdl *wrap_mdl;
	struct mdl *mdl = NULL;
	int mdl_size = MmSizeOfMdl(virt, length);
	KIRQL irql;

	if (mdl_size <= CACHE_MDL_SIZE) {
		unsigned int alloc_flags;

		if (current_irql() < DISPATCH_LEVEL)
			alloc_flags = GFP_KERNEL;
		else
			alloc_flags = GFP_ATOMIC;
		
		wrap_mdl = kmem_cache_alloc(mdl_cache, alloc_flags);
		if (!wrap_mdl)
			return NULL;
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		mdl = (struct mdl *)wrap_mdl->mdl;
		DBGTRACE3("allocated mdl cache: %p(%p)", wrap_mdl, mdl);
		memset(mdl, 0, CACHE_MDL_SIZE);
		MmInitializeMdl(mdl, virt, length);
		/* mark the MDL as allocated from cache pool so when
		 * it is freed, we free it back to the pool */
		mdl->flags = MDL_CACHE_ALLOCATED;
	} else {
		wrap_mdl =
			kmalloc(sizeof(*wrap_mdl) + mdl_size - CACHE_MDL_SIZE,
				GFP_ATOMIC);
		if (!wrap_mdl)
			return NULL;
		mdl = (struct mdl *)wrap_mdl->mdl;
		DBGTRACE3("allocated mdl: %p (%p)", wrap_mdl, mdl);
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		memset(mdl, 0, mdl_size);
		MmInitializeMdl(mdl, virt, length);
	}
	MmBuildMdlForNonPagedPool(mdl);
	return mdl;
}

void free_mdl(struct mdl *mdl)
{
	KIRQL irql;

	/* A driver may allocate Mdl with NdisAllocateBuffer and free
	 * with IoFreeMdl (e.g., 64-bit Broadcom). Since we need to
	 * treat buffers allocated with Ndis calls differently, we
	 * must call NdisFreeBuffer if it is allocated with Ndis
	 * function. We set 'process' field in Ndis functions. */
	if (!mdl)
		return;
	if (mdl->process)
		NdisFreeBuffer(mdl);
	else {
		struct wrap_mdl *wrap_mdl;
		wrap_mdl = (struct wrap_mdl *)
			((char *)mdl - offsetof(struct wrap_mdl, mdl));
		irql = kspin_lock_irql(&ntoskernel_lock,
				       DISPATCH_LEVEL);
		RemoveEntryList(&wrap_mdl->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		
		if (mdl->flags & MDL_CACHE_ALLOCATED) {
			DBGTRACE3("freeing mdl cache: %p (%hu)",
				  wrap_mdl, mdl->flags);
			kmem_cache_free(mdl_cache, wrap_mdl);
		} else {
			DBGTRACE3("freeing mdl: %p (%hu)",
				  wrap_mdl, mdl->flags);
			kfree(wrap_mdl);
		}
	}
	return;
}

STDCALL void WRAP_EXPORT(IoBuildPartialMdl)
	(struct mdl *source, struct mdl *target, void *virt, ULONG length)
{
	MmInitializeMdl(target, virt, length);
}

/* FIXME: We don't update MDL to physical page mapping, since in Linux
 * the pages are in memory anyway; if a driver treats an MDL as
 * opaque, we should be safe; otherwise, the driver may break */
STDCALL void WRAP_EXPORT(MmBuildMdlForNonPagedPool)
	(struct mdl *mdl)
{
	mdl->flags |= MDL_SOURCE_IS_NONPAGED_POOL;
	mdl->mappedsystemva = MmGetMdlVirtualAddress(mdl);
	return;
}

STDCALL void *WRAP_EXPORT(MmMapLockedPages)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode)
{
	mdl->flags |= MDL_MAPPED_TO_SYSTEM_VA;
	return MmGetMdlVirtualAddress(mdl);
}

STDCALL void *WRAP_EXPORT(MmMapLockedPagesSpecifyCache)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode,
	 enum memory_caching_type cache_type, void *base_address,
	 ULONG bug_check, enum mm_page_priority priority)
{
	return MmMapLockedPages(mdl, access_mode);
}

STDCALL void WRAP_EXPORT(MmUnmapLockedPages)
	(void *base, struct mdl *mdl)
{
	mdl->flags &= ~MDL_MAPPED_TO_SYSTEM_VA;
	return;
}

STDCALL void WRAP_EXPORT(MmProbeAndLockPages)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode,
	 enum lock_operation operation)
{
	mdl->flags |= MDL_PAGES_LOCKED;
	return;
}

STDCALL void WRAP_EXPORT(MmUnlockPages)
	(struct mdl *mdl)
{
	mdl->flags &= ~MDL_PAGES_LOCKED;
	return;
}

STDCALL BOOLEAN WRAP_EXPORT(MmIsAddressValid)
	(void *virt_addr)
{
	if (virt_addr_valid(virt_addr))
		return TRUE;
	else
		return FALSE;
}

STDCALL void *WRAP_EXPORT(MmLockPagableDataSection)
	(void *address)
{
	return address;
}

STDCALL void WRAP_EXPORT(MmUnlockPagableImageSection)
	(void *handle)
{
	return;
}

STDCALL NTSTATUS WRAP_EXPORT(ObReferenceObjectByHandle)
	(void *handle, ACCESS_MASK desired_access, void *obj_type,
	 KPROCESSOR_MODE access_mode, void **object, void *handle_info)
{
	struct common_object_header *hdr;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	hdr = HANDLE_TO_HEADER(handle);
	hdr->ref_count++;
	*object = HEADER_TO_OBJECT(hdr);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return STATUS_SUCCESS;
}

/* DDK doesn't say if return value should be before incrementing or
 * after incrementing reference count, but according to #reactos
 * devels, it should be return value after incrementing */
_FASTCALL LONG WRAP_EXPORT(ObfReferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	struct common_object_header *hdr;
	KIRQL irql;
	LONG ret;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	hdr = OBJECT_TO_HEADER(object);
	ret = ++hdr->ref_count;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return ret;
}

_FASTCALL void WRAP_EXPORT(ObfDereferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	struct common_object_header *hdr;
	KIRQL irql;
	int ref_count;

	TRACEENTER2("object: %p", object);
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	hdr = OBJECT_TO_HEADER(object);
	DBGTRACE2("hdr: %p", hdr);
	hdr->ref_count--;
	ref_count = hdr->ref_count;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	if (ref_count < 0)
		ERROR("invalid object: %p (%d)", object, ref_count);
	if (ref_count <= 0) {
		free_object(object);
	}
}

STDCALL NTSTATUS WRAP_EXPORT(ZwCreateFile)
	(void **handle, ULONG access_mask, struct object_attr *obj_attr,
	 struct io_status_block *iosb, LARGE_INTEGER *size,
	 ULONG file_attr, ULONG share_access, ULONG create_disposition,
	 ULONG create_options, void *ea_buffer, ULONG ea_length)
{
	struct common_object_header *header;
	struct object_attr *oa;
	struct ansi_string ansi;
	struct wrap_bin_file *bin_file;
	KIRQL irql;
	char *file_basename;

	TRACEENTER2("");
	if (RtlUnicodeStringToAnsiString(&ansi, obj_attr->name, TRUE) !=
	    STATUS_SUCCESS)
		TRACEEXIT2(return STATUS_INSUFFICIENT_RESOURCES);
	DBGTRACE2("Filename: %s", ansi.buf);

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(header, &object_list, list) {
		if (header->type != OBJECT_TYPE_FILE)
			continue;
		oa = HEADER_TO_OBJECT(header);
		if (!RtlCompareUnicodeString(oa->name, obj_attr->name,
					     FALSE)) {
			bin_file = oa->file;
			*handle = header;
			iosb->status = FILE_OPENED;
			iosb->status_info = bin_file->size;
			kspin_unlock_irql(&ntoskernel_lock, irql);
			TRACEEXIT2(return STATUS_SUCCESS);
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);

	oa = allocate_object(sizeof(struct object_attr), OBJECT_TYPE_FILE,
			     ansi.buf);
	if (!oa) {
		iosb->status = FILE_DOES_NOT_EXIST;
		iosb->status_info = 0;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return STATUS_FAILURE);
	}
	*handle = OBJECT_TO_HEADER(oa);
	DBGTRACE2("handle: %p", *handle);
	file_basename = strrchr(ansi.buf, '\\');
	if (file_basename)
		file_basename++;
	else
		file_basename = ansi.buf;
	DBGTRACE2("file_basename: '%s'", file_basename);
	bin_file = get_bin_file(file_basename);
	if (bin_file) {
		oa->file = bin_file;
		iosb->status = FILE_OPENED;
		iosb->status_info = bin_file->size;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return STATUS_SUCCESS);
	} else {
		iosb->status = FILE_DOES_NOT_EXIST;
		iosb->status_info = 0;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return STATUS_FAILURE);
	}
}

STDCALL NTSTATUS WRAP_EXPORT(ZwReadFile)
	(void *handle, struct nt_event *event, void *apc_routine,
	 void *apc_context, struct io_status_block *iosb, void *buffer,
	 ULONG length, LARGE_INTEGER *byte_offset, ULONG *key)
{
	struct object_attr *oa;
	ULONG count;
	size_t offset;
	struct wrap_bin_file *file;

	oa = HANDLE_TO_OBJECT(handle);
	file = oa->file;
	DBGTRACE2("file: %s (%d)", file->name, file->size);
	if (byte_offset)
		offset = *byte_offset;
	else
		offset = 0;
	count = min((size_t)length, file->size - offset);
	DBGTRACE2("count: %u, offset: %zu, length: %u", count, offset, length);
	memcpy(buffer, ((void *)file->data) + offset, count);
	iosb->status = STATUS_SUCCESS;
	iosb->status_info = count;
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(ZwClose)
	(void *handle)
{
	struct object_attr *oa;
	struct wrap_bin_file *bin_file;
	struct common_object_header *coh;

	coh = handle;
	if (coh == NULL) {
		DBGTRACE1("");
		TRACEEXIT2(return STATUS_SUCCESS);
	}
	if (coh->type == OBJECT_TYPE_FILE) {
		oa = HANDLE_TO_OBJECT(handle);
		bin_file = oa->file;
		free_bin_file(bin_file);
		ObDereferenceObject(oa);
	} else
		WARNING("object type %d not implemented", coh->type);
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(ZwQueryInformationFile)
	(void *handle, struct io_status_block *iosb, void *info,
	 ULONG length, enum file_info_class class)
{
	struct object_attr *oa;
	struct file_name_info *fni;
	struct file_std_info *fsi;
	struct wrap_bin_file *file;

	TRACEENTER2("%p", handle);
	oa = HANDLE_TO_OBJECT(handle);
	DBGTRACE2("attr: %p", oa);
	switch (class) {
	case FileNameInformation:
		fni = info;
		fni->length = oa->name->max_length;
		memcpy(fni->name, oa->name->buf, oa->name->max_length);
		break;
	case FileStandardInformation:
		fsi = info;
		file = oa->file;
		fsi->alloc_size = file->size;
		fsi->eof = file->size;
		fsi->num_links = 1;
		fsi->delete_pending = FALSE;
		fsi->dir = FALSE;
		break;
	default:
		WARNING("type %d not implemented yet", class);
		TRACEEXIT2(return STATUS_FAILURE);
	}
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(ZwCreateKey)
	(void **handle, ACCESS_MASK desired_access, struct object_attr *attr,
	 ULONG title_index, struct unicode_string *class,
	 ULONG create_options, ULONG *disposition)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, attr->name, TRUE) ==
	    STATUS_SUCCESS) {
		DBGTRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*handle = NULL;
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(ZwOpenKey)
	(void **handle, ACCESS_MASK desired_access, struct object_attr *attr)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, attr->name, TRUE) ==
	    STATUS_SUCCESS) {
		DBGTRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*handle = NULL;
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(ZwSetValueKey)
	(void *handle, struct unicode_string *name, ULONG title_index,
	 ULONG type, void *data, ULONG data_size)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, name, TRUE) ==
	    STATUS_SUCCESS) {
		DBGTRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(ZwQueryValueKey)
	(void *handle, struct unicode_string *name,
	 enum key_value_information_class class, void *info,
	 ULONG length, ULONG *res_length)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, name, TRUE) ==
	    STATUS_SUCCESS) {
		DBGTRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	UNIMPL();
	return STATUS_INVALID_PARAMETER;
}

STDCALL NTSTATUS WRAP_EXPORT(WmiSystemControl)
	(struct wmilib_context *info, struct device_object *dev_obj,
	 struct irp *irp, void *irp_disposition)
{
	UNIMPL();
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(WmiCompleteRequest)
	(struct device_object *dev_obj, struct irp *irp, NTSTATUS status,
	 ULONG buffer_used, CCHAR priority_boost)
{
	UNIMPL();
	return STATUS_SUCCESS;
}

NOREGPARM NTSTATUS WRAP_EXPORT(WmiTraceMessage)
	(void *tracehandle, ULONG message_flags,
	 void *message_guid, USHORT message_no, ...)
{
	UNIMPL();
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(WmiQueryTraceInformation)
	(enum trace_information_class trace_info_class, void *trace_info,
	 ULONG *req_length, void *buf)
{
	UNIMPL();
	TRACEEXIT2(return STATUS_SUCCESS);
}

/* this function can't be STDCALL as it takes variable number of args */
NOREGPARM ULONG WRAP_EXPORT(DbgPrint)
	(char *format, ...)
{
#ifdef DEBUG
	va_list args;
	static char buf[1024];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	printk(KERN_DEBUG "%s (%s): %s", DRIVER_NAME, __FUNCTION__, buf);
	va_end(args);
#endif
	return STATUS_SUCCESS;
}

STDCALL void WRAP_EXPORT(KeBugCheckEx)
	(ULONG code, ULONG_PTR param1, ULONG_PTR param2,
	 ULONG_PTR param3, ULONG_PTR param4)
{
	UNIMPL();
	return;
}

STDCALL void WRAP_EXPORT(ExSystemTimeToLocalTime)
	(LARGE_INTEGER *system_time, LARGE_INTEGER *local_time)
{
	*local_time = *system_time;
}

STDCALL ULONG WRAP_EXPORT(ExSetTimerResolution)
	(ULONG time, BOOLEAN set)
{
	/* yet another "innovation"! */
	return time;
}

STDCALL void WRAP_EXPORT(DbgBreakPoint)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(_except_handler3)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(__C_specific_handler)(void){UNIMPL();}
void WRAP_EXPORT(_purecall)(void) { UNIMPL(); }

#include "ntoskernel_exports.h"

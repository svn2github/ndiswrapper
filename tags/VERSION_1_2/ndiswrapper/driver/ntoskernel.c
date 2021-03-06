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

struct bus_driver {
	struct nt_list list;
	char name[MAX_DRIVER_NAME_LEN];
	struct driver_object *drv_obj;
};

/* everything here is for all drivers/devices - not per driver/device */
static KSPIN_LOCK kevent_lock;
KSPIN_LOCK irp_cancel_lock;
KSPIN_LOCK ntoskernel_lock;
static kmem_cache_t *mdl_cache;
static struct nt_list wrap_mdl_list;
static struct nt_list obj_mgr_obj_list;

struct work_struct kdpc_work;
static struct nt_list kdpc_list;
static void kdpc_worker(void *data);

static struct nt_list callback_objects;

static struct nt_list bus_driver_list;
static struct driver_object pci_bus_driver;
static struct driver_object usb_bus_driver;
static void del_bus_drivers(void);

WRAP_EXPORT_MAP("KeTickCount", &jiffies);

static int add_bus_driver(struct driver_object *drv_obj, const char *name);

int ntoskernel_init(void)
{
	kspin_lock_init(&kevent_lock);
	kspin_lock_init(&irp_cancel_lock);
	kspin_lock_init(&ntoskernel_lock);
	InitializeListHead(&obj_mgr_obj_list);
	InitializeListHead(&wrap_mdl_list);
	InitializeListHead(&kdpc_list);
	InitializeListHead(&callback_objects);
	InitializeListHead(&bus_driver_list);
	INIT_WORK(&kdpc_work, &kdpc_worker, NULL);
	if (add_bus_driver(&pci_bus_driver, "PCI") ||
	    add_bus_driver(&usb_bus_driver, "USB")) {
		ntoskernel_exit();
		return -ENOMEM;
	}
	mdl_cache = kmem_cache_create("ndis_mdl", sizeof(struct wrap_mdl),
				      0, 0, NULL, NULL);
	if (!mdl_cache) {
		ERROR("couldn't allocate MDL cache");
		return -ENOMEM;
	}
	return 0;
}

void ntoskernel_exit(void)
{
	struct nt_list *cur;
	KIRQL irql;

	del_bus_drivers();
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
	return;
}

static int add_bus_driver(struct driver_object *drv_obj, const char *name)
{
	struct bus_driver *bus_driver;

	bus_driver = kmalloc(sizeof(*bus_driver), GFP_KERNEL);
	if (!bus_driver) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	memset(bus_driver, 0, sizeof(*bus_driver));
	strncpy(bus_driver->name, name, sizeof(bus_driver->name));
	InsertTailList(&bus_driver_list, &bus_driver->list);
	bus_driver->drv_obj = drv_obj;
	return 0;
}

static void del_bus_drivers(void)
{
	struct nt_list *ent;
	while ((ent = RemoveHeadList(&bus_driver_list))) {
		struct bus_driver *bus_driver;
		bus_driver = container_of(ent, struct bus_driver, list);
		/* TODO: make sure all all drivers are shutdown/removed */
		kfree(bus_driver);
	}
}

struct driver_object *find_bus_driver(const char *name)
{
	struct nt_list *ent;
	nt_list_for_each(ent, &bus_driver_list) {
		struct bus_driver *bus_driver;
		bus_driver = container_of(ent, struct bus_driver, list);
		if (strcmp(bus_driver->name, name) == 0)
			return bus_driver->drv_obj;
	}
	return NULL;
}

static struct device_object *find_pdo(struct driver_object *drv_obj,
				      struct phys_dev *dev)
{
	struct device_object *pdo;

	pdo = drv_obj->dev_obj;
	while (pdo && pdo->dev_ext != dev)
		pdo = pdo->next;
	return pdo;
}

struct device_object *alloc_pdo(struct driver_object *drv_obj,
				struct phys_dev *dev)
{
	struct device_object *pdo;
	NTSTATUS res ;

	res = IoCreateDevice(drv_obj, 0, NULL, FILE_DEVICE_UNKNOWN,
			     0, FALSE, &pdo);
	DBGTRACE2("%d, %p", res, pdo);
	if (res != STATUS_SUCCESS)
		return NULL;
	pdo->dev_ext = dev;
	return pdo;
}

void free_pdo(struct driver_object *drv_obj, struct phys_dev *dev)
{
	struct device_object *pdo;

	pdo = find_pdo(drv_obj, dev);
	IoDeleteDevice(pdo);
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedInsertHeadList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	struct nt_list *first;
	KIRQL irql;

	TRACEENTER4("head = %p, entry = %p", head, entry);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	first = InsertHeadList(head, entry);
	kspin_unlock_irql(lock, irql);
	DBGTRACE4("head = %p, old = %p", head, first);
	return first;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedInsertHeadList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExfInterlockedInsertHeadList(FASTCALL_ARGS_3(head, entry,
							    lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedInsertTailList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	struct nt_list *last;
	KIRQL irql;

	TRACEENTER4("head = %p, entry = %p", head, entry);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	last = InsertTailList(head, entry);
	kspin_unlock_irql(lock, irql);
	DBGTRACE4("head = %p, old = %p", head, last);
	return last;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedInsertTailList)
	(FASTCALL_DECL_3(struct nt_list *head, struct nt_list *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExfInterlockedInsertTailList(FASTCALL_ARGS_3(head, entry,
							    lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedRemoveHeadList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	struct nt_list *ret;
	KIRQL irql;

	TRACEENTER4("head = %p", head);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	ret = RemoveHeadList(head);
	kspin_unlock_irql(lock, irql);
	DBGTRACE4("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedRemoveHeadList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExfInterlockedRemoveHeadList(FASTCALL_ARGS_2(head, lock));
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExfInterlockedRemoveTailList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	struct nt_list *ret;
	KIRQL irql;

	TRACEENTER4("head = %p", head);
	irql = kspin_lock_irql(lock, DISPATCH_LEVEL);
	ret = RemoveTailList(head);
	kspin_unlock_irql(lock, irql);
	DBGTRACE4("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_list *WRAP_EXPORT(ExInterlockedRemoveTailList)
	(FASTCALL_DECL_2(struct nt_list *head, KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExfInterlockedRemoveTailList(FASTCALL_ARGS_2(head, lock));
}

STDCALL struct nt_slist *WRAP_EXPORT(ExpInterlockedPushEntrySList)
	(union nt_slist_head *head, struct nt_slist *entry)
{
	struct nt_slist *ret;
	KIRQL irql;

	TRACEENTER4("head = %p", head);
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	ret = PushEntryList(head, entry);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	DBGTRACE4("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_slist *WRAP_EXPORT(ExInterlockedPushEntrySList)
	(FASTCALL_DECL_3(union nt_slist_head *head, struct nt_slist *entry,
			 KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExpInterlockedPushEntrySList(head, entry);
}

_FASTCALL struct nt_slist *WRAP_EXPORT(InterlockedPushEntrySList)
	(FASTCALL_DECL_2(union nt_slist_head *head, struct nt_slist *entry))
{
	TRACEENTER4("%p", head);
	return ExpInterlockedPushEntrySList(head, entry);
}

STDCALL struct nt_slist *WRAP_EXPORT(ExpInterlockedPopEntrySList)
	(union nt_slist_head *head)
{
	struct nt_slist *ret;
	KIRQL irql;

	TRACEENTER4("head = %p", head);
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	ret = PopEntryList(head);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	DBGTRACE4("head = %p, ret = %p", head, ret);
	return ret;
}

_FASTCALL struct nt_slist *WRAP_EXPORT(ExInterlockedPopEntrySList)
	(FASTCALL_DECL_2(union nt_slist_head *head, KSPIN_LOCK *lock))
{
	TRACEENTER4("%p", head);
	return ExpInterlockedPopEntrySList(head);
}

_FASTCALL struct nt_slist *WRAP_EXPORT(InterlockedPopEntrySList)
	(FASTCALL_DECL_1(union nt_slist_head *head))
{
	TRACEENTER4("%p", head);
	return ExpInterlockedPopEntrySList(head);
}

STDCALL USHORT WRAP_EXPORT(ExQueryDepthSList)
	(union nt_slist_head *head)
{
	TRACEENTER4("%p", head);
	return head->list.depth;
}

STDCALL void WRAP_EXPORT(KeInitializeTimer)
	(struct ktimer *ktimer)
{
	TRACEENTER4("%p", ktimer);

	KeInitializeEvent((struct kevent *)ktimer, NotificationEvent, FALSE);
	wrapper_init_timer(ktimer, NULL, NULL);
}

STDCALL void WRAP_EXPORT(KeInitializeTimerEx)
	(struct ktimer *ktimer)
{
	TRACEENTER4("%p", ktimer);

	KeInitializeEvent((struct kevent *)ktimer, SynchronizationEvent,
			  FALSE);
	wrapper_init_timer(ktimer, NULL, NULL);
}

STDCALL void WRAP_EXPORT(KeInitializeDpc)
	(struct kdpc *kdpc, void *func, void *ctx)
{
	KIRQL irql;

	TRACEENTER4("%p, %p, %p", kdpc, func, ctx);
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	kdpc->func = func;
	kdpc->ctx  = ctx;
	kspin_unlock_irql(&ntoskernel_lock, irql);
}

static void kdpc_worker(void *data)
{
	struct nt_list *entry;
	struct kdpc *kdpc;
	KIRQL irql;
	DPC dpc_func;

	while (1) {
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		entry = RemoveHeadList(&kdpc_list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		if (!entry)
			break;
		kdpc = container_of(entry, struct kdpc, list);
		dpc_func = kdpc->func;
		irql = raise_irql(DISPATCH_LEVEL);
		LIN2WIN4(dpc_func, kdpc, kdpc->ctx, kdpc->arg1, kdpc->arg2);
		lower_irql(irql);
	}
}

/* this function should be called with ntoskernel_lock held at
 * DISPATCH_LEVEL */
BOOLEAN insert_kdpc_work(struct kdpc *kdpc)
{
	struct nt_list *cur;

	nt_list_for_each(cur, &kdpc_list) {
		struct kdpc *tmp;
		tmp = container_of(cur, struct kdpc, list);
		if (tmp == kdpc)
			return FALSE;
	}
	InsertTailList(&kdpc_list, &kdpc->list);
	schedule_work(&kdpc_work);
	return TRUE;
}

/* this function should be called with ntoskernel_lock held at
 * DISPATCH_LEVEL */
BOOLEAN remove_kdpc_work(struct kdpc *kdpc)
{
	struct nt_list *cur;

	nt_list_for_each(cur, &kdpc_list) {
		struct kdpc *tmp = container_of(cur, struct kdpc, list);
		if (tmp == kdpc) {
			RemoveEntryList(&kdpc->list);
			return TRUE;
		}
	}
	return FALSE;
}

STDCALL BOOLEAN KeInsertQueueDpc(struct kdpc *kdpc, void *arg1, void *arg2)
{
	BOOLEAN ret;

	/* this function is called at IRQL >= DISPATCH_LEVEL */
	kspin_lock(&ntoskernel_lock);
	kdpc->arg1 = arg1;
	kdpc->arg2 = arg2;
	ret = insert_kdpc_work(kdpc);
	kspin_unlock(&ntoskernel_lock);
	return ret;
}

STDCALL BOOLEAN KeRemoveQueueDpc(struct kdpc *kdpc)
{
	BOOLEAN ret;
	KIRQL irql;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	ret = remove_kdpc_work(kdpc);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return ret;
}

STDCALL BOOLEAN WRAP_EXPORT(KeSetTimerEx)
	(struct ktimer *ktimer, LARGE_INTEGER due_time, LONG period,
	 struct kdpc *kdpc)
{
	unsigned long expires;
	unsigned long repeat;

	TRACEENTER4("%p, %ld, %u, %p", ktimer, (long)due_time, period, kdpc);

	if (due_time < 0)
		expires = HZ * (-due_time) / TICKSPERSEC;
	else
		expires = HZ * due_time / TICKSPERSEC - jiffies;
	repeat = HZ * period / TICKSPERSEC;
	return wrapper_set_timer(ktimer->wrapper_timer, expires, repeat, kdpc);
}

STDCALL BOOLEAN WRAP_EXPORT(KeSetTimer)
	(struct ktimer *ktimer, LARGE_INTEGER due_time, struct kdpc *kdpc)
{
	TRACEENTER4("%p, %ld, %p", ktimer, (long)due_time, kdpc);
	return KeSetTimerEx(ktimer, due_time, 0, kdpc);
}

STDCALL BOOLEAN WRAP_EXPORT(KeCancelTimer)
	(struct ktimer *ktimer)
{
	char canceled;

	TRACEENTER4("%p", ktimer);
	wrapper_cancel_timer(ktimer->wrapper_timer, &canceled);
	return canceled;
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

	TRACEENTER4("%s", "");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	(*val)--;
	x = *val;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedIncrement)
	(FASTCALL_DECL_1(LONG volatile *val))
{
	LONG x;
	KIRQL irql;

	TRACEENTER4("%s", "");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	(*val)++;
	x = *val;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedExchange)
	(FASTCALL_DECL_2(LONG volatile *target, LONG val))
{
	LONG x;
	KIRQL irql;

	TRACEENTER4("%s", "");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	x = *target;
	*target = val;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedCompareExchange)
	(FASTCALL_DECL_3(LONG volatile *dest, LONG xchg, LONG comperand))
{
	LONG x;
	KIRQL irql;

	TRACEENTER4("%s", "");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	x = *dest;
	if (*dest == comperand)
		*dest = xchg;
	kspin_unlock_irql(&ntoskernel_lock, irql);
	TRACEEXIT4(return x);
}

_FASTCALL void WRAP_EXPORT(ExInterlockedAddLargeStatistic)
	(FASTCALL_DECL_2(LARGE_INTEGER *plint, ULONG n))
{
	unsigned long flags;

	TRACEENTER4("%p = %llu, n = %u", plint, *plint, n);
	kspin_lock_irqsave(&ntoskernel_lock, flags);
	*plint += n;
	kspin_unlock_irqrestore(&ntoskernel_lock, flags);
}

STDCALL void *WRAP_EXPORT(ExAllocatePoolWithTag)
	(enum pool_type pool_type, SIZE_T size, ULONG tag)
{
	void *ret;

	TRACEENTER1("pool_type: %d, size: %lu, tag: %u", pool_type,
		    size, tag);

	if (current_irql() == DISPATCH_LEVEL)
		ret = kmalloc(size, GFP_ATOMIC);
	else
		ret = kmalloc(size, GFP_KERNEL);
			
	DBGTRACE2("%p", ret);
	return ret;
}

STDCALL void WRAP_EXPORT(ExFreePool)
	(void *p)
{
	TRACEENTER2("%p", p);
	kfree(p);
	TRACEEXIT2(return);
}

WRAP_FUNC_PTR_DECL(ExAllocatePoolWithTag)
WRAP_FUNC_PTR_DECL(ExFreePool)

STDCALL void WRAP_EXPORT(ExInitializeNPagedLookasideList)
	(struct npaged_lookaside_list *lookaside,
	 LOOKASIDE_ALLOC_FUNC *alloc_func, LOOKASIDE_FREE_FUNC *free_func,
	 ULONG flags, SIZE_T size, ULONG tag, USHORT depth)
{
	TRACEENTER3("lookaside: %p, size: %lu, flags: %u,"
		    " head: %p, alloc: %p, free: %p",
		    lookaside, size, flags, lookaside->head.list.next,
		    alloc_func, free_func);

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

	TRACEENTER3("lookaside = %p", lookaside);
	while ((entry = ExpInterlockedPopEntrySList(&lookaside->head)))
		ExFreePool(entry);

	TRACEEXIT4(return);
}

STDCALL NTSTATUS WRAP_EXPORT(ExCreateCallback)
	(struct callback_object **object, struct object_attributes *attributes,
	 BOOLEAN create, BOOLEAN allow_multiple_callbacks)
{
	struct callback_object *obj;
	struct nt_list *cur;
	KIRQL irql;

	TRACEENTER2("");
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(cur, &callback_objects) {
		obj = container_of(cur, struct callback_object,
				   callback_funcs);
		if (obj->attributes == attributes) {
			kspin_unlock_irql(&ntoskernel_lock, irql);
			*object = obj;
			return STATUS_SUCCESS;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
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
	return;
}

STDCALL void WRAP_EXPORT(ExNotifyCallback)
	(struct callback_object *object, void *arg1, void *arg2)
{
	struct nt_list *cur;
	struct callback_func *callback;
	KIRQL irql;

	TRACEENTER3("%p", object);
	irql = kspin_lock_irql(&object->lock, DISPATCH_LEVEL);
	nt_list_for_each(cur, &object->callback_funcs){
		callback = container_of(cur, struct callback_func, list);
		LIN2WIN3(callback->func, callback->context, arg1, arg2);
	}
	kspin_unlock_irql(&object->lock, irql);
	return;
}

STDCALL void WRAP_EXPORT(KeInitializeEvent)
	(struct kevent *kevent, enum event_type type, BOOLEAN state)
{
	TRACEENTER3("event = %p, type = %d, state = %d", kevent, type, state);
	DBGTRACE2("");
	kevent->dh.type = type;
	if (state == TRUE)
		kevent->dh.signal_state = 1;
	else
		kevent->dh.signal_state = 0;
	DBGTRACE2("");
	InitializeListHead(&kevent->dh.wait_list);
	TRACEEXIT3(return);
}

/* this function should be called holding kevent_lock spinlock at
 * DISPATCH_LEVEL */
static void wakeup_event(struct kevent *kevent)
{
	struct nt_list *ent;

	while (kevent->dh.signal_state > 0 &&
	       (ent = RemoveHeadList(&kevent->dh.wait_list))) {
		struct wait_block *wb;

		wb = container_of(ent, struct wait_block, list_entry);
		DBGTRACE3("waking up process %p (%p,%p)", wb->thread, wb,
			  kevent);
		if (wb->thread) {
			/* make sure that the thread calls schedule
			   before trying to wake it up; otherwise we
			   may wake up a thread before it puts itself
			   to sleep, and it will stay in sleep */
			wake_up_process((task_t *)wb->thread);
//			set_task_state((task_t *)wb->thread, TASK_RUNNING);
		} else
			ERROR("illegal wait block %p(%p)", wb, kevent);
		if (kevent->dh.type == SynchronizationEvent)
			break;
	}
	return;
}

STDCALL LONG WRAP_EXPORT(KeSetEvent)
	(struct kevent *kevent, KPRIORITY incr, BOOLEAN wait)
{
	LONG old_state;
	KIRQL irql;

	TRACEENTER4("event = %p, type = %d, wait = %d",
		    kevent, kevent->dh.type, wait);
	if (wait == TRUE)
		WARNING("wait = %d, not yet implemented", wait);

	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	old_state = kevent->dh.signal_state;
	kevent->dh.signal_state = 1;
	wakeup_event(kevent);
	kspin_unlock_irql(&kevent_lock, irql);
	TRACEEXIT4(return old_state);
}

STDCALL void WRAP_EXPORT(KeClearEvent)
	(struct kevent *kevent)
{
	KIRQL irql;

	TRACEENTER4("event = %p", kevent);
	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	kevent->dh.signal_state = 0;
	kspin_unlock_irql(&kevent_lock, irql);
	TRACEEXIT4(return);
}

STDCALL LONG WRAP_EXPORT(KeResetEvent)
	(struct kevent *kevent)
{
	LONG old_state;
	KIRQL irql;

	TRACEENTER4("event = %p", kevent);

	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	old_state = kevent->dh.signal_state;
	kevent->dh.signal_state = 0;
	kspin_unlock_irql(&kevent_lock, irql);

	TRACEEXIT4(return old_state);
}

STDCALL void WRAP_EXPORT(KeInitializeMutex)
	(struct kmutex *kmutex, BOOLEAN wait)
{
	TRACEENTER3("%p", kmutex);
	memset(kmutex, 0, sizeof(*kmutex));
	kmutex->dh.type = SynchronizationEvent;
	kmutex->dh.size = sizeof(*kmutex);
	kmutex->dh.signal_state = 1;
	InitializeListHead(&kmutex->dh.wait_list);
	InitializeListHead(&kmutex->list);
	kmutex->abandoned = FALSE;
	kmutex->apc_disable = 1;
	kmutex->owner_thread = NULL;
	return;
}

STDCALL LONG WRAP_EXPORT(KeReleaseMutex)
	(struct kmutex *kmutex, BOOLEAN wait)
{
	LONG ret;
	KIRQL irql;

	TRACEENTER5("%p", kmutex);
	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	ret = kmutex->dh.signal_state++;
	if (kmutex->dh.signal_state > 0) {
		kmutex->owner_thread = NULL;
		wakeup_event((struct kevent *)kmutex);
		kspin_unlock_irql(&kevent_lock, irql);
	} else
		kspin_unlock_irql(&kevent_lock, irql);
	return ret;
}

STDCALL void WRAP_EXPORT(KeInitializeSemaphore)
	(struct ksemaphore *ksemaphore, LONG count, LONG limit)
{
	TRACEENTER3("%p", ksemaphore);
	memset(ksemaphore, 0, sizeof(*ksemaphore));
	/* if limit > 1, we need to satisfy as many waits (until count
	 * becomes 0); so we set it as notification event, and keep
	 * decrementing count everytime a wait is satisified */
	ksemaphore->dh.type = NotificationEvent;
	ksemaphore->dh.size = sizeof(*ksemaphore);
	ksemaphore->dh.signal_state = count;
	InitializeListHead(&ksemaphore->dh.wait_list);
	ksemaphore->limit = limit;
}

STDCALL LONG WRAP_EXPORT(KeReleaseSemaphore)
	(struct ksemaphore *ksemaphore, KPRIORITY incr, LONG adjustment,
	 BOOLEAN wait)
{
	LONG ret;
	KIRQL irql;

	TRACEENTER5("%p", ksemaphore);
	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	ret = ksemaphore->dh.signal_state;
	ksemaphore->dh.signal_state += adjustment;
	if (ksemaphore->dh.signal_state > ksemaphore->limit)
		ksemaphore->dh.signal_state = ksemaphore->limit;
	if (ksemaphore->dh.signal_state > 0)
		wakeup_event((struct kevent *)ksemaphore);
	kspin_unlock_irql(&kevent_lock, irql);
	return ret;
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForMultipleObjects)
	(ULONG count, struct kevent *object[],
	 enum wait_type wait_type, KWAIT_REASON wait_reason,
	 KPROCESSOR_MODE wait_mode, BOOLEAN alertable, LARGE_INTEGER *timeout,
	 struct wait_block *wait_block_array)
{
	int i, res = 0, wait_count, wait_index = 0;
	long wait_jiffies;
	struct wait_block *wb, wb_array[THREAD_WAIT_OBJECTS];
	struct kmutex *kmutex;
	struct ksemaphore *ksemaphore;
	struct dispatch_header *dh;
	KIRQL irql;

	TRACEENTER2("count = %d, reason = %u, waitmode = %u, alertable = %u,"
		    " timeout = %p", count, wait_reason, wait_mode,
		    alertable, timeout);

	if (count > MAX_WAIT_OBJECTS)
		TRACEEXIT2(return STATUS_INVALID_PARAMETER);
	if (count > THREAD_WAIT_OBJECTS && wait_block_array == NULL)
		TRACEEXIT2(return STATUS_INVALID_PARAMETER);

	if (wait_block_array == NULL)
		wb = &wb_array[0];
	else
		wb = wait_block_array;

	irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
	/* first get the list of objects the thread need to wait on
	 * and add put it on the wait list for each such object */
	for (i = 0, wait_count = 0; i < count; i++) {
		dh = &object[i]->dh;
		kmutex = (struct kmutex *)object[i];
		/* wait succeeds if either object is in signal state
		 * (i.e., dh->signal_state > 0) or it is mutex and we
		 * already own it (in case of recursive mutexes,
		 * signal state can be negative) */
		if (dh->signal_state > 0 ||
		    (dh->size == sizeof(*kmutex) &&
		     kmutex->owner_thread == get_current())) {
			DBGTRACE3("%p is already signaled", object[i]);
			/* if synchronization event or semaphore,
			 * decrement count */
			if (dh->type == SynchronizationEvent ||
			    dh->size == sizeof(*ksemaphore))
				dh->signal_state--;
			if (dh->size == sizeof(*kmutex))
				kmutex->owner_thread = get_current;
			if (wait_type == WaitAny) {
				kspin_unlock_irql(&kevent_lock, irql);
				TRACEEXIT3(return STATUS_WAIT_0 + i);
			}
			/* mark that we are not waiting on this object */
			wb[i].thread = NULL;
			wb[i].object = NULL;
		} else {
			wb[i].thread = get_current();
			wb[i].object = object[i];
			InsertTailList(&dh->wait_list, &wb[i].list_entry);
			wait_count++;
			DBGTRACE3("%p (%p) waiting on event %p", &wb[i],
				  wb[i].thread, wb[i].object);
		}
	}
	if (wait_count == 0) {
		kspin_unlock_irql(&kevent_lock, irql);
		TRACEEXIT3(return STATUS_SUCCESS);
	}

	if (timeout) {
		DBGTRACE2("timeout = %Ld", *timeout);
		if (*timeout == 0) {
			kspin_unlock_irql(&kevent_lock, irql);
			TRACEEXIT2(return STATUS_TIMEOUT);
		} else if (*timeout > 0) {
			long d = (*timeout) - ticks_1601();
			/* some drivers call this function with much
			 * smaller numbers that suggest either drivers
			 * are broken or explanation for this is
			 * wrong */
			if (d > 0)
				wait_jiffies = HZ * d / TICKSPERSEC;
			else
				wait_jiffies = 0;
		} else
			wait_jiffies = HZ * (-(*timeout)) / TICKSPERSEC;
	} else
		wait_jiffies = 0;

	/* we put the task state in appropriate state before releasing
	 * the spinlock, so that if the event is set to signaled state
	 * after putting the thread on the wait list but before we put
	 * the thread into sleep (with 'schedule'), wakup_event will
	 * put the thread back into running state; otherwise,
	 * wakeup_event may put this into running state and _then_ we
	 * go to sleep causing thread to miss the event */
	if (alertable)
		set_current_state(TASK_INTERRUPTIBLE);
	else
		set_current_state(TASK_UNINTERRUPTIBLE);
	kspin_unlock_irql(&kevent_lock, irql);

	DBGTRACE3("%p is going to sleep for %ld", get_current(), wait_jiffies);
	while (wait_count) {
		if (wait_jiffies > 0)
			res = schedule_timeout(wait_jiffies);
		else {
			schedule();
			res = 1;
		}
		if (signal_pending(current))
			res = -ERESTARTSYS;
		irql = kspin_lock_irql(&kevent_lock, DISPATCH_LEVEL);
		DBGTRACE3("%p woke up, res = %d", get_current(), res);
		for (i = 0; i < count; i++) {
			dh = &object[i]->dh;
			kmutex = (struct kmutex *)object[i];
			if (dh->signal_state > 0) {
				if (dh->type == SynchronizationEvent ||
				    dh->size == sizeof(*ksemaphore))
					dh->signal_state--;
				if (dh->size == sizeof(*kmutex))
					kmutex->owner_thread = get_current();
				/* mark that this wb is not on the list */
				wb[i].thread = NULL;
				wb[i].object = NULL;
				RemoveEntryList(&wb[i].list_entry);
				wait_count--;
				wait_index = i;
			}
		}
		if (res <= 0 || wait_type == WaitAny || wait_count == 0) {
			/* we are done; remove from wait list */
			for (i = 0; i < count; i++)
				if (wb[i].thread)
					RemoveEntryList(&wb[i].list_entry);
			kspin_unlock_irql(&kevent_lock, irql);
			DBGTRACE3("%p woke up, res = %d", get_current(), res);
			if (res > 0 && wait_type == WaitAny)
				TRACEEXIT2(return STATUS_WAIT_0 + wait_index);
			if (wait_count == 0) // res > 0
				TRACEEXIT2(return STATUS_SUCCESS);
			if (res < 0)
				TRACEEXIT2(return STATUS_ALERTED);
			if (res == 0)
				TRACEEXIT2(return STATUS_TIMEOUT);
		}
		wait_jiffies = res;
		if (alertable)
			set_current_state(TASK_INTERRUPTIBLE);
		else
			set_current_state(TASK_UNINTERRUPTIBLE);
		kspin_unlock_irql(&kevent_lock, irql);
	}
	/* this should never reach, but compiler wants return value */
	set_current_state(TASK_RUNNING);
	TRACEEXIT1(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForSingleObject)
	(struct kevent *object, KWAIT_REASON wait_reason,
	 KPROCESSOR_MODE wait_mode, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
	struct kevent *obj = object;
	return KeWaitForMultipleObjects(1, &obj, WaitAll, wait_reason,
					wait_mode, alertable, timeout, NULL);
}

STDCALL NTSTATUS WRAP_EXPORT(KeDelayExecutionThread)
	(KPROCESSOR_MODE wait_mode, BOOLEAN alertable,
	 LARGE_INTEGER *interval)
{
	int res;
	long timeout;
	long t = *interval;

	TRACEENTER3("thread: %p, interval: %ld", get_current(), t);
	if (wait_mode != 0)
		ERROR("illegal wait_mode %d", wait_mode);

	if (t < 0)
		timeout = HZ * (-t) / TICKSPERSEC;
	else
		timeout = HZ * t / TICKSPERSEC - jiffies;

	if (timeout <= 0)
		TRACEEXIT3(return STATUS_SUCCESS);

	if (alertable)
		set_current_state(TASK_INTERRUPTIBLE);
	else
		set_current_state(TASK_UNINTERRUPTIBLE);

	res = schedule_timeout(timeout);
	if (res == 0)
		TRACEEXIT3(return STATUS_SUCCESS);
	else
		TRACEEXIT3(return STATUS_ALERTED);
}

STDCALL KPRIORITY WRAP_EXPORT(KeQueryPriorityThread)
	(void *thread)
{
	KPRIORITY prio;

	TRACEENTER5("thread = %p", thread);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	prio = 1;
#else
	if (rt_task((task_t *)thread))
		prio = LOW_REALTIME_PRIORITY;
	else
		prio = MAXIMUM_PRIORITY;
#endif
	TRACEEXIT5(return prio);
}

STDCALL ULONGLONG WRAP_EXPORT(KeQueryInterruptTime)
	(void)
{
	TRACEEXIT4(return jiffies * TICKSPERSEC / HZ);
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

STDCALL LARGE_INTEGER WRAP_EXPORT(KeQueryPerformanceCounter)
	(LARGE_INTEGER *counter)
{
	unsigned long res;

	res = jiffies;
	if (counter)
		*counter = res;
	return res;
}

STDCALL void *WRAP_EXPORT(KeGetCurrentThread)
	(void)
{
	void *thread = get_current();

	TRACEENTER2("current thread = %p", thread);
	return thread;
}

STDCALL KPRIORITY WRAP_EXPORT(KeSetPriorityThread)
	(void *thread, KPRIORITY priority)
{
	KPRIORITY old_prio;

	TRACEENTER2("thread = %p, priority = %u", thread, priority);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	/* FIXME: is there a way to set kernel thread prio on 2.4? */
	old_prio = LOW_PRIORITY;
#else
	if (rt_task((task_t *)thread))
		old_prio = LOW_REALTIME_PRIORITY;
	else
		old_prio = MAXIMUM_PRIORITY;
	if (priority == LOW_REALTIME_PRIORITY)
		set_user_nice((task_t *)thread, -20);
	else
		set_user_nice((task_t *)thread, 10);
#endif
	return old_prio;
}

STDCALL BOOLEAN WRAP_EXPORT(KeRemoveEntryDeviceQueue)
	(struct kdevice_queue *dev_queue, struct kdevice_queue_entry *entry)
{
	struct nt_list *cur;
	KIRQL irql;

	irql = kspin_lock_irql(&dev_queue->lock, DISPATCH_LEVEL);
	nt_list_for_each(cur, &dev_queue->list) {
		struct kdevice_queue_entry *e;
		e = container_of(cur, struct kdevice_queue_entry, list);
		if (e == entry) {
			RemoveEntryList(cur);
			kspin_unlock_irql(&dev_queue->lock, irql);
			return TRUE;
		}
	}
	kspin_unlock_irql(&dev_queue->lock, irql);
	return FALSE;
}

STDCALL NTSTATUS WRAP_EXPORT(IoGetDeviceProperty)
	(struct device_object *pdo,
	 enum device_registry_property dev_property,
	 ULONG buffer_len, void *buffer, ULONG *result_len)
{
	struct ansi_string ansi;
	struct unicode_string unicode;
	struct wrapper_dev *wd;
	char buf[32];

	wd = (struct wrapper_dev *)pdo->wd;

	TRACEENTER1("dev_obj = %p, dev_property = %d, buffer_len = %u, "
		"buffer = %p, result_len = %p", pdo, dev_property,
		buffer_len, buffer, result_len);

	switch (dev_property) {
	case DevicePropertyDeviceDescription:
		if (buffer_len > 0 && buffer) {
			*result_len = 4;
			memset(buffer, 0xFF, *result_len);
			TRACEEXIT1(return STATUS_SUCCESS);
		} else {
			*result_len = 4;
			TRACEEXIT1(return STATUS_SUCCESS);
		}
		break;

	case DevicePropertyFriendlyName:
		if (buffer_len > 0 && buffer) {
			ansi.len = snprintf(buf, sizeof(buf), "%d",
					    wd->dev.usb->devnum);
			ansi.buf = buf;
			ansi.len = strlen(ansi.buf);
			if (ansi.len <= 0) {
				*result_len = 0;
				TRACEEXIT1(return STATUS_BUFFER_TOO_SMALL);
			}
			ansi.buflen = ansi.len;
			unicode.buf = buffer;
			unicode.buflen = buffer_len;
			DBGTRACE1("unicode.buflen = %d, ansi.len = %d",
					unicode.buflen, ansi.len);
			if (RtlAnsiStringToUnicodeString(&unicode, &ansi, 0)) {
				*result_len = 0;
				TRACEEXIT1(return STATUS_BUFFER_TOO_SMALL);
			} else {
				*result_len = unicode.len;
				TRACEEXIT1(return STATUS_SUCCESS);
			}
		} else {
			ansi.len = snprintf(buf, sizeof(buf), "%d",
					    wd->dev.usb->devnum);
			*result_len = 2 * (ansi.len + 1);
			TRACEEXIT1(return STATUS_BUFFER_TOO_SMALL);
		}
		break;

	case DevicePropertyDriverKeyName:
//		ansi.buf = wd->driver->name;
		ansi.buf = buf;
		ansi.len = strlen(ansi.buf);
		ansi.buflen = ansi.len;
		if (buffer_len > 0 && buffer) {
			unicode.buf = buffer;
			unicode.buflen = buffer_len;
			if (RtlAnsiStringToUnicodeString(&unicode, &ansi, 0)) {
				*result_len = 0;
				TRACEEXIT1(return STATUS_BUFFER_TOO_SMALL);
			} else {
				*result_len = unicode.len;
				TRACEEXIT1(return STATUS_SUCCESS);
			}
		} else {
				*result_len = 2 * (strlen(buf) + 1);
				TRACEEXIT1(return STATUS_SUCCESS);
		}
		break;
	default:
		TRACEEXIT1(return STATUS_INVALID_PARAMETER_2);
	}
}

STDCALL int WRAP_EXPORT(IoIsWdmVersionAvailable)
	(UCHAR major, UCHAR minor)
{
	TRACEENTER3("%d, %x", major, minor);
	if (major == 1 &&
	    (minor == 0x30 || // Windows 2003
	     minor == 0x20 || // Windows XP
	     minor == 0x10)) // Windows 2000
		TRACEEXIT3(return 1);
	TRACEEXIT3(return 0);
}

STDCALL BOOLEAN WRAP_EXPORT(IoIs32bitProcess)
	(struct irp *irp)
{
#ifdef CONFIG_X86_64
	return FALSE;
#else
	return TRUE;
#endif
}

STDCALL void WRAP_EXPORT(IoBuildSynchronousFsdRequest)
	(void)
{
	UNIMPL();
}

STDCALL struct irp *WRAP_EXPORT(IoAllocateIrp)
	(char stack_size, BOOLEAN charge_quota)
{
	struct irp *irp;
	int size;

	USBTRACEENTER("stack_size = %d, charge_quota = %d",
		      stack_size, charge_quota);

	size = sizeof(struct irp) +
		stack_size * sizeof(struct io_stack_location);
	/* FIXME: we should better check what GFP_ is required */
	irp = kmalloc(size, GFP_ATOMIC);
	if (irp) {
		USBTRACE("allocated irp %p", irp);
		memset(irp, 0, size);

		irp->size = size;
		irp->stack_size = stack_size;
		irp->stack_pos = stack_size;
		IRP_CUR_STACK_LOC(irp) =
			((struct io_stack_location *)(irp + 1)) + stack_size;
	}

	USBTRACEEXIT(return irp);
}

STDCALL void WRAP_EXPORT(IoInitializeIrp)
	(struct irp *irp, USHORT size, CHAR stack_size)
{
	USBTRACEENTER("irp = %p, size = %d, stack_size = %d",
		      irp, size, stack_size);

	if (irp) {
		USBTRACE("initializing irp %p", irp);
		memset(irp, 0, size);

		irp->size = size;
		irp->stack_size = stack_size;
		irp->stack_pos = stack_size;
		IRP_CUR_STACK_LOC(irp) =
			((struct io_stack_location *)(irp+1)) + stack_size;
	}

	USBTRACEEXIT(return);
}

STDCALL void WRAP_EXPORT(IoReuseIrp)
	(struct irp *irp, NTSTATUS status)
{
	USBTRACEENTER("irp = %p, status = %d", irp, status);
	if (irp)
		irp->io_status.status = status;
	USBTRACEEXIT(return);
}

STDCALL BOOLEAN WRAP_EXPORT(IoCancelIrp)
	(struct irp *irp)
{
	struct io_stack_location *stack = IRP_CUR_STACK_LOC(irp) - 1;
	void (*cancel_routine)(struct device_object *, struct irp *) STDCALL;

	USBTRACEENTER("irp = %p", irp);

	kspin_lock(&irp_cancel_lock);
	cancel_routine = xchg(&irp->cancel_routine, NULL);

	if (cancel_routine) {
		irp->cancel_irql = current_irql();
		irp->pending_returned = 1;
		irp->cancel = 1;
		cancel_routine(stack->dev_obj, irp);
		kspin_unlock(&irp_cancel_lock);
		USBTRACEEXIT(return TRUE);
	} else {
		kspin_unlock(&irp_cancel_lock);
		USBTRACEEXIT(return FALSE);
	}
}

STDCALL void WRAP_EXPORT(IoFreeIrp)
	(struct irp *irp)
{
	USBTRACEENTER("irp = %p", irp);

	kfree(irp);

	USBTRACEEXIT(return);
}

STDCALL struct irp *WRAP_EXPORT(IoBuildDeviceIoControlRequest)
	(ULONG ioctl, struct device_object *dev_obj,
	 void *input_buf, ULONG input_buf_len, void *output_buf,
	 ULONG output_buf_len, BOOLEAN internal_ioctl,
	 struct kevent *event, struct io_status_block *io_status)
{
	struct irp *irp;
	struct io_stack_location *stack;

	USBTRACEENTER("");

	irp = kmalloc(sizeof(struct irp) + sizeof(struct io_stack_location),
		GFP_KERNEL); /* we are running at IRQL = PASSIVE_LEVEL */
	if (irp) {
		USBTRACE("allocated irp %p", irp);
		memset(irp, 0, sizeof(struct irp) +
		       sizeof(struct io_stack_location));

		irp->size = sizeof(struct irp) +
			sizeof(struct io_stack_location);
		irp->stack_size = 1;
		irp->stack_pos = 1;
		irp->user_status = io_status;
		irp->user_event = event;
		irp->user_buf = output_buf;

		stack = (struct io_stack_location *)(irp + 1);
		IRP_CUR_STACK_LOC(irp) = stack + 1;

		stack->params.ioctl.code = ioctl;
		stack->params.ioctl.input_buf_len = input_buf_len;
		stack->params.ioctl.output_buf_len = output_buf_len;
		stack->params.ioctl.type3_input_buf = input_buf;
		stack->dev_obj = dev_obj;

		stack->major_fn = (internal_ioctl) ?
			IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
	}

	USBTRACEEXIT(return irp);
}

_FASTCALL void WRAP_EXPORT(IofCompleteRequest)
	(FASTCALL_DECL_2(struct irp *irp, CHAR prio_boost))
{
	struct io_stack_location *stack = IRP_CUR_STACK_LOC(irp) - 1;

	USBTRACEENTER("irp = %p", irp);

	if (irp->user_status) {
		irp->user_status->status = irp->io_status.status;
		irp->user_status->status_info = irp->io_status.status_info;
	}

	if ((stack->completion_handler) &&
	    ((((irp->io_status.status == 0) &&
	       (stack->control & CALL_ON_SUCCESS)) ||
	      ((irp->io_status.status == STATUS_CANCELLED) &&
	       (stack->control & CALL_ON_CANCEL)) ||
	      ((irp->io_status.status != 0) &&
	       (stack->control & CALL_ON_ERROR))))) {
		USBTRACE("calling %p", stack->completion_handler);

		if (LIN2WIN3(stack->completion_handler, stack->dev_obj, irp,
			     stack->handler_arg) ==
		    STATUS_MORE_PROCESSING_REQUIRED)
			USBTRACEEXIT(return);
	}

	if (irp->user_event) {
		USBTRACE("setting event %p", irp->user_event);
		KeSetEvent(irp->user_event, 0, FALSE);
	}

	/* To-Do: what about IRP_DEALLOCATE_BUFFER...? */
	USBTRACE("freeing irp %p", irp);
	kfree(irp);
	USBTRACEEXIT(return);
}

_FASTCALL NTSTATUS WRAP_EXPORT(IofCallDriver)
	(FASTCALL_DECL_2(struct device_object *dev_obj, struct irp *irp))
{
	struct io_stack_location *stack = IRP_CUR_STACK_LOC(irp) - 1;
	NTSTATUS ret = STATUS_NOT_SUPPORTED;
	unsigned long result;


	USBTRACEENTER("dev_obj = %p, irp = %p, major_fn = %x, ioctl = %u",
		      dev_obj, irp, stack->major_fn, stack->params.ioctl.code);

	if (stack->major_fn == IRP_MJ_INTERNAL_DEVICE_CONTROL) {
		switch (stack->params.ioctl.code) {
#ifdef CONFIG_USB
			case IOCTL_INTERNAL_USB_SUBMIT_URB:
				ret = usb_submit_nt_urb(dev_obj->device.usb,
					stack->params.generic.arg1, irp);
				break;

			case IOCTL_INTERNAL_USB_RESET_PORT:
				ret = usb_reset_port(dev_obj->device.usb);
				break;
#endif

			default:
				ERROR("ioctl %08X NOT IMPLEMENTED!",
					stack->params.ioctl.code);
		}
	} else if (stack->major_fn == IRP_MJ_CREATE) {
		struct file_object *file_object;

		file_object = stack->file_obj;
		if (file_object) {
			struct ansi_string ansi;
			char file_name[256];

			ansi.buf = file_name;
			ansi.buflen = sizeof(file_name);
			if (!RtlUnicodeStringToAnsiString(&ansi,
							  &file_object->file_name, 0)) {
				file_name[sizeof(file_name) - 1] = 0;
				INFO("file: %s", file_name);
			}
			INFO("context: %p", file_object->fs_context);
		}
		irp->io_status.status_info = 0;
		ret = STATUS_SUCCESS;
	} else if (stack->major_fn & IRP_MJ_CLOSE) {
		irp->io_status.status_info = 0;
		ret = STATUS_SUCCESS;
		ERROR("major_fn %08X NOT IMPLEMENTED!\n", stack->major_fn);
	} else {
		irp->io_status.status_info = 0;
		ret = STATUS_SUCCESS;
		ERROR("major_fn %08X NOT IMPLEMENTED!\n", stack->major_fn);
	}

	if (ret == STATUS_PENDING) {
		stack->control |= IS_PENDING;
		USBTRACEEXIT(return ret);
	} else {
		irp->io_status.status = ret;
		if (irp->user_status)
			irp->user_status->status = ret;

		if ((stack->completion_handler) &&
		    ((((ret == 0) && (stack->control & CALL_ON_SUCCESS)) ||
		      ((ret != 0) && (stack->control & CALL_ON_ERROR))))) {
			USBTRACE("calling %p", stack->completion_handler);

			result = stack->completion_handler(stack->dev_obj, irp,
				stack->handler_arg);
			if (result == STATUS_MORE_PROCESSING_REQUIRED)
				USBTRACEEXIT(return ret);
		}

		if (irp->user_event) {
			USBTRACE("setting event %p", irp->user_event);
			KeSetEvent(irp->user_event, 0, FALSE);
		}
	}

	/* To-Do: what about IRP_DEALLOCATE_BUFFER...? */
	USBTRACE("freeing irp %p", irp);
	kfree(irp);

	USBTRACEEXIT(return ret);
}

static irqreturn_t io_irq_th(int irq, void *data, struct pt_regs *pt_regs)
{
	struct kinterrupt *interrupt = (struct kinterrupt *)data;
	KSPIN_LOCK *spinlock;
	BOOLEAN ret;
	KIRQL irql = PASSIVE_LEVEL;

	if (interrupt->actual_lock)
		spinlock = interrupt->actual_lock;
	else
		spinlock = &interrupt->lock;
	if (interrupt->synch_irql >= DISPATCH_LEVEL)
		irql = kspin_lock_irql(spinlock, DISPATCH_LEVEL);
	else
		kspin_lock(spinlock);
	ret = interrupt->service_routine(interrupt,
					 interrupt->service_context);
	if (interrupt->synch_irql >= DISPATCH_LEVEL)
		kspin_unlock_irql(spinlock, irql);
	else
		kspin_unlock(spinlock);

	if (ret == TRUE)
		return IRQ_HANDLED;
	else
		return IRQ_NONE;
}

STDCALL NTSTATUS WRAP_EXPORT(IoConnectInterrupt)
	(struct kinterrupt *interrupt, PKSERVICE_ROUTINE service_routine,
	 void *service_context, KSPIN_LOCK *lock, ULONG vector,
	 KIRQL irql, KIRQL synch_irql, enum kinterrupt_mode interrupt_mode,
	 BOOLEAN shareable, KAFFINITY processor_enable_mask,
	 BOOLEAN floating_save)
{
	TRACEENTER1("");

	interrupt->vector = vector;
	interrupt->processor_enable_mask = processor_enable_mask;
	kspin_lock_init(&interrupt->lock);
	interrupt->actual_lock = lock;
	interrupt->shareable = shareable;
	interrupt->floating_save = floating_save;
	interrupt->service_routine = service_routine;
	interrupt->service_context = service_context;
	InitializeListHead(&interrupt->list);
	interrupt->irql = irql;
	if (synch_irql > DISPATCH_LEVEL)
		interrupt->synch_irql = DISPATCH_LEVEL;
	else
		interrupt->synch_irql = synch_irql;
	interrupt->interrupt_mode = interrupt_mode;
	if (request_irq(vector, io_irq_th, shareable ? SA_SHIRQ : 0,
			"io_irq", interrupt)) {
		WARNING("request for irq %d failed", vector);
		TRACEEXIT1(return STATUS_INSUFFICIENT_RESOURCES);
	}
	TRACEEXIT1(return STATUS_SUCCESS);
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

STDCALL void WRAP_EXPORT(IoDisconnectInterrupt)
	(struct kinterrupt *interrupt)
{
	free_irq(interrupt->vector, interrupt);
}

STDCALL NTSTATUS WRAP_EXPORT(PoCallDriver)
	(struct device_object *dev_obj, struct irp *irp)
{
	TRACEENTER5("irp = %p", irp);
	TRACEEXIT5(return IofCallDriver(FASTCALL_ARGS_2(dev_obj, irp)));
}

struct trampoline_context {
	void (*start_routine)(void *) STDCALL;
	void *context;
};

int kthread_trampoline(void *data)
{
	struct trampoline_context ctx;

	memcpy(&ctx, data, sizeof(ctx));
	kfree(data);

	ctx.start_routine(ctx.context);

	return 0;
}

STDCALL NTSTATUS WRAP_EXPORT(PsCreateSystemThread)
	(void **phandle, ULONG access, void *obj_attr, void *process,
	 void *client_id, void (*start_routine)(void *) STDCALL, void *context)
{
	struct trampoline_context *ctx;
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

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,7)
	pid = kernel_thread(kthread_trampoline, ctx,
		CLONE_FS|CLONE_FILES|CLONE_SIGHAND);
	DBGTRACE2("pid = %d", pid);
	if (pid < 0) {
		kfree(ctx);
		TRACEEXIT2(return STATUS_FAILURE);
	}
	*phandle = find_task_by_pid(pid);
	DBGTRACE2("*phandle = %p", *phandle);
#else
	*phandle = KTHREAD_RUN(kthread_trampoline, ctx, DRIVER_NAME);
	DBGTRACE2("*phandle = %p", *phandle);
	if (IS_ERR(*phandle)) {
		kfree(ctx);
		TRACEEXIT2(return STATUS_FAILURE);
	}
#endif

	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(PsTerminateSystemThread)
	(NTSTATUS status)
{
	struct nt_list *ent;
	struct obj_mgr_obj *object;
	struct kevent *event;
	KIRQL irql;

	TRACEENTER2("status = %u", status);
	event = NULL;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(ent, &obj_mgr_obj_list) {
		object = container_of(ent, struct obj_mgr_obj, list);
		if (object->handle == get_current()) {
			event = (struct kevent *)object;
			break;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	if (event)
		KeSetEvent(event, 0, FALSE);
	complete_and_exit(NULL, status);
	return STATUS_SUCCESS;
}

STDCALL void WRAP_EXPORT(PoStartNextPowerIrp)
	(struct irp *irp)
{
	TRACEENTER5("irp = %p", irp);
	TRACEEXIT5(return);
}

STDCALL void *WRAP_EXPORT(MmMapIoSpace)
	(PHYSICAL_ADDRESS phys_addr, SIZE_T size,
	 enum memory_caching_type cache)
{
	void *virt;
	if (cache)
		virt = ioremap(phys_addr, size);
	else
		virt = ioremap_nocache(phys_addr, size);
	DBGTRACE3("%Lx, %lu, %d: %p", phys_addr, size, cache, virt);
	return virt;
}

STDCALL void WRAP_EXPORT(MmUnmapIoSpace)
	(void *addr, SIZE_T size)
{
	TRACEENTER3("%p, %lu", addr, size);
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
		wrap_mdl = kmem_cache_alloc(mdl_cache, GFP_ATOMIC);
		if (!wrap_mdl)
			return NULL;
		DBGTRACE3("allocated mdl cache: %p", wrap_mdl);
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		mdl = (struct mdl *)wrap_mdl->mdl;
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
		DBGTRACE3("allocated mdl: %p", wrap_mdl);
		mdl = (struct mdl *)wrap_mdl->mdl;
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
		memset(mdl, 0, mdl_size);
		MmInitializeMdl(mdl, virt, length);
	}
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
	if (mdl) {
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
	}
	return;
}

STDCALL struct mdl *WRAP_EXPORT(IoAllocateMdl)
	(void *virt, ULONG length, BOOLEAN second_buf, BOOLEAN charge_quota,
	 struct irp *irp)
{
	struct mdl *mdl;
	mdl = allocate_init_mdl(virt, length);
	if (!mdl)
		return NULL;
	if (irp) {
		if (second_buf == TRUE) {
			struct mdl *last;

			last = irp->mdl;
			while (last->next)
				last = last->next;
			last->next = mdl;
		} else
			irp->mdl = mdl;
	}
	return mdl;
}

STDCALL void WRAP_EXPORT(IoFreeMdl)
	(struct mdl *mdl)
{
	free_mdl(mdl);
	TRACEEXIT3(return);
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

/* The object manager functions are not implemented the way DDK
 * describes - we don't return pointer to Windows Objects, but to a
 * dummy object that we allocate. The effect should be same as long as
 * drivers don't use this object for anything other than
 * object-manager functions below */

/* If handle is already in the list of objects in the list, just
 * increment the count; otherwise, allocate a new object, put it on
 * the list and increment the count */
STDCALL NTSTATUS WRAP_EXPORT(ObReferenceObjectByHandle)
	(void *handle, ACCESS_MASK desired_access, void *obj_type,
	 KPROCESSOR_MODE access_mode, void **object, void *handle_info)
{
	struct obj_mgr_obj *obj_mgr_obj;
	struct nt_list *ent;
	KIRQL irql;

	obj_mgr_obj = NULL;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(ent, &obj_mgr_obj_list) {
		struct obj_mgr_obj *tmp;
		tmp = container_of(ent, struct obj_mgr_obj, list);
		if (tmp->handle == handle) {
			obj_mgr_obj = tmp;
			break;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	if (!obj_mgr_obj) {
		obj_mgr_obj = kmalloc(sizeof(*obj_mgr_obj), GFP_KERNEL);
		if (!obj_mgr_obj)
			return STATUS_ACCESS_DENIED;

		memset(obj_mgr_obj, 0, sizeof(*obj_mgr_obj));
		InitializeListHead(&obj_mgr_obj->list);
		obj_mgr_obj->handle = handle;
		obj_mgr_obj->ref_count = 1;
		obj_mgr_obj->dh.size = sizeof(*obj_mgr_obj);
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		InsertTailList(&obj_mgr_obj_list, &obj_mgr_obj->list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
	} else {
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		obj_mgr_obj->ref_count++;
		kspin_unlock_irql(&ntoskernel_lock, irql);
	}
	*object = obj_mgr_obj;
	return STATUS_SUCCESS;
}

/* DDK doesn't say if return value should be before incrementing or
 * after incrementing reference count, but according to #reactos
 * devels, it should be return value after incrementing */
_FASTCALL LONG WRAP_EXPORT(ObfReferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	struct obj_mgr_obj *obj_mgr_obj;
	struct nt_list *ent;
	LONG ret;
	KIRQL irql;

	ret = 0;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(ent, &obj_mgr_obj_list) {
		obj_mgr_obj = container_of(ent, struct obj_mgr_obj, list);
		if (obj_mgr_obj == object) {
			ret = ++(obj_mgr_obj->ref_count);
			break;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return ret;
}

_FASTCALL void WRAP_EXPORT(ObfDereferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	struct obj_mgr_obj *obj_mgr_obj;
	struct nt_list *ent;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(ent, &obj_mgr_obj_list) {
		obj_mgr_obj = container_of(ent, struct obj_mgr_obj, list);
		if (obj_mgr_obj == object) {
			if (obj_mgr_obj->ref_count <= 0)
				ERROR("illegal reference count: %d",
				      obj_mgr_obj->ref_count);
			obj_mgr_obj->ref_count--;
			if (obj_mgr_obj->ref_count == 0) {
				RemoveEntryList(&obj_mgr_obj->list);
				/* FIXME: should we delete the handle too? */
				/* kfree(obj_mgr_obj->handle); */
				kfree(obj_mgr_obj);
			}
			kspin_unlock_irql(&ntoskernel_lock, irql);
			return;
		}
	}
	ERROR("object %p not found", object);
}

STDCALL NTSTATUS WRAP_EXPORT(ZwClose)
	(void *object)
{
	/* FIXME: should we just call ObfDereferenceObject here? Some
	 * drivers use this without calling ZwCreate/Open */
	TRACEEXIT3(return STATUS_SUCCESS);
}

NOREGPARM NTSTATUS WRAP_EXPORT(WmiTraceMessage)
	(void *tracehandle, ULONG message_flags,
	 void *message_guid, USHORT message_no, ...)
{
	TRACEENTER2("%s", "");
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(WmiQueryTraceInformation)
	(enum trace_information_class trace_info_class, void *trace_info,
	 ULONG *req_length, void *buf)
{
	TRACEENTER2("%s", "");
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL unsigned int WRAP_EXPORT(IoWMIRegistrationControl)
	(struct device_object *dev_obj, ULONG action)
{
	TRACEENTER2("%s", "");
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
	printk("DbgPrint: ");
	printk(buf);
	va_end(args);
#endif
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(IoAllocateDriverObjectExtension)
	(struct driver_object *drv_obj, void *client_id, ULONG extlen,
	 void **ext)
{
	struct custom_ext *ce;
	KIRQL irql;

	TRACEENTER2("%p, %p", drv_obj, client_id);
	ce = kmalloc(sizeof(*ce) + extlen, GFP_ATOMIC);
	if (ce == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	TRACEENTER1("custom_ext: %p", ce);
	ce->client_id = client_id;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	InsertTailList(&drv_obj->drv_ext->custom_ext, &ce->list);
	kspin_unlock_irql(&ntoskernel_lock, irql);

	*ext = (void *)ce + sizeof(*ce);
	TRACEENTER1("ext: %p", *ext);
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL void *WRAP_EXPORT(IoGetDriverObjectExtension)
	(struct driver_object *drv_obj, void *client_id)
{
	struct nt_list *head, *ent;
	void *ret;
	KIRQL irql;

	TRACEENTER2("drv_obj: %p, client_id: %p", drv_obj, client_id);
	head = &drv_obj->drv_ext->custom_ext;
	ret = NULL;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	nt_list_for_each(ent, head) {
		struct custom_ext *ce;
		ce = container_of(ent, struct custom_ext, list);
		if (ce->client_id == client_id) {
			ret = (void *)ce + sizeof(*ce);
			break;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);
	DBGTRACE2("ret: %p", ret);
	TRACEEXIT2(return ret);
}

void free_custom_ext(struct driver_extension *drv_ext)
{
	struct nt_list *head, *ent;
	KIRQL irql;

	TRACEENTER2("%p", drv_ext);
	head = &drv_ext->custom_ext;
	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	while ((ent = RemoveHeadList(head)))
		kfree(ent);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	TRACEEXIT2(return);
}


STDCALL NTSTATUS WRAP_EXPORT(IoCreateDevice)
	(struct driver_object *drv_obj, ULONG dev_ext_length,
	 struct unicode_string *dev_name, DEVICE_TYPE dev_type,
	 ULONG dev_chars, BOOLEAN exclusive, struct device_object **newdev)
{
	struct device_object *dev;

	TRACEENTER2("%p", drv_obj);
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		TRACEEXIT2(return STATUS_INSUFFICIENT_RESOURCES);
	memset(dev, 0, sizeof(*dev));
	dev->type = dev_type;
	dev->drv_obj = drv_obj;
	dev->flags = 0;
	if (dev_ext_length) {
		dev->dev_ext = kmalloc(dev_ext_length, GFP_KERNEL);
		if (!dev->dev_ext) {
			kfree(dev);
			TRACEEXIT2(return STATUS_INSUFFICIENT_RESOURCES);
		}
	} else
		dev->dev_ext = NULL;
	dev->size = sizeof(*dev) + dev_ext_length;
	dev->ref_count = 1;
	dev->attached = NULL;
	dev->next = NULL;
	dev->type = dev_type;
	dev->stack_size = 1;
	dev->align_req = 1;
	dev->characteristics = dev_chars;
	dev->io_timer = NULL;
	KeInitializeEvent(&dev->lock, SynchronizationEvent, TRUE);
	dev->vpb = NULL;
	dev->dev_obj_ext = kmalloc(sizeof(*(dev->dev_obj_ext)), GFP_KERNEL);
	if (!dev->dev_obj_ext) {
		if (dev->dev_ext)
			kfree(dev->dev_ext);
		kfree(dev);
		TRACEEXIT2(return STATUS_INSUFFICIENT_RESOURCES);
	}
	dev->dev_obj_ext->type = 0;
	dev->dev_obj_ext->size = sizeof(*dev->dev_obj_ext);
	dev->dev_obj_ext->dev_obj = dev;
	drv_obj->dev_obj = dev;
	if (drv_obj->dev_obj)
		dev->next = drv_obj->dev_obj;
	else
		dev->next = NULL;
	DBGTRACE2("%p", dev);
	*newdev = dev;
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL void WRAP_EXPORT(IoDeleteDevice)
	(struct device_object *dev)
{
	struct device_object *prev;

	TRACEENTER2("%p", dev);
	if (dev == NULL)
		TRACEEXIT2(return);
	if (dev->dev_obj_ext)
		kfree(dev->dev_obj_ext);
	if (dev->dev_ext)
		kfree(dev->dev_ext);
	prev = dev->drv_obj->dev_obj;
	if (prev == dev)
		dev->drv_obj->dev_obj = dev->next;
	else {
		while (prev->next != dev)
			prev = prev->next;
		prev->next = dev->next;
	}
	kfree(dev);
	TRACEEXIT2(return);
}

STDCALL struct device_object *WRAP_EXPORT(IoGetAttachedDevice)
	(struct device_object *dev)
{
	struct device_object *d;

	TRACEENTER2("%p", dev);
	if (!dev)
		TRACEEXIT2(return NULL);
	for (d = dev; d->attached; d = d->attached)
		;

	TRACEEXIT2(return d);
}

STDCALL struct device_object *WRAP_EXPORT(IoAttachDeviceToDeviceStack)
	(struct device_object *src, struct device_object *dst)
{
	struct device_object *attached;

	TRACEENTER2("%p, %p", src, dst);
	attached = IoGetAttachedDevice(dst);
	DBGTRACE3("%p", attached);
	if (attached)
		attached->attached = src;
	src->attached = NULL;
	src->stack_size = attached->stack_size + 1;
	TRACEEXIT2(return attached);
}

STDCALL void WRAP_EXPORT(IoDetachDevice)
	(struct device_object *topdev)
{
	struct device_object *tail;

	TRACEENTER2("%p", topdev);
	tail = topdev->attached;
	if (!tail)
		TRACEEXIT2(return);
	topdev->attached = tail->attached;
	topdev->ref_count--;

	tail = topdev->attached;
	while (tail) {
		tail->stack_size--;
		tail = tail->attached;
	}

	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(KeBugCheckEx)
	(ULONG code, ULONG_PTR param1, ULONG_PTR param2,
	 ULONG_PTR param3, ULONG_PTR param4)
{
	UNIMPL();
	return;
}

STDCALL ULONG WRAP_EXPORT(ExSetTimerResolution)
	(ULONG time, BOOLEAN set)
{
	/* yet another "innovation"! */
	return time;
}

STDCALL void WRAP_EXPORT(DbgBreakPoint)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoReleaseCancelSpinLock)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoCreateSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoCreateUnprotectedSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoDeleteSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(_except_handler3)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(__C_specific_handler)(void){UNIMPL();}


#include "ntoskernel_exports.h"

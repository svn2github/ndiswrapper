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

static wait_queue_head_t dispatch_event_wq;
KSPIN_LOCK dispatch_event_lock;
KSPIN_LOCK irp_cancel_lock;
KSPIN_LOCK ntoskernel_lock;
static kmem_cache_t *mdl_cache;

int ntoskernel_init(void)
{
	kspin_lock_init(&dispatch_event_lock);
	kspin_lock_init(&irp_cancel_lock);
	kspin_lock_init(&ntoskernel_lock);
	init_waitqueue_head(&dispatch_event_wq);
	mdl_cache = kmem_cache_create("ndis_mdl", CACHE_MDL_SIZE, 0, 0,
				      NULL, NULL);
	if (!mdl_cache) {
		ERROR("couldn't allocate MDL cache");
		return -ENOMEM;
	}
	return 0;
}

void ntoskernel_exit(void)
{
	if (mdl_cache && kmem_cache_destroy(mdl_cache))
		ERROR("A Windows driver didn't free all MDL(s);"
		      "memory is leaking");
	return;
}

WRAP_EXPORT_MAP("KeTickCount", &jiffies);

STDCALL void WRAP_EXPORT(KeInitializeTimer)
	(struct ktimer *ktimer)
{
	TRACEENTER4("%p", ktimer);

	wrapper_init_timer(ktimer, NULL);
	ktimer->dispatch_header.signal_state = FALSE;
}

STDCALL void WRAP_EXPORT(KeInitializeDpc)
	(struct kdpc *kdpc, void *func, void *ctx)
{
	TRACEENTER4("%p, %p, %p", kdpc, func, ctx);
	init_dpc(kdpc, func, ctx);
}

STDCALL BOOLEAN WRAP_EXPORT(KeSetTimerEx)
	(struct ktimer *ktimer, LARGE_INTEGER due_time, LONG period,
	 struct kdpc *kdpc)
{
	unsigned long expires;
	unsigned long repeat;

	TRACEENTER4("%p, %ld, %u, %p", ktimer, (long)due_time, period, kdpc);

	if (due_time < 0)
		expires = jiffies + HZ * (-due_time) / TICKSPERSEC;
	else
		expires = HZ * due_time / TICKSPERSEC;
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

STDCALL KIRQL WRAP_EXPORT(KeGetCurrentIrql)
	(void)
{
	return current_irql();
}

STDCALL void WRAP_EXPORT(KeInitializeSpinLock)
	(KSPIN_LOCK *lock)
{
	kspin_lock_init(lock);
}

STDCALL void WRAP_EXPORT(KeAcquireSpinLock)
	(KSPIN_LOCK *lock, KIRQL *irql)
{
	*irql = KfAcquireSpinLock(FASTCALL_ARGS_1(lock));
}

STDCALL void WRAP_EXPORT(KeReleaseSpinLock)
	(KSPIN_LOCK *lock, KIRQL oldirql)
{
	KfReleaseSpinLock(FASTCALL_ARGS_2(lock, oldirql));
}

STDCALL void WRAP_EXPORT(KeAcquireSpinLockAtDpcLevel)
	(KSPIN_LOCK *lock)
{
	KfAcquireSpinLock(FASTCALL_ARGS_1(lock));
}

STDCALL void WRAP_EXPORT(KeLowerIrql)
	(KIRQL irql)
{
	KfLowerIrql(FASTCALL_ARGS_1(irql));
}

STDCALL KIRQL WRAP_EXPORT(KeAcquireSpinLockRaiseToDpc)
        (KSPIN_LOCK *lock)
{
        return KfAcquireSpinLock(FASTCALL_ARGS_1(lock));
}

STDCALL void WRAP_EXPORT(KeReleaseSpinLockFromDpcLevel)
	(KSPIN_LOCK *lock)
{
	KefReleaseSpinLockFromDpcLevel(FASTCALL_ARGS_1(lock));
}

_FASTCALL struct slist_entry *WRAP_EXPORT(ExInterlockedPushEntrySList)
	(FASTCALL_DECL_3(union slist_head *head, struct slist_entry *entry, 
			 KSPIN_LOCK *lock))
{
	struct slist_entry *oldhead;
	KIRQL irql;

	TRACEENTER4("head = %p, entry = %p", head, entry);

	KeAcquireSpinLock(lock, &irql);
	oldhead = head->list.next;
	entry->next = head->list.next;
	head->list.next = entry;
	head->list.depth++;
	KeReleaseSpinLock(lock, irql);
	DBGTRACE4("head = %p, oldhead = %p", head, oldhead);
	return(oldhead);
}

_FASTCALL struct slist_entry *WRAP_EXPORT(ExpInterlockedPushEntrySList)
	(FASTCALL_DECL_3(union slist_head *head, struct slist_entry *entry, 
			 KSPIN_LOCK *lock))
{
	return ExInterlockedPushEntrySList(FASTCALL_ARGS_3(head, entry, lock));
}

_FASTCALL struct slist_entry *WRAP_EXPORT(InterlockedPushEntrySList)
	(FASTCALL_DECL_2(union slist_head *head, struct slist_entry *entry))
{
	return ExInterlockedPushEntrySList(FASTCALL_ARGS_3(head, entry,
							   &ntoskernel_lock));
}

_FASTCALL struct slist_entry * WRAP_EXPORT(ExInterlockedPopEntrySList)
	(FASTCALL_DECL_2(union slist_head *head, KSPIN_LOCK *lock))
{
	struct slist_entry *first;
	KIRQL irql;

	TRACEENTER4("head = %p", head);

	KeAcquireSpinLock(lock, &irql);
	first = NULL;
	if (head) {
		first = head->list.next;
		if (first) {
			head->list.next = first->next;
			head->list.depth--;
		}
	}
	KeReleaseSpinLock(lock, irql);
	DBGTRACE4("returning %p", first);
	return first;
}

_FASTCALL struct slist_entry * WRAP_EXPORT(ExpInterlockedPopEntrySList)
	(FASTCALL_DECL_2(union slist_head *head, KSPIN_LOCK *lock))
{
	return ExInterlockedPopEntrySList(FASTCALL_ARGS_2(head, lock));
}

_FASTCALL struct slist_entry * WRAP_EXPORT(InterlockedPopEntrySList)
	(FASTCALL_DECL_1(union slist_head *head))
{
	return ExInterlockedPopEntrySList(FASTCALL_ARGS_2(head,
							  &ntoskernel_lock));

}

_FASTCALL struct list_entry *WRAP_EXPORT(ExfInterlockedInsertTailList)
	(FASTCALL_DECL_3(struct list_entry *head, struct list_entry *entry, 
			 KSPIN_LOCK *lock))
{
	struct list_entry *oldhead;
	KIRQL irql;

	TRACEENTER4("head = %p", head);

	KeAcquireSpinLock(lock, &irql);
	if (head == NULL)
		oldhead = NULL;
	else
		oldhead = head->bwd_link;

	entry->fwd_link = head;
	entry->bwd_link = head->bwd_link;
	head->bwd_link->fwd_link = entry;
	head->bwd_link = entry;
	KeReleaseSpinLock(lock, irql);
	DBGTRACE4("head = %p, oldhead = %p", head, oldhead);
	return(oldhead);
}

_FASTCALL struct list_entry *WRAP_EXPORT(ExfInterlockedRemoveHeadList)
	(FASTCALL_DECL_2(struct list_entry *head, KSPIN_LOCK *lock))
{
	struct list_entry *entry, *tmp;
	KIRQL irql;

	TRACEENTER4("head = %p", head);

	KeAcquireSpinLock(lock, &irql);
	if (head == NULL)
		TRACEEXIT4(return NULL);
	
	entry = head->fwd_link;
	if (entry == NULL || entry->bwd_link == NULL ||
	    entry->fwd_link == NULL ||
	    entry->bwd_link->fwd_link != entry ||
	    entry->fwd_link->bwd_link != entry) {
		ERROR("illegal list_entry %p", entry);
		TRACEEXIT4(return NULL);
	}

	tmp = entry->bwd_link;
	entry->fwd_link->bwd_link = entry->bwd_link;
	tmp->fwd_link = entry->fwd_link;

	entry->fwd_link = NULL;
	entry->bwd_link = NULL;
	KeReleaseSpinLock(lock, irql);
	DBGTRACE4("head = %p", head);
	TRACEEXIT4(return entry);
}

_FASTCALL USHORT WRAP_EXPORT(ExQueryDepthSList)
	(union slist_head *head)
{
	return head->list.depth;
}

_FASTCALL LONG WRAP_EXPORT(InterlockedDecrement)
	(FASTCALL_DECL_1(LONG volatile *val))
{
	LONG x;

	TRACEENTER4("%s", "");
	kspin_lock(&ntoskernel_lock);
	(*val)--;
	x = *val;
	kspin_unlock(&ntoskernel_lock);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedIncrement)
	(FASTCALL_DECL_1(LONG volatile *val))
{
	LONG x;

	TRACEENTER4("%s", "");
	kspin_lock(&ntoskernel_lock);
	(*val)++;
	x = *val;
	kspin_unlock(&ntoskernel_lock);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedExchange)
	(FASTCALL_DECL_2(LONG volatile *target, LONG val))
{
	LONG x;

	TRACEENTER4("%s", "");
	kspin_lock(&ntoskernel_lock);
	x = *target;
	*target = val;
	kspin_unlock(&ntoskernel_lock);
	TRACEEXIT4(return x);
}

_FASTCALL LONG WRAP_EXPORT(InterlockedCompareExchange)
	(FASTCALL_DECL_3(LONG volatile *dest, LONG xchg, LONG comperand))
{
	LONG x;

	TRACEENTER4("%s", "");
	kspin_lock(&ntoskernel_lock);
	x = *dest;
	if (*dest == comperand)
		*dest = xchg;
	kspin_unlock(&ntoskernel_lock);
	TRACEEXIT4(return x);
}

_FASTCALL void WRAP_EXPORT(ExInterlockedAddLargeStatistic)
	(FASTCALL_DECL_2(LARGE_INTEGER *plint, ULONG n))
{
	unsigned long flags;
	TRACEENTER4("Stat %p = %llu, n = %u", plint, *plint, n);
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
			
	DBGTRACE2("return value = %p", ret);
	return ret;
}

STDCALL void WRAP_EXPORT(ExFreePool)
	(void *p)
{
	TRACEENTER2("%p", p);
	kfree(p);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(ExInitializeNPagedLookasideList)
	(struct npaged_lookaside_list *lookaside,
	 LOOKASIDE_ALLOC_FUNC *alloc_func, LOOKASIDE_FREE_FUNC *free_func,
	 ULONG flags, SIZE_T size, ULONG tag, USHORT depth)
{
	TRACEENTER3("lookaside: %p, size: %lu, flags: %u,"
		    " head: %p, size of lookaside: %lu",
		    lookaside, size, flags, lookaside->head.list.next,
		    (unsigned long)sizeof(struct npaged_lookaside_list));

	memset(lookaside, 0, sizeof(*lookaside));

	lookaside->size = size;
	lookaside->tag = tag;
	lookaside->depth = 4;
	lookaside->maxdepth = 256;

	if (alloc_func)
		lookaside->alloc_func = alloc_func;
	else
		lookaside->alloc_func = ExAllocatePoolWithTag;
	if (free_func)
		lookaside->free_func = free_func;
	else
		lookaside->free_func = ExFreePool;

	KeInitializeSpinLock(&lookaside->obsolete);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(ExDeleteNPagedLookasideList)
	(struct npaged_lookaside_list *lookaside)
{
	struct slist_entry *entry, *p;

	TRACEENTER3("lookaside = %p", lookaside);
	entry = lookaside->head.list.next;
	while (entry) {
		p = entry;
		entry = entry->next;
		lookaside->free_func(p);
	}
	TRACEEXIT4(return);
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

STDCALL int WRAP_EXPORT(IoIsWdmVersionAvailable)
	(UCHAR major, UCHAR minor)
{
	TRACEENTER3("%d, %x", major, minor);
	if (major == 1 &&
	    (minor == 0x30 || // Windows 2003
	     minor == 0x20 || // Windows XP
	     minor == 0x10)) // Windows 2000
		return 1;
	return 0;
}

STDCALL void WRAP_EXPORT(KeInitializeEvent)
	(struct kevent *kevent, enum event_type type, BOOLEAN state)
{
	TRACEENTER3("event = %p, type = %d, state = %d",
		    kevent, type, state);
	kspin_lock(&dispatch_event_lock);
	kevent->header.type = type;
	kevent->header.signal_state = state;
	kspin_unlock(&dispatch_event_lock);
}

STDCALL LONG WRAP_EXPORT(KeSetEvent)
	(struct kevent *kevent, KPRIORITY incr, BOOLEAN wait)
{
	LONG old_state = kevent->header.signal_state;

	TRACEENTER3("event = %p, type = %d, wait = %d",
		    kevent, kevent->header.type, wait);
	if (wait == TRUE)
		WARNING("wait = %d, not yet implemented", wait);

	kspin_lock(&dispatch_event_lock);
	kevent->header.signal_state = TRUE;
	if (kevent->header.type == SynchronizationEvent)
		wake_up_nr(&dispatch_event_wq, 1);
	else
		wake_up_all(&dispatch_event_wq);
	DBGTRACE3("woken up %p", kevent);
	kspin_unlock(&dispatch_event_lock);
	TRACEEXIT3(return old_state);
}

STDCALL void WRAP_EXPORT(KeClearEvent)
	(struct kevent *kevent)
{
	TRACEENTER3("event = %p", kevent);
	kevent->header.signal_state = FALSE;
}

STDCALL LONG WRAP_EXPORT(KeResetEvent)
	(struct kevent *kevent)
{
	LONG old_state;

	TRACEENTER3("event = %p", kevent);

	old_state = kevent->header.signal_state;
	kevent->header.signal_state = FALSE;

	TRACEEXIT3(return old_state);
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForSingleObject)
	(void *object, KWAIT_REASON reason, KPROCESSOR_MODE waitmode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout)
{
	struct kevent *kevent = (struct kevent *)object;
	struct dispatch_header *header = &kevent->header;
	int res;
	long wait_jiffies;

	/* Note: for now, object can only point to an event */
	TRACEENTER2("event = %p, reason = %u, waitmode = %u, alertable = %u,"
		" timeout = %p", kevent, reason, waitmode, alertable,
		timeout);

	DBGTRACE2("object type = %d, size = %d", header->type, header->size);

	kspin_lock(&dispatch_event_lock);
	if (header->signal_state == TRUE) {
		if (header->type == SynchronizationEvent)
			header->signal_state = FALSE;
		kspin_unlock(&dispatch_event_lock);
 		TRACEEXIT3(return STATUS_SUCCESS);
	}
	kspin_unlock(&dispatch_event_lock);

	if (timeout) {
		DBGTRACE2("timeout = %Ld", *timeout);
		if (*timeout == 0)
			TRACEEXIT2(return STATUS_TIMEOUT);
		else if (*timeout > 0) {
			long d = (*timeout) - ticks_1601();
			/* many drivers call this function with much
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

	if (wait_jiffies == 0) {
		if (alertable)
			res = wait_event_interruptible(
				dispatch_event_wq,
				(header->signal_state == TRUE));
		else {
			wait_event(dispatch_event_wq,
				   (header->signal_state == TRUE));
			res = 1;
		}
	} else {
		if (alertable)
			res = wait_event_interruptible_timeout(
				dispatch_event_wq,
				(header->signal_state == TRUE), wait_jiffies);
		else
			res = wait_event_timeout(
				dispatch_event_wq,
				(header->signal_state == TRUE), wait_jiffies);
	}

	DBGTRACE3("%p, type = %d woke up (%d), res = %d",
		  kevent, header->type, header->signal_state, res);
	if (res < 0)
		TRACEEXIT2(return STATUS_ALERTED);

	if (res == 0)
		TRACEEXIT2(return STATUS_TIMEOUT);

	/* res > 0 */
	if (header->type == SynchronizationEvent)
		header->signal_state = FALSE;
	TRACEEXIT2(return STATUS_SUCCESS);
}

STDCALL NTSTATUS WRAP_EXPORT(KeWaitForMultipleObjects)
	(ULONG count, void *object[], enum wait_type wait_type,
	 KWAIT_REASON wait_reason, KPROCESSOR_MODE waitmode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout,
	 struct wait_block *wait_block)
{
	UNIMPL();
	return STATUS_SUCCESS;
}

STDCALL NTSTATUS WRAP_EXPORT(KeDelayExecutionThread)
	(KPROCESSOR_MODE wait_mode, BOOLEAN alertable,
	 LARGE_INTEGER *interval)
{
	int res;
	long timeout;
	long t = *interval;

	TRACEENTER3("thread: %p", get_current());
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

	if (res > 0)
		TRACEEXIT3(return STATUS_ALERTED);
	else
		TRACEEXIT3(return STATUS_SUCCESS);
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
	TRACEEXIT4(return jiffies);
}

STDCALL ULONG WRAP_EXPORT(KeQueryTimeIncrement)
	(void)
{
	TRACEEXIT5(return TICKSPERSEC/HZ);
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

STDCALL void WRAP_EXPORT(KeInitializeMutex)
	(struct kmutex *mutex, BOOLEAN wait)
{
	INIT_LIST_HEAD(&mutex->dispatch_header.wait_list_head);
	mutex->abandoned = FALSE;
	mutex->apc_disable = 1;
	mutex->dispatch_header.signal_state = TRUE;
	mutex->dispatch_header.type = SynchronizationEvent;
	mutex->dispatch_header.size = NT_OBJ_MUTEX;
	mutex->u.count = 0;
	mutex->owner_thread = NULL;
	return;
}

STDCALL LONG WRAP_EXPORT(KeReleaseMutex)
	(struct kmutex *mutex, BOOLEAN wait)
{
	kspin_lock(&dispatch_event_lock);
	mutex->u.count--;
	if (mutex->u.count == 0) {
		mutex->owner_thread = NULL;
		kspin_unlock(&dispatch_event_lock);
		KeSetEvent((struct kevent *)&mutex->dispatch_header, 0, 0);
	} else
		kspin_unlock(&dispatch_event_lock);
	return mutex->u.count;
}

STDCALL void * WRAP_EXPORT(KeGetCurrentThread)
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

STDCALL NTSTATUS WRAP_EXPORT(IoGetDeviceProperty)
	(struct device_object *dev_obj,
	 enum device_registry_property dev_property,
	 ULONG buffer_len, void *buffer, ULONG *result_len)
{
	struct ansi_string ansi;
	struct unicode_string unicode;
	struct ndis_handle *handle;
	char buf[32];

	handle = (struct ndis_handle *)dev_obj->handle;

	TRACEENTER1("dev_obj = %p, dev_property = %d, buffer_len = %u, "
		"buffer = %p, result_len = %p", dev_obj, dev_property,
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
					    handle->dev.usb->devnum);
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
					    handle->dev.usb->devnum);
			*result_len = 2 * (ansi.len + 1);
			TRACEEXIT1(return STATUS_BUFFER_TOO_SMALL);
		}
		break;

	case DevicePropertyDriverKeyName:
//		ansi.buf = handle->driver->name;
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
		USBTRACEEXIT(return 1);
	} else {
		kspin_unlock(&irp_cancel_lock);
		USBTRACEEXIT(return 0);
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

		if (stack->completion_handler(stack->dev_obj, irp,
		                              stack->handler_arg) ==
		    STATUS_MORE_PROCESSING_REQUIRED)
			USBTRACEEXIT(return);
	}

	if (irp->user_event) {
		USBTRACE("setting event %p", irp->user_event);
		KeSetEvent(irp->user_event, 0, 0);
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
		UNIMPL();
		ret = STATUS_SUCCESS;
	}
	else
		ERROR("major_fn %08X NOT IMPLEMENTED!\n", stack->major_fn);

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
			KeSetEvent(irp->user_event, 0, 0);
		}
	}

	/* To-Do: what about IRP_DEALLOCATE_BUFFER...? */
	USBTRACE("freeing irp %p", irp);
	kfree(irp);

	USBTRACEEXIT(return ret);
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
	TRACEENTER2("status = %u", status);
	complete_and_exit(NULL, status);
	return 0;
}

STDCALL void WRAP_EXPORT(PoStartNextPowerIrp)
	(struct irp *irp)
{
	TRACEENTER5("irp = %p", irp);
	TRACEEXIT5(return);
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

STDCALL ULONG WRAP_EXPORT(MmSizeOfMdl)
	(void *base, ULONG length)
{
	return (sizeof(struct mdl) +
		SPAN_PAGES((ULONG_PTR)base, length) * sizeof(ULONG));
}

struct mdl *allocate_init_mdl(void *virt, ULONG length)
{
	struct mdl *mdl;
	int mdl_size = MmSizeOfMdl(virt, length);

	if (mdl_size <= CACHE_MDL_SIZE) {
		mdl = kmem_cache_alloc(mdl_cache, GFP_ATOMIC);
		if (!mdl)
			return NULL;
		memset(mdl, 0, CACHE_MDL_SIZE);
		MmInitializeMdl(mdl, virt, length);
		/* mark the MDL as allocated from cache pool so when
		 * it is freed, we free it back to the pool */
		mdl->flags = MDL_CACHE_ALLOCATED;
	} else {
		mdl = kmalloc(mdl_size, GFP_ATOMIC);
		if (!mdl)
			return NULL;
		memset(mdl, 0, mdl_size);
		MmInitializeMdl(mdl, virt, length);
	}
	return mdl;
}

void free_mdl(struct mdl *mdl)
{
	/* A driver may allocate Mdl with NdisAllocateBuffer and free
	 * with IoFreeMdl (e.g., 64-bit Broadcom). Since we need to
	 * treat buffers allocated with Ndis calls differently, we
	 * must call NdisFreeBuffer if it is allocated with Ndis
	 * function. We set 'process' field in Ndis functions. */
	if (mdl) {
		if (mdl->process)
			NdisFreeBuffer(mdl);
		else if (mdl->flags & MDL_CACHE_ALLOCATED)
			kmem_cache_free(mdl_cache, mdl);
		else
			kfree(mdl);
	}
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

_FASTCALL void WRAP_EXPORT(ObfDereferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	struct object_header *header;
	LONG ref_count;
	BOOLEAN permanent;
	ULONG handle_count;

	if (!object)
		TRACEEXIT3(return);

	header = container_of(&((struct common_body_header *)object)->type,
			      struct object_header, type);
	permanent = header->permanent;
	handle_count = header->handle_count;
	ref_count = InterlockedDecrement(FASTCALL_ARGS_1(&header->ref_count));
	if (ref_count < 0 || permanent)
		TRACEEXIT3(return);
	/* since we didn't allocate it, we don't free it */
	/*
	  if (ref_count == 0 && handle_count == 0)
		kfree(header);
	*/
	TRACEEXIT3(return);
}

_FASTCALL void WRAP_EXPORT(ObDereferenceObject)
	(FASTCALL_DECL_1(void *object))
{
	ObfDereferenceObject(FASTCALL_ARGS_1(object));
	TRACEEXIT3(return);
}

STDCALL NTSTATUS WRAP_EXPORT(ZwClose)
	(void *object)
{
	ObfDereferenceObject(FASTCALL_ARGS_1(object));
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

STDCALL void WRAP_EXPORT(KeBugCheckEx)
	(ULONG code, ULONG_PTR param1, ULONG_PTR param2,
	 ULONG_PTR param3, ULONG_PTR param4)
{
	UNIMPL();
	return;
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

STDCALL void WRAP_EXPORT(DbgBreakPoint)
	(void)
{
	UNIMPL();
}

STDCALL void WRAP_EXPORT(IoReleaseCancelSpinLock)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoDeleteDevice)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoCreateSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoCreateUnprotectedSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoCreateDevice)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(IoDeleteSymbolicLink)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(ObfReferenceObject)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(ObReferenceObjectByHandle)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(_except_handler3)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(__C_specific_handler)(void){UNIMPL();}

#include "ntoskernel_exports.h"

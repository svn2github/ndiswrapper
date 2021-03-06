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

#ifndef _NTOSKERNEL_H_
#define _NTOSKERNEL_H_

#define UTILS_VERSION "1.6"

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/time.h>

#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/ctype.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <asm/mman.h>
#include <linux/version.h>
#include <linux/etherdevice.h>
#include <net/iw_handler.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>

#include "winnt_types.h"
#include "ndiswrapper.h"
#include "pe_linker.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,7)
#include <linux/kthread.h>
#endif

#if defined(DISABLE_USB)
#undef CONFIG_USB
#undef CONFIG_USB_MODULE
#endif

#if !defined(CONFIG_USB) && defined(CONFIG_USB_MODULE)
#define CONFIG_USB 1
#endif

#define addr_offset(drvr) (__builtin_return_address(0) - \
			     (drvr)->drv_obj->driver_start)

/* Workqueue / task queue backwards compatibility stuff */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,41)
#include <linux/workqueue.h>
/* pci functions in 2.6 kernels have problems allocating dma buffers,
 * but seem to work fine with dma functions
 */
typedef struct workqueue_struct *workqueue;
#include <asm/dma-mapping.h>

#define PCI_DMA_ALLOC_COHERENT(pci_dev,size,dma_handle) \
	dma_alloc_coherent(&pci_dev->dev,size,dma_handle, \
			   GFP_KERNEL | __GFP_REPEAT | GFP_DMA)
#define PCI_DMA_FREE_COHERENT(pci_dev,size,cpu_addr,dma_handle) \
	dma_free_coherent(&pci_dev->dev,size,cpu_addr,dma_handle)
#define PCI_DMA_MAP_SINGLE(pci_dev,addr,size,direction) \
	dma_map_single(&pci_dev->dev,addr,size,direction)
#define PCI_DMA_UNMAP_SINGLE(pci_dev,dma_handle,size,direction) \
	dma_unmap_single(&pci_dev->dev,dma_handle,size,direction)
#define MAP_SG(pci_dev, sglist, nents, direction) \
	dma_map_sg(&pci_dev->dev, sglist, nents, direction)
#define UNMAP_SG(pci_dev, sglist, nents, direction) \
	dma_unmap_sg(&pci_dev->dev, sglist, nents, direction)

#else // linux version <= 2.5.41

#define PCI_DMA_ALLOC_COHERENT(dev,size,dma_handle) \
	pci_alloc_consistent(dev,size,dma_handle)
#define PCI_DMA_FREE_COHERENT(dev,size,cpu_addr,dma_handle) \
	pci_free_consistent(dev,size,cpu_addr,dma_handle)
#define PCI_DMA_MAP_SINGLE(dev,addr,size,direction) \
	pci_map_single(dev,addr,size,direction)
#define PCI_DMA_UNMAP_SINGLE(dev,dma_handle,size,direction) \
	pci_unmap_single(dev,dma_handle,size,direction)
#define MAP_SG(dev, sglist, nents, direction) \
	pci_map_sg(dev, sglist, nents, direction)
#define UNMAP_SG(dev, sglist, nents, direction) \
	pci_unmap_sg(dev, sglist, nents, direction)
#include <linux/tqueue.h>
#define work_struct tq_struct
#define INIT_WORK INIT_TQUEUE
#define DECLARE_WORK(n, f, d) struct tq_struct n = { \
		list: LIST_HEAD_INIT(n.list),	     \
		sync: 0,			     \
		routine: f,			     \
		data: d				     \
}
#define schedule_work schedule_task
#define flush_scheduled_work flush_scheduled_tasks
typedef task_queue workqueue;
#include <linux/smp_lock.h>

/* RedHat kernels #define irqs_disabled this way */
#ifndef irqs_disabled
#define irqs_disabled()                \
({                                     \
	unsigned long flags;	       \
       __save_flags(flags);            \
       !(flags & (1<<9));              \
})
#endif

#ifndef in_atomic
#ifdef CONFIG_PREEMPT
#define in_atomic() ((preempt_get_count() & ~PREEMPT_ACTIVE) != kernel_locked())
#else
#define in_atomic() (in_interrupt())
#endif // CONFIG_PREEMPT
#endif // in_atomic

#define __GFP_NOWARN 0

#endif // LINUX_VERSION_CODE

#ifndef offset_in_page
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
#include <linux/scatterlist.h>
#else
#define sg_init_one(sg, addr, len) do {				 \
		(sg)->page = virt_to_page(addr);		 \
		(sg)->offset = offset_in_page(addr);		 \
		(sg)->length = len;				 \
	} while (0)
#endif // KERNEL_VERSION(2,6,9)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,23)
#define HAVE_ETHTOOL 1
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

#ifndef preempt_enable
#define preempt_enable()  do { } while (0)
#endif
#ifndef preempt_disable
#define preempt_disable() do { } while (0)
#endif

#ifndef preempt_enable_no_resched
#define preempt_enable_no_resched() preempt_enable()
#endif

#ifndef container_of
#define container_of(ptr, type, member)					\
({									\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);		\
	(type *)( (char *)__mptr - offsetof(type,member) );		\
})
#endif

#ifndef virt_addr_valid
#define virt_addr_valid(addr) VALID_PAGE(virt_to_page(addr))
#endif

#ifndef SET_NETDEV_DEV
#define SET_NETDEV_DEV(net,pdev) do { } while (0)
#endif

#endif // LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)

#ifndef PMSG_SUSPEND
#ifdef PM_SUSPEND
/* this is not correct - the value of PM_SUSPEND is different from
 * PMSG_SUSPEND, but ndiswrapper doesn't care about the value when
 * suspending */
#define PMSG_SUSPEND PM_SUSPEND
#define PSMG_ON PM_ON
#else
typedef u32 pm_message_t;
#define PMSG_SUSPEND 3
#define PMSG_ON 0
#endif
#endif

#ifndef PCI_D0
#define PCI_D0 0
#define PCI_D3hot 3
#endif

#ifndef PM_EVENT_SUSPEND
#define PM_EVENT_SUSPEND 2
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)
#define pci_choose_state(dev, state) (state)
#endif

#if defined(CONFIG_SOFTWARE_SUSPEND2) || defined(CONFIG_SUSPEND2)
#define KTHREAD_RUN(a,b,c) kthread_run(a,b,0,c)
#else
#define KTHREAD_RUN(a,b,c) kthread_run(a,b,c)
#endif

#if !defined(HAVE_NETDEV_PRIV)
#define netdev_priv(dev)  ((dev)->priv)
#endif

#ifdef CONFIG_X86_64
#define LIN2WIN1(func, arg1)			\
	lin_to_win1(func, (unsigned long)arg1)
#define LIN2WIN2(func, arg1, arg2)					\
	lin_to_win2(func, (unsigned long)arg1, (unsigned long)arg2)
#define LIN2WIN3(func, arg1, arg2, arg3)				\
	lin_to_win3(func, (unsigned long)arg1, (unsigned long)arg2,	\
		    (unsigned long)arg3)
#define LIN2WIN4(func, arg1, arg2, arg3, arg4)				\
	lin_to_win4(func, (unsigned long)arg1, (unsigned long)arg2,	\
		    (unsigned long)arg3, (unsigned long)arg4)
#define LIN2WIN5(func, arg1, arg2, arg3, arg4, arg5)			\
	lin_to_win5(func, (unsigned long)arg1, (unsigned long)arg2,	\
		    (unsigned long)arg3, (unsigned long)arg4,		\
		    (unsigned long)arg5)
#define LIN2WIN6(func, arg1, arg2, arg3, arg4, arg5, arg6)		\
	lin_to_win6(func, (unsigned long)arg1, (unsigned long)arg2,	\
		    (unsigned long)arg3, (unsigned long)arg4,		\
		    (unsigned long)arg5, (unsigned long)arg6)
#else
#define LIN2WIN1(func, arg1) func(arg1)
#define LIN2WIN2(func, arg1, arg2) func(arg1, arg2)
#define LIN2WIN3(func, arg1, arg2, arg3) func(arg1, arg2, arg3)
#define LIN2WIN4(func, arg1, arg2, arg3, arg4) func(arg1, arg2, arg3, arg4)
#define LIN2WIN5(func, arg1, arg2, arg3, arg4, arg5)	\
	func(arg1, arg2, arg3, arg4, arg5)
#define LIN2WIN6(func, arg1, arg2, arg3, arg4, arg5, arg6)	\
	func(arg1, arg2, arg3, arg4, arg5, arg6)
#endif

#ifndef __wait_event_interruptible_timeout
#define __wait_event_interruptible_timeout(wq, condition, ret)		\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (condition)						\
			break;						\
		if (!signal_pending(current)) {				\
			ret = schedule_timeout(ret);			\
			if (!ret)					\
				break;					\
			continue;					\
		}							\
		ret = -ERESTARTSYS;					\
		break;							\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)
#endif

#ifndef wait_event_interruptible_timeout
#define wait_event_interruptible_timeout(wq, condition, timeout)	\
({									\
	long __ret = timeout;						\
	if (!(condition))						\
		__wait_event_interruptible_timeout(wq, condition, __ret); \
	__ret;								\
})
#endif

#ifndef __wait_event_timeout
#define __wait_event_timeout(wq, condition, ret)			\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_UNINTERRUPTIBLE);		\
		if (condition)						\
			break;						\
		ret = schedule_timeout(ret);				\
		if (!ret)						\
			break;						\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)
#endif

#ifndef wait_event_timeout
#define wait_event_timeout(wq, condition, timeout)			\
({									\
	long __ret = timeout;						\
	if (!(condition))						\
		__wait_event_timeout(wq, condition, __ret);		\
	 __ret;								\
})
#endif

/* Interrupt backwards compatibility stuff */
#include <linux/interrupt.h>
#ifndef IRQ_HANDLED
#define IRQ_HANDLED
#define IRQ_NONE
#define irqreturn_t void
#endif

#ifndef free_netdev
#define free_netdev kfree
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
#define WRAP_MODULE_PARM_INT(name, perm) module_param(name, int, perm)
#define WRAP_MODULE_PARM_STRING(name, perm) module_param(name, charp, perm)
#else
#define WRAP_MODULE_PARM_INT(name, perm) MODULE_PARM(name, "i")
#define WRAP_MODULE_PARM_STRING(name, perm) MODULE_PARM(name, "s")
#endif

/* this ugly hack is to handle RH kernels; I don't know any better,
 * but this has to be fixed soon */
#ifndef rt_task
#define rt_task(p) ((p)->prio < MAX_RT_PRIO)
#endif

#define KMALLOC_THRESHOLD 131072

/* TICK is 100ns */
#define TICKSPERSEC		10000000LL
#define TICKSPERMSEC		10000
#define SECSPERDAY		86400
#define SECSPERHOUR		3600
#define SECSPERMIN		60
#define DAYSPERWEEK		7

/* 1601 to 1970 is 369 years plus 89 leap days */
#define SECS_1601_TO_1970	((369 * 365 + 89) * (u64)SECSPERDAY)
#define TICKS_1601_TO_1970	(SECS_1601_TO_1970 * TICKSPERSEC)

/* 100ns units to HZ; if sys_time is negative, relative to current
 * clock, otherwise from year 1601 */
#define SYSTEM_TIME_TO_HZ(sys_time)					\
	((((sys_time) <= 0) ? (((u64)HZ * (-(sys_time))) / TICKSPERSEC) : \
	  (((u64)HZ * ((sys_time) - ticks_1601())) / TICKSPERSEC)))

#define MSEC_TO_HZ(ms) ((ms) * HZ / 1000)
#define USEC_TO_HZ(ms) ((us) * HZ / 1000000)

extern u64 wrap_ticks_to_boot;

static inline u64 ticks_1601(void)
{
	return wrap_ticks_to_boot + (u64)jiffies * TICKSPERSEC / HZ;
}

typedef void (*WRAP_EXPORT_FUNC)(void);

struct wrap_export {
	const char *name;
	WRAP_EXPORT_FUNC func;
};

#ifdef CONFIG_X86_64
#define WRAP_EXPORT_SYMBOL(f) {#f, (WRAP_EXPORT_FUNC)x86_64_ ## f}
#define WRAP_EXPORT_WIN_FUNC(f) {#f, (WRAP_EXPORT_FUNC)x86_64__win_ ## f}
#define WRAP_FUNC_PTR(f) &x86_64_ ## f
#define WRAP_FUNC_PTR_DECL(f) void x86_64_ ## f(void);
#else
#define WRAP_EXPORT_SYMBOL(f) {#f, (WRAP_EXPORT_FUNC)f}
#define WRAP_EXPORT_WIN_FUNC(f) {#f, (WRAP_EXPORT_FUNC)_win_ ## f}
#define WRAP_FUNC_PTR(f) &f
#define WRAP_FUNC_PTR_DECL(f)
#endif
/* map name s to function f - if f is different from s */
#define WRAP_EXPORT_MAP(s,f)
#define WRAP_EXPORT(x) x

#define POOL_TAG(A, B, C, D)					\
	((ULONG)((A) + ((B) << 8) + ((C) << 16) + ((D) << 24)))

struct wrap_alloc {
	struct nt_list list;
	void *ptr;
};

struct pe_image {
	char name[MAX_DRIVER_NAME_LEN];
	void *entry;
	void *image;
	int size;
	int type;

	IMAGE_NT_HEADERS *nt_hdr;
	IMAGE_OPTIONAL_HEADER *opt_hdr;
};

extern KSPIN_LOCK atomic_lock;
extern KSPIN_LOCK cancel_lock;

//#define DEBUG_IRQL 1

struct wrap_timer {
	long repeat;
	struct nt_list list;
	struct timer_list timer;
	struct nt_timer *nt_timer;
#ifdef DEBUG_TIMER
	unsigned long wrap_timer_magic;
#endif
};

struct wrap_work_item {
	struct nt_list list;
	void *arg1;
	void *arg2;
	void *func;
	BOOLEAN win_func;
};

#define MAX_ALLOCATED_URBS 15

struct wrap_device_setting {
	struct nt_list list;
	char name[MAX_SETTING_NAME_LEN];
	char value[MAX_SETTING_VALUE_LEN];
	void *encoded;
};

struct wrap_bin_file {
	char name[MAX_SETTING_NAME_LEN];
	int size;
	void *data;
};

#define CE_WRAP_DRIVER_CLIENT_ID 1

struct wrap_driver {
	struct nt_list list;
	struct driver_object *drv_obj;
	char name[MAX_DRIVER_NAME_LEN];
	char version[MAX_SETTING_VALUE_LEN];
	unsigned int num_pe_images;
	struct pe_image pe_images[MAX_DRIVER_PE_IMAGES];
	int num_bin_files;
	struct wrap_bin_file *bin_files;
	struct wrap_device_setting *settings;
	union {
		struct wrap_ndis_driver *ndis_driver;
	};
};

enum hw_status {
	HW_NORMAL, HW_SUSPENDED, HW_HALTED, HW_RMMOD, HW_AVAILABLE,
	HW_INITIALIZED,
};

struct wrap_device {
	/* first part is (de)initialized once by loader */
	int dev_bus_type;
	int vendor;
	int device;
	int subvendor;
	int subdevice;
	struct wrap_driver *driver;
	/* we need driver_name before driver is loaded */
	char driver_name[MAX_DRIVER_NAME_LEN];
	char conf_file_name[MAX_DRIVER_NAME_LEN];
	struct nt_list settings;

	/* rest should be (de)initialized during every
	 * (de)initialization */
	struct device_object *pdo;
	union {
		struct {
			struct pci_dev *pdev;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,9)
			u32 pci_state[16];
#endif
		} pci;
		struct {
			struct usb_device *udev;
			struct usb_interface *intf;
			int num_alloc_urbs;
			struct nt_list wrap_urb_list;
		} usb;
	};
	unsigned long hw_status;
	union {
		struct wrap_ndis_device *wnd;
	};
	struct nt_list timer_list;
	KSPIN_LOCK timer_lock;
	struct cm_resource_list *resource_list;
};

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,0)
/* until issues with threads hogging cpu are resolved, we don't want
 * to use shared workqueue, lest the threads take keyboard etc down */
#define USE_OWN_WORKQUEUE 1
extern struct workqueue_struct *wrapper_wq;
#define schedule_work(work_struct) queue_work(wrapper_wq, (work_struct))
#endif

int ntoskernel_init(void);
void ntoskernel_exit(void);
int ntoskernel_init_device(struct wrap_device *wd);
void ntoskernel_exit_device(struct wrap_device *wd);
void *allocate_object(ULONG size, enum common_object_type type,
		      struct unicode_string *name);
void  free_object(void *object);

int usb_init(void);
void usb_exit(void);
int usb_init_device(struct wrap_device *wd);
void usb_exit_device(struct wrap_device *wd);
void usb_cancel_pending_urbs(void);

int misc_funcs_init(void);
void misc_funcs_exit(void);

int wrap_procfs_init(void);
void wrap_procfs_remove(void);

int stricmp(const char *s1, const char *s2);
void dump_bytes(const char *name, const u8 *from, int len);

struct driver_object *find_bus_driver(const char *name);

STDCALL void WRITE_PORT_UCHAR(ULONG_PTR port, UCHAR value);
STDCALL UCHAR READ_PORT_UCHAR(ULONG_PTR port);

STDCALL void *ExAllocatePoolWithTag(enum pool_type pool_type, SIZE_T size,
				    ULONG tag);
STDCALL void ExFreePool(void *p);
STDCALL ULONG MmSizeOfMdl(void *base, ULONG length);
STDCALL void *MmMapIoSpace(PHYSICAL_ADDRESS phys_addr, SIZE_T size,
			   enum memory_caching_type cache);
STDCALL void MmUnmapIoSpace(void *addr, SIZE_T size);
STDCALL void KeInitializeEvent(struct nt_event *nt_event,
			       enum event_type type, BOOLEAN state);
STDCALL LONG KeSetEvent(struct nt_event *nt_event, KPRIORITY incr,
			BOOLEAN wait);
STDCALL LONG KeResetEvent(struct nt_event *nt_event);
STDCALL void KeClearEvent(struct nt_event *nt_event);
STDCALL void KeInitializeDpc(struct kdpc *kdpc, void *func, void *ctx);
STDCALL BOOLEAN KeInsertQueueDpc(struct kdpc *kdpc, void *arg1, void *arg2);
STDCALL BOOLEAN KeRemoveQueueDpc(struct kdpc *kdpc);
STDCALL void KeFlushQueuedDpcs(void);
STDCALL NTSTATUS KeWaitForSingleObject(void *object, KWAIT_REASON reason,
				       KPROCESSOR_MODE waitmode,
				       BOOLEAN alertable,
				       LARGE_INTEGER *timeout);
struct mdl *allocate_init_mdl(void *virt, ULONG length);
void free_mdl(struct mdl *mdl);
STDCALL struct mdl *IoAllocateMdl(void *virt, ULONG length, BOOLEAN second_buf,
				  BOOLEAN charge_quota, struct irp *irp);
STDCALL void MmBuildMdlForNonPagedPool(struct mdl *mdl);
STDCALL void IoFreeMdl(struct mdl *mdl);
_FASTCALL LONG InterlockedDecrement(FASTCALL_DECL_1(LONG volatile *val));
_FASTCALL LONG InterlockedIncrement(FASTCALL_DECL_1(LONG volatile *val));
_FASTCALL struct nt_list *
ExInterlockedInsertHeadList(FASTCALL_DECL_3(struct nt_list *head,
					    struct nt_list *entry,
					    KSPIN_LOCK *lock));
_FASTCALL struct nt_list *
ExInterlockedInsertTailList(FASTCALL_DECL_3(struct nt_list *head,
					    struct nt_list *entry,
					    KSPIN_LOCK *lock));
_FASTCALL struct nt_list *
ExInterlockedRemoveHeadList(FASTCALL_DECL_2(struct nt_list *head,
					    KSPIN_LOCK *lock));
STDCALL NTSTATUS IoCreateDevice(struct driver_object *driver,
				ULONG dev_ext_length,
				struct unicode_string *dev_name,
				DEVICE_TYPE dev_type,
				ULONG dev_chars, BOOLEAN exclusive,
				struct device_object **dev_obj);
STDCALL NTSTATUS IoCreateSymbolicLink(struct unicode_string *link,
				      struct unicode_string *dev_name);
STDCALL void IoDeleteDevice(struct device_object *dev);
STDCALL void IoDetachDevice(struct device_object *topdev);
STDCALL struct device_object *IoGetAttachedDevice(struct device_object *dev);
STDCALL NTSTATUS
IoAllocateDriverObjectExtension(struct driver_object *drv_obj,
				void *client_id, ULONG extlen, void **ext);
STDCALL void *IoGetDriverObjectExtension(struct driver_object *drv,
					 void *client_id);
STDCALL struct device_object *IoAttachDeviceToDeviceStack
	(struct device_object *src, struct device_object *dst);
STDCALL void KeInitializeEvent(struct nt_event *nt_event, enum event_type type,
			       BOOLEAN state);
void free_custom_extensions(struct driver_extension *drv_obj_ext);

STDCALL struct irp *IoAllocateIrp(char stack_size, BOOLEAN charge_quota);
STDCALL void IoFreeIrp(struct irp *irp);
STDCALL BOOLEAN IoCancelIrp(struct irp *irp);
_FASTCALL NTSTATUS IofCallDriver
	(FASTCALL_DECL_2(struct device_object *dev_obj, struct irp *irp));
STDCALL struct irp *WRAP_EXPORT(IoBuildSynchronousFsdRequest)
	(ULONG major_func, struct device_object *dev_obj, void *buf,
	 ULONG length, LARGE_INTEGER *offset, struct nt_event *event,
	 struct io_status_block *status);
STDCALL struct irp *WRAP_EXPORT(IoBuildAsynchronousFsdRequest)
	(ULONG major_func, struct device_object *dev_obj, void *buf,
	 ULONG length, LARGE_INTEGER *offset,
	 struct io_status_block *status);
STDCALL NTSTATUS PoCallDriver(struct device_object *dev_obj, struct irp *irp);

struct nt_thread *wrap_create_thread(struct task_struct *task);
void wrap_remove_thread(struct nt_thread *thread);
u64 ticks_1601(void);

int schedule_wrap_work_item(WRAP_WORK_FUNC func, void *arg1, void *arg2,
			    BOOLEAN win_func);

STDCALL KIRQL KeGetCurrentIrql(void);
STDCALL void KeInitializeSpinLock(KSPIN_LOCK *lock);
STDCALL void KeAcquireSpinLock(KSPIN_LOCK *lock, KIRQL *irql);
STDCALL void KeReleaseSpinLock(KSPIN_LOCK *lock, KIRQL oldirql);
STDCALL KIRQL KeAcquireSpinLockRaiseToDpc(KSPIN_LOCK *lock);

STDCALL void IoAcquireCancelSpinLock(KIRQL *irql);
STDCALL void IoReleaseCancelSpinLock(KIRQL irql);

_FASTCALL KIRQL KfRaiseIrql(FASTCALL_DECL_1(KIRQL newirql));
_FASTCALL void KfLowerIrql(FASTCALL_DECL_1(KIRQL oldirql));
_FASTCALL KIRQL KfAcquireSpinLock(FASTCALL_DECL_1(KSPIN_LOCK *lock));
_FASTCALL void
KfReleaseSpinLock(FASTCALL_DECL_2(KSPIN_LOCK *lock, KIRQL oldirql));
_FASTCALL void
IofCompleteRequest(FASTCALL_DECL_2(struct irp *irp, CHAR prio_boost));
_FASTCALL void
KefReleaseSpinLockFromDpcLevel(FASTCALL_DECL_1(KSPIN_LOCK *lock));
STDCALL void RtlCopyMemory(void *dst, const void *src, SIZE_T length);
STDCALL NTSTATUS RtlUnicodeStringToAnsiString(struct ansi_string *dst,
					      const struct unicode_string *src,
					      BOOLEAN dup);
STDCALL NTSTATUS RtlAnsiStringToUnicodeString(struct unicode_string *dst,
					       const struct ansi_string *src,
					       BOOLEAN dup);
STDCALL void RtlInitAnsiString(struct ansi_string *dst, const char *src);
STDCALL void RtlInitString(struct ansi_string *dst, const char *src);
STDCALL void RtlInitUnicodeString(struct unicode_string *dest,
				  const wchar_t *src);
STDCALL void RtlFreeUnicodeString(struct unicode_string *string);
STDCALL void RtlFreeAnsiString(struct ansi_string *string);
STDCALL LONG RtlCompareUnicodeString
	(const struct unicode_string *s1, const struct unicode_string *s2,
	 BOOLEAN case_insensitive);
STDCALL void RtlCopyUnicodeString
	(struct unicode_string *dst, struct unicode_string *src);
NOREGPARM SIZE_T _win_wcslen(const wchar_t *s);

void *wrap_kmalloc(size_t size);
void wrap_kfree(void *ptr);
void wrap_init_timer(struct nt_timer *nt_timer, enum timer_type type,
		     struct wrap_device *wd);
BOOLEAN wrap_set_timer(struct nt_timer *nt_timer, unsigned long expires_hz,
		       unsigned long repeat_hz, struct kdpc *kdpc);

STDCALL void KeInitializeTimer(struct nt_timer *nt_timer);
STDCALL void KeInitializeTimerEx(struct nt_timer *nt_timer,
				 enum timer_type type);
STDCALL BOOLEAN KeSetTimerEx(struct nt_timer *nt_timer,
			     LARGE_INTEGER duetime_ticks, LONG period_ms,
			     struct kdpc *kdpc);
STDCALL BOOLEAN KeSetTimer(struct nt_timer *nt_timer,
			   LARGE_INTEGER duetime_ticks, struct kdpc *kdpc);
STDCALL BOOLEAN KeCancelTimer(struct nt_timer *nt_timer);
STDCALL void KeInitializeDpc(struct kdpc *kdpc, void *func, void *ctx);

unsigned long lin_to_win1(void *func, unsigned long);
unsigned long lin_to_win2(void *func, unsigned long, unsigned long);
unsigned long lin_to_win3(void *func, unsigned long, unsigned long,
			  unsigned long);
unsigned long lin_to_win4(void *func, unsigned long, unsigned long,
			  unsigned long, unsigned long);
unsigned long lin_to_win5(void *func, unsigned long, unsigned long,
			  unsigned long, unsigned long, unsigned long);
unsigned long lin_to_win6(void *func, unsigned long, unsigned long,
			  unsigned long, unsigned long, unsigned long,
			  unsigned long);

STDCALL struct nt_thread *KeGetCurrentThread(void);
STDCALL NTSTATUS
ObReferenceObjectByHandle(void *handle, ACCESS_MASK desired_access,
			  void *obj_type, KPROCESSOR_MODE access_mode,
			  void **object, void *handle_info);

_FASTCALL LONG ObfReferenceObject(FASTCALL_DECL_1(void *object));
_FASTCALL void ObfDereferenceObject(FASTCALL_DECL_1(void *object));
STDCALL NTSTATUS ZwClose(void *object);
#define ObReferenceObject(object)			\
	ObfReferenceObject(FASTCALL_ARGS_1(object))
#define ObDereferenceObject(object)			\
	ObfDereferenceObject(FASTCALL_ARGS_1(object))

#define MSG(level, fmt, ...)				\
	printk(level "ndiswrapper (%s:%d): " fmt "\n",	\
	       __FUNCTION__, __LINE__ , ## __VA_ARGS__)
#define WARNING(fmt, ...) MSG(KERN_WARNING, fmt, ## __VA_ARGS__)
#define ERROR(fmt, ...) MSG(KERN_ERR, fmt , ## __VA_ARGS__)
#define INFO(fmt, ...) MSG(KERN_INFO, fmt , ## __VA_ARGS__)

#define INFOEXIT(stmt) do { INFO("Exit"); stmt; } while(0)

#define UNIMPL() ERROR("--UNIMPLEMENTED--")

void adjust_user_shared_data_addr(char *driver, unsigned long length);

#define IoCompleteRequest(irp, prio)			\
	IofCompleteRequest(FASTCALL_ARGS_2(irp, prio));
#define IoCallDriver(dev, irp)				\
	IofCallDriver(FASTCALL_ARGS_2(dev, irp));

static inline KIRQL current_irql(void)
{
	if (in_atomic() || irqs_disabled())
		return DISPATCH_LEVEL;
	else
		return PASSIVE_LEVEL;
}

static inline KIRQL raise_irql(KIRQL newirql)
{
	KIRQL irql = current_irql();
	if (irql < DISPATCH_LEVEL && newirql >= DISPATCH_LEVEL) {
		local_bh_disable();
		preempt_disable();
	}
	return irql;
}

static inline void lower_irql(KIRQL oldirql)
{
	KIRQL irql = current_irql();
	if (oldirql < DISPATCH_LEVEL && irql >= DISPATCH_LEVEL) {
		preempt_enable();
		local_bh_enable();
	}
}

/* Windows spinlocks are of type ULONG_PTR which is not big enough to
 * store Linux spinlocks; so we implement Windows spinlocks using
 * ULONG_PTR space with our own functions/macros */

/* the reason for value of unlocked spinlock to be 0, instead of 1
 * (which is what linux spinlocks use), is that some drivers don't
 * first call to initialize spinlock; in those case, the value of the
 * lock seems to be 0 (presumably in Windows value of unlocked
 * spinlock is 0).
 */

/* define CONFIG_DEBUG_SPINLOCK if a Windows driver is suspected of
 * obtaining a lock while holding the same lock */

//#ifndef CONFIG_DEBUG_SPINLOCK
//#define CONFIG_DEBUG_SPINLOCK
//#endif

#undef CONFIG_DEBUG_SPINLOCK

#ifdef CONFIG_DEBUG_SPINLOCK
#define KSPIN_LOCK_LOCKED ((ULONG_PTR)get_current())
#else
#define KSPIN_LOCK_LOCKED 1
#endif

#define KSPIN_LOCK_UNLOCKED 0

#define kspin_lock_init(lock) *(lock) = KSPIN_LOCK_UNLOCKED

#ifdef CONFIG_SMP

#define raw_kspin_lock(lock)						\
	while (cmpxchg((lock), KSPIN_LOCK_UNLOCKED, KSPIN_LOCK_LOCKED) != \
	       KSPIN_LOCK_UNLOCKED)

#ifdef CONFIG_DEBUG_SPINLOCK
#define raw_kspin_unlock(lock)						\
	__asm__ __volatile__("movw $0,%0"				\
			     :"=m" (*(lock)) : : "memory")
#else // DEBUG_SPINLOCK
#define raw_kspin_unlock(lock)						\
	__asm__ __volatile__("movb $0,%0"				\
			     :"=m" (*(lock)) : : "memory")
#endif // DEBUG_SPINLOCK

#else // SMP

#define raw_kspin_lock(lock) *(lock) = KSPIN_LOCK_LOCKED
#define raw_kspin_unlock(lock) *(lock) = KSPIN_LOCK_UNLOCKED

#endif // SMP

#ifdef CONFIG_DEBUG_SPINLOCK

#define kspin_lock(lock)						\
	do {								\
		if (*(lock) == KSPIN_LOCK_LOCKED)			\
			ERROR("eeek: process %p already owns lock %p",	\
			      get_current(), lock);			\
		else							\
			raw_kspin_lock(lock);				\
	} while (0)
#define kspin_unlock(lock)						\
	do {								\
		if (*(lock) != KSPIN_LOCK_LOCKED)			\
			ERROR("kspin_lock %p not locked!", (lock));	\
		raw_kspin_unlock(lock);					\
	} while (0)

#else // DEBUG_SPINLOCK

#define kspin_lock(lock) raw_kspin_lock(lock)
#define kspin_unlock(lock) raw_kspin_unlock(lock)

#endif // DEBUG_SPINLOCK

/* raise IRQL to given (higher) IRQL if necessary before locking */
#define kspin_lock_irql(lock, newirql)					\
({									\
	KIRQL _cur_irql_ = current_irql();				\
	if (_cur_irql_ < DISPATCH_LEVEL && newirql == DISPATCH_LEVEL) {	\
		local_bh_disable();					\
		preempt_disable();					\
	}								\
	kspin_lock(lock);						\
	_cur_irql_;							\
})

/* lower IRQL to given (lower) IRQL if necessary after unlocking */
#define kspin_unlock_irql(lock, oldirql)				\
do {									\
	KIRQL _cur_irql_ = current_irql();				\
	kspin_unlock(lock);						\
	if (oldirql < DISPATCH_LEVEL && _cur_irql_ == DISPATCH_LEVEL) {	\
		preempt_enable_no_resched();				\
		local_bh_enable();					\
	}								\
} while (0)

#define kspin_lock_irqsave(lock, flags)					\
do {									\
	local_irq_save(flags);						\
	preempt_disable();						\
	kspin_lock(lock);						\
} while (0)

#define kspin_unlock_irqrestore(lock, flags)				\
do {									\
	kspin_unlock(lock);						\
	local_irq_restore(flags);					\
	preempt_enable();						\
} while (0)

static inline ULONG SPAN_PAGES(ULONG_PTR ptr, SIZE_T length)
{
	ULONG n;

	n = (((ULONG_PTR)ptr & (PAGE_SIZE - 1)) +
	     length + (PAGE_SIZE - 1)) >> PAGE_SHIFT;

	return n;
}

/* DEBUG macros */

#define DBGTRACE(fmt, ...) do { } while (0)
#define DBGTRACE1(fmt, ...) do { } while (0)
#define DBGTRACE2(fmt, ...) do { } while (0)
#define DBGTRACE3(fmt, ...) do { }  while (0)
#define DBGTRACE4(fmt, ...) do { } while (0)
#define DBGTRACE5(fmt, ...) do { } while (0)
#define DBGTRACE6(fmt, ...) do { } while (0)

/* for a block of code */
#define DBG_BLOCK() while (0)

extern int debug;

#if defined DEBUG
#undef DBGTRACE
#define DBGTRACE(level, fmt, ...) do {					\
		if (debug >= level)					\
			printk(KERN_INFO "%s (%s:%d): " fmt "\n",	\
			       DRIVER_NAME, __FUNCTION__,		\
			       __LINE__ , ## __VA_ARGS__);		\
	} while (0)
#undef DBG_BLOCK
#define DBG_BLOCK()
#endif

#if defined(DEBUG) && DEBUG >= 1
#undef DBGTRACE1
#define DBGTRACE1(fmt, ...) DBGTRACE(1, fmt , ## __VA_ARGS__)
#endif

#if defined(DEBUG) && DEBUG >= 2
#undef DBGTRACE2
#define DBGTRACE2(fmt, ...) DBGTRACE(2, fmt , ## __VA_ARGS__)
#endif

#if defined(DEBUG) && DEBUG >= 3
#undef DBGTRACE3
#define DBGTRACE3(fmt, ...) DBGTRACE(3, fmt , ## __VA_ARGS__)
#endif

#if defined(DEBUG) && DEBUG >= 4
#undef DBGTRACE4
#define DBGTRACE4(fmt, ...) DBGTRACE(4, fmt , ## __VA_ARGS__)
#endif

#if defined(DEBUG) && DEBUG >= 5
#undef DBGTRACE5
#define DBGTRACE5(fmt, ...) DBGTRACE(5, fmt , ## __VA_ARGS__)
#endif

#if defined(DEBUG) && DEBUG >= 6
#undef DBGTRACE6
#define DBGTRACE6(fmt, ...) DBGTRACE(6, fmt , ## __VA_ARGS__)
#endif

#define TRACEENTER1(fmt, ...) DBGTRACE1("Enter " fmt , ## __VA_ARGS__)
#define TRACEENTER2(fmt, ...) DBGTRACE2("Enter " fmt , ## __VA_ARGS__)
#define TRACEENTER3(fmt, ...) DBGTRACE3("Enter " fmt , ## __VA_ARGS__)
#define TRACEENTER4(fmt, ...) DBGTRACE4("Enter " fmt , ## __VA_ARGS__)
#define TRACEENTER5(fmt, ...) DBGTRACE5("Enter " fmt , ## __VA_ARGS__)
#define TRACEENTER6(fmt, ...) DBGTRACE6("Enter " fmt , ## __VA_ARGS__)

#define TRACEEXIT1(stmt) do { DBGTRACE1("Exit"); stmt; } while(0)
#define TRACEEXIT2(stmt) do { DBGTRACE2("Exit"); stmt; } while(0)
#define TRACEEXIT3(stmt) do { DBGTRACE3("Exit"); stmt; } while(0)
#define TRACEEXIT4(stmt) do { DBGTRACE4("Exit"); stmt; } while(0)
#define TRACEEXIT5(stmt) do { DBGTRACE5("Exit"); stmt; } while(0)
#define TRACEEXIT6(stmt) do { DBGTRACE6("Exit"); stmt; } while(0)

//#define USB_DEBUG 1
//#define EVENT_DEBUG 1
//#define IO_DEBUG 1

#if defined(USB_DEBUG)
#define USBTRACE DBGTRACE1
#define USBENTER TRACEENTER1
#define USBEXIT TRACEEXIT1
#else
#define USBTRACE(fmt, ...)
#define USBENTER(fmt, ...)
#define USBEXIT(stmt) stmt
#endif

#if defined(EVENT_DEBUG)
#define EVENTTRACE DBGTRACE1
#define EVENTENTER TRACEENTER1
#define EVENTEXIT TRACEEXIT1
#else
#define EVENTTRACE(fmt, ...)
#define EVENTENTER(fmt, ...)
#define EVENTEXIT(stmt) stmt
#endif

#if defined(IO_DEBUG)
#define IOTRACE DBGTRACE1
#define IOENTER TRACEENTER1
#define IOEXIT TRACEEXIT1
#else
#define IOTRACE(fmt, ...)
#define IOENTER(fmt, ...)
#define IOEXIT(stmt) stmt
#endif

#if defined DEBUG
#define assert(expr) do {						\
		if (!(expr))						\
			ERROR("assertion failed: %s", (#expr));		\
	} while (0)
#else
#define assert(expr) do { } while (0)
#endif

#if defined(IO_DEBUG)
#define DUMP_IRP(__irp)							\
	do {								\
		struct io_stack_location *_irp_sl;			\
		_irp_sl = IoGetCurrentIrpStackLocation(__irp);		\
		IOTRACE("irp: %p, stack size: %d, cl: %d, sl: %p, "	\
			"dev_obj: %p, mj_fn: %d, minor_fn: %d, "	\
			"nt_urb: %p, event: %p",			\
			__irp, __irp->stack_count, (__irp)->current_location, \
			_irp_sl, _irp_sl->dev_obj, _irp_sl->major_fn,	\
			_irp_sl->minor_fn, URB_FROM_IRP(__irp),		\
			(__irp)->user_event);				\
	} while (0)
#else
#define DUMP_IRP(__irp) do { } while (0)
#endif

#define sleep(nsec)					\
	do {						\
		set_current_state(TASK_INTERRUPTIBLE);	\
		schedule_timeout(nsec * HZ);		\
	} while (0)

#endif // _NTOSKERNEL_H_

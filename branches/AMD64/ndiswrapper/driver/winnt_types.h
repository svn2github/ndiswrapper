/*
 *  Copyright (C) 2003-2004 Pontus Fuchs, Giridhar Pemmasani
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

#ifndef WINNT_TYPES_H
#define WINNT_TYPES_H

#define MAX_STR_LEN 512

#define TRUE 1
#define FALSE 0

#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2

#define STATUS_WAIT_0			0
#define STATUS_SUCCESS                  0
#define STATUS_ALERTED                  0x00000101
#define STATUS_TIMEOUT                  0x00000102
#define STATUS_PENDING                  0x00000103
#define STATUS_FAILURE                  0xC0000001
#define STATUS_INVALID_PARAMETER        0xC000000D
#define STATUS_MORE_PROCESSING_REQUIRED 0xC0000016
#define STATUS_BUFFER_TOO_SMALL         0xC0000023
#define STATUS_RESOURCES                0xC000009A
#define STATUS_NOT_SUPPORTED            0xC00000BB
#define STATUS_INVALID_PARAMETER        0xC000000D
#define STATUS_INVALID_PARAMETER_2      0xC00000F0
#define STATUS_CANCELLED                0xC0000120

#define IS_PENDING                      0x01
#define CALL_ON_CANCEL                  0x20
#define CALL_ON_SUCCESS                 0x40
#define CALL_ON_ERROR                   0x80

#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_MAXIMUM_FUNCTION           0x1b

#define THREAD_WAIT_OBJECTS 3
#define MAX_WAIT_OBJECTS 64

#define NOTIFICATION_TIMER 1

#define LOW_PRIORITY 		1
#define LOW_REALTIME_PRIORITY	16
#define HIGH_PRIORITY		32
#define MAXIMUM_PRIORITY	32

#ifdef CONFIG_X86_64
#define STDCALL
#define _FASTCALL
#define FASTCALL_DECL_1(decl1) decl1
#define FASTCALL_DECL_2(decl1,decl2) decl1, decl2
#define FASTCALL_DECL_3(decl1,decl2,decl3) decl1, decl2, decl3
#define FASTCALL_ARGS_1(arg1) arg1
#define FASTCALL_ARGS_2(arg1,arg2) arg1, arg2
#define FASTCALL_ARGS_3(arg1,arg2,arg3) arg1, arg2, arg3
#else 
#define STDCALL __attribute__((__stdcall__, regparm(0)))
#define _FASTCALL __attribute__((__stdcall__)) __attribute__((regparm (3)))
#define FASTCALL_DECL_1(decl1) int _dummy1_, int _dummy2_, decl1
#define FASTCALL_DECL_2(decl1,decl2) int _dummy1_, decl2, decl1
#define FASTCALL_DECL_3(decl1,decl2,decl3) int _dummy1_, decl2, decl1, decl3
#define FASTCALL_ARGS_1(arg1) 0, 0, arg1
#define FASTCALL_ARGS_2(arg1,arg2) 0, arg2, arg1
#define FASTCALL_ARGS_3(arg1,arg2,arg3) 0, arg2, arg1, arg3
#endif

#define NOREGPARM __attribute__((regparm(0)))
#define packed __attribute__((packed))

typedef uint8_t BOOLEAN;
typedef uint8_t  BYTE;
typedef uint8_t  *LPBYTE;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;

typedef size_t SIZE_T;
typedef SHORT wchar_t;
typedef short CSHORT;
typedef int32_t INT;
typedef long long LARGE_INTEGER;
typedef uint32_t UINT;

typedef char CHAR;
typedef unsigned char UCHAR;

typedef LONG NTSTATUS;

typedef LONG KPRIORITY;
typedef INT NT_STATUS;
typedef LARGE_INTEGER	PHYSICAL_ADDRESS;
typedef unsigned char KIRQL;
typedef CHAR KPROCESSOR_MODE;

#ifdef CONFIG_X86_64
typedef uint64_t ULONG_PTR;
#else
typedef uint32_t ULONG_PTR;
#endif

typedef ULONG_PTR	KAFFINITY;

struct ansi_string {
	USHORT len;
	USHORT buflen;
	char *buf;
};

struct unicode_string {
	USHORT len;
	USHORT buflen;
	wchar_t *buf;
};

struct slist_entry {
	struct slist_entry  *next;
};

union slist_head {
	ULONGLONG align;
	struct packed {
		struct slist_entry  *next;
		USHORT depth;
		USHORT sequence;
	} list;
};

struct list_entry {
	struct list_entry *fwd_link;
	struct list_entry *bwd_link;
};

struct dispatch_header {
	UCHAR type;
	UCHAR absolute;
	UCHAR size;
	UCHAR inserted;
	LONG signal_state;
	struct list_head wait_list_head;
};

struct kevent {
	struct dispatch_header header;
};

/* KSPIN_LOCK is typedef to ULONG_PTR, where ULONG_PTR is 32-bit
 * 32-bit platforms, 64-bit on 64 bit platforms; it is NOT pointer to
 * unsigned long  */
/* spinlock_t is 32-bits, provided CONFIG_DEBUG_SPINLOCK is disabled;
 * so for x86 32-bits, we can safely typedef KSPIN_LOCK to
 * spinlock_t */

#ifdef CONFIG_DEBUG_SPINLOCK
struct wrap_spinlock {
	spinlock_t spinlock;
	KIRQL use_bh;
};
typedef struct wrap_spinlock *KSPIN_LOCK;
#define WRAP_SPINLOCK(lock) &((lock)->spinlock)
#define K_SPINLOCK(lock) &((*lock)->spinlock)

#else

typedef union {
	spinlock_t spinlock;
	ULONG_PTR ntoslock;
} KSPIN_LOCK;
struct wrap_spinlock {
	KSPIN_LOCK klock;
	KIRQL use_bh;
};

#define WRAP_SPINLOCK(lock) &((lock)->klock.spinlock)
#define K_SPINLOCK(lock) &(lock)->spinlock
#endif

struct kdpc {
	SHORT type;
	UCHAR number;
	UCHAR importance;
	struct list_entry dpc_list_entry;

	void *func;
	void *ctx;
	void *arg1;
	void *arg2;
	KSPIN_LOCK lock;
};

enum pool_type {
	NonPagedPool,
	PagedPool,
	NonPagedPoolMustSucceed,
	DontUseThisType,
	NonPagedPoolCacheAligned,
	PagedPoolCacheAligned,
	NonPagedPoolCacheAlignedMustS
};

enum memory_caching_type {
	MM_NON_CACHED = FALSE,
	MM_CACHED = TRUE,
	MM_WRITE_COMBINED = 2,
	MM_HARDWARE_COHERENT_CACHED,
	MM_NON_CACHED_UNORDERED,
	MM_USWC_CACHED,
	MM_MAXIMUM_CACHE_TYPE
};

struct mdl {
	struct mdl* next;
	SHORT size;
	SHORT mdlflags;
	void *process;
	void *mappedsystemva;
	void *startva;
	ULONG bytecount;
	ULONG byteoffset;
};

struct device_queue_entry {
	struct list_entry list_entry;
	ULONG sort_key;
	BOOLEAN inserted;
};

struct wait_ctx_block {
	struct device_queue_entry wait_queue_entry;
	void *dev_routine;
	void *dev_ctx;
	ULONG map_reg_count;
	void *current_irp;
	void *buffer_chaining_dpc;
};

struct kdevice_queue {
	USHORT type;
	USHORT size;
	struct list_entry devlist_head;
	KSPIN_LOCK lock;
	BOOLEAN busy;
};

struct kdpc;
struct irp;

struct device_object {
	SHORT type;
	USHORT size;
	LONG ref_count;
	void *drv_obj;
	struct device_object *next_dev;
	void *attached_dev;
	struct irp *current_irp;
	void *io_timer;
	ULONG flags;
	ULONG characteristics;
	void *vpb;
	void *dev_ext;
	BYTE stack_size;
	union {
		struct list_entry list_entry;
		struct wait_ctx_block wcb;
	} queue;
	ULONG align_req;
	struct kdevice_queue dev_queue;
	struct kdpc dpc;
	UINT active_threads;
	void *security_desc;
	struct kevent dev_lock;
	USHORT sector_size;
	USHORT spare;
	void *dev_obj_ext;
	void *reserved;

	/* ndiswrapper-specific data */
	union {
		struct usb_device *usb;
	} device;
	void *handle;
};

struct io_status_block {
	NT_STATUS status;
	ULONG status_info;
};

struct io_stack_location {
	char major_fn;
	char minor_fn;
	char flags;
	char control;
	union {
		struct {
			ULONG output_buf_len;
			ULONG input_buf_len; /*align to pointer size*/
			ULONG code; /*align to pointer size*/
			void *type3_input_buf;
		} ioctl;
		struct {
			void *arg1;
			void *arg2;
			void *arg3;
			void *arg4;
		} generic;
	} params;
	struct device_object *dev_obj;
	void *fill;
	ULONG (*completion_handler)(struct device_object *,
				    struct irp *, void *) STDCALL;
	void *handler_arg;
};

enum irp_work_type {
	IRP_WORK_NONE,
	IRP_WORK_COMPLETE,
	IRP_WORK_CANCEL,
};

struct irp {
	SHORT type;
	USHORT size;
	void *mdl;
	ULONG flags;
	union {
		struct irp *master_irp;
		void *sys_buf;
	} associated_irp;

	void *fill1[2];

	struct io_status_block io_status;
	CHAR requestor_mode;
	UCHAR pending_returned;
	CHAR stack_size;
	CHAR stack_pos;
	UCHAR cancel;
	UCHAR cancel_irql;

	CHAR fill2[2];

	struct io_status_block *user_status;
	struct kevent *user_event;

	void *fill3[2];

	void (*cancel_routine)(struct device_object *, struct irp *) STDCALL;
	void *user_buf;
	void *driver_context[4];
	void *thread;

	void *fill4;

	struct list_entry list_entry;
	struct io_stack_location *current_stack_location;

	void *fill5[3];

	/* ndiswrapper extension */
	enum irp_work_type irp_work_type;
	struct list_head cancel_list_entry;
};

enum nt_obj_type {
	NT_OBJ_EVENT,
	NT_OBJ_MUTEX,
	NT_OBJ_THREAD,
	NT_OBJ_TIMER,
};

struct ktimer {
	struct dispatch_header dispatch_header;
	ULONGLONG due_time;
	struct list_entry timer_list;
	/* the space for kdpc is used for wrapper timer */
	/* struct kdpc *kdpc; */
	struct wrapper_timer *wrapper_timer;
	LONG period;
};

struct kmutex {
	struct dispatch_header dispatch_header;
	union {
		struct list_entry list_entry;
		UINT count;
	} u;
	void *owner_thread;
	BOOLEAN abandoned;
	BOOLEAN apc_disable;
};

enum wait_type {
	WAIT_ALL,
	WAIT_ANY
};

struct wait_block {
	struct list_entry list_entry;
	void *thread;
	struct dispatch_header *object;
	struct wait_block *next;
	USHORT wait_key;
	USHORT wait_type;
};

enum ntos_event_type {
	NOTIFICATION_EVENT,
	SYNCHRONIZATION_EVENT
};

typedef enum ntos_event_type KEVENT_TYPE;

enum ntos_wait_reason {
	WAIT_REASON_EXECUTIVE,
	WAIT_REASON_FREEPAGE,
	WAIT_REASON_PAGEIN,
	WAIT_REASON_POOLALLOCATION,
	WAIT_REASON_DELAYEXECUTION,
	WAIT_REASON_SUSPENDED,
	WAIT_REASON_USERREQUEST,
	WAIT_REASON_WREXECUTIVE,
	WAIT_REASON_WRFREEPAGE,
	WAIT_REASON_WRPAGEIN,
	WAIT_REASON_WRPOOLALLOCATION,
	WAIT_REASON_WRDELAYEXECUTION,
	WAIT_REASON_WRSUSPENDED,
	WAIT_REASON_WRUSERREQUEST,
	WAIT_REASON_WREVENTPAIR,
	WAIT_REASON_WRQUEUE,
	WAIT_REASON_WRLPCRECEIVE,
	WAIT_REASON_WRLPCREPLY,
	WAIT_REASON_WRVIRTUALMEMORY,
	WAIT_REASON_WRPAGEOUT,
	WAIT_REASON_WRRENDEZVOUS,
	WAIT_REASON_SPARE2,
	WAIT_REASON_SPARE3,
	WAIT_REASON_SPARE4,
	WAIT_REASON_SPARE5,
	WAIT_REASON_SPARE6,
	WAIT_REASON_WRKERNEL,
	WAIT_REASON_MAXIMUM
};

typedef enum ntos_wait_reason KWAIT_REASON;

typedef STDCALL void *LOOKASIDE_ALLOC_FUNC(enum pool_type pool_type,
					   SIZE_T size, ULONG tag);
typedef STDCALL void LOOKASIDE_FREE_FUNC(void *);

struct npaged_lookaside_list {
	union slist_head head;
	USHORT depth;
	USHORT maxdepth;
	ULONG totalallocs;
	ULONG allocmisses;
	ULONG totalfrees;
	ULONG freemisses;
	enum pool_type pool_type;
	ULONG tag;
	ULONG size;
	LOOKASIDE_ALLOC_FUNC *alloc_func;
	LOOKASIDE_FREE_FUNC *free_func;
	struct list_entry listent;
	ULONG lasttotallocs;
	ULONG lastallocmisses;
	ULONG pad[2];
	KSPIN_LOCK obsolete;
};

enum device_registry_property {
	DEVPROP_DEVICE_DESCRIPTION,
	DEVPROP_HARDWARE_ID,
	DEVPROP_COMPATIBLE_IDS,
	DEVPROP_BOOTCONF,
	DEVPROP_BOOTCONF_TRANSLATED,
	DEVPROP_CLASS_NAME,
	DEVPROP_CLASS_GUID,
	DEVPROP_DRIVER_KEYNAME,
	DEVPROP_MANUFACTURER,
	DEVPROP_FRIENDLYNAME,
	DEVPROP_LOCATION_INFO,
	DEVPROP_PHYSDEV_NAME,
	DEVPROP_BUSTYPE_GUID,
	DEVPROP_LEGACY_BUSTYPE,
	DEVPROP_BUS_NUMBER,
	DEVPROP_ENUMERATOR_NAME,
	DEVPROP_ADDRESS,
	DEVPROP_UINUMBER,
	DEVPROP_INSTALL_STATE,
	DEVPROP_REMOVAL_POLICY,
};

enum trace_information_class {
	TRACE_ID_CLASS,
	TRACE_HANDLE_CLASS,
	TRACE_ENABLE_FLAGS_CLASS,
	TRACE_ENABLE_LEVEL_CLASS,
	GLOBAL_LOGGER_HANDLE_CLASS,
	EVENT_LOGGER_HANDLE_CLASS,
	ALL_LOGGER_HANDLES_CLASS,
	TRACE_HANDLE_BY_NAME_CLASS
};

#endif /* WINNT_TYPES_H */

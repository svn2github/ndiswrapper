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

#ifndef NDIS_H
#define NDIS_H

#include "ntoskernel.h"

typedef UINT NDIS_STATUS;
typedef UCHAR NDIS_DMA_SIZE;

typedef union packed ndis_phy_address {
	ULONGLONG quad;
	struct packed {
		ULONG low;
		ULONG high;
	} s;
} NDIS_PHY_ADDRESS;

struct packed ndis_sg_element {
	NDIS_PHY_ADDRESS address;
	UINT len;
	UINT reserved;
};

struct packed ndis_sg_list {
	UINT len;
	UINT reserved;
	struct ndis_sg_element *elements;
};

struct ndis_phy_addr_unit {
	NDIS_PHY_ADDRESS phy_addr;
	UINT length;
};

struct ndis_buffer {
	struct ndis_buffer *next;
	UINT len;
	UINT offset;
	UCHAR *data;
};

enum mm_page_priority {
	LOW_PAGE_PRIORITY,
	NORMAL_PAGE_PRIORITY = 16,
	HIGH_PAGE_PRIORITY = 32
};

enum kinterrupt_mode {
	INTERRUT_MODE_LEVELSENSITIVE,
	INTERRUPT_MODE_LATCHED
};

enum ndis_per_packet_info {
	NDIS_TCPIP_CSUM_INFO,
	NDIS_IPSEC_INFO,
	NDIS_LARGE_SEND_INFO,
	NDIS_CLASS_HANDLE_INFO,
	NDIS_RSVD,
	NDIS_SCLIST_INFO,
	NDIS_IEEE8021Q_INFO,
	NDIS_ORIGINAL_PACKET_INFO,
        NDIS_PACKET_CANCELID,
	NDIS_MAX_PACKET_INFO
};

struct ndis_packet_extension {
	void *info[NDIS_MAX_PACKET_INFO];
};

struct ndis_packet_private {
	UINT nr_pages;
	UINT len;
	struct ndis_buffer *buffer_head;
	struct ndis_buffer *buffer_tail;
	void *pool;
	UINT count;
	ULONG flags;
	BOOLEAN valid_counts;
	UCHAR packet_flags;
	USHORT oob_offset;
};

struct packed ndis_packet {

	struct ndis_packet_private private;

	/* for use by miniport */
	union {
		/* for connectionless mininports */
		struct {
			BYTE miniport_reserved[2 * sizeof(void *)];
			BYTE wrapper_reserved[2 * sizeof(void *)];
		} cl_reserved;
		/* for deserialized miniports */
		struct {
			BYTE miniport_reserved_ex[3 * sizeof(void *)];
			BYTE wrapper_reserved_ex[sizeof(void *)];
		} deserailized_reserved;
		struct {
			BYTE mac_reserved[4 * sizeof(void *)];
		} mac_reserved;
	} u;
	ULONG_PTR reserved[2];
	UCHAR protocol_reserved[1];

	/* OOB data */
	union {
		ULONGLONG time_to_tx;
		ULONGLONG time_txed;
	} oob_tx;
	ULONGLONG time_rxed;
	UINT header_size;
	UINT mediaspecific_size;
	void *mediaspecific;
	NDIS_STATUS status;

	struct ndis_packet_extension extension;

	/* ndiswrapper-specific info */
	struct ndis_sg_list sg_list;
	/* since we haven't implemented sg, we use one dummy entry */
	struct ndis_sg_element sg_element;
	dma_addr_t dataphys;
	unsigned char header[ETH_HLEN];
	unsigned char *look_ahead;
	unsigned int look_ahead_size;
};

enum ndis_pnp_event {
	NDIS_PNP_QUERY_REMOVED,
	NDIS_PNP_REMOVED,
	NDIS_PNP_SURPRISE_REMOVED,
	NDIS_PNP_QUERY_STOPPED,
	NDIS_PNP_STOPPED,
	NDIS_PNP_PROFILE_CHANGED,
	NDIS_PNP_MAXIMUM,
};

enum work_queue_type {
	CRITICAL_WORK_QUEUE,
	DELAYED_WORK_QUEUE,
	HYPER_CRITICAL_WORK_QUEUE,
	MAXIMUM_WORK_QUEUE
};

enum ndis_request_type {
	NDIS_REQUEST_QUERY_INFORMATION,
	NDIS_REQUEST_SET_INFORMATION,
	NDIS_REQUEST_QUERY_STATISTICS,
	NDIS_REQUEST_OPEN,
	NDIS_REQUEST_CLOSE,
	NDIS_REQUEST_SEND,
	NDIS_REQUEST_TRANSFER_DATA,
	NDIS_REQUEST_RESET,
	NDIS_REQUEST_GENERIC1,
	NDIS_REQUEST_GENERIC2,
	NDIS_REQUEST_GENERIC3,
	NDIS_REQUEST_GENERIC4
};

struct ndis_request {
	mac_address mac;
	enum ndis_request_type request_type;
	union data {
		struct query_info {
			UINT oid;
			void *buf;
			UINT buf_len;
			UINT written;
			UINT needed;
		} query_info;
		struct set_info {
			UINT oid;
			void *buf;
			UINT buf_len;
			UINT written;
			UINT needed;
		} set_info;
	} data;
};

enum ndis_medium {
	NDIS_MEDIUM_802_3,
	NDIS_MEDIUM_802_5,
	NDIS_MEDIUM_FDDI,
	NDIS_MEDIUM_WAN,
	NDIS_MEDIUM_LOCALTALK,
	NDIS_MEDIUM_DIX,
	NDIS_MEDIUM_ARCNETRAW,
	NDIS_MEDIUM_ARCNET878_2,
	NDIS_MEDIUM_ATM,
	NDIS_MEDIUM_WIRELESSWAN,
	NDIS_MEDIUM_IRDA,
	NDIS_MEDIUM_BPC,
	NDIS_MEDIUM_COWAN,
	NDIS_MEDIUM_1394,
	NDIS_MEDIUM_MAX
};

enum ndis_phys_medium {
	NDIS_PHYSICAL_MEDIUM_UNSPECIFIED,
	NDIS_PHYSICAL_MEDIUM_WIRELESSLAN,
	NDIS_PHYSICAL_MEDIUM_CABLEMODEM,
	NDIS_PHYSICAL_MEDIUM_PHONELINE,
	NDIS_PHYSICAL_MEDIUM_POWERLINE,
	NDIS_PHYSICAL_MEDIUM_DSL,
	NDIS_PHYSICAL_MEDIUM_FIBRECHANNEL,
	NDIS_PHYSICAL_MEDIUM_1394,
	NDIS_PHYSICAL_MEDIUM_WIRELESSWAN,
	NDIS_PHYSICAL_MEDIUM_MAX,
};

typedef void (*ndis_isr_handler)(unsigned int *taken, unsigned int *callme,
				 void *ctx) STDCALL;
typedef void (*ndis_interrupt_handler)(void *ctx) STDCALL;

struct miniport_char {
	/* NDIS 3.0 */
	UCHAR majorVersion;
	UCHAR minorVersion;
	USHORT filler;
	UINT reserved;

	BOOLEAN (*hangcheck)(void *ctx) STDCALL;
	void (*disable_interrupts)(void *ctx) STDCALL;
	void (*enable_interrupts)(void *ctx) STDCALL;

	/* Stop miniport */
	void (*halt)(void *ctx) STDCALL;

	/* Interrupt BH */
	ndis_interrupt_handler handle_interrupt;

	/* Start miniport driver */
	UINT (*init)(UINT *status, UINT *medium_index,
		     enum ndis_medium medium[], UINT medium_array_size,
		     void *ndis_handle, void *conf_handle) STDCALL;

	/* Interrupt TH */
	ndis_isr_handler isr;

	/* Query parameters */
	UINT (*query)(void *ctx, UINT oid, char *buffer,
		      UINT buflen, UINT *written, UINT *needed) STDCALL;

	void * ReconfigureHandler;
	INT (*reset)(INT *needs_set, void *ctx) STDCALL;

	/* Send one packet */
	UINT (*send)(void *ctx, struct ndis_packet *packet,
		     UINT flags) STDCALL;

	/* Set parameters */
	UINT (*setinfo)(void *ctx, UINT oid, char *buffer, UINT buflen,
			UINT *written, UINT *needed) STDCALL;

	/* transfer data from received packet */
	UINT (*tx_data)(struct ndis_packet *ndis_packet, UINT *bytes_txed,
				void *adapter_ctx, void *rx_ctx,
				UINT offset, UINT bytes_to_tx) STDCALL;

	/* NDIS 4.0 extensions */
	/* upper layer is done with RX packet */
	void (*return_packet)(void *ctx, void *packet) STDCALL;

	/* Send packets */
	UINT (*send_packets)(void *ctx, struct ndis_packet **packets,
			     INT nr_of_packets) STDCALL;

	void (*alloc_complete)(void *handle, void *virt,
			       NDIS_PHY_ADDRESS *phys,
			       ULONG size, void *ctx) STDCALL;

	/* NDIS 5.0 extensions */
	UINT (*co_create_vc)(void *ctx, void *vc_handle, void *vc_ctx) STDCALL;
	UINT (*co_delete_vc)(void *vc_ctx) STDCALL;
	UINT (*co_activate_vc)(void *vc_ctx, void *call_params) STDCALL;
	UINT (*co_deactivate_vc)(void *vc_ctx) STDCALL;
	UINT (*co_send_packets)(void *vc_ctx, void **packets,
				UINT nr_of_packets) STDCALL;
	UINT (*co_request)(void *ctx, void *vc_ctx, UINT *req) STDCALL;

	/* NDIS 5.1 extensions */
	void *cancel_send_packets;
	void (*pnp_event_notify)(void *ctx, enum ndis_pnp_event, void *inf_buf,
				 ULONG inf_buf_len) STDCALL;
	void (*adapter_shutdown)(void *ctx) STDCALL;
	void *reserved1;
	void *reserved2;
	void *reserved3;
	void *reserved4;

};

#ifdef CONFIG_DEBUG_SPINLOCK
#define NDIS_SPINLOCK(lock) (lock)->lock
#else
#define NDIS_SPINLOCK(lock) (struct wrap_spinlock *)(lock)
#endif

struct ndis_spinlock {
	KSPIN_LOCK lock;
	KIRQL use_bh;
};

struct handle_ctx_entry {
	struct list_head list;
	void *handle;
	void *ctx;
};

struct ndis_sched_work_item {
	void *ctx;
	void (*func)(struct ndis_sched_work_item *, void *) STDCALL;
	UCHAR reserved[8 * sizeof(void *)];
};

struct ndis_io_work_item {
	void *ctx;
	void *device_object;
	void (*func)(void *device_object, void *ctx) STDCALL;
};

struct ndis_alloc_mem_work_item {
	unsigned long size;
	char cached;
	void *ctx;
};

struct ndis_free_mem_work_item {
	void *addr;
	unsigned int length;
	unsigned int flags;
};

enum ndis_work_entry_type {
	NDIS_SCHED_WORK_ITEM,
	NDIS_ALLOC_MEM_WORK_ITEM,
	NDIS_FREE_MEM_WORK_ITEM,
	NDIS_IO_WORK_ITEM,
	NDIS_RETURN_PACKET_WORK_ITEM,
};

struct ndis_work_entry {
	struct list_head list;
	enum ndis_work_entry_type type;
	struct ndis_handle *handle;
	union {
		struct ndis_sched_work_item *sched_work_item;
		struct ndis_alloc_mem_work_item alloc_mem_work_item;
		struct ndis_free_mem_work_item free_mem_work_item;
		struct ndis_io_work_item *io_work_item;
		struct ndis_packet *return_packet;
	} entry;
};

struct ndis_irq {
	/* void *intr_obj is used for irq */
	union {
		void *intr_obj;
		unsigned int irq;
	} irq;
	/* Taken by ISR, DisableInterrupt and SynchronizeWithInterrupt */
	KSPIN_LOCK lock;
	void *id;
	ndis_isr_handler isr;
	void *dpc;

	struct kdpc intr_dpc;
	struct ndis_handle *handle;
	UCHAR dpc_count;
	/* unsigned char filler1 is used for enabled */
	UCHAR enabled;
	struct kevent completed_event;
	UCHAR shared;
	UCHAR req_isr;
};

struct ndis_binary_data {
	unsigned short len;
	void *buf;
};

enum ndis_config_param_type {
	NDIS_CONFIG_PARAM_INT,
	NDIS_CONFIG_PARAM_HEXINT,
	NDIS_CONFIG_PARAM_STRING,
	NDIS_CONFIG_PARAM_MULTISTRING,
	NDIS_CONFIG_PARAM_BINARY,
	NDIS_CONFIG_PARAM_NONE,
};

struct ndis_config_param {
	enum ndis_config_param_type type;
	union {
		unsigned long intval;
		struct unicode_string ustring;
		struct ndis_binary_data binary_data;
	} data;
};

struct device_setting {
	struct list_head list;
	char name[MAX_NDIS_SETTING_NAME_LEN];
	char value[MAX_NDIS_SETTING_VALUE_LEN];
	struct ndis_config_param config_param;
};

struct ndis_bin_file {
	char name[MAX_NDIS_SETTING_NAME_LEN];
	int size;
	void *data;
};

/*
 * There is one of these per driver. One per loaded driver exists.
 *
 */
struct ndis_driver {
	CSHORT type;
	CSHORT size;
	void *dev_object;
	ULONG flags;
	void *driver_start;
	ULONG driver_size;
	void *driver_section;
	void *driver_extension;
	struct ustring *driver_name;
	void *hardware_database;
	void *fast_io_dispatch;
	void *driver_init;
	void *driver_start_io;
	void *driver_unload;
	void *major_func[IRP_MJ_MAXIMUM_FUNCTION + 1];

	/* rest is ndiswrapper specific info */
	struct list_head list;
	char name[MAX_NDIS_SETTING_NAME_LEN];
	char version[MAX_NDIS_SETTING_VALUE_LEN];

	int bustype;

	unsigned int num_pe_images;
	struct pe_image pe_images[MAX_PE_IMAGES];

	int num_bin_files;
	struct ndis_bin_file *bin_files;

	atomic_t users;
	struct miniport_char miniport_char;
};

struct ndis_handle;

/*
 * There is one of these per handeled device-id
 *
 */
struct ndis_device {
	struct list_head settings;
	int bustype;
	int vendor;
	int device;
	int subvendor;
	int subdevice;

	struct ndis_driver *driver;
	char driver_name[MAX_DRIVER_NAME_LEN];
	struct ndis_handle *handle;
};

struct ndis_wireless_stats {
	LARGE_INTEGER length;
	LARGE_INTEGER tx_frag;
	LARGE_INTEGER tx_multi_frag;
	LARGE_INTEGER failed;
	LARGE_INTEGER retry;
	LARGE_INTEGER multi_retry;
	LARGE_INTEGER rtss_succ;
	LARGE_INTEGER rtss_fail;
	LARGE_INTEGER ack_fail;
	LARGE_INTEGER frame_dup;
	LARGE_INTEGER rx_frag;
	LARGE_INTEGER rx_multi_frag;
	LARGE_INTEGER fcs_err;
};

enum wrapper_work {
	WRAPPER_LINK_STATUS,
	SET_OP_MODE,
	SET_ESSID,
	SET_PACKET_FILTER,
	COLLECT_STATS,
	SUSPEND_RESUME,
	/* do not work when this is set */
	SHUTDOWN
};

enum ndis_attributes {
	ATTR_SERIALIZED,
	ATTR_SURPRISE_REMOVE,
	ATTR_HALT_ON_SUSPEND,
};

enum hw_status {
	HW_NORMAL,
	HW_SUSPENDED,
	HW_HALTED,
	HW_UNAVAILABLE,
};

struct encr_info {
	struct encr_key {
		unsigned int length;
		unsigned char key[NDIS_ENCODING_TOKEN_MAX];
	} keys[MAX_ENCR_KEYS];
	int active;
};

struct packed ndis_essid {
	unsigned int length;
	char essid[NDIS_ESSID_MAX_SIZE];
};

struct packed ndis_encr_key {
	unsigned long struct_size;
	unsigned long index;
	unsigned long length;
	unsigned char key[NDIS_ENCODING_TOKEN_MAX];
};

enum auth_mode {
	AUTHMODE_OPEN,
	AUTHMODE_RESTRICTED,
	AUTHMODE_AUTO,
	AUTHMODE_WPA,
	AUTHMODE_WPAPSK,
	AUTHMODE_WPANONE,
	AUTHMODE_WPA2,
	AUTHMODE_WPA2PSK,
};

enum encr_mode {
	ENCR1_ENABLED,
	ENCR_DISABLED,
	ENCR1_NOKEY,
	ENCR1_NO_SUPPORT,
	ENCR2_ENABLED,
	ENCR2_ABSENT,
	ENCR3_ENABLED,
	ENCR3_ABSENT,
};

enum op_mode {
	NDIS_MODE_ADHOC,
	NDIS_MODE_INFRA,
	NDIS_MODE_AUTO
};

struct ndis_timer {
	struct ktimer ktimer;
	struct kdpc kdpc;
};

struct ndis_miniport_timer {
	struct ktimer ktimer;
	struct kdpc kdpc;
	void *timer_func;
	void *timer_ctx;
	struct ndis_handle *handle;
	struct ndis_miniport_timer *next;
};

struct packed ndis_resource_entry {
	UCHAR type;
	UCHAR share;
	USHORT flags;
	union {
		struct {
			PHYSICAL_ADDRESS start;
			ULONG length;
		} generic;
		struct {
			PHYSICAL_ADDRESS start;
			ULONG length;
		} port;
		struct {
			ULONG level;
			ULONG vector;
			KAFFINITY affinity;
		} interrupt;
		struct {
			PHYSICAL_ADDRESS start;
			ULONG length;
		} memory;
		struct {
			ULONG channel;
			ULONG port;
			ULONG reserved1;
		} dma;
		struct {
			ULONG data[3];
		} device_private;
		struct {
			ULONG start;
			ULONG length;
			ULONG reserved;
		} bus_number;
		struct {
			ULONG data_size;
			ULONG reserved1;
			ULONG reserved2;
		} device_specific_data;
	} u;
};

struct packed ndis_resource_list {
	USHORT version;
	USHORT revision;
	ULONG length;
	struct ndis_resource_entry list[0];
};

struct ndis_event {
	struct kevent kevent;
};

struct ndis_bind_paths {
	UINT number;
	struct unicode_string paths[1];
};

struct ndis_reference {
	KSPIN_LOCK lock;
	USHORT ref_count;
	BOOLEAN closing;
};

struct ndis_miniport_interrupt {
	void *object;
	KSPIN_LOCK dpc_count_lock;
	void *reserved;
	ndis_isr_handler irq_th;
	ndis_interrupt_handler irq_bh;
	struct kdpc interrupt_dpc;
	struct ndis_miniport_block *miniport;
	UCHAR dpc_count;
	BOOLEAN filler1;
	struct kevent dpcs_completed_event;
        BOOLEAN shared_interrupt;
	BOOLEAN isr_requested;
};

struct ndis_filterdbs {
	union {
		void *eth_db;
		void *null_db;
	} u;
	void *trdb;
	void *fddidb;
	void *arcdb;
};

/*
 * This is the per device struct. One per PCI-device exists.
 *
 * This struct contains function pointers that the drivers references
 * directly via macros, so it's important that they are at the correct
 * position hence the paddings.
 */
struct packed ndis_handle {
	void *signature;
	struct ndis_handle *next;
	struct ndis_driver *driver;
	void *adapter_ctx;
	struct unicode_string name;
	struct ndis_bind_paths *bindpaths;
	void *openqueue;
	struct ndis_reference reference;
	void *device_ctx;
	UCHAR padding;
	UCHAR lock_acquired;
	UCHAR pmode_opens;
	UCHAR assigned_cpu;
	KSPIN_LOCK lock;
	enum ndis_request_type *mediarequest;
	struct ndis_miniport_interrupt *interrupt;
	ULONG flags;
	ULONG pnp_flags;
	struct list_entry packet_list;
	struct ndis_packet *first_pending_tx_packet;
	struct ndis_packet *return_packet_queue;
	ULONG request_buffer;
	void *set_mcast_buffer;
	struct ndis_handle *primary_miniport;
	void *wrapper_ctx;
	void *bus_data_ctx;
	ULONG pnp_capa;
	void *resources;
	struct ndis_timer wakeup_dpc_timer;
	struct unicode_string basename;
	struct unicode_string symlink_name;
	ULONG ndis_hangcheck_interval;
	USHORT hanghcheck_ticks;
	USHORT hangcheck_tick;
	NDIS_STATUS ndis_reset_status;
	void *resetopen;
	struct ndis_filterdbs filterdbs;
	void *rx_packet;
	void *send_complete;
	void *send_resource_avail;
	void *reset_complete;

	ULONG media_type;
	UINT bus_number;
	UINT bus_type;
	UINT adapter_type;
	struct device_object *device_obj;
	struct device_object *phys_device_obj;
	struct device_object *next_device_obj;
	void *mapreg;
	void *call_mgraflist;
	void *miniport_thread;
	void *setinfobuf;
	USHORT setinfo_buf_len;
	USHORT max_send_pkts;
	UINT fake_status;
	void *lock_handler;
	struct unicode_string *adapter_instance_name;
	void *timer_queue;
	UINT mac_options;
	void *pending_req;
	UINT max_long_addrs;
	UINT max_short_addrs;
	UINT cur_lookahead;
	UINT max_lookahead;

	ndis_interrupt_handler irq_bh;
	void *disable_intr;
	void *enable_intr;
	void *send_pkts;
	void *deferred_send;
	void *eth_rx_indicate;
	void *txrx_indicate;
	void *fddi_rx_indicate;
	void *eth_rx_complete;
	void *txrx_complete;
	void *fddi_rx_complete;

	void *status;
	void *status_complete;
	void *td_complete;

	void *query_complete;
	void *set_complete;
	void *wan_tx_complete;
	void *wan_rx;
	void *wan_rx_complete;

	/* the rest are ndiswrapper specific */

	/* keep a barrier in cases of over-stepping */
	char barrier[200];

	int device_type;
	union {
		struct pci_dev *pci;
		struct usb_device *usb;
		void *ptr;
	} dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	struct usb_interface *intf;
#endif
	struct net_device *net_dev;
//	void *adapter_ctx;
	void *shutdown_ctx;

	struct work_struct irq_work;

	struct ndis_irq *ndis_irq;
	unsigned long mem_start;
	unsigned long mem_end;

	struct net_device_stats stats;
	struct iw_statistics wireless_stats;
	struct ndis_wireless_stats ndis_stats;
	struct ndis_device *device;

	struct work_struct xmit_work;
	struct wrap_spinlock xmit_ring_lock;
	struct ndis_packet *xmit_ring[XMIT_RING_SIZE];
	struct ndis_packet **xmit_array;
	unsigned int xmit_ring_start;
	unsigned int xmit_ring_pending;
	unsigned int max_send_packets;

	unsigned char send_ok;
	struct wrap_spinlock send_packet_done_lock;

	struct semaphore ndis_comm_mutex;
	wait_queue_head_t ndis_comm_wq;
	int ndis_comm_res;
	int ndis_comm_done;

	int serialized;
	int use_scatter_gather;
	int map_count;
	int multicast_list_size;
	char *multicast_list;
	dma_addr_t *map_dma_addr;

	int hangcheck_interval;
	int hangcheck_active;
	struct timer_list hangcheck_timer;
	int reset_status;

	struct timer_list stats_timer;

	unsigned long scan_timestamp;

	unsigned char link_status;
	struct encr_info encr_info;
	char nick[IW_ESSID_MAX_SIZE+1];

	u32 pci_state[16];
	unsigned long hw_status;

	struct ndis_essid essid;

	unsigned long capa;
	enum auth_mode auth_mode;
	enum encr_mode encr_mode;
	enum op_mode op_mode;

	mac_address mac;

	/* List of initialized timers */
	struct list_head timers;
	struct wrap_spinlock timers_lock;

	struct proc_dir_entry *procfs_iface;

	struct work_struct wrapper_worker;
	unsigned long wrapper_work;

	unsigned long attributes;
};

enum ndis_pm_state {
	NDIS_PM_STATE_D0 = 1,
	NDIS_PM_STATE_D1 = 2,
	NDIS_PM_STATE_D2 = 3,
	NDIS_PM_STATE_D3 = 4,
};

STDCALL void NdisMIndicateReceivePacket(struct ndis_handle *handle,
					struct ndis_packet **packets,
					UINT nr_packets);
STDCALL void NdisMSendComplete(struct ndis_handle *handle,
			       struct ndis_packet *packet,
			       NDIS_STATUS status);
STDCALL void NdisMSendResourcesAvailable(struct ndis_handle *handle);
STDCALL void NdisMIndicateStatus(struct ndis_handle *handle,
				 NDIS_STATUS status, void *buf, UINT len);
STDCALL void NdisMIndicateStatusComplete(struct ndis_handle *handle);
STDCALL void NdisMQueryInformationComplete(struct ndis_handle *handle,
					   NDIS_STATUS status);
STDCALL void NdisMSetInformationComplete(struct ndis_handle *handle,
					 NDIS_STATUS status);
STDCALL void NdisMResetComplete(struct ndis_handle *handle, NDIS_STATUS status,
				BOOLEAN address_reset);
STDCALL ULONG NDIS_BUFFER_TO_SPAN_PAGES(struct ndis_buffer *buffer);
STDCALL BOOLEAN NdisWaitEvent(struct ndis_event *event, UINT timeout);
STDCALL void NdisSetEvent(struct ndis_event *event);
STDCALL void NdisMDeregisterInterrupt(struct ndis_irq *ndis_irq);
STDCALL void EthRxIndicateHandler(void *adapter_ctx, void *rx_ctx,
				  char *header1,
				  char *header, UINT header_size,
				  void *look_ahead,
				  UINT look_ahead_size, UINT packet_size);
STDCALL void EthRxComplete(struct ndis_handle *handle);
STDCALL void NdisMTransferDataComplete(struct ndis_handle *handle,
				       struct ndis_packet *packet,
				       NDIS_STATUS status,
				       UINT bytes_txed);
STDCALL void NdisWriteConfiguration(NDIS_STATUS *status,
				    struct ndis_handle *handle,
				    struct unicode_string *key,
				    struct ndis_config_param *val);

STDCALL NT_STATUS RtlUnicodeStringToAnsiString(struct ansi_string *dst,
					       struct unicode_string *src,
					       BOOLEAN dup);
STDCALL NT_STATUS RtlAnsiStringToUnicodeString(struct unicode_string *dst,
					       struct ansi_string *src,
					       BOOLEAN dup);
STDCALL void RtlInitAnsiString(struct ansi_string *dst, CHAR *src);
STDCALL void RtlFreeUnicodeString(struct unicode_string *string);
STDCALL void RtlFreeAnsiString(struct ansi_string *string);

void *get_sp(void);
int ndis_init(void);
void ndis_exit_handle(struct ndis_handle *handle);
void ndis_exit(void);

void usb_init(void);

int ndiswrapper_procfs_init(void);
int ndiswrapper_procfs_add_iface(struct ndis_handle *handle);
void ndiswrapper_procfs_remove_iface(struct ndis_handle *handle);
void ndiswrapper_procfs_remove(void);

void packet_recycler(void *param);
int stricmp(const char *s1, const char *s2);

#endif /* NDIS_H */

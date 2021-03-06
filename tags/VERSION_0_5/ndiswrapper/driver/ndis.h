/*
 *  Copyright (C) 2003 Pontus Fuchs
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

#include <linux/types.h>
#include <linux/timer.h>

#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <asm/mman.h>

#include <linux/version.h>
#define DRV_NAME "ndiswrapper"

/* Workqueue / task queue backwards compatibility stuff */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,41)
#include <linux/workqueue.h>
#else
#include <linux/tqueue.h>
#define work_struct tq_struct
#define INIT_WORK INIT_TQUEUE
#define schedule_work schedule_task
#define flush_scheduled_work flush_scheduled_tasks
#endif

//#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,23)
#define HAVE_ETHTOOL 1
//#endif

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


#define STDCALL __attribute__((__stdcall__))
#define packed __attribute__((packed))

int getSp(void);


#ifdef DEBUG
#define DBGTRACE(s, args...) printk(s, args)
#else
#define DBGTRACE(s, ...)
#endif

struct packed ndis_scatterentry
{
	unsigned int physlo;
	unsigned int physhi;
	unsigned int len;
	unsigned int reserved;
};

struct packed ndis_scatterlist
{
	unsigned int len;
	unsigned int reserved;
	struct ndis_scatterentry entry;
};

struct packed ndis_phy_address
{
	__u32 low;
	__u32 high;
};

struct ndis_phy_addr_unit {
    struct ndis_phy_address phy_addr;
    unsigned int length;
};

struct ndis_buffer
{
	struct ndis_buffer *next;
	unsigned int len;
	unsigned int offset;
	unsigned char *data;
};


struct packed ndis_packet
{
	unsigned int nr_pages;

	/* 4: Packet length */
	unsigned int len;

	struct ndis_buffer *buffer_head;
	struct ndis_buffer *buffer_tail; 
	void *pool;

	/* 14 Number of buffers */
	unsigned int count;

	unsigned int flags;

	/* 1 If buffer count is valid? */
	__u8 valid_counts;
	__u8 packet_flags;
	__u16 oob_offset;

	/* For use by miniport */
	unsigned char private_1 [6*sizeof(void*)];
	unsigned char private_2[4]; 

	/* OOB data */
	__u32 timesent1;
	__u32 timesent2;
	__u32 timerec1;
	__u32 timerec2;
	unsigned int header_size;
	unsigned int mediaspecific_size;
	void *mediaspecific;
	unsigned int status;

	void *ext1;
	void *ext2;
	void *ext3;
	void *ext4;
	void *ext5;
	struct ndis_scatterlist *scatter_gather_ext;
	void *ext7;
	void *ext8;
	void *ext9;
	void *ext10;
	void *ext11;
	void *ext12;
	
	struct ndis_scatterlist scatterlist;
	dma_addr_t dataphys;
};


struct miniport_char
{
	unsigned char majorVersion;
	unsigned char minorVersion;
	unsigned int reserved;

	int (*hangcheck)(void *ctx) STDCALL;
	void * DisableInterruptHandler;
	void * EnableInterruptHandler;

	/* Stop miniport */
	void (*halt)(void *ctx) STDCALL;

	/* Interrupt BH */
	void (*handle_interrupt)(void *ctx) STDCALL;

	/* Start miniport driver */
	unsigned int (*init)(unsigned int *OpenErrorStatus, unsigned int *SelectedmediumIndex, unsigned int *MediumArray, unsigned int MediumArraySize, void *ndis_handle, void *conf_handle) STDCALL;

	/* Interrupt TH */
	void (*isr)(unsigned int *taken, unsigned int *callme, void *ctx) STDCALL;

	/* Query parameters */
	unsigned int (*query)(void *ctx, unsigned int oid, char *buffer, unsigned int buflen, unsigned int *written, unsigned int *needed) STDCALL;

	void * ReconfigureHandler;
	int (*reset)(int *needs_set, void *ctx) STDCALL;

	/* Send one packet */
	unsigned int (*send)(void *ctx, struct ndis_packet *packet, unsigned int flags) STDCALL;

	/* Set parameters */
	unsigned int (*setinfo)(void *ctx, unsigned int oid, char *buffer, unsigned int buflen, unsigned int *written, unsigned int *needed) STDCALL;

	void * TransferDataHandler;

	/* upper layer is done with RX packet */	
	void (*return_packet)(void *ctx, void *packet) STDCALL;

	/* Send packets */
	unsigned int (*send_packets)(void *ctx, struct ndis_packet **packets, int nr_of_packets) STDCALL;
};

struct ndis_work
{
	void *ctx;
	void (*func)(struct ndis_work *work, void *ctx) STDCALL;
	struct list_head list;
};


struct ndis_workentry
{
	struct list_head list;
	struct ndis_work *work;
};



struct ndis_irq
{
	int irq;
	struct ndis_handle *handle;

	/* Taken by ISR, DisableInterrupt and SynchronizeWithInterrupt */
	spinlock_t spinlock;

};


struct ndis_linux_spin_lock
{
	spinlock_t lock;
	unsigned long flags;
};


struct ndis_spin_lock
{
	struct ndis_linux_spin_lock *linux_lock;
	unsigned char kirql;
};

struct packed ustring
{
	__u16 len;
	__u16 buflen;
	char *buf;
};

struct ndis_binary_data {
	__u16 len;
	void *buf;
};

enum ndis_setting_type {
	NDIS_SETTING_INT,
	NDIS_SETTING_HEXINT,
	NDIS_SETTING_STRING,
	NDIS_SETTING_MULTISTRING,
	NDIS_SETTING_BINARY,
	NDIS_SETTING_NONE,
};

struct ndis_setting_val
{
	enum ndis_setting_type type;
	union
	{
		unsigned long intval;
		struct ustring ustring;
//		struct ndis_binary_data binary_data;
	} data;
};

struct ndis_setting
{
	struct list_head list;
	char *name;
	char *val_str;
	struct ndis_setting_val value;
};


struct ndis_file
{
	struct list_head list;
	char name[32];
	int size;
	void *data;
};


/*
 * There is one of these per driver. One per loaded driver exists.
 *
 */
struct ndis_driver
{
	struct list_head list;
	char name[32];

	struct list_head devices;
	struct list_head files;
	struct pci_driver pci_driver;
	struct pci_device_id *pci_idtable;

	int nr_devices;
	int started;

	unsigned int pci_registered; 

	void *image;
	unsigned int (*entry)(void *obj, char *p2) STDCALL;
	struct miniport_char miniport_char;
	struct ndis_device *current_device;
};

/*
 * There is one of these per handeled pci-id
 *
 */
struct ndis_device
{
	struct list_head list;
	struct list_head settings;
	struct ndis_driver *driver;

	int pci_vendor;
	int pci_device;
	int pci_subvendor;
	int pci_subdevice;
	int fuzzy;
};


typedef __u64 LARGE_INTEGER;
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

#define NDIS_ESSID_MAX_SIZE 32
struct packed essid_req
{
	unsigned int len;
	char essid[NDIS_ESSID_MAX_SIZE];
};

#define NDIS_ENCODING_TOKEN_MAX 32
struct packed wep_req
{
	unsigned long len;
	unsigned long keyindex;
	unsigned long keylength;
	unsigned char keymaterial[NDIS_ENCODING_TOKEN_MAX];
};

struct packed ndis_configuration
{
	__u32 length;
	__u32 beacon_period;
	__u32 atim_window;
	__u32 ds_config;
	struct ndis_configuration_fh
	{
		__u32 length;
		__u32 hop_pattern;
		__u32 hop_set;
		__u32 dwell_time;
	} fh_config;
};

#define XMIT_RING_SIZE 16
/*
 * This is the per device struct. One per PCI-device exists.
 *
 *  This struct contains function pointers that the drivers references directly via macros,
 * so it's important that they are at the correct position hence the paddings.
 */
struct packed ndis_handle
{
	char fill1[232];
	void *indicate_receive_packet;
	void *send_complete;
	char fill2[4];
	void *reset_complete;
	char fill3[132];
	void *indicate_status;
	void *indicate_status_complete;
	char fill4[4];
	void *query_complete;
	void *set_complete;
	char fill5[200];

	struct pci_dev *pci_dev;
	struct net_device *net_dev;
	void *adapter_ctx;

	struct work_struct irq_bh;
	int ndis_irq_enabled;

	int irq;
	unsigned long mem_start;
	unsigned long mem_end;

	struct net_device_stats stats;
	struct iw_statistics wireless_stats;
	struct ndis_wireless_stats ndis_stats;
	struct ndis_driver *driver;
	struct ndis_device *device;

	struct work_struct xmit_work;
	spinlock_t xmit_ring_lock;
	struct ndis_buffer *xmit_ring[XMIT_RING_SIZE];
	unsigned int xmit_ring_start;
	unsigned int xmit_ring_pending;
	
	struct semaphore query_set_mutex;
	wait_queue_head_t query_set_wqhead;
	int query_set_wait_res;
	int query_set_wait_done;

	int use_scatter_gather;
	int serialized_driver;
	int map_count;
	dma_addr_t *map_dma_addr;

	int hangcheck_interval;
	struct timer_list hangcheck_timer;
	struct work_struct hangcheck_work;
	int reset_status;

	struct timer_list statcollector_timer;
	struct work_struct statcollector_work;

	
	unsigned long scan_timestamp;

	u32 link_status;
	struct wep_req wep;
	char nick[IW_ESSID_MAX_SIZE+1];
	spinlock_t send_packet_lock;

	u32 pci_state[16];
	unsigned int pm_state;

	int wireless_mode;
};

struct ndis_timer
{
	struct timer_list timer;
	void *func;
	void *ctx;
	int repeat;
	int active;
};

struct packed ndis_resource_entry
{
	__u8 type;
	__u8 share;
	__u16 flags;
	__u32 param1;
	__u32 param2;
	__u32 param3;
};

struct packed ndis_resource_list
{
	__u16 version;
	__u16 revision;
	__u32 length;
	struct ndis_resource_entry list[0];
};


struct ndis_event
{
	int state;
};

#define NDIS_MAX_RATES 16
struct ssid_item
{
	unsigned long length;
	__u8 mac[ETH_ALEN];
	unsigned char reserved[2];
	struct essid_req ssid;
	unsigned long privacy;
	long rssi;
	unsigned int net_type;
	struct ndis_configuration config;
	unsigned int mode;
	unsigned char rates[NDIS_MAX_RATES];
	unsigned long ie_length;
	unsigned char ies[1];
};

#define MAX_SCAN_ITEMS 20
struct list_scan
{
	unsigned long num_items;
	struct ssid_item items[MAX_SCAN_ITEMS];
};

#define NDIS_ENCODE_ENABLED 0
#define NDIS_ENCODE_DISABLED 1
#define NDIS_ENCODE_NOKEY 2

#define NDIS_ENCODE_OPEN 0
#define NDIS_ENCODE_RESTRICTED 1
#define NDIS_ENCODE_OPEN_RESTRICTED 2

#define NDIS_MODE_ADHOC 0
#define NDIS_MODE_INFRA 1
#define NDIS_MODE_AUTO 2

#define NDIS_PRIV_ACCEPT_ALL 0
#define NDIS_PRIV_WEP 1

#define NDIS_POWER_OFF 0
#define NDIS_POWER_MAX 1
#define NDIS_POWER_MIN 2

#define NDIS_PM_STATE_D0 1
#define NDIS_PM_STATE_D1 2
#define NDIS_PM_STATE_D2 3
#define NDIS_PM_STATE_D3 4


void ndis_sendpacket_done(struct ndis_handle *handle, struct ndis_packet *packet);



void NdisMIndicateReceivePacket(struct ndis_handle *handle, struct ndis_packet **packets, unsigned int nr_packets) STDCALL;
void NdisMSendComplete(struct ndis_handle *handle, struct ndis_packet *packet, unsigned int status) STDCALL;
void NdisIndicateStatus(struct ndis_handle *handle, unsigned int status, void *buf, unsigned int len) STDCALL;
void NdisIndicateStatusComplete(struct ndis_handle *handle) STDCALL;
void NdisMQueryInformationComplete(struct ndis_handle *handle, unsigned int status) STDCALL;
void NdisMSetInformationComplete(struct ndis_handle *handle, unsigned int status) STDCALL;
int RtlUnicodeStringToAnsiString(struct ustring *dst, struct ustring *src, unsigned int dup) STDCALL;
int RtlAnsiStringToUnicodeString(struct ustring *dst, struct ustring *src, unsigned int dup) STDCALL;

int ndis_init_proc(struct ndis_handle *handle);
void ndis_remove_proc(struct ndis_handle *handle);
int doquery(struct ndis_handle *handle, unsigned int oid, char *buf, int bufsize, unsigned int *written , unsigned int *needed);
int query_int(struct ndis_handle *handle, int oid, int *data);


#define NDIS_OID_STAT_TX_OK         0x00020101
#define NDIS_OID_STAT_RX_OK         0x00020102
#define NDIS_OID_STAT_TX_ERROR      0x00020103
#define NDIS_OID_STAT_RX_ERROR      0x00020104

#define NDIS_OID_ESSID              0x0D010102
#define NDIS_OID_BSSID              0x0D010101
#define NDIS_OID_MODE               0x0D010108
#define NDIS_OID_RSSI               0x0D010206
#define NDIS_OID_RSSI_TRIGGER       0x0D010207
#define NDIS_OID_CONFIGURATION      0x0D010211
#define NDIS_OID_TX_POWER_LEVEL     0x0D010205
#define NDIS_OID_RTS_THRESH         0x0D01020A
#define NDIS_OID_FRAG_THRESH        0x0D010209
#define NDIS_OID_PACKET_FILTER      0x0001010E
#define NDIS_OID_ADD_WEP            0x0D010113
#define NDIS_OID_REMOVE_WEP         0x0D010114
#define NDIS_OID_WEP_STATUS         0x0D01011B
#define NDIS_OID_AUTH_MODE          0x0D010118
#define NDIS_OID_PRIVACY_FILTER     0x0D010119
#define NDIS_OID_NETWORK_TYPE_IN_USE 0x0D010204
#define NDIS_OID_BSSID_LIST_SCAN    0x0D01011A
#define NDIS_OID_BSSID_LIST         0x0D010217
#define NDIS_OID_POWER_MODE         0x0D010216
#define NDIS_OID_DISASSOCIATE       0x0D010115
#define NDIS_OID_STATISTICS         0x0D020212
#define NDIS_OID_SUPPORTED_RATES    0x0D01020E
#define NDIS_OID_DESIRED_RATES      0x0D010210


/* general OIDs */
#define NDIS_OID_GEN_SPEED          0x00010107

#define NDIS_OID_PNP_SET_POWER      0xFD010101
#define NDIS_OID_PNP_QUERY_POWER    0xFD010102
#define NDIS_OID_CURRENT_MAC_ADDRESS 0x01010102

#define NDIS_STATUS_SUCCESS		0
#define NDIS_STATUS_PENDING		0x00000103
#define NDIS_STATUS_NOT_RECOGNIZED	0x00010001
#define NDIS_STATUS_NOT_COPIED		0x00010002
#define NDIS_STATUS_NOT_ACCEPTED	0x00010003
#define NDIS_STATUS_CALL_ACTIVE		0x00010007
#define NDIS_STATUS_ONLINE		0x40010003
#define NDIS_STATUS_RESET_START		0x40010004
#define NDIS_STATUS_RESET_END		0x40010005
#define NDIS_STATUS_RING_STATUS		0x40010006
#define NDIS_STATUS_CLOSED		0x40010007
#define NDIS_STATUS_WAN_LINE_UP		0x40010008
#define NDIS_STATUS_WAN_LINE_DOWN	0x40010009
#define NDIS_STATUS_WAN_FRAGMENT	0x4001000A
#define NDIS_STATUS_MEDIA_CONNECT	0x4001000B
#define NDIS_STATUS_MEDIA_DISCONNECT	0x4001000C
#define NDIS_STATUS_HARDWARE_LINE_UP	0x4001000D
#define NDIS_STATUS_HARDWARE_LINE_DOWN	0x4001000E
#define NDIS_STATUS_INTERFACE_UP	0x4001000F
#define NDIS_STATUS_INTERFACE_DOWN	0x40010010
#define NDIS_STATUS_MEDIA_BUSY		0x40010011
#define NDIS_STATUS_MEDIA_SPECIFIC_INDICATION	0x40010012
#define NDIS_STATUS_WW_INDICATION NDIS_STATUS_MEDIA_SPECIFIC_INDICATION
#define NDIS_STATUS_LINK_SPEED_CHANGE	0x40010013
#define NDIS_STATUS_WAN_GET_STATS	0x40010014
#define NDIS_STATUS_WAN_CO_FRAGMENT	0x40010015
#define NDIS_STATUS_WAN_CO_LINKPARAMS	0x40010016
#define NDIS_STATUS_NOT_RESETTABLE	0x80010001
#define NDIS_STATUS_SOFT_ERRORS		0x80010003
#define NDIS_STATUS_HARD_ERRORS		0x80010004
#define NDIS_STATUS_BUFFER_OVERFLOW	0x80000005
#define NDIS_STATUS_FAILURE		0xC0000001
#define NDIS_STATUS_INVALID_PARAMETER 0xC000000D
#define NDIS_STATUS_RESOURCES		0xC000009A
#define NDIS_STATUS_CLOSING		0xC0010002
#define NDIS_STATUS_BAD_VERSION		0xC0010004
#define NDIS_STATUS_BAD_CHARACTERISTICS	0xC0010005
#define NDIS_STATUS_ADAPTER_NOT_FOUND	0xC0010006
#define NDIS_STATUS_OPEN_FAILED		0xC0010007
#define NDIS_STATUS_DEVICE_FAILED	0xC0010008
#define NDIS_STATUS_MULTICAST_FULL	0xC0010009
#define NDIS_STATUS_MULTICAST_EXISTS	0xC001000A
#define NDIS_STATUS_MULTICAST_NOT_FOUND	0xC001000B
#define NDIS_STATUS_REQUEST_ABORTED	0xC001000C
#define NDIS_STATUS_RESET_IN_PROGRESS	0xC001000D
#define NDIS_STATUS_CLOSING_INDICATING	0xC001000E
#define NDIS_STATUS_BAD_VERSION		0xC0010004
#define NDIS_STATUS_NOT_SUPPORTED	0xC00000BB
#define NDIS_STATUS_INVALID_PACKET	0xC001000F
#define NDIS_STATUS_OPEN_LIST_FULL	0xC0010010
#define NDIS_STATUS_ADAPTER_NOT_READY	0xC0010011
#define NDIS_STATUS_ADAPTER_NOT_OPEN	0xC0010012
#define NDIS_STATUS_NOT_INDICATING	0xC0010013
#define NDIS_STATUS_INVALID_LENGTH	0xC0010014
#define NDIS_STATUS_INVALID_DATA	0xC0010015
#define NDIS_STATUS_BUFFER_TOO_SHORT	0xC0010016
#define NDIS_STATUS_INVALID_OID		0xC0010017
#define NDIS_STATUS_ADAPTER_REMOVED	0xC0010018
#define NDIS_STATUS_UNSUPPORTED_MEDIA	0xC0010019
#define NDIS_STATUS_GROUP_ADDRESS_IN_USE	0xC001001A
#define NDIS_STATUS_FILE_NOT_FOUND	0xC001001B
#define NDIS_STATUS_ERROR_READING_FILE	0xC001001C
#define NDIS_STATUS_ALREADY_MAPPED	0xC001001D
#define NDIS_STATUS_RESOURCE_CONFLICT	0xC001001E
#define NDIS_STATUS_NO_CABLE		0xC001001F
#define NDIS_STATUS_INVALID_SAP		0xC0010020
#define NDIS_STATUS_SAP_IN_USE		0xC0010021
#define NDIS_STATUS_INVALID_ADDRESS	0xC0010022
#define NDIS_STATUS_VC_NOT_ACTIVATED	0xC0010023
#define NDIS_STATUS_DEST_OUT_OF_ORDER	0xC0010024
#define NDIS_STATUS_VC_NOT_AVAILABLE	0xC0010025
#define NDIS_STATUS_CELLRATE_NOT_AVAILABLE	0xC0010026
#define NDIS_STATUS_INCOMPATABLE_QOS	0xC0010027
#define NDIS_STATUS_AAL_PARAMS_UNSUPPORTED	0xC0010028
#define NDIS_STATUS_NO_ROUTE_TO_DESTINATION	0xC0010029
#define NDIS_STATUS_TOKEN_RING_OPEN_ERROR	0xC0011000
#define NDIS_STATUS_INVALID_DEVICE_REQUEST	0xC0000010
#define NDIS_STATUS_NETWORK_UNREACHABLE         0xC000023C

/* Event codes */

#define EVENT_NDIS_RESOURCE_CONFLICT	0xC0001388
#define EVENT_NDIS_OUT_OF_RESOURCE	0xC0001389
#define EVENT_NDIS_HARDWARE_FAILURE	0xC000138A
#define EVENT_NDIS_ADAPTER_NOT_FOUND	0xC000138B
#define EVENT_NDIS_INTERRUPT_CONNECT	0xC000138C
#define EVENT_NDIS_DRIVER_FAILURE	0xC000138D
#define EVENT_NDIS_BAD_VERSION		0xC000138E
#define EVENT_NDIS_TIMEOUT		0x8000138F
#define EVENT_NDIS_NETWORK_ADDRESS	0xC0001390
#define EVENT_NDIS_UNSUPPORTED_CONFIGURATION	0xC0001391
#define EVENT_NDIS_INVALID_VALUE_FROM_ADAPTER	0xC0001392
#define EVENT_NDIS_MISSING_CONFIGURATION_PARAMETER	0xC0001393
#define EVENT_NDIS_BAD_IO_BASE_ADDRESS	0xC0001394
#define EVENT_NDIS_RECEIVE_SPACE_SMALL	0x40001395
#define EVENT_NDIS_ADAPTER_DISABLED	0x80001396
#define EVENT_NDIS_IO_PORT_CONFLICT	0x80001397
#define EVENT_NDIS_PORT_OR_DMA_CONFLICT	0x80001398
#define EVENT_NDIS_MEMORY_CONFLICT	0x80001399
#define EVENT_NDIS_INTERRUPT_CONFLICT	0x8000139A
#define EVENT_NDIS_DMA_CONFLICT		0x8000139B
#define EVENT_NDIS_INVALID_DOWNLOAD_FILE_ERROR	0xC000139C
#define EVENT_NDIS_MAXRECEIVES_ERROR	0x8000139D
#define EVENT_NDIS_MAXTRANSMITS_ERROR	0x8000139E
#define EVENT_NDIS_MAXFRAMESIZE_ERROR	0x8000139F
#define EVENT_NDIS_MAXINTERNALBUFS_ERROR	0x800013A0
#define EVENT_NDIS_MAXMULTICAST_ERROR	0x800013A1
#define EVENT_NDIS_PRODUCTID_ERROR	0x800013A2
#define EVENT_NDIS_LOBE_FAILUE_ERROR	0x800013A3
#define EVENT_NDIS_SIGNAL_LOSS_ERROR	0x800013A4
#define EVENT_NDIS_REMOVE_RECEIVED_ERROR	0x800013A5
#define EVENT_NDIS_TOKEN_RING_CORRECTION	0x400013A6
#define EVENT_NDIS_ADAPTER_CHECK_ERROR	0xC00013A7
#define EVENT_NDIS_RESET_FAILURE_ERROR	0x800013A8
#define EVENT_NDIS_CABLE_DISCONNECTED_ERROR	0x800013A9
#define EVENT_NDIS_RESET_FAILURE_CORRECTION	0x800013AA

/* packet filter bits used by NDIS_OID_PACKET_FILTER */
#define NDIS_PACKET_TYPE_DIRECTED               0x00000001
#define NDIS_PACKET_TYPE_MULTICAST              0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST          0x00000004
#define NDIS_PACKET_TYPE_BROADCAST              0x00000008
#define NDIS_PACKET_TYPE_SOURCE_ROUTING         0x00000010
#define NDIS_PACKET_TYPE_PROMISCUOUS            0x00000020
#define NDIS_PACKET_TYPE_SMT                    0x00000040
#define NDIS_PACKET_TYPE_ALL_LOCAL              0x00000080
#define NDIS_PACKET_TYPE_GROUP                  0x00001000
#define NDIS_PACKET_TYPE_ALL_FUNCTIONAL         0x00002000
#define NDIS_PACKET_TYPE_FUNCTIONAL             0x00004000
#define NDIS_PACKET_TYPE_MAC_FRAME              0x00008000

#define UNIMPL() do { \
    printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );	\
  } while (0)


#endif /* NDIS_H */

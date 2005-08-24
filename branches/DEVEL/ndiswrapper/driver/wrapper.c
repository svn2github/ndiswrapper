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
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/kmod.h>

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <net/iw_handler.h>
#include <linux/rtnetlink.h>
#include <asm/scatterlist.h>
#include <asm/uaccess.h>

#include "wrapper.h"
#include "iw_ndis.h"
#include "loader.h"

#ifndef DRIVER_VERSION
#error ndiswrapper version is not defined; run 'make' only from ndiswrapper \
	directory or driver directory
#endif

static char *if_name = "wlan%d";
int proc_uid, proc_gid;
static int hangcheck_interval;

#if defined(DEBUG) && (DEBUG > 0)
int debug = DEBUG;
#else
int debug = 0;
#endif

/* used to implement Windows spinlocks */
spinlock_t spinlock_kspin_lock;

NW_MODULE_PARM_STRING(if_name, 0400);
MODULE_PARM_DESC(if_name, "Network interface name or template "
		 "(default: wlan%d)");
NW_MODULE_PARM_INT(proc_uid, 0600);
MODULE_PARM_DESC(proc_uid, "The uid of the files created in /proc "
		 "(default: 0).");
NW_MODULE_PARM_INT(proc_gid, 0600);
MODULE_PARM_DESC(proc_gid, "The gid of the files created in /proc "
		 "(default: 0).");
NW_MODULE_PARM_INT(hangcheck_interval, 0600);
/* 0 - default value provided by NDIS driver,
 * positive value - force hangcheck interval to that many seconds
 * negative value - disable hangcheck
 */
NW_MODULE_PARM_INT(debug, 0600);
MODULE_PARM_DESC(debug, "debug level");

MODULE_PARM_DESC(hangcheck_interval, "The interval, in seconds, for checking"
		 " if driver is hung. (default: 0)");

MODULE_AUTHOR("ndiswrapper team <ndiswrapper-general@lists.sourceforge.net>");
#ifdef MODULE_VERSION
MODULE_VERSION(DRIVER_VERSION);
#endif
static void ndis_set_rx_mode(struct net_device *dev);
static void set_multicast_list(struct net_device *dev,
			       struct wrapper_dev *wd);

/*
 * MiniportReset
 */
NDIS_STATUS miniport_reset(struct wrapper_dev *wd)
{
	KIRQL irql;
	NDIS_STATUS res = 0;
	struct miniport_char *miniport;
	UINT cur_lookahead;
	UINT max_lookahead;

	TRACEENTER2("wd: %p", wd);

	if (wd->hw_unavailable)
		TRACEEXIT1(return NDIS_STATUS_FAILURE);

	if (wd->reset_status)
		return NDIS_STATUS_PENDING;

	if (down_interruptible(&wd->ndis_comm_mutex))
		TRACEEXIT3(return NDIS_STATUS_FAILURE);
	miniport = &wd->driver->miniport;
	/* reset_status is used for two purposes: to check if windows
	 * driver needs us to reset filters etc (as per NDIS) and to
	 * check if another reset is in progress */
	wd->reset_status = NDIS_STATUS_PENDING;
	wd->ndis_comm_res = NDIS_STATUS_PENDING;
	wd->ndis_comm_done = 0;
	cur_lookahead = wd->nmb->cur_lookahead;
	max_lookahead = wd->nmb->max_lookahead;
	irql = raise_irql(DISPATCH_LEVEL);
	res = LIN2WIN2(miniport->reset, &wd->reset_status,
		       wd->nmb->adapter_ctx);
	lower_irql(irql);

	DBGTRACE2("res = %08X, reset_status = %08X",
		  res, wd->reset_status);
	if (res == NDIS_STATUS_PENDING) {
		if (wait_event_interruptible_timeout(
			    wd->ndis_comm_wq,
			    (wd->ndis_comm_done == 1), 5*HZ))
			res = wd->ndis_comm_res;
		else
			res = NDIS_STATUS_FAILURE;
		DBGTRACE2("res = %08X, reset_status = %08X",
			  res, wd->reset_status);
	}
	DBGTRACE2("reset: res = %08X, reset status = %08X",
		  res, wd->reset_status);

	if (res == NDIS_STATUS_SUCCESS && wd->reset_status) {
		/* NDIS says we should set lookahead size (?)
		 * functional address (?) or multicast filter */
		wd->nmb->cur_lookahead = cur_lookahead;
		wd->nmb->max_lookahead = max_lookahead;
		ndis_set_rx_mode(wd->net_dev);
	}
	wd->reset_status = 0;
	up(&wd->ndis_comm_mutex);

	TRACEEXIT3(return res);
}

/*
 * MiniportQueryInformation
 * Perform a sync query and deal with the possibility of an async operation.
 * This function must be called from process context as it will sleep.
 */
NDIS_STATUS miniport_query_info_needed(struct wrapper_dev *wd,
				       ndis_oid oid, void *buf,
				       ULONG bufsize, ULONG *needed)
{
	NDIS_STATUS res;
	ULONG written;
	struct miniport_char *miniport = &wd->driver->miniport;
	KIRQL irql;

	TRACEENTER3("query is at %p", miniport->query);

	if (wd->hw_unavailable)
		TRACEEXIT1(return NDIS_STATUS_FAILURE);

	if (down_interruptible(&wd->ndis_comm_mutex))
		TRACEEXIT3(return NDIS_STATUS_FAILURE);
	wd->ndis_comm_done = 0;
	irql = raise_irql(DISPATCH_LEVEL);
	res = LIN2WIN6(miniport->query, wd->nmb->adapter_ctx, oid, buf,
		       bufsize, &written, needed);
	lower_irql(irql);

	DBGTRACE3("res = %08x", res);
	if (res == NDIS_STATUS_PENDING) {
		/* wait for NdisMQueryInformationComplete upto HZ */
		if (wait_event_interruptible_timeout(
			    wd->ndis_comm_wq,
			    (wd->ndis_comm_done == 1), 2*HZ))
			res = wd->ndis_comm_res;
		else
			res = NDIS_STATUS_FAILURE;
	}
	up(&wd->ndis_comm_mutex);
	if (res && needed)
		DBGTRACE2("res: %08X, bufsize: %d, written: %d, needed: %d",
			  res, bufsize, written, *needed);
	TRACEEXIT3(return res);
}

NDIS_STATUS miniport_query_info(struct wrapper_dev *wd, ndis_oid oid,
				void *buf, ULONG bufsize)
{
	NDIS_STATUS res;
	ULONG needed;

	res = miniport_query_info_needed(wd, oid, buf, bufsize, &needed);
	return res;
}

/*
 * MiniportSetInformation
 * Perform a sync setinfo and deal with the possibility of an async operation.
 * This function must be called from process context as it will sleep.
 */
NDIS_STATUS miniport_set_info(struct wrapper_dev *wd, ndis_oid oid,
			      void *buf, ULONG bufsize)
{
	NDIS_STATUS res;
	ULONG written, needed;
	struct miniport_char *miniport = &wd->driver->miniport;
	KIRQL irql;

	TRACEENTER3("setinfo is at %p", miniport->setinfo);

	if (wd->hw_unavailable)
		TRACEEXIT1(return NDIS_STATUS_FAILURE);

	if (down_interruptible(&wd->ndis_comm_mutex))
		TRACEEXIT3(return NDIS_STATUS_FAILURE);
	wd->ndis_comm_done = 0;
	irql = raise_irql(DISPATCH_LEVEL);
	res = LIN2WIN6(miniport->setinfo, wd->nmb->adapter_ctx, oid, buf,
		       bufsize, &written, &needed);
	lower_irql(irql);
	DBGTRACE3("res = %08x", res);

	if (res == NDIS_STATUS_PENDING) {
		/* wait for NdisMSetInformationComplete upto HZ */
		if (wait_event_interruptible_timeout(
			    wd->ndis_comm_wq,
			    (wd->ndis_comm_done == 1), 2*HZ))
			res = wd->ndis_comm_res;
		else
			res = NDIS_STATUS_FAILURE;
		DBGTRACE2("res = %x", res);
	}
	up(&wd->ndis_comm_mutex);
	if (res && needed)
		DBGTRACE2("res: %08X, bufsize: %d, written: %d, needed: %d",
			  res, bufsize, written, needed);
	TRACEEXIT3(return res);
}

/* Make a query that has an int as the result. */
NDIS_STATUS miniport_query_int(struct wrapper_dev *wd, ndis_oid oid,
			       void *data)
{
	NDIS_STATUS res;

	res = miniport_query_info(wd, oid, data, sizeof(ULONG));
	if (!res)
		return 0;
	*(int *)data = 0;
	return res;
}

/* Set an int */
NDIS_STATUS miniport_set_int(struct wrapper_dev *wd, ndis_oid oid,
			     ULONG data)
{
	return miniport_set_info(wd, oid, &data, sizeof(data));
}

/*
 * MiniportInitialize
 */
NDIS_STATUS miniport_init(struct wrapper_dev *wd)
{
	NDIS_STATUS status, res;
	UINT medium_index;
	UINT medium_array[] = {NdisMedium802_3};
	struct miniport_char *miniport = &wd->driver->miniport;

	TRACEENTER1("driver init routine is at %p", miniport->init);
	if (miniport->init == NULL) {
		ERROR("%s", "initialization function is not setup correctly");
		return -EINVAL;
	}
	res = LIN2WIN6(miniport->init, &status, &medium_index, medium_array,
		       sizeof(medium_array) / sizeof(medium_array[0]),
		       wd->nmb, wd->nmb);
	if (res)
		TRACEEXIT1(return res);

	/* do we need to power up the card explicitly? */
//	res = miniport_set_pm_state(wd, NdisDeviceStateD0);
	/* do we need to reset the device? */
//	res = miniport_reset(wd);

	/* Wait a little to let card power up otherwise ifup might fail after
	   boot; USB devices seem to need long delays */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ/2);

	TRACEEXIT1(return 0);
}

NTSTATUS ndiswrapper_start_device(struct wrapper_dev *wd)
{
	NTSTATUS status;
#if 0
	struct irp *irp;
	struct io_stack_location *irp_sl;

	TRACEENTER1("%p", wd);
	irp = IoAllocateIrp(wd->nmb->fdo->stack_size, FALSE);
	irp_sl = IoGetNextIrpStackLocation(irp);
	DBGTRACE1("irp = %p, stack = %p", irp, irp_sl);
	irp_sl->major_fn = IRP_MJ_PNP;
	irp_sl->minor_fn = IRP_MN_START_DEVICE;
	irp->io_status.status = STATUS_NOT_SUPPORTED;
	status = IoCallDriver(wd->nmb->fdo, irp);
#else
	status = miniport_init(wd);
#endif
	TRACEEXIT1(return status);
}

struct irp *allocate_power_irp(struct device_object *dev,
			       enum device_power_state new_state)
{
	struct irp *irp;
	struct io_stack_location *stack;

	irp = IoAllocateIrp(dev->stack_size, FALSE);
	DBGTRACE1("stack size: %d, irp = %p", dev->stack_size, irp);
	DBGTRACE1("drv_obj: %p", dev->drv_obj);
	stack = IoGetNextIrpStackLocation(irp);
	stack->major_fn = IRP_MJ_POWER;
	stack->minor_fn = IRP_MN_SET_POWER;
	stack->params.power.type = DevicePowerState;
	stack->params.power.state.device_state = new_state;
	irp->io_status.status = STATUS_NOT_SUPPORTED;
	return irp;
}

NDIS_STATUS miniport_set_pm_state(struct wrapper_dev *wd,
				  enum ndis_pm_state pm_state)
{
	NDIS_STATUS res;
	struct miniport_char *miniport;

	res = NDIS_STATUS_SUCCESS;
	if (pm_state != NdisDeviceStateD0) {
		enum ndis_pm_state ps;
		ps = pm_state;
		res = miniport_query_int(wd, OID_PNP_QUERY_POWER, &ps);
		DBGTRACE2("query_power returns %08X", res);
	}
	if (res == NDIS_STATUS_SUCCESS)
		res = miniport_set_int(wd, OID_PNP_SET_POWER, pm_state);
	DBGTRACE2("set_power returns %08X", res);
#if 0
	if (res != NDIS_STATUS_SUCCESS) {
		struct device_object *fdo;
		struct irp *irp;

		fdo = wd->nmb->fdo;
		DBGTRACE1("fdo: %p", fdo);
		irp = allocate_power_irp(fdo, pm_state);
		res = PoCallDriver(fdo, irp);
		DBGTRACE1("res = %d", res);
	}
#endif
	miniport = &wd->driver->miniport;

	/* According NDIS, pnp_event_notify should be called whenever power
	 * is set to D0
	 * Only NDIS 5.1 drivers are required to supply this function; some
	 * drivers don't seem to support it (at least Orinoco)
	 */
	if (res == NDIS_STATUS_SUCCESS && pm_state == NdisDeviceStateD0 &&
	    miniport->pnp_event_notify) {
		ULONG pnp_info;
		pnp_info = NdisPowerProfileAcOnLine;
		DBGTRACE2("calling pnp_event_notify");
		LIN2WIN4(miniport->pnp_event_notify, wd->nmb->adapter_ctx,
			 NdisDevicePnPEventPowerProfileChanged,
			 &pnp_info, (ULONG)sizeof(pnp_info));
	}
	return res;
}

/*
 * MiniportHalt
 */
void miniport_halt(struct wrapper_dev *wd)
{
	struct miniport_char *miniport = &wd->driver->miniport;
	DBGTRACE1("driver halt is at %p", miniport->halt);

	LIN2WIN1(miniport->halt, wd->nmb->adapter_ctx);

	ndis_exit_device(wd);
	misc_funcs_exit_device(wd);

	TRACEEXIT1(return);
}

void ndiswrapper_stop_device(struct wrapper_dev *wd)
{
#if 0
	struct irp *irp;
	struct io_stack_location *irp_sl;

	irp = IoAllocateIrp(wd->nmb->fdo->stack_size, FALSE);
	irp_sl = IoGetNextIrpStackLocation(irp);
	DBGTRACE1("irp = %p, stack = %p", irp, irp_sl);
	irp_sl->major_fn = IRP_MJ_PNP;
	irp_sl->minor_fn = IRP_MN_STOP_DEVICE;
	irp->io_status.status = STATUS_NOT_SUPPORTED;
	IoCallDriver(wd->nmb->fdo, irp);
#else
	miniport_halt(wd);
#endif
}

static void hangcheck_proc(unsigned long data)
{
	struct wrapper_dev *wd = (struct wrapper_dev *)data;
	KIRQL irql;

	TRACEENTER3("%s", "");
	return;
	set_bit(HANGCHECK, &wd->wrapper_work);
	schedule_work(&wd->wrapper_worker);

	irql = kspin_lock_irql(&wd->timer_lock, DISPATCH_LEVEL);
	if (wd->hangcheck_active) {
		wd->hangcheck_timer.expires =
			jiffies + wd->hangcheck_interval;
		add_timer(&wd->hangcheck_timer);
	}
	kspin_unlock_irql(&wd->timer_lock, irql);

	TRACEEXIT3(return);
}

void hangcheck_add(struct wrapper_dev *wd)
{
	KIRQL irql;

	if (!wd->driver->miniport.hangcheck || wd->hangcheck_interval <= 0) {
		wd->hangcheck_active = 0;
		return;
	}

	init_timer(&wd->hangcheck_timer);
	wd->hangcheck_timer.data = (unsigned long)wd;
	wd->hangcheck_timer.function = &hangcheck_proc;

	irql = kspin_lock_irql(&wd->timer_lock, DISPATCH_LEVEL);
	add_timer(&wd->hangcheck_timer);
	wd->hangcheck_active = 1;
	kspin_unlock_irql(&wd->timer_lock, irql);
	return;
}

void hangcheck_del(struct wrapper_dev *wd)
{
	KIRQL irql;

	if (!wd->driver->miniport.hangcheck || wd->hangcheck_interval <= 0)
		return;

	irql = kspin_lock_irql(&wd->timer_lock, DISPATCH_LEVEL);
	wd->hangcheck_active = 0;
	del_timer(&wd->hangcheck_timer);
	kspin_unlock_irql(&wd->timer_lock, irql);
}

static void stats_proc(unsigned long data)
{
	struct wrapper_dev *wd = (struct wrapper_dev *)data;

	set_bit(COLLECT_STATS, &wd->wrapper_work);
	schedule_work(&wd->wrapper_worker);
	wd->stats_timer.expires = jiffies + 5 * HZ;
	add_timer(&wd->stats_timer);
}

static void stats_timer_add(struct wrapper_dev *wd)
{
	init_timer(&wd->stats_timer);
	wd->stats_timer.data = (unsigned long)wd;
	wd->stats_timer.function = &stats_proc;
	wd->stats_timer.expires = jiffies + 5 * HZ;
	add_timer(&wd->stats_timer);
}

static void stats_timer_del(struct wrapper_dev *wd)
{
	KIRQL irql;

	irql = kspin_lock_irql(&wd->timer_lock, DISPATCH_LEVEL);
	del_timer_sync(&wd->stats_timer);
	kspin_unlock_irql(&wd->timer_lock, irql);
}

static int ndis_open(struct net_device *dev)
{
	TRACEENTER1("%s", "");
	netif_device_attach(dev);
	netif_start_queue(dev);
	return 0;
}

static int ndis_close(struct net_device *dev)
{
	TRACEENTER1("%s", "");

	if (netif_running(dev)) {
		netif_stop_queue(dev);
		netif_device_detach(dev);
	}
	return 0;
}

/*
 * query functions may not be called from this function as they might
 * sleep which is not allowed from the context this function is
 * running in.
 */
static struct net_device_stats *ndis_get_stats(struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	return &wd->stats;
}

static int ndis_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	int rc = -ENODEV;
	return rc;
}

static void set_multicast_list(struct net_device *dev,
			       struct wrapper_dev *wd)
{
	struct dev_mc_list *mclist;
	int i, size = 0;
	char *list = wd->multicast_list;
	NDIS_STATUS res;

	for (i = 0, mclist = dev->mc_list;
	     mclist && i < dev->mc_count && size < wd->multicast_list_size;
	     i++, mclist = mclist->next) {
		memcpy(list, mclist->dmi_addr, ETH_ALEN);
		list += ETH_ALEN;
		size += ETH_ALEN;
	}
	DBGTRACE1("%d entries. size=%d", dev->mc_count, size);

	res = miniport_set_info(wd, OID_802_3_MULTICAST_LIST, list, size);
	if (res)
		ERROR("Unable to set multicast list (%08X)", res);
}

/*
 * This function is called fom BH context...no sleep!
 */
static void ndis_set_rx_mode(struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	set_bit(SET_PACKET_FILTER, &wd->wrapper_work);
	schedule_work(&wd->wrapper_worker);
}

static struct ndis_packet *
allocate_send_packet(struct wrapper_dev *wd, ndis_buffer *buffer)
{
	struct ndis_packet *packet;

	packet = allocate_ndis_packet();
	if (!packet)
		return NULL;

	/*
	  packet->private.nr_pages = NDIS_BUFFER_TO_SPAN_PAGES(buffer);
	  packet->private.len = MmGetMdlByteCount(buffer);
	  packet->private.count = 1;
	  packet->private.valid_counts = TRUE;
	*/
	packet->private.buffer_head = buffer;
	packet->private.buffer_tail = buffer;
//	packet->private.flags = NDIS_PROTOCOL_ID_TCP_IP;

	if (wd->use_sg_dma) {
		packet->ndis_sg_element.address =
			PCI_DMA_MAP_SINGLE(wd->dev.pci,
					   MmGetMdlVirtualAddress(buffer),
					   MmGetMdlByteCount(buffer),
					   PCI_DMA_TODEVICE);

		packet->ndis_sg_element.length = MmGetMdlByteCount(buffer);
		packet->ndis_sg_list.nent = 1;
		packet->ndis_sg_list.elements = &packet->ndis_sg_element;
		packet->extension.info[ScatterGatherListPacketInfo] =
			&packet->ndis_sg_list;
	}

	return packet;
}

static void free_send_packet(struct wrapper_dev *wd,
			     struct ndis_packet *packet)
{
	ndis_buffer *buffer;

	TRACEENTER3("packet: %p", packet);
	if (!packet) {
		ERROR("illegal packet from %p", wd);
		return;
	}

	buffer = packet->private.buffer_head;
	if (wd->use_sg_dma)
		PCI_DMA_UNMAP_SINGLE(wd->dev.pci,
				     packet->ndis_sg_element.address,
				     packet->ndis_sg_element.length,
				     PCI_DMA_TODEVICE);

	DBGTRACE3("freeing buffer %p", buffer);
	kfree(MmGetMdlVirtualAddress(buffer));
	free_mdl(buffer);

	DBGTRACE3("freeing packet %p", packet);
	free_ndis_packet(packet);
	TRACEEXIT3(return);
}

/*
 * MiniportSend and MiniportSendPackets
 * this function is called with lock held in DISPATCH_LEVEL, so no need
 * to raise irql to DISPATCH_LEVEL during MiniportSend(Packets)
*/
static int send_packets(struct wrapper_dev *wd, unsigned int start,
			unsigned int pending)
{
	NDIS_STATUS res;
	struct miniport_char *miniport = &wd->driver->miniport;
	unsigned int sent, n;
	struct ndis_packet *packet;

	TRACEENTER3("start: %d, pending: %d", start, pending);

	if (pending > wd->max_send_packets)
		n = wd->max_send_packets;
	else
		n = pending;

	if (miniport->send_packets) {
		unsigned int i;
		/* copy packets from xmit ring to linear xmit array */
		for (i = 0; i < n; i++) {
			int j = (start + i) % XMIT_RING_SIZE;
			wd->xmit_array[i] = wd->xmit_ring[j];
		}
		DBG_BLOCK() {
			if (wd->nmb->flags)
				WARNING("miniport is not ready: %08X",
					wd->nmb->flags);
		}
		LIN2WIN3(miniport->send_packets, wd->nmb->adapter_ctx,
			 wd->xmit_array, n);
		DBGTRACE3("sent");
		if (test_bit(ATTR_SERIALIZED, &wd->attributes)) {
			for (sent = 0; sent < n && wd->send_ok; sent++) {
				packet = wd->xmit_array[sent];
				switch(packet->oob_data.status) {
				case NDIS_STATUS_SUCCESS:
					sendpacket_done(wd, packet);
					break;
				case NDIS_STATUS_PENDING:
					break;
				case NDIS_STATUS_RESOURCES:
					wd->send_ok = 0;
					break;
				case NDIS_STATUS_FAILURE:
				default:
					free_send_packet(wd, packet);
					break;
				}
			}
		} else {
			sent = n;
		}
	} else {
		packet = wd->xmit_ring[start];
		res = LIN2WIN3(miniport->send, wd->nmb->adapter_ctx,
			       packet, 0);

		sent = 1;
		switch (res) {
		case NDIS_STATUS_SUCCESS:
			sendpacket_done(wd, packet);
			break;
		case NDIS_STATUS_PENDING:
			break;
		case NDIS_STATUS_RESOURCES:
			wd->send_ok = 0;
			sent = 0;
			break;
		case NDIS_STATUS_FAILURE:
			free_send_packet(wd, packet);
			break;
		}
	}
	TRACEEXIT3(return sent);
}

static void xmit_worker(void *param)
{
	struct wrapper_dev *wd = (struct wrapper_dev *)param;
	int n;
	KIRQL irql;

	TRACEENTER3("send_ok %d", wd->send_ok);

	/* some drivers e.g., new RT2500 driver, crash if any packets
	 * are sent when the card is not associated */
	irql = kspin_lock_irql(&wd->xmit_lock, DISPATCH_LEVEL);
	while (wd->send_ok) {
		if (wd->xmit_ring_pending == 0)
			break;
		n = send_packets(wd, wd->xmit_ring_start,
				 wd->xmit_ring_pending);
		wd->xmit_ring_start =
			(wd->xmit_ring_start + n) % XMIT_RING_SIZE;
		wd->xmit_ring_pending -= n;
		if (n > 0 && netif_queue_stopped(wd->net_dev))
			netif_wake_queue(wd->net_dev);
	}
	kspin_unlock_irql(&wd->xmit_lock, irql);

	TRACEEXIT3(return);
}

/*
 * Free and unmap packet created in xmit
 */
void sendpacket_done(struct wrapper_dev *wd,
		     struct ndis_packet *packet)
{
	KIRQL irql;

	TRACEENTER3("%s", "");
	irql = kspin_lock_irql(&wd->send_packet_done_lock, DISPATCH_LEVEL);
	wd->stats.tx_bytes += packet->private.len;
	wd->stats.tx_packets++;
	free_send_packet(wd, packet);
	kspin_unlock_irql(&wd->send_packet_done_lock, irql);
	TRACEEXIT3(return);
}

/*
 * This function is called in BH disabled context and ndis drivers
 * must have their send-functions called from sleepeable context so we
 * just queue the packets up here and schedule a workqueue to run
 * later.
 */
static int start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	ndis_buffer *buffer;
	struct ndis_packet *packet;
	unsigned int xmit_ring_next_slot;
	char *data;
	KIRQL irql;

	data = kmalloc(skb->len, GFP_ATOMIC);
	if (!data)
		return 1;

	buffer = allocate_init_mdl(data, skb->len);
	if (!buffer) {
		kfree(data);
		return 1;
	}
	packet = allocate_send_packet(wd, buffer);
	if (!packet) {
		free_mdl(buffer);
		kfree(data);
		return 1;
	}

	skb_copy_and_csum_dev(skb, data);
	dev_kfree_skb(skb);

	irql = kspin_lock_irql(&wd->xmit_lock, DISPATCH_LEVEL);
	xmit_ring_next_slot =
		(wd->xmit_ring_start +
		 wd->xmit_ring_pending) % XMIT_RING_SIZE;
	wd->xmit_ring[xmit_ring_next_slot] = packet;
	wd->xmit_ring_pending++;
	if (wd->xmit_ring_pending == XMIT_RING_SIZE)
		netif_stop_queue(wd->net_dev);
	kspin_unlock_irql(&wd->xmit_lock, irql);

	schedule_work(&wd->xmit_work);

	return 0;
}

int ndiswrapper_resume_device(struct wrapper_dev *wd);

int ndiswrapper_suspend_device(struct wrapper_dev *wd,
			       enum ndis_pm_state pm_state)
{
	NDIS_STATUS status;
	if (!wd)
		return -1;

	if (test_bit(HW_SUSPENDED, &wd->hw_status) ||
	    test_bit(HW_HALTED, &wd->hw_status))
		return -1;

	DBGTRACE2("irql: %d", current_irql());
	DBGTRACE2("detaching device: %s", wd->net_dev->name);
	netif_poll_disable(wd->net_dev);
	if (netif_running(wd->net_dev)) {
		netif_stop_queue(wd->net_dev);
		netif_device_detach(wd->net_dev);
	}
	hangcheck_del(wd);
	stats_timer_del(wd);

	/* USB devices seem to die if halted during suspend, so avoid
	 * it */
	status = miniport_set_pm_state(wd, pm_state);
	DBGTRACE2("suspending returns %08X", status);
	if (status == NDIS_STATUS_SUCCESS)
		set_bit(HW_SUSPENDED, &wd->hw_status);
	else {
		if (test_bit(ATTR_NO_HALT_ON_SUSPEND, &wd->attributes)) {
			set_bit(SUSPEND_RESUME, &wd->wrapper_work);
			schedule_work(&wd->wrapper_worker);
			ndiswrapper_resume_device(wd);
			return -1;
		} else {
			DBGTRACE2("no pm: halting the device");
			ndiswrapper_stop_device(wd);
			set_bit(HW_HALTED, &wd->hw_status);
		}
	}
	wd->hw_unavailable++;
	return 0;
}

int ndiswrapper_suspend_pci(struct pci_dev *pdev, pm_message_t state)
{
	struct wrapper_dev *wd;
	int ret;

	if (!pdev)
		return -1;
	wd = pci_get_drvdata(pdev);
	/* some drivers support only D3, so force it */
	ret = ndiswrapper_suspend_device(wd, NdisDeviceStateD3);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
	pci_save_state(pdev);
#else
	pci_save_state(pdev, wd->pci_state);
#endif
	pci_disable_device(pdev);
	pci_set_power_state(pdev, PMSG_SUSPEND);

	DBGTRACE2("%s: device suspended", wd->net_dev->name);
	return 0;
}

int ndiswrapper_resume_device(struct wrapper_dev *wd)
{
	if (!wd)
		return -1;
	if (!((test_bit(HW_SUSPENDED, &wd->hw_status) ||
	       test_bit(HW_HALTED, &wd->hw_status)) &&
	      wd->hw_unavailable > 0))
		return -1;

	wd->hw_unavailable--;
	set_bit(SUSPEND_RESUME, &wd->wrapper_work);
	schedule_work(&wd->wrapper_worker);
	return 0;
}

int ndiswrapper_resume_pci(struct pci_dev *pdev)
{
	struct wrapper_dev *wd;
	int ret;

	if (!pdev)
		return -1;
	wd = pci_get_drvdata(pdev);
	if (!wd)
		return -1;
	pci_enable_device(pdev);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
	pci_restore_state(pdev);
#else
	pci_restore_state(pdev, wd->pci_state);
#endif
	ret = ndiswrapper_resume_device(wd);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
int ndiswrapper_suspend_usb(struct usb_interface *intf, pm_message_t state)
{
	struct wrapper_dev *wd;
	int ret;

	wd = usb_get_intfdata(intf);
	/* some drivers support only D3, so force it */
	ret = ndiswrapper_suspend_device(wd, NdisDeviceStateD3);
	DBGTRACE2("ret = %d", ret);
	return ret;
}

int ndiswrapper_resume_usb(struct usb_interface *intf)
{
	struct wrapper_dev *wd;

	wd = usb_get_intfdata(intf);
	return ndiswrapper_resume_device(wd);
}
#endif

void ndiswrapper_remove_device(struct wrapper_dev *wd)
{
	KIRQL irql;
//	struct miniport_char *miniport = &wd->driver->miniport;

	TRACEENTER1("%s", wd->net_dev->name);

	set_bit(SHUTDOWN, &wd->wrapper_work);

	stats_timer_del(wd);
	hangcheck_del(wd);
	ndiswrapper_procfs_remove_iface(wd);

	ndis_close(wd->net_dev);
	netif_carrier_off(wd->net_dev);

	/* flush_scheduled_work here causes crash with 2.4 kernels */
	/* instead, throw away pending packets */
	irql = kspin_lock_irql(&wd->xmit_lock, DISPATCH_LEVEL);
	while (wd->xmit_ring_pending) {
		struct ndis_packet *packet;

		packet = wd->xmit_ring[wd->xmit_ring_start];
		free_send_packet(wd, packet);
		wd->xmit_ring_start =
			(wd->xmit_ring_start + 1) % XMIT_RING_SIZE;
		wd->xmit_ring_pending--;
	}
	kspin_unlock_irql(&wd->xmit_lock, irql);

	miniport_set_int(wd, OID_802_11_DISASSOCIATE, 0);

#if 0
	miniport = &wd->driver->miniport;
	DBGTRACE1("%d, %p",
		  test_bit(ATTR_SURPRISE_REMOVE, &wd->attributes),
		  miniport->pnp_event_notify);
	if (test_bit(ATTR_SURPRISE_REMOVE, &wd->attributes) &&
	    miniport->pnp_event_notify) {
		LIN2WIN4(miniport->pnp_event_notify, wd->nmb->adapter_ctx,
			 NdisDevicePnPEventSurpriseRemoved, NULL, 0);
	}
#endif
	TRACEENTER1("stopping device");
	ndiswrapper_stop_device(wd);
	TRACEENTER1("stopped");
	IoDeleteDevice(wd->nmb->fdo);
	IoDeleteDevice(wd->nmb->pdo);
	if (wd->xmit_array)
		kfree(wd->xmit_array);
	if (wd->multicast_list)
		kfree(wd->multicast_list);
	if (wd->net_dev) {
		printk(KERN_INFO "%s: device %s removed\n", DRIVER_NAME,
		       wd->net_dev->name);
		unregister_netdev(wd->net_dev);
		free_netdev(wd->net_dev);
	}
	TRACEEXIT1(return);
}

static void link_status_handler(struct wrapper_dev *wd)
{
	struct ndis_assoc_info *ndis_assoc_info;
#if WIRELESS_EXT < 18
	unsigned char *wpa_assoc_info, *ies;
	unsigned char *p;
#endif
	unsigned char *assoc_info;
	union iwreq_data wrqu;
	unsigned int i;
	NDIS_STATUS res;
	const int assoc_size = sizeof(*ndis_assoc_info) + IW_CUSTOM_MAX;
	struct encr_info *encr_info = &wd->encr_info;

	TRACEENTER2("link status: %d", wd->link_status);
	if (wd->link_status == 0) {
		if (wd->encr_mode == Ndis802_11Encryption1Enabled ||
		    wd->infrastructure_mode == Ndis802_11IBSS) {
			for (i = 0; i < MAX_ENCR_KEYS; i++) {
				if (encr_info->keys[i].length == 0)
					continue;
				add_wep_key(wd, encr_info->keys[i].key,
					    encr_info->keys[i].length, i);
			}

			set_bit(SET_ESSID, &wd->wrapper_work);
			schedule_work(&wd->wrapper_worker);
			TRACEEXIT2(return);
		}
		/* FIXME: not clear if NDIS says keys should
		 * be cleared here */
		for (i = 0; i < MAX_ENCR_KEYS; i++)
			wd->encr_info.keys[i].length = 0;

		memset(&wrqu, 0, sizeof(wrqu));
		wrqu.ap_addr.sa_family = ARPHRD_ETHER;
		wireless_send_event(wd->net_dev, SIOCGIWAP, &wrqu, NULL);
		TRACEEXIT2(return);
	}

	if (!(test_bit(Ndis802_11Encryption2Enabled, &wd->capa.encr) ||
	      test_bit(Ndis802_11Encryption3Enabled, &wd->capa.encr)))
		TRACEEXIT2(return);

	assoc_info = kmalloc(assoc_size, GFP_KERNEL);
	if (!assoc_info) {
		ERROR("%s", "couldn't allocate memory");
		TRACEEXIT2(return);
	}
	memset(assoc_info, 0, assoc_size);

	ndis_assoc_info = (struct ndis_assoc_info *)assoc_info;
	ndis_assoc_info->length = sizeof(*ndis_assoc_info);
	ndis_assoc_info->offset_req_ies = sizeof(*ndis_assoc_info);
	ndis_assoc_info->req_ie_length = IW_CUSTOM_MAX / 2;
	ndis_assoc_info->offset_resp_ies = sizeof(*ndis_assoc_info) +
		ndis_assoc_info->req_ie_length;
	ndis_assoc_info->resp_ie_length = IW_CUSTOM_MAX / 2;

	res = miniport_query_info(wd, OID_802_11_ASSOCIATION_INFORMATION,
				  assoc_info, assoc_size);
	if (res) {
		DBGTRACE2("query assoc_info failed (%08X)", res);
		kfree(assoc_info);
		TRACEEXIT2(return);
	}

	/* we need 28 extra bytes for the format strings */
	if ((ndis_assoc_info->req_ie_length +
	     ndis_assoc_info->resp_ie_length + 28) > IW_CUSTOM_MAX) {
		WARNING("information element is too long! (%u,%u),"
			"association information dropped",
			ndis_assoc_info->req_ie_length,
			ndis_assoc_info->resp_ie_length);
		kfree(assoc_info);
		TRACEEXIT2(return);
	}

	/*
	 * TODO: backwards compatibility would require that IWEVCUSTOM
	 * is sent even if WIRELESS_EXT > 17. This version does not do
	 * this in order to allow wpa_supplicant to be tested with
	 * WE-18.
	 */
#if WIRELESS_EXT > 17
	memset(&wrqu, 0, sizeof(wrqu));
	wrqu.data.length = ndis_assoc_info->req_ie_length;
	wireless_send_event(wd->net_dev, IWEVASSOCREQIE, &wrqu,
			    ((char *) ndis_assoc_info) +
			    ndis_assoc_info->offset_req_ies);
	wrqu.data.length = ndis_assoc_info->resp_ie_length;
	wireless_send_event(wd->net_dev, IWEVASSOCRESPIE, &wrqu,
			    ((char *) ndis_assoc_info) +
			    ndis_assoc_info->offset_resp_ies);
#else
	wpa_assoc_info = kmalloc(IW_CUSTOM_MAX, GFP_KERNEL);
	if (!wpa_assoc_info) {
		ERROR("%s", "couldn't allocate memory");
		kfree(assoc_info);
		TRACEEXIT2(return);
	}
	p = wpa_assoc_info;
	p += sprintf(p, "ASSOCINFO(ReqIEs=");
	ies = ((char *)ndis_assoc_info) +
		ndis_assoc_info->offset_req_ies;
	for (i = 0; i < ndis_assoc_info->req_ie_length; i++)
		p += sprintf(p, "%02x", ies[i]);

	p += sprintf(p, " RespIEs=");
	ies = ((char *)ndis_assoc_info) +
		ndis_assoc_info->offset_resp_ies;
	for (i = 0; i < ndis_assoc_info->resp_ie_length; i++)
		p += sprintf(p, "%02x", ies[i]);

	p += sprintf(p, ")");

	memset(&wrqu, 0, sizeof(wrqu));
	wrqu.data.length = p - wpa_assoc_info;
	DBGTRACE2("adding %d bytes", wrqu.data.length);
	wireless_send_event(wd->net_dev, IWEVCUSTOM, &wrqu,
			    wpa_assoc_info);

	kfree(wpa_assoc_info);
#endif

	kfree(assoc_info);

	get_ap_address(wd, (char *)&wrqu.ap_addr.sa_data);
	wrqu.ap_addr.sa_family = ARPHRD_ETHER;
	wireless_send_event(wd->net_dev, SIOCGIWAP, &wrqu, NULL);
	DBGTRACE2("%s", "associate_event");
	TRACEEXIT2(return);
}

static void set_packet_filter(struct wrapper_dev *wd)
{
	struct net_device *dev = (struct net_device *)wd->net_dev;
	ULONG packet_filter;
	NDIS_STATUS res;

	packet_filter = (NDIS_PACKET_TYPE_DIRECTED |
			 NDIS_PACKET_TYPE_BROADCAST |
			 NDIS_PACKET_TYPE_ALL_MULTICAST);

	if (dev->flags & IFF_PROMISC) {
		packet_filter |= NDIS_PACKET_TYPE_ALL_LOCAL |
			NDIS_PACKET_TYPE_PROMISCUOUS;
	} else if ((dev->mc_count > wd->multicast_list_size) ||
		   (dev->flags & IFF_ALLMULTI) ||
		   (wd->multicast_list == 0)) {
		/* too many to filter perfectly -- accept all multicasts. */
		DBGTRACE1("multicast list too long; accepting all");
		packet_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
	} else if (dev->mc_count > 0) {
		packet_filter |= NDIS_PACKET_TYPE_MULTICAST;
		set_multicast_list(dev, wd);
	}

	res = miniport_set_info(wd, OID_GEN_CURRENT_PACKET_FILTER,
				&packet_filter, sizeof(packet_filter));
	if (res && (packet_filter & NDIS_PACKET_TYPE_PROMISCUOUS)) {
		/* 802.11 drivers may fail when PROMISCUOUS flag is
		 * set, so try without */
		packet_filter &= ~NDIS_PACKET_TYPE_PROMISCUOUS;
		res = miniport_set_info(wd, OID_GEN_CURRENT_PACKET_FILTER,
					&packet_filter, sizeof(packet_filter));
	}
	if (res && res != NDIS_STATUS_NOT_SUPPORTED)
		ERROR("unable to set packet filter (%08X)", res);
	TRACEEXIT2(return);
}

static void update_wireless_stats(struct wrapper_dev *wd)
{
	struct iw_statistics *iw_stats = &wd->wireless_stats;
	struct ndis_wireless_stats ndis_stats;
	NDIS_STATUS res;
	ndis_rssi rssi;

	return;
	TRACEENTER2("");
	/* Prism1 USB and Airgo cards crash kernel if RSSI is queried */
	if ((wd->ndis_device->vendor == 0x17cb &&
	       wd->ndis_device->device == 0x0001) ||
	      (wd->ndis_device->vendor == 0x2001 &&
	       wd->ndis_device->device == 0x3700))
		return;

	rssi = 0;
	res = miniport_query_info(wd, OID_802_11_RSSI, &rssi, sizeof(rssi));
	if (res == NDIS_STATUS_SUCCESS)
		iw_stats->qual.level = rssi;

	memset(&ndis_stats, 0, sizeof(ndis_stats));
	res = miniport_query_info(wd, OID_802_11_STATISTICS,
				  &ndis_stats, sizeof(ndis_stats));
	if (res == NDIS_STATUS_NOT_SUPPORTED)
		iw_stats->qual.qual = ((rssi & 0x7F) * 100) / 154;
	else {
		iw_stats->discard.retries = (u32)ndis_stats.retry +
			(u32)ndis_stats.multi_retry;
		iw_stats->discard.misc = (u32)ndis_stats.fcs_err +
			(u32)ndis_stats.rtss_fail +
			(u32)ndis_stats.ack_fail +
			(u32)ndis_stats.frame_dup;

		if ((u32)ndis_stats.tx_frag)
			iw_stats->qual.qual = 100 - 100 *
				((u32)ndis_stats.retry +
				 2 * (u32)ndis_stats.multi_retry +
				 3 * (u32)ndis_stats.failed) /
				(6 * (u32)ndis_stats.tx_frag);
		else
			iw_stats->qual.qual = 100;
	}
	TRACEEXIT2(return);
}

static struct iw_statistics *get_wireless_stats(struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	return &wd->wireless_stats;
}

#ifdef HAVE_ETHTOOL
static u32 ndis_get_link(struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	return wd->link_status;
}

static struct ethtool_ops ndis_ethtool_ops = {
	.get_link		= ndis_get_link,
};
#endif

/* worker procedure to take care of setting/checking various states */
static void wrapper_worker_proc(void *param)
{
	struct wrapper_dev *wd = (struct wrapper_dev *)param;

	DBGTRACE2("%lu", wd->wrapper_work);

	if (test_bit(SHUTDOWN, &wd->wrapper_work))
		TRACEEXIT3(return);

	if (test_and_clear_bit(SET_INFRA_MODE, &wd->wrapper_work))
		set_infra_mode(wd, wd->infrastructure_mode);

	if (test_and_clear_bit(LINK_STATUS_CHANGED, &wd->wrapper_work))
		link_status_handler(wd);

	if (test_and_clear_bit(SET_ESSID, &wd->wrapper_work))
		set_essid(wd, wd->essid.essid, wd->essid.length);

	if (test_and_clear_bit(SET_PACKET_FILTER, &wd->wrapper_work))
		set_packet_filter(wd);

	if (test_and_clear_bit(COLLECT_STATS, &wd->wrapper_work))
		update_wireless_stats(wd);

	if (test_and_clear_bit(HANGCHECK, &wd->wrapper_work) &&
	    wd->reset_status == 0) {
		NDIS_STATUS res;
		struct miniport_char *miniport;
		KIRQL irql;

		miniport = &wd->driver->miniport;
		irql = raise_irql(DISPATCH_LEVEL);
		res = LIN2WIN1(miniport->hangcheck, wd->nmb->adapter_ctx);
		lower_irql(irql);
		if (res) {
			WARNING("%s is being reset", wd->net_dev->name);
			res = miniport_reset(wd);
			DBGTRACE3("reset returns %08X, %d",
				  res, wd->reset_status);
		}
	}

	if (test_bit(SUSPEND_RESUME, &wd->wrapper_work)) {
		NDIS_STATUS res;
		struct net_device *net_dev = wd->net_dev;

		DBGTRACE2("resuming device %s", net_dev->name);
#if defined(DEBUG) && DBEBUG > 1
		/* see if pm issues are with kernel or ndiswrapper; if
		 * we make it this far, kernel is sane */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(20 * HZ);
#endif
		clear_bit(SUSPEND_RESUME, &wd->wrapper_work);
		DBGTRACE2("continuing resume of device %s", net_dev->name);
		if (wd->hw_unavailable)
			WARNING("device %s: hw_unavailable: %d",
				net_dev->name, wd->hw_unavailable);
		if (test_and_clear_bit(HW_HALTED, &wd->hw_status)) {
			res = ndiswrapper_start_device(wd);
			DBGTRACE2("res: %08X", res);
			if (res)
				ERROR("initialization failed: %08X", res);
		} else if (test_and_clear_bit(HW_SUSPENDED, &wd->hw_status)) {
			res = miniport_set_pm_state(wd, NdisDeviceStateD0);
			DBGTRACE2("%s: setting power to state %d returns %08X",
				  net_dev->name, NdisDeviceStateD0, res);
			if (res)
				WARNING("No pnp capabilities for pm (%08X)",
					res);
		}
		hangcheck_add(wd);
		stats_timer_add(wd);
		set_scan(wd);
		set_bit(SET_ESSID, &wd->wrapper_work);

		if (netif_running(net_dev)) {
			netif_device_attach(net_dev);
			netif_start_queue(net_dev);
		}
		netif_poll_enable(net_dev);

		DBGTRACE2("%s: device resumed", net_dev->name);
	}
	TRACEEXIT3(return);
}

/* check capabilites - mainly for WPA */
void check_capa(struct wrapper_dev *wd)
{
	int i, mode;
	NDIS_STATUS res;
	struct ndis_assoc_info ndis_assoc_info;
	struct ndis_add_key ndis_key;
	struct ndis_capability *c;
	char *buf;
	const int buf_len = 512;

	TRACEENTER1("%s", "");

	/* check if WEP is supported */
	if (set_encr_mode(wd, Ndis802_11Encryption1Enabled) == 0 &&
	    get_encr_mode(wd) == Ndis802_11Encryption1KeyAbsent)
		set_bit(Ndis802_11Encryption1Enabled, &wd->capa.encr);

	/* check if WPA is supported */
	DBGTRACE1("%s", "");
	if (set_auth_mode(wd, Ndis802_11AuthModeWPA) == 0 &&
	    get_auth_mode(wd) == Ndis802_11AuthModeWPA)
		set_bit(Ndis802_11AuthModeWPA, &wd->capa.auth);
	else
		TRACEEXIT1(return);

	if (set_auth_mode(wd, Ndis802_11AuthModeWPAPSK) == 0 &&
	    get_auth_mode(wd) == Ndis802_11AuthModeWPAPSK)
		set_bit(Ndis802_11AuthModeWPAPSK, &wd->capa.auth);

	/* check for highest encryption */
	mode = 0;
	if (set_encr_mode(wd, Ndis802_11Encryption3Enabled) == 0 &&
	    (i = get_encr_mode(wd)) > 0 &&
	    (i == Ndis802_11Encryption3KeyAbsent ||
	     i == Ndis802_11Encryption3Enabled))
		mode = Ndis802_11Encryption3Enabled;
	else if (set_encr_mode(wd, Ndis802_11Encryption2Enabled) == 0 &&
		 (i = get_encr_mode(wd)) > 0 &&
		 (i == Ndis802_11Encryption2KeyAbsent ||
		  i == Ndis802_11Encryption2Enabled))
		mode = Ndis802_11Encryption2Enabled;
	else if (set_encr_mode(wd, Ndis802_11Encryption1Enabled) == 0 &&
		 (i = get_encr_mode(wd)) > 0 &&
		 (i == Ndis802_11Encryption1KeyAbsent ||
		  i == Ndis802_11Encryption1Enabled))
		mode = Ndis802_11Encryption1Enabled;

	if (mode == 0)
		TRACEEXIT1(return);
	set_bit(Ndis802_11Encryption1Enabled, &wd->capa.encr);
	if (mode == Ndis802_11Encryption1Enabled)
		TRACEEXIT1(return);

	ndis_key.length = 32;
	ndis_key.index = 0xC0000001;
	ndis_key.struct_size = sizeof(ndis_key);
	res = miniport_set_info(wd, OID_802_11_ADD_KEY, &ndis_key,
				ndis_key.struct_size);

	DBGTRACE2("add key returns %08X, size = %lu",
		  res, (unsigned long)sizeof(ndis_key));
	if (res != NDIS_STATUS_INVALID_DATA)
		TRACEEXIT1(return);
	res = miniport_query_info(wd, OID_802_11_ASSOCIATION_INFORMATION,
				  &ndis_assoc_info, sizeof(ndis_assoc_info));
	DBGTRACE2("assoc info returns %d", res);
	if (res == NDIS_STATUS_NOT_SUPPORTED)
		TRACEEXIT1(return);

	set_bit(Ndis802_11Encryption2Enabled, &wd->capa.encr);
	if (mode == Ndis802_11Encryption3Enabled)
		set_bit(Ndis802_11Encryption3Enabled, &wd->capa.encr);
	/* not all drivers support OID_802_11_CAPABILITY, so we don't
	 * know for sure if driver support WPA or WPAPSK; assume
	 * WPA */
	set_bit(Ndis802_11AuthModeWPA, &wd->capa.auth);

	/* check for wpa2 */
	buf = kmalloc(buf_len, GFP_KERNEL);
	if (!buf) {
		ERROR("couldn't allocate memory");
		TRACEEXIT1(return);
	}
	memset(buf, 0, buf_len);
	c = (struct ndis_capability *)buf;
	res = miniport_query_info(wd, OID_802_11_CAPABILITY, buf, buf_len);
	if (!(res == NDIS_STATUS_SUCCESS && c->version == 2)) {
		DBGTRACE1("res: %X", res);
		kfree(buf);
		TRACEEXIT1(return);
	}
	wd->num_pmkids = c->num_PMKIDs;

	for (i = 0; i < c->num_auth_encr_pair; i++) {
		struct ndis_auth_encr_pair *ae;

		ae = &c->auth_encr_pair[i];
		if ((char *)(ae + 1) > buf + buf_len)
			break;
		switch (ae->auth_mode) {
		case Ndis802_11AuthModeOpen:
		case Ndis802_11AuthModeShared:
		case Ndis802_11AuthModeWPA:
		case Ndis802_11AuthModeWPAPSK:
		case Ndis802_11AuthModeWPANone:
		case Ndis802_11AuthModeWPA2:
		case Ndis802_11AuthModeWPA2PSK:
			set_bit(ae->auth_mode, &wd->capa.auth);
			break;
		default:
			WARNING("unknown auth_mode: %d", ae->auth_mode);
			break;
		}
		switch (ae->encr_mode) {
		case Ndis802_11EncryptionDisabled:
		case Ndis802_11Encryption1Enabled:
		case Ndis802_11Encryption2Enabled:
		case Ndis802_11Encryption3Enabled:
			set_bit(ae->encr_mode, &wd->capa.encr);
			break;
		default:
			WARNING("unknown encr_mode: %d", ae->encr_mode);
			break;
		}
	}
	kfree(buf);
	TRACEEXIT1(return);
}

int ndis_reinit(struct wrapper_dev *wd)
{
	ndiswrapper_stop_device(wd);
	ndiswrapper_start_device(wd);
	return 0;
}

static int ndis_set_mac_addr(struct net_device *dev, void *p)
{
	struct wrapper_dev *wd = dev->priv;
	struct sockaddr *addr = p;
	struct ndis_config_param param;
	struct unicode_string key;
	struct ansi_string ansi;
	unsigned int i;
	NDIS_STATUS res;
	unsigned char mac_string[3 * ETH_ALEN];
	mac_address mac;

	/* string <-> ansi <-> unicode conversion is driving me nuts */

	for (i = 0; i < sizeof(mac); i++)
		mac[i] = addr->sa_data[i];
	memset(mac_string, 0, sizeof(mac_string));
	res = snprintf(mac_string, sizeof(mac_string), MACSTR,
		       MAC2STR(mac));
	DBGTRACE2("res = %d, mac_tring = %s", res, mac_string);
	if (res != (sizeof(mac_string) - 1))
		TRACEEXIT1(return -EINVAL);

	ansi.buf = "mac_address";
	ansi.buflen = ansi.len = strlen(ansi.buf);
	if (RtlAnsiStringToUnicodeString(&key, &ansi, 1))
		TRACEEXIT1(return -EINVAL);

	ansi.buf = mac_string;
	ansi.buflen = ansi.len = sizeof(mac_string);
	if (RtlAnsiStringToUnicodeString(&param.data.ustring, &ansi, 1) !=
	    NDIS_STATUS_SUCCESS) {
		RtlFreeUnicodeString(&key);
		TRACEEXIT1(return -EINVAL);
	}
	param.type = NDIS_CONFIG_PARAM_STRING;
	NdisWriteConfiguration(&res, wd->nmb, &key, &param);
	if (res != NDIS_STATUS_SUCCESS) {
		RtlFreeUnicodeString(&key);
		RtlFreeUnicodeString(&param.data.ustring);
		TRACEEXIT1(return -EINVAL);
	}
	ndis_reinit(wd);
	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	RtlFreeUnicodeString(&key);
	RtlFreeUnicodeString(&param.data.ustring);
	TRACEEXIT1(return 0);
}

int setup_device(struct net_device *dev)
{
	struct wrapper_dev *wd = dev->priv;
	NDIS_STATUS res;
	mac_address mac;
	int i;

	if (strlen(if_name) > (IFNAMSIZ-1)) {
		ERROR("interface name '%s' is too long", if_name);
		return -1;
	}
	strncpy(dev->name, if_name, IFNAMSIZ-1);
	dev->name[IFNAMSIZ-1] = '\0';

	DBGTRACE1("%s: querying for mac", DRIVER_NAME);
	res = miniport_query_info(wd, OID_802_3_CURRENT_ADDRESS,
				  mac, sizeof(mac));
	if (res) {
		ERROR("%s", "unable to get mac address from driver");
		return -EINVAL;
	}
	DBGTRACE1("mac:" MACSTR, MAC2STR(mac));
	memcpy(&dev->dev_addr, mac, ETH_ALEN);

	dev->open = ndis_open;
	dev->hard_start_xmit = start_xmit;
	dev->stop = ndis_close;
	dev->get_stats = ndis_get_stats;
	dev->do_ioctl = ndis_ioctl;
	dev->get_wireless_stats = get_wireless_stats;
	dev->wireless_handlers	= (struct iw_handler_def *)&ndis_handler_def;
	dev->set_multicast_list = ndis_set_rx_mode;
	dev->set_mac_address = ndis_set_mac_addr;
#ifdef HAVE_ETHTOOL
	dev->ethtool_ops = &ndis_ethtool_ops;
#endif
	if (wd->ndis_irq)
		dev->irq = wd->ndis_irq->irq.irq;
	dev->mem_start = wd->mem_start;
	dev->mem_end = wd->mem_end;

	res = register_netdev(dev);
	if (res) {
		ERROR("cannot register net device %s", dev->name);
		return res;
	}

	printk(KERN_INFO "%s: %s ethernet device " MACSTR " using driver %s,"
	       " configuration file %s\n",
	       dev->name, DRIVER_NAME, MAC2STR(dev->dev_addr),
	       wd->driver->name, wd->ndis_device->conf_file_name);

//	netif_stop_queue(dev);

	check_capa(wd);
//	debug = 2;

	DBGTRACE1("capbilities = %ld", wd->capa.encr);
	printk(KERN_INFO "%s: encryption modes supported: %s%s%s%s%s%s%s\n",
	       dev->name,
	       test_bit(Ndis802_11Encryption1Enabled, &wd->capa.encr) ?
	       "WEP" : "none",

	       test_bit(Ndis802_11Encryption2Enabled, &wd->capa.encr) ?
	       "; TKIP with WPA" : "",
	       test_bit(Ndis802_11AuthModeWPA2, &wd->capa.auth) ?
	       ", WPA2" : "",
	       test_bit(Ndis802_11AuthModeWPA2PSK, &wd->capa.auth) ?
	       ", WPA2PSK" : "",

	       test_bit(Ndis802_11Encryption3Enabled, &wd->capa.encr) ?
	       "; AES/CCMP with WPA" : "",
	       test_bit(Ndis802_11AuthModeWPA2, &wd->capa.auth) ?
	       ", WPA2" : "",
	       test_bit(Ndis802_11AuthModeWPA2PSK, &wd->capa.auth) ?
	       ", WPA2PSK" : "");

	wd->max_send_packets = 1;
	if (wd->driver->miniport.send_packets) {
		res = miniport_query_int(wd, OID_GEN_MAXIMUM_SEND_PACKETS,
					 &wd->max_send_packets);
		DBGTRACE2("maximum send packets supported by driver: %d",
			  wd->max_send_packets);
		if (res == NDIS_STATUS_NOT_SUPPORTED)
			wd->max_send_packets = 1;
		else if (wd->max_send_packets > XMIT_RING_SIZE)
			wd->max_send_packets = XMIT_RING_SIZE;

		wd->xmit_array = kmalloc(sizeof(struct ndis_packet *) *
					     wd->max_send_packets,
					     GFP_KERNEL);
		if (!wd->xmit_array) {
			ERROR("couldn't allocate memory for tx_packets");
			unregister_netdev(dev);
			return -ENOMEM;
		}
	}
	DBGTRACE2("maximum send packets used by ndiswrapper: %d",
		  wd->max_send_packets);

	res = miniport_query_int(wd, OID_802_3_MAXIMUM_LIST_SIZE, &i);
	if (res == NDIS_STATUS_SUCCESS) {
		DBGTRACE1("Multicast list size is %d", i);
		wd->multicast_list_size = i;
	}

	if (wd->multicast_list_size)
		wd->multicast_list =
			kmalloc(wd->multicast_list_size * ETH_ALEN,
				GFP_KERNEL);

	if (set_privacy_filter(wd, Ndis802_11PrivFilterAcceptAll))
		WARNING("%s", "Unable to set privacy filter");

	/* check_capa changes auth_mode and encr_mode, so set them again */

	set_infra_mode(wd, Ndis802_11Infrastructure);
	miniport_set_int(wd, OID_802_11_NETWORK_TYPE_IN_USE,
			 Ndis802_11Automode);
	/* some cards (e.g., RaLink) need a scan before they can associate */
	set_scan(wd);
	set_auth_mode(wd, Ndis802_11AuthModeOpen);
	set_encr_mode(wd, Ndis802_11EncryptionDisabled);
	set_essid(wd, "", 0);
//	ndis_set_rx_mode(dev);

	hangcheck_add(wd);
	stats_timer_add(wd);
	ndiswrapper_procfs_add_iface(wd);

	return 0;
}

struct net_device *ndis_init_netdev(struct wrapper_dev **pwd,
				    struct ndis_device *device,
				    struct ndis_driver *driver)
{
	struct net_device *dev;
	struct wrapper_dev *wd;

	dev = alloc_etherdev(sizeof(*wd));
	if (!dev) {
		ERROR("%s", "Unable to alloc etherdev");
		return NULL;
	}
	SET_MODULE_OWNER(dev);
	wd = dev->priv;
	DBGTRACE1("wd= %p", wd);

	wd->nmb = kmalloc(sizeof(struct ndis_miniport_block), GFP_KERNEL);
	if (!wd->nmb) {
		ERROR("couldn't allocate memory");
		free_netdev(dev);
		return NULL;
	}
	memset(wd->nmb, 0, sizeof(struct ndis_miniport_block));
	wd->nmb->wd = wd;
	wd->nmb->filterdbs.eth_db = wd->nmb;
	wd->nmb->filterdbs.tr_db = wd->nmb;
	wd->nmb->filterdbs.fddi_db = wd->nmb;
	wd->nmb->filterdbs.arc_db = wd->nmb;

	wd->driver = driver;
	wd->ndis_device = device;
	wd->net_dev = dev;
	wd->ndis_irq = NULL;
	kspin_lock_init(&wd->xmit_lock);
	init_MUTEX(&wd->ndis_comm_mutex);
	init_waitqueue_head(&wd->ndis_comm_wq);
	wd->ndis_comm_done = 0;
	/* don't send packets until the card is associated */
	wd->send_ok = 0;
	INIT_WORK(&wd->xmit_work, xmit_worker, wd);
	wd->xmit_ring_start = 0;
	wd->xmit_ring_pending = 0;
	kspin_lock_init(&wd->send_packet_done_lock);
	wd->encr_mode = Ndis802_11EncryptionDisabled;
	wd->auth_mode = Ndis802_11AuthModeOpen;
	wd->capa.encr = 0;
	wd->capa.auth = 0;
	wd->attributes = 0;
	wd->reset_status = 0;
	InitializeListHead(&wd->wrap_timer_list);
	kspin_lock_init(&wd->timer_lock);
	wd->map_count = 0;
	wd->map_dma_addr = NULL;
	wd->nick[0] = 0;
	wd->hangcheck_interval = hangcheck_interval;
	wd->hangcheck_active = 0;
	wd->scan_timestamp = 0;
	wd->hw_status = 0;
	wd->wrapper_work = 0;
	memset(&wd->essid, 0, sizeof(wd->essid));
	memset(&wd->encr_info, 0, sizeof(wd->encr_info));
	wd->infrastructure_mode = Ndis802_11Infrastructure;
	INIT_WORK(&wd->wrapper_worker, wrapper_worker_proc, wd);
	wd->hw_unavailable = 0;

	*pwd = wd;
	return dev;
}

static void module_cleanup(void)
{
	loader_exit();
	ndiswrapper_procfs_remove();
	ndis_exit();
	ntoskernel_exit();
	misc_funcs_exit();
}

static int __init wrapper_init(void)
{
	char *argv[] = {"loadndisdriver", 
#if defined DEBUG && DEBUG >= 1
			"1"
#else
			"0"
#endif
			, UTILS_VERSION, "-a", 0};
	char *env[] = {NULL};
	int err;

	debug = 0;
	spin_lock_init(&spinlock_kspin_lock);
	printk(KERN_INFO "%s version %s loaded (preempt=%s,smp=%s)\n",
	       DRIVER_NAME, DRIVER_VERSION,
#if defined CONFIG_PREEMPT
	       "yes",
#else
	       "no",
#endif
#ifdef CONFIG_SMP
	       "yes"
#else
	       "no"
#endif
		);

	if (misc_funcs_init() || ntoskernel_init() || ndis_init() ||
	    loader_init()
#ifdef CONFIG_USB
	     || usb_init()
#endif
		) {
		module_cleanup();
		ERROR("couldn't initialize %s", DRIVER_NAME);
		TRACEEXIT1(return -EPERM);
	}
	ndiswrapper_procfs_init();
	DBGTRACE1("%s", "calling loadndisdriver");
	err = call_usermodehelper("/sbin/loadndisdriver", argv, env
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
				  , 1
#endif
		);
	if (err) {
		ERROR("loadndiswrapper failed (%d); check system log "
		      "for messages from 'loadndisdriver'", err);
		module_cleanup();
		TRACEEXIT1(return -EPERM);
	}
	TRACEEXIT1(return 0);
}

static void __exit wrapper_exit(void)
{
	TRACEENTER1("");
	module_cleanup();
}

module_init(wrapper_init);
module_exit(wrapper_exit);

MODULE_LICENSE("GPL");
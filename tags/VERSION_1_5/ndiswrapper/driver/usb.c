/*
 *  Copyright (C) 2004 Jan Kiszka
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

#include "ndis.h"
#include "usb.h"

#ifdef USB_DEBUG
static unsigned int urb_id = 0;
#endif

#define DUMP_WRAP_URB(wrap_urb, dir)					\
	USBTRACE("urb %p (%d) %s: buf: %p, len: %d, pipe: %u",	\
		 (wrap_urb)->urb, (wrap_urb)->id,			\
		 (dir == 0) ? "going down" : "coming back",		\
		 (wrap_urb)->urb->transfer_buffer,			\
		 (wrap_urb)->urb->transfer_buffer_length,		\
		 (wrap_urb)->urb->pipe)

#define DUMP_BUFFER(buf, len)						\
	while (debug >= 2) {						\
		int __i;						\
		char __msg[100], *__t;					\
		if (!buf)						\
			break;						\
		__t = __msg;						\
		for (__i = 0; __i < len &&				\
			     __t < &__msg[sizeof(__msg) - 4]; __i++) {	\
			__t += sprintf(__t, "%02X ",			\
				       (((UCHAR *)buf)[__i]));		\
		}							\
		USBTRACE("%s", __msg);					\
		break;							\
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,5)
#define CUR_ALT_SETTING(intf) (intf)->cur_altsetting
#else
#define CUR_ALT_SETTING(intf) (intf)->altsetting[(intf)->act_altsetting]
#endif

#ifndef USB_CTRL_SET_TIMEOUT
#define USB_CTRL_SET_TIMEOUT 5000
#endif

static int inline wrap_cancel_urb(struct urb *urb)
{
	int ret;
	ret = usb_unlink_urb(urb);
	if (ret != -EINPROGRESS) {
		WARNING("unlink failed: %d", ret);
		return ret;
	} else
		return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

#define URB_NO_TRANSFER_DMA_MAP		0x0004
#define URB_STATUS(wrap_urb) (wrap_urb->urb_status)

static void *usb_buffer_alloc(struct usb_device *udev, size_t size,
			      unsigned mem_flags, dma_addr_t *dma)
{
	return kmalloc(size, mem_flags);
}

static void usb_buffer_free(struct usb_device *udev, size_t size, void *addr,
			    dma_addr_t dma)
{
	kfree(addr);
}

static void usb_init_urb(struct urb *urb)
{
	memset(urb, 0, sizeof(*urb));
}

#else

#define URB_STATUS(wrap_urb) (wrap_urb->urb->status)

#endif

extern KSPIN_LOCK irp_cancel_lock;

/* TODO: using a worker instead of tasklet is probably preferable,
 * since we don't know if IoCompleteRequest can be called from atomic
 * context, but with ZyDas driver the worker never gets executed, even
 * though it gets scheduled */
static struct tasklet_struct irp_complete_work;
static struct nt_list irp_complete_list;

static STDCALL void wrap_cancel_irp(struct device_object *dev_obj,
				    struct irp *irp);
static void irp_complete_worker(unsigned long data);

int usb_init(void)
{
	InitializeListHead(&irp_complete_list);
	tasklet_init(&irp_complete_work, irp_complete_worker, 0);
#ifdef USB_DEBUG
	urb_id = 0;
#endif
	return 0;
}

void usb_exit(void)
{
	tasklet_kill(&irp_complete_work);
	TRACEEXIT1(return);
}

int usb_init_device(struct wrapper_dev *wd)
{
	InitializeListHead(&wd->dev.usb.wrap_urb_list);
	wd->dev.usb.num_alloc_urbs = 0;
	return 0;
}

void usb_exit_device(struct wrapper_dev *wd)
{
	struct nt_list *ent;
	struct wrap_urb *wrap_urb;
	KIRQL irql;

	while (1) {
		IoAcquireCancelSpinLock(&irql);
		ent = RemoveHeadList(&wd->dev.usb.wrap_urb_list);
		IoReleaseCancelSpinLock(irql);
		if (!ent)
			break;
		wrap_urb = container_of(ent, struct wrap_urb, list);
		if (wrap_urb->state == URB_SUBMITTED) {
			WARNING("Windows driver %s didn't free urb: %p",
				wd->driver->name, wrap_urb->urb);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
			wrap_cancel_urb(wrap_urb->urb);
#else
			usb_kill_urb(wrap_urb->urb);
#endif
			wrap_urb->state = URB_FREE;
		}
		usb_free_urb(wrap_urb->urb);
		kfree(wrap_urb);
	}
	wd->dev.usb.num_alloc_urbs = 0;
	return;
}

/* for a given Linux urb status code, return corresponding NT urb status */
static USBD_STATUS wrap_urb_status(int urb_status)
{
	switch (urb_status) {
	case 0:
		return USBD_STATUS_SUCCESS;
	case -EPROTO:
		return USBD_STATUS_BTSTUFF;
	case -EILSEQ:
		return USBD_STATUS_CRC;
	case -EPIPE:
		return USBD_STATUS_INVALID_PIPE_HANDLE;
	case -ECOMM:
		return USBD_STATUS_DATA_OVERRUN;
	case -ENOSR:
		return USBD_STATUS_DATA_UNDERRUN;
	case -EOVERFLOW:
		return USBD_STATUS_BABBLE_DETECTED;
	case -EREMOTEIO:
		return USBD_STATUS_ERROR_SHORT_TRANSFER;;
	case -ENODEV:
	case -ESHUTDOWN:
		return USBD_STATUS_DEVICE_GONE;
	case -ENOMEM:
		return USBD_STATUS_NO_MEMORY;
	default:
		return USBD_STATUS_NOT_SUPPORTED;
	}
}

/* for a given USBD_STATUS, return its corresponding NTSTATUS (for irp) */
static NTSTATUS nt_urb_irp_status(USBD_STATUS nt_urb_status)
{
	switch (nt_urb_status) {
	case USBD_STATUS_SUCCESS:
		return STATUS_SUCCESS;
	case USBD_STATUS_DEVICE_GONE:
		return STATUS_DEVICE_REMOVED;
	case USBD_STATUS_PENDING:
		return STATUS_PENDING;
	case USBD_STATUS_NOT_SUPPORTED:
		return STATUS_NOT_IMPLEMENTED;
	case USBD_STATUS_NO_MEMORY:
		return STATUS_NO_MEMORY;
	default:
		return STATUS_FAILURE;
	}
}

static void wrap_free_urb(struct urb *urb)
{
	struct irp *irp;
	struct wrap_urb *wrap_urb;
	struct wrapper_dev *wd;

	USBTRACE("freeing urb: %p", urb);
	wrap_urb = urb->context;
	irp = wrap_urb->irp;
	IoAcquireCancelSpinLock(&irp->cancel_irql);
	wd = irp->wd;
	irp->cancel_routine = NULL;
	irp->wrap_urb = NULL;
	if (urb->transfer_buffer &&
	    (wrap_urb->alloc_flags & URB_NO_TRANSFER_DMA_MAP)) {
		USBTRACE("freeing DMA buffer for URB: %p %p",
			 urb, urb->transfer_buffer);
		usb_buffer_free(irp->wd->dev.usb.udev,
				urb->transfer_buffer_length, 
				urb->transfer_buffer, urb->transfer_dma);
	}
	if (urb->setup_packet)
		kfree(urb->setup_packet);
	if (wd->dev.usb.num_alloc_urbs > MAX_ALLOCATED_URBS) {
		RemoveEntryList(&wrap_urb->list);
		wd->dev.usb.num_alloc_urbs--;
		usb_free_urb(urb);
		kfree(wrap_urb);
	} else {
		wrap_urb->state = URB_FREE;
		wrap_urb->alloc_flags = 0;
		wrap_urb->irp = NULL;
	}
	if (wd->dev.usb.num_alloc_urbs < 0)
		WARNING("num_allocated_urbs: %d",
			wd->dev.usb.num_alloc_urbs);
	IoReleaseCancelSpinLock(irp->cancel_irql);
	return;
}

void wrap_suspend_urbs(struct wrapper_dev *wd)
{
	/* TODO: do we need to cancel urbs? */
}

void wrap_resume_urbs(struct wrapper_dev *wd)
{
	/* TODO: do we need to resubmit urbs? */
}

static struct urb *wrap_alloc_urb(struct irp *irp, unsigned int pipe,
				  void *buf, unsigned int buf_len)
{
	struct urb *urb;
	unsigned int alloc_flags;
	struct wrap_urb *wrap_urb;
	struct wrapper_dev *wd;

	if (current_irql() < DISPATCH_LEVEL)
		alloc_flags = GFP_KERNEL;
	else
		alloc_flags = GFP_ATOMIC;
	wd = irp->wd;
	IoAcquireCancelSpinLock(&irp->cancel_irql);
	urb = NULL;
	nt_list_for_each_entry(wrap_urb, &wd->dev.usb.wrap_urb_list, list) {
		if (wrap_urb->state == URB_FREE) {
			wrap_urb->state = URB_ALLOCATED;
			urb = wrap_urb->urb;
			usb_init_urb(urb);
			break;
		}
	}

	if (!urb) {
		IoReleaseCancelSpinLock(irp->cancel_irql);
		wrap_urb = kmalloc(sizeof(*wrap_urb), alloc_flags);
		if (!wrap_urb) {
			WARNING("couldn't allocate memory");
			return NULL;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
		urb = usb_alloc_urb(0, alloc_flags);
#else
		urb = usb_alloc_urb(0);
#endif
		if (!urb) {
			WARNING("couldn't allocate urb");
			kfree(wrap_urb);
			return NULL;
		}
		IoAcquireCancelSpinLock(&irp->cancel_irql);
		memset(wrap_urb, 0, sizeof(*wrap_urb));
		wrap_urb->urb = urb;
		wrap_urb->state = URB_ALLOCATED;
		InsertTailList(&wd->dev.usb.wrap_urb_list, &wrap_urb->list);
		wd->dev.usb.num_alloc_urbs++;
	}

#ifdef USB_ASYNC_UNLINK
	urb->transfer_flags |= URB_ASYNC_UNLINK;
#endif
	urb->context = wrap_urb;
	wrap_urb->irp = irp;
	wrap_urb->state = URB_ALLOCATED;
	wrap_urb->pipe = pipe;
	irp->wrap_urb = wrap_urb;
	irp->cancel_routine = wrap_cancel_irp;
	IoReleaseCancelSpinLock(irp->cancel_irql);
	USBTRACE("allocated urb: %p", urb);
	if (buf_len && buf) {
		if (virt_addr_valid(buf))
			urb->transfer_buffer = buf;
		else {
			urb->transfer_buffer =
				usb_buffer_alloc(irp->wd->dev.usb.udev,
						 buf_len, alloc_flags,
						 &urb->transfer_dma);
			if (!urb->transfer_buffer) {
				WARNING("couldn't allocate dma buf");
				IoAcquireCancelSpinLock(&irp->cancel_irql);
				wrap_urb->state = URB_FREE;
				wrap_urb->irp = NULL;
				irp->wrap_urb = NULL;
				IoReleaseCancelSpinLock(irp->cancel_irql);
				return NULL;
			}
			urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			wrap_urb->alloc_flags |= URB_NO_TRANSFER_DMA_MAP;
			if (usb_pipeout(pipe))
				memcpy(urb->transfer_buffer, buf, buf_len);
			USBTRACE("DMA buffer for urb %p is %p",
				 urb, urb->transfer_buffer);
		}
	} else
		urb->transfer_buffer = NULL;
	urb->transfer_buffer_length = buf_len;
	return urb;
}

NTSTATUS wrap_submit_urb(struct irp *irp)
{
	int ret;
	struct urb *urb;
	unsigned int alloc_flags;
	NTSTATUS status;
	union nt_urb *nt_urb;

	if (current_irql() < DISPATCH_LEVEL)
		alloc_flags = GFP_KERNEL;
	else
		alloc_flags = GFP_ATOMIC;
	IoAcquireCancelSpinLock(&irp->cancel_irql);
	urb = irp->wrap_urb->urb;
	nt_urb = URB_FROM_IRP(irp);
	if (irp->wrap_urb->state != URB_ALLOCATED) {
		ERROR("urb %p is in wrong state: %d",
		      urb, irp->wrap_urb->state);
		status = NT_URB_STATUS(nt_urb) = USBD_STATUS_REQUEST_FAILED;
		irp->io_status.status = STATUS_NOT_SUPPORTED;
		irp->io_status.status_info = 0;
		IoReleaseCancelSpinLock(irp->cancel_irql);
		return status;
	}
	irp->wrap_urb->state = URB_SUBMITTED;
#ifdef USB_DEBUG
	irp->wrap_urb->id = urb_id++;
#endif
	IoReleaseCancelSpinLock(irp->cancel_irql);
	DUMP_WRAP_URB(irp->wrap_urb, 0);
	irp->io_status.status = STATUS_PENDING;
	irp->io_status.status_info = 0;
	NT_URB_STATUS(nt_urb) = USBD_STATUS_PENDING;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	ret = usb_submit_urb(urb, alloc_flags);
#else
	ret = usb_submit_urb(urb);
#endif
	if (ret) {
		USBTRACE("ret: %d", ret);
		wrap_free_urb(urb);
		irp->io_status.status = STATUS_NOT_SUPPORTED;
		irp->io_status.status_info = 0;
		NT_URB_STATUS(nt_urb) = USBD_STATUS_REQUEST_FAILED;
		USBEXIT(return irp->io_status.status);
	} else
		USBEXIT(return STATUS_PENDING);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
static void wrap_urb_complete(struct urb *urb, struct pt_regs *regs)
#else
static void wrap_urb_complete(struct urb *urb)
#endif
{
	struct irp *irp;
	struct wrap_urb *wrap_urb;

	wrap_urb = urb->context;
	irp = wrap_urb->irp;
	IoAcquireCancelSpinLock(&irp->cancel_irql);
	DUMP_WRAP_URB(wrap_urb, 1);
	irp->cancel_routine = NULL;
	if (wrap_urb->state != URB_SUBMITTED &&
	    wrap_urb->state != URB_CANCELED)
		WARNING("urb %p in wrong state: %d", urb, wrap_urb->state);
	wrap_urb->state = URB_COMPLETED;
	/* To prevent 2.4 kernels from resubmiting interrupt URBs
	 * (Windows driver also resubmits them); UHCI doesn't resubmit
	 * URB if status == -ENOENT */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	if (usb_pipeint(urb->pipe)) {
		wrap_urb->urb_status = urb->status;
		urb->status = -ENOENT;
	}
#endif
	InsertTailList(&irp_complete_list, &irp->complete_list);
	IoReleaseCancelSpinLock(irp->cancel_irql);
	USBTRACE("urb %p (irp: %p) completed", urb, irp);
	tasklet_schedule(&irp_complete_work);
}

/* one worker for all devices */
static void irp_complete_worker(unsigned long data)
{
	struct irp *irp;
	struct urb *urb;
	struct usbd_bulk_or_intr_transfer *bulk_int_tx;
	struct usbd_vendor_or_class_request *vc_req;
	union nt_urb *nt_urb;
	struct wrap_urb *wrap_urb;
	struct nt_list *ent;
	KIRQL irql;

	while (1) {
		IoAcquireCancelSpinLock(&irql);
		ent = RemoveHeadList(&irp_complete_list);
		IoReleaseCancelSpinLock(irql);
		if (!ent)
			break;
		irp = container_of(ent, struct irp, complete_list);
		DUMP_IRP(irp);
		wrap_urb = irp->wrap_urb;
		urb = wrap_urb->urb;
		if (wrap_urb->state != URB_COMPLETED)
			WARNING("urb %p in wrong state: %d",
				urb, wrap_urb->state);
		nt_urb = URB_FROM_IRP(irp);
		USBTRACE("urb: %p, nt_urb: %p, status: %d",
			 urb, nt_urb, URB_STATUS(wrap_urb));
		switch (URB_STATUS(wrap_urb)) {
		case 0:
			/* succesfully transferred */
			NT_URB_STATUS(nt_urb) =
				wrap_urb_status(URB_STATUS(wrap_urb));
			irp->io_status.status = STATUS_SUCCESS;
			irp->io_status.status_info = urb->actual_length;

			/* from WDM examples, it seems we don't need
			 * to update MDL's byte count if MDL is used */
			switch (nt_urb->header.function) {
			case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
				bulk_int_tx = &nt_urb->bulk_int_transfer;
				bulk_int_tx->transfer_buffer_length =
					urb->actual_length;
				DUMP_BUFFER(urb->transfer_buffer,
					    urb->actual_length);
				if ((wrap_urb->alloc_flags &
				     URB_NO_TRANSFER_DMA_MAP) &&
				    usb_pipein(urb->pipe))
					memcpy(bulk_int_tx->transfer_buffer,
					       urb->transfer_buffer,
					       urb->actual_length);
				break;
			case URB_FUNCTION_VENDOR_DEVICE:
			case URB_FUNCTION_VENDOR_INTERFACE:
			case URB_FUNCTION_VENDOR_ENDPOINT:
			case URB_FUNCTION_VENDOR_OTHER:
			case URB_FUNCTION_CLASS_DEVICE:
			case URB_FUNCTION_CLASS_INTERFACE:
			case URB_FUNCTION_CLASS_ENDPOINT:
			case URB_FUNCTION_CLASS_OTHER:
				vc_req = &nt_urb->vendor_class_request;
				vc_req->transfer_buffer_length =
					urb->actual_length;
				DUMP_BUFFER(urb->transfer_buffer,
					    urb->actual_length);
				DUMP_BUFFER(urb->setup_packet,
					    sizeof(struct usb_ctrlrequest));
				if ((wrap_urb->alloc_flags &
				     URB_NO_TRANSFER_DMA_MAP) &&
				    usb_pipein(urb->pipe))
					memcpy(vc_req->transfer_buffer,
					       urb->transfer_buffer,
					       urb->actual_length);
				break;
			default:
				ERROR("nt_urb type: %d unknown",
				      nt_urb->header.function);
				break;
			}
			break;
		case -ENOENT:
		case -ECONNRESET:
			/* irp canceled */
			if (wrap_urb->state == URB_SUSPEND)
				continue;
			NT_URB_STATUS(nt_urb) = USBD_STATUS_CANCELLED;
			irp->io_status.status = STATUS_CANCELLED;
			irp->io_status.status_info = 0;
			USBTRACE("irp %p canceled", irp);
			break;
		default:
			NT_URB_STATUS(nt_urb) =
				wrap_urb_status(URB_STATUS(wrap_urb));
			irp->io_status.status =
				nt_urb_irp_status(NT_URB_STATUS(nt_urb));
			irp->io_status.status_info = 0;
			USBTRACE("irp: %p, status: %d", irp,
				 URB_STATUS(wrap_urb));
			break;
		}
		wrap_free_urb(urb);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
	return;
}

static STDCALL void wrap_cancel_irp(struct device_object *dev_obj,
				    struct irp *irp)
{
	struct urb *urb;

	/* NB: this function is called holding Cancel spinlock */
	USBENTER("irp: %p", irp);
	urb = irp->wrap_urb->urb;
	USBTRACE("canceling urb %p", urb);
	if (irp->wrap_urb->state == URB_SUBMITTED &&
	    wrap_cancel_urb(urb) == 0) {
		USBTRACE("urb %p canceled", urb);
		irp->wrap_urb->state = URB_CANCELED;
		/* this IRP will be returned in urb's completion function */
		IoReleaseCancelSpinLock(irp->cancel_irql);
	} else {
		union nt_urb *nt_urb;
		nt_urb = URB_FROM_IRP(irp);
		ERROR("urb %p not canceld: %d", urb, irp->wrap_urb->state);
		NT_URB_STATUS(nt_urb) = USBD_STATUS_REQUEST_FAILED;
		irp->io_status.status = STATUS_INVALID_PARAMETER;
		irp->io_status.status_info = 0;
		IoReleaseCancelSpinLock(irp->cancel_irql);
		wrap_free_urb(urb);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
}

static USBD_STATUS wrap_bulk_or_intr_trans(struct irp *irp)
{
	usbd_pipe_handle pipe_handle;
	struct urb *urb;
	unsigned int pipe;
	struct usbd_bulk_or_intr_transfer *bulk_int_tx;
	USBD_STATUS status;
	struct usb_device *udev;
	union nt_urb *nt_urb;

	nt_urb = URB_FROM_IRP(irp);
	udev = irp->wd->dev.usb.udev;
	bulk_int_tx = &nt_urb->bulk_int_transfer;
	USBTRACE("flags = %X, length = %u, buffer = %p",
		  bulk_int_tx->transfer_flags,
		  bulk_int_tx->transfer_buffer_length,
		  bulk_int_tx->transfer_buffer);

	DUMP_IRP(irp);
	pipe_handle = bulk_int_tx->pipe_handle;
	if (bulk_int_tx->transfer_flags & USBD_TRANSFER_DIRECTION_IN)
		pipe = usb_rcvbulkpipe(udev, pipe_handle->bEndpointAddress);
	else
		pipe = usb_sndbulkpipe(udev, pipe_handle->bEndpointAddress);

	if (unlikely(bulk_int_tx->transfer_buffer == NULL &&
		     bulk_int_tx->transfer_buffer_length > 0)) {
		if (MmGetMdlByteCount(bulk_int_tx->mdl) !=
		    bulk_int_tx->transfer_buffer_length)
			WARNING("mdl size %d != %d",
				MmGetMdlByteCount(bulk_int_tx->mdl),
				bulk_int_tx->transfer_buffer_length);
		bulk_int_tx->transfer_buffer =
			MmGetMdlVirtualAddress(bulk_int_tx->mdl);
	}

	urb = wrap_alloc_urb(irp, pipe, bulk_int_tx->transfer_buffer,
			     bulk_int_tx->transfer_buffer_length);
	if (!urb) {
		ERROR("couldn't allocate urb");
		return USBD_STATUS_NO_MEMORY;
	}
	if (usb_pipein(pipe) &&
	    (!(bulk_int_tx->transfer_flags & USBD_SHORT_TRANSFER_OK))) {
		USBTRACE("short not ok");
		urb->transfer_flags |= URB_SHORT_NOT_OK;
	}

	switch(usb_pipetype(pipe)) {
	case USB_ENDPOINT_XFER_BULK:
		usb_fill_bulk_urb(urb, udev, pipe, urb->transfer_buffer,
				  bulk_int_tx->transfer_buffer_length,
				  wrap_urb_complete, urb->context);
		USBTRACE("submitting bulk urb %p on pipe %u",
			 urb, pipe_handle->bEndpointAddress);
		status = USBD_STATUS_PENDING;
		break;
	case USB_ENDPOINT_XFER_INT:
		usb_fill_int_urb(urb, udev, pipe, urb->transfer_buffer,
				 bulk_int_tx->transfer_buffer_length,
				 wrap_urb_complete, urb->context,
				 pipe_handle->bInterval);
		USBTRACE("submitting interrupt urb %p on pipe %u, intvl: %d",
			 urb, pipe_handle->bEndpointAddress,
			 pipe_handle->bInterval);
		status = USBD_STATUS_PENDING;
		break;
	default:
		ERROR("unknown pipe type: %u", pipe_handle->bEndpointAddress);
		status = USBD_STATUS_NOT_SUPPORTED;
		break;
	}
	USBEXIT(return status);
}

static USBD_STATUS wrap_vendor_or_class_req(struct irp *irp)
{
	struct urb *urb;
	struct usb_ctrlrequest *dr;
	char req_type;
	unsigned int pipe;
	struct usbd_vendor_or_class_request *vc_req;
	struct usb_device *udev;
	union nt_urb *nt_urb;
	USBD_STATUS status;

	nt_urb = URB_FROM_IRP(irp);
	udev = irp->wd->dev.usb.udev;
	vc_req = &nt_urb->vendor_class_request;
	USBTRACE("bits = %x, req = %x, val = %08x, index = %08x, flags = %x,"
		 "tx_buf = %p, tx_buf_len = %d", vc_req->reserved_bits,
		 vc_req->request, vc_req->value, vc_req->index,
		 vc_req->transfer_flags, vc_req->transfer_buffer,
		 vc_req->transfer_buffer_length);

	switch (nt_urb->header.function) {
	case URB_FUNCTION_VENDOR_DEVICE:
		req_type = USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		break;
	case URB_FUNCTION_VENDOR_INTERFACE:
		req_type = USB_TYPE_VENDOR | USB_RECIP_INTERFACE;
		break;
	case URB_FUNCTION_VENDOR_ENDPOINT:
		req_type = USB_TYPE_VENDOR | USB_RECIP_ENDPOINT;
		break;
	case URB_FUNCTION_VENDOR_OTHER:
		req_type = USB_TYPE_VENDOR | USB_RECIP_OTHER;
		break;
	case URB_FUNCTION_CLASS_DEVICE:
		req_type = USB_TYPE_CLASS | USB_RECIP_DEVICE;
		break;
	case URB_FUNCTION_CLASS_INTERFACE:
		req_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
		break;
	case URB_FUNCTION_CLASS_ENDPOINT:
		req_type = USB_TYPE_CLASS | USB_RECIP_ENDPOINT;
		break;
	case URB_FUNCTION_CLASS_OTHER:
		req_type = USB_TYPE_CLASS | USB_RECIP_OTHER;
		break;
	default:
		ERROR("unknown request type: %x", nt_urb->header.function);
		req_type = 0;
		break;
	}

	req_type |= vc_req->reserved_bits;
	USBTRACE("req type: %08x", req_type);

	if (unlikely(vc_req->transfer_buffer == NULL &&
		     vc_req->transfer_buffer_length > 0)) {
		if (MmGetMdlByteCount(vc_req->mdl) !=
		    vc_req->transfer_buffer_length)
			WARNING("mdl size %d != %d",
				MmGetMdlByteCount(vc_req->mdl),
				vc_req->transfer_buffer_length);
		vc_req->transfer_buffer = MmGetMdlVirtualAddress(vc_req->mdl);
	}

	if (vc_req->transfer_flags & USBD_TRANSFER_DIRECTION_IN) {
		pipe = usb_rcvctrlpipe(udev, 0);
		req_type |= USB_DIR_IN;
		USBTRACE("pipe: %u, dir in", pipe);
	} else {
		pipe = usb_sndctrlpipe(udev, 0);
		req_type |= USB_DIR_OUT;
		USBTRACE("pipe: %u, dir out", pipe);
	}

	urb = wrap_alloc_urb(irp, pipe, vc_req->transfer_buffer,
			     vc_req->transfer_buffer_length);
	if (!urb) {
		ERROR("couldn't allocate urb");
		return USBD_STATUS_NO_MEMORY;
	}

	if (usb_pipein(pipe) &&
	    (!(vc_req->transfer_flags & USBD_SHORT_TRANSFER_OK))) {
		USBTRACE("short not ok");
		urb->transfer_flags |= URB_SHORT_NOT_OK;
	}

	dr = kmalloc(sizeof(*dr), GFP_ATOMIC);
	if (!dr) {
		ERROR("couldn't allocate memory");
		wrap_free_urb(urb);
		return USBD_STATUS_NO_MEMORY;
	}
	memset(dr, 0, sizeof(*dr));
	dr->bRequestType = req_type;
	dr->bRequest = vc_req->request;
	dr->wValue = cpu_to_le16(vc_req->value);
	dr->wIndex = cpu_to_le16(vc_req->index);
	dr->wLength = cpu_to_le16(vc_req->transfer_buffer_length);

	usb_fill_control_urb(urb, udev, pipe, (unsigned char *)dr,
			     urb->transfer_buffer,
			     vc_req->transfer_buffer_length,
			     wrap_urb_complete, urb->context);

	status = USBD_STATUS_PENDING;
	USBEXIT(return status);
}

static USBD_STATUS wrap_reset_pipe(struct usb_device *udev, struct irp *irp)
{
	unsigned int pipe1, pipe2;
	int ret;
	union nt_urb *nt_urb;
	usbd_pipe_handle pipe_handle;
	enum pipe_type pipe_type;

	USBTRACE("irp = %p", irp);
	nt_urb = URB_FROM_IRP(irp);

	pipe_handle = nt_urb->pipe_req.pipe_handle;
	pipe_type = pipe_handle->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
	/* TODO: not clear if both directions should be cleared? */
	if (pipe_type == USB_ENDPOINT_XFER_BULK) {
		pipe1 = usb_rcvbulkpipe(udev, pipe_handle->bEndpointAddress);
		pipe2 = usb_sndbulkpipe(udev, pipe_handle->bEndpointAddress);
	} else if (pipe_type == USB_ENDPOINT_XFER_INT) {
		pipe1 = usb_rcvintpipe(udev, pipe_handle->bEndpointAddress);
		/* no outgoing interrupt pipes */
		pipe2 = pipe1;
	} else if (pipe_type == USB_ENDPOINT_XFER_CONTROL) {
		pipe1 = usb_rcvctrlpipe(udev, pipe_handle->bEndpointAddress);
		pipe2 = usb_sndctrlpipe(udev, pipe_handle->bEndpointAddress);
	} else {
		WARNING("pipe type %d not handled", pipe_type);
		return USBD_STATUS_SUCCESS;
	}
	ret = usb_clear_halt(udev, pipe1);
	if (ret)
		WARNING("resetting pipe %d failed: %d", pipe_type, ret);
	if (pipe2 != pipe1) {
		ret = usb_clear_halt(udev, pipe2);
		if (ret)
			WARNING("resetting pipe %d failed: %d",
				pipe_type, ret);
	}
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_abort_pipe(struct usb_device *udev, struct irp *irp)
{
	union nt_urb *nt_urb;
	usbd_pipe_handle pipe_handle;
	unsigned long flags;
	struct urb *urb;
	struct wrap_urb *wrap_urb;
	struct wrapper_dev *wd;

	USBENTER("irp = %p", irp);
	wd = irp->wd;
	nt_urb = URB_FROM_IRP(irp);
	pipe_handle = nt_urb->pipe_req.pipe_handle;

	nt_urb = URB_FROM_IRP(irp);
	while (1) {
		kspin_lock_irqsave(&irp_cancel_lock, flags);
		urb = NULL;
		nt_list_for_each_entry(wrap_urb, &wd->dev.usb.wrap_urb_list,
				       list) {
			if (wrap_urb->state == URB_SUBMITTED &&
			    (usb_pipeendpoint(wrap_urb->pipe) ==
			     pipe_handle->bEndpointAddress)) {
				wrap_urb->state = URB_CANCELED;
				urb = wrap_urb->urb;
				break;
			}
		}
		kspin_unlock_irqrestore(&irp_cancel_lock, flags);
		if (urb) {
			wrap_cancel_urb(urb);
			USBTRACE("canceled urb: %p", urb);
		} else
			break;
	}
	USBEXIT(return USBD_STATUS_SUCCESS);
}

static USBD_STATUS wrap_select_configuration(struct wrapper_dev *wd,
					     union nt_urb *nt_urb,
					     struct irp *irp)
{
	struct usbd_pipe_information *pipe;
	int i, n, ret, pipe_num;
	struct usb_endpoint_descriptor *ep;
	struct usbd_select_configuration *sel_conf;
	struct usb_device *udev;
	struct usbd_interface_information *intf;
	struct usb_config_descriptor *config;
	struct usb_interface *usb_intf;

	udev = wd->dev.usb.udev;
	sel_conf = &nt_urb->select_conf;
	config = sel_conf->config;
	if (config == NULL) {
		/* TODO: set to unconfigured state (configuration 0):
		 * is this correctt? */
		ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				      USB_REQ_SET_CONFIGURATION, 0,
				      0, 0, NULL, 0, USB_CTRL_SET_TIMEOUT);
		return wrap_urb_status(ret);
	}

	USBTRACE("conf: %d, type: %d, length: %d, numif: %d, attr: %08x",
		 config->bConfigurationValue, config->bDescriptorType,
		 config->wTotalLength, config->bNumInterfaces,
		 config->bmAttributes);

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      USB_REQ_SET_CONFIGURATION, 0,
			      config->bConfigurationValue, 0,
			      NULL, 0, USB_CTRL_SET_TIMEOUT);
	if (ret < 0) {
		ERROR("ret: %d", ret);
		return wrap_urb_status(ret);
	}

	pipe_num = 0;
	intf = &sel_conf->intf;
	for (n = 0; n < config->bNumInterfaces && intf->bLength > 0;
	     n++, intf = (((void *)intf) + intf->bLength)) {

		USBTRACE("intf: %d, alt setting: %d",
			 intf->bInterfaceNumber, intf->bAlternateSetting);
		ret = usb_set_interface(udev, intf->bInterfaceNumber,
					intf->bAlternateSetting);
		if (ret < 0) {
			ERROR("failed with %d", ret);
			return wrap_urb_status(ret);
		}
		usb_intf = usb_ifnum_to_if(udev, intf->bInterfaceNumber);
		if (!usb_intf) {
			ERROR("couldn't obtain ifnum");
			return USBD_STATUS_REQUEST_FAILED;
		}
		USBTRACE("intf: %p, num ep: %d", intf, intf->bNumEndpoints);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
		for (i = 0; i < CUR_ALT_SETTING(usb_intf)->desc.bNumEndpoints;
		     i++, pipe_num++) {
			ep = &(CUR_ALT_SETTING(usb_intf)->endpoint + i)->desc;
#else
		for (i = 0; i < CUR_ALT_SETTING(usb_intf).bNumEndpoints;
		     i++, pipe_num++) {
			ep = &((CUR_ALT_SETTING(usb_intf)).endpoint[i]);
#endif
			if (i >= intf->bNumEndpoints) {
				ERROR("intf %p has only %d endpoints, "
				      "ignoring endpoints above %d",
				      intf, intf->bNumEndpoints, i);
				break;
			}
			pipe = &intf->pipes[i];

			if (pipe->flags & USBD_PF_CHANGE_MAX_PACKET)
				WARNING("pkt_sz: %d: %d", pipe->wMaxPacketSize,
					pipe->max_tx_size);
			USBTRACE("driver wants max_tx_size to %d",
				 pipe->max_tx_size);

			pipe->wMaxPacketSize = ep->wMaxPacketSize;
			pipe->bEndpointAddress = ep->bEndpointAddress;
			pipe->bInterval = ep->bInterval;
			pipe->type =
				ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

			pipe->handle = ep;
			USBTRACE("%d: addr %X, type %d, pkt_sz %d, intv %d,"
				 "handle %p", i, ep->bEndpointAddress,
				 ep->bmAttributes, ep->wMaxPacketSize,
				 ep->bInterval, pipe->handle);
		}
	}
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_get_descriptor(struct wrapper_dev *wd,
				       union nt_urb *nt_urb, struct irp *irp)
{
	struct usbd_control_descriptor_request *ctrl_req;
	int ret = 0;
	struct usb_device *udev;

	udev = wd->dev.usb.udev;
	ctrl_req = &nt_urb->control_request;
	USBTRACE("desctype = %d, descindex = %d, transfer_buffer = %p,"
		 "transfer_buffer_length = %d", ctrl_req->desc_type,
		 ctrl_req->index, ctrl_req->transfer_buffer,
		 ctrl_req->transfer_buffer_length);

	if (ctrl_req->desc_type == USB_DT_STRING) {
		USBTRACE("langid: %d", ctrl_req->language_id);
		ret = usb_get_string(udev, ctrl_req->language_id,
				     ctrl_req->index,
				     ctrl_req->transfer_buffer,
				     ctrl_req->transfer_buffer_length);
	} else {
		ret = usb_get_descriptor(udev, ctrl_req->desc_type,
					 ctrl_req->index,
					 ctrl_req->transfer_buffer,
					 ctrl_req->transfer_buffer_length);
	}
	if (ret < 0) {
		WARNING("request %d failed with %d", ctrl_req->desc_type, ret);
		ctrl_req->transfer_buffer_length = 0;
		return USBD_STATUS_REQUEST_FAILED;
	} else {
		USBTRACE("ret: %08x", ret);
		DUMP_BUFFER(ctrl_req->transfer_buffer, ret);
		ctrl_req->transfer_buffer_length = ret;
		irp->io_status.status_info = ret;
		return USBD_STATUS_SUCCESS;
	}
}

static USBD_STATUS wrap_process_nt_urb(struct irp *irp)
{
	union nt_urb *nt_urb;
	struct usb_device *udev;
	USBD_STATUS status;
	struct wrapper_dev *wd;

	wd = irp->wd;
	udev = wd->dev.usb.udev;
	nt_urb = URB_FROM_IRP(irp);
	USBENTER("nt_urb = %p, irp = %p, length = %d, function = %x",
		 nt_urb, irp, nt_urb->header.length, nt_urb->header.function);

	DUMP_IRP(irp);
	switch (nt_urb->header.function) {
		/* bulk/int and vendor/class urbs are submitted
		 * asynchronously later by pdoDispatchDeviceControl */
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		USBTRACE("submitting bulk/int irp: %p", irp);
		status = wrap_bulk_or_intr_trans(irp);
		break;

	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
	case URB_FUNCTION_VENDOR_OTHER:
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
		USBTRACE("submitting vendor/class irp: %p", irp);
		status = wrap_vendor_or_class_req(irp);
		break;

		/* rest are synchronous */
	case URB_FUNCTION_SELECT_CONFIGURATION:
		status = wrap_select_configuration(wd, nt_urb, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
		status = wrap_get_descriptor(wd, nt_urb, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		status = wrap_reset_pipe(udev, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_ABORT_PIPE:
		status = wrap_abort_pipe(udev, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	default:
		ERROR("function %x not implemented", nt_urb->header.function);
		status = NT_URB_STATUS(nt_urb) = USBD_STATUS_NOT_SUPPORTED;
		break;
	}
	USBTRACE("status: %08X", status);
	return status;
}

static USBD_STATUS wrap_reset_port(struct irp *irp)
{
	int ret;
	struct wrapper_dev *wd;

	wd = irp->wd;
	USBENTER("%p, %p", wd, wd->dev.usb.udev);

	ret = usb_reset_device(wd->dev.usb.udev);
	if (ret < 0)
		WARNING("reset failed: %d", ret);
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_get_port_status(struct irp *irp)
{
	struct wrapper_dev *wd;
	ULONG *status;

	wd = irp->wd;
	USBENTER("%p, %p", wd, wd->dev.usb.udev);
	status = IoGetCurrentIrpStackLocation(irp)->params.others.arg1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	{
		enum usb_device_state state;
		state = wd->dev.usb.udev->state;
		if (state != USB_STATE_NOTATTACHED &&
		    state != USB_STATE_SUSPENDED) {
			*status |= USBD_PORT_CONNECTED;
			if (state == USB_STATE_CONFIGURED)
				*status |= USBD_PORT_ENABLED;
		}
	}
#else
	{
		/* TODO: how to get current status? */
		*status = USBD_PORT_CONNECTED | USBD_PORT_ENABLED;
	}
#endif
	return USBD_STATUS_SUCCESS;
}

NTSTATUS wrap_submit_irp(struct device_object *pdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct wrapper_dev *wd;
	USBD_STATUS status;

	irp_sl = IoGetCurrentIrpStackLocation(irp);
	wd = pdo->reserved;

	if (unlikely(wd->dev.usb.intf == NULL)) {
		irp->io_status.status = STATUS_DEVICE_REMOVED;
		irp->io_status.status_info = 0;
		return irp->io_status.status;
	}

	irp->wd = wd;
	switch (irp_sl->params.ioctl.code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		status = wrap_process_nt_urb(irp);
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
		status = wrap_reset_port(irp);
		break;
	case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
		status = wrap_get_port_status(irp);
		break;
	default:
 		ERROR("ioctl %08X NOT IMPLEMENTED", irp_sl->params.ioctl.code);
		status = USBD_STATUS_NOT_SUPPORTED;
		break;
	}

	USBTRACE("status: %08X", status);
	if (status == USBD_STATUS_PENDING)
		return STATUS_PENDING;
	irp->io_status.status = nt_urb_irp_status(status);
	if (status != USBD_STATUS_SUCCESS)
		irp->io_status.status_info = 0;
	USBEXIT(return irp->io_status.status);
}

/* TODO: The example on msdn in reference section suggests that second
 * argument should be an array of usbd_interface_information, but
 * description and examples elsewhere suggest that it should be
 * usbd_interface_list_entry structre. Which is correct? */

STDCALL union nt_urb *WRAP_EXPORT(USBD_CreateConfigurationRequestEx)
	(struct usb_config_descriptor *config,
	 struct usbd_interface_list_entry *intf_list)
{
	int size, i, n;
	struct usbd_interface_information *intf;
	struct usbd_pipe_information *pipe;
	struct usb_interface_descriptor *intf_desc;
	struct usbd_select_configuration *select_conf;

	USBENTER("config = %p, intf_list = %p", config, intf_list);

	/* calculate size required; select_conf already has space for
	 * one intf structure */
	size = sizeof(*select_conf) - sizeof(*intf);
	for (n = 0; n < config->bNumInterfaces; n++) {
		i = intf_list[n].intf_desc->bNumEndpoints;
		/* intf already has space for one pipe */
		size += sizeof(*intf) + (i - 1) * sizeof(*pipe);
	}
	/* don't use kmalloc - driver frees it with ExFreePool */
	select_conf = ExAllocatePoolWithTag(NonPagedPool, size,
					    POOL_TAG('L', 'U', 'S', 'B'));
	if (!select_conf) {
		WARNING("couldn't allocate memory");
		return NULL;
	}
	memset(select_conf, 0, size);
	intf = &select_conf->intf;
	/* handle points to beginning of interface information */
	select_conf->handle = intf;
	for (n = 0; n < config->bNumInterfaces && intf_list[n].intf_desc;
	     n++) {
		/* initialize 'intf' fields in intf_list so they point
		 * to appropriate entry; these may be read/written by
		 * driver after this function returns */
		intf_list[n].intf = intf;
		intf_desc = intf_list[n].intf_desc;

		i = intf_desc->bNumEndpoints;
		intf->bLength = sizeof(*intf) + (i - 1) * sizeof(*pipe);

		intf->bInterfaceNumber = intf_desc->bInterfaceNumber;
		intf->bAlternateSetting = intf_desc->bAlternateSetting;
		intf->bInterfaceClass = intf_desc->bInterfaceClass;
		intf->bInterfaceSubClass = intf_desc->bInterfaceSubClass;
		intf->bInterfaceProtocol = intf_desc->bInterfaceProtocol;
		intf->bNumEndpoints = intf_desc->bNumEndpoints;

		pipe = &intf->pipes[0];
		for (i = 0; i < intf->bNumEndpoints; i++) {
			memset(&pipe[i], 0, sizeof(*pipe));
			pipe[i].max_tx_size =
				USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;
		}
		intf = (((void *)intf) + intf->bLength);
	}
	select_conf->header.function = URB_FUNCTION_SELECT_CONFIGURATION;
	select_conf->header.length = size;
	select_conf->config = config;
	USBEXIT(return (union nt_urb *)select_conf);
}

WRAP_EXPORT_MAP("_USBD_CreateConfigurationRequestEx@8",	USBD_CreateConfigurationRequestEx);

STDCALL struct usb_interface_descriptor *
WRAP_EXPORT(USBD_ParseConfigurationDescriptorEx)
	(struct usb_config_descriptor *config, void *start,
	 LONG bInterfaceNumber, LONG bAlternateSetting, LONG bInterfaceClass,
	 LONG bInterfaceSubClass, LONG bInterfaceProtocol)
{
	void *pos;
	struct usb_interface_descriptor *intf;

	USBENTER("config = %p, start = %p, ifnum = %d, alt_setting = %d,"
		      " class = %d, subclass = %d, proto = %d", config, start,
		      bInterfaceNumber, bAlternateSetting,
		      bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol);

	for (pos = start; pos < ((void *)config + config->wTotalLength);
	     pos += intf->bLength) {

		intf = pos;

		if ((intf->bDescriptorType == USB_DT_INTERFACE) &&
		    ((bInterfaceNumber == -1) ||
		     (intf->bInterfaceNumber == bInterfaceNumber)) &&
		    ((bAlternateSetting == -1) ||
		     (intf->bAlternateSetting == bAlternateSetting)) &&
		    ((bInterfaceClass == -1) ||
		     (intf->bInterfaceClass == bInterfaceClass)) &&
		    ((bInterfaceSubClass == -1) ||
		     (intf->bInterfaceSubClass == bInterfaceSubClass)) &&
		    ((bInterfaceProtocol == -1) ||
		     (intf->bInterfaceProtocol == bInterfaceProtocol))) {
			USBTRACE("selected interface = %p", intf);
			USBEXIT(return intf);
		}
	}
	USBEXIT(return NULL);
}

WRAP_EXPORT_MAP("_USBD_ParseConfigurationDescriptorEx@28", USBD_ParseConfigurationDescriptorEx);

STDCALL union nt_urb *WRAP_EXPORT(USBD_CreateConfigurationRequest)
	(struct usb_config_descriptor *config, USHORT *size)
{
	union nt_urb *nt_urb;
	struct usbd_interface_list_entry intf_list[2];
	struct usb_interface_descriptor *intf_desc;

	USBENTER("config = %p, urb_size = %p", config, size);

	intf_desc = USBD_ParseConfigurationDescriptorEx(config, config, -1, -1,
							-1, -1, -1);
	intf_list[0].intf_desc = intf_desc;
	intf_list[0].intf = NULL;
	intf_list[1].intf_desc = NULL;
	intf_list[1].intf = NULL;
	nt_urb = USBD_CreateConfigurationRequestEx(config, intf_list);
	if (!nt_urb)
		return NULL;

	*size = nt_urb->select_conf.header.length;
	USBEXIT(return nt_urb);
}

STDCALL struct usb_interface_descriptor *
WRAP_EXPORT(USBD_ParseConfigurationDescriptor)
	(struct usb_config_descriptor *config, UCHAR bInterfaceNumber,
	 UCHAR bAlternateSetting)
{
	return USBD_ParseConfigurationDescriptorEx(config, config,
						   bInterfaceNumber,
						   bAlternateSetting,
						   -1, -1, -1);
}

#include "usb_exports.h"

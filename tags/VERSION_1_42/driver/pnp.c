/*
 *  Copyright (C) 2005 Giridhar Pemmasani
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

#include "usb.h"
#include "pnp.h"
#include "wrapndis.h"
#include "loader.h"

extern struct semaphore loader_mutex;

static NTSTATUS start_pdo(struct device_object *pdo)
{
	int i, ret, count, resources_size;
	struct wrap_device *wd;
	struct pci_dev *pdev;
	struct cm_partial_resource_descriptor *entry;
	struct cm_partial_resource_list *partial_resource_list;

	ENTER1("%p, %p", pdo, pdo->reserved);
	wd = pdo->reserved;
	if (ntoskernel_init_device(wd))
		EXIT1(return STATUS_FAILURE);
	if (wrap_is_usb_bus(wd->dev_bus)) {
#ifdef CONFIG_USB
		if (usb_init_device(wd)) {
			ntoskernel_exit_device(wd);
			EXIT1(return STATUS_FAILURE);
		}
#endif
		EXIT1(return STATUS_SUCCESS);
	}
	if (!wrap_is_pci_bus(wd->dev_bus))
		EXIT1(return STATUS_SUCCESS);
	pdev = wd->pci.pdev;
	ret = pci_enable_device(pdev);
	if (ret) {
		ERROR("couldn't enable PCI device: %x", ret);
		return STATUS_FAILURE;
	}
	ret = pci_request_regions(pdev, DRIVER_NAME);
	if (ret) {
		ERROR("couldn't request PCI regions: %x", ret);
		goto err_enable;
	}
	pci_set_power_state(pdev, PCI_D0);
#ifdef CONFIG_X86_64
	/* 64-bit broadcom driver doesn't work if DMA is allocated
	 * from over 1GB */
	if (wd->vendor == 0x14e4) {
		if (pci_set_dma_mask(pdev, 0x3fffffff) ||
		    pci_set_consistent_dma_mask(pdev, 0x3fffffff))
			WARNING("couldn't set DMA mask; this driver "
				"may not work with more than 1GB RAM");
	}
#endif
	/* IRQ resource entry is filled in from pdev, instead of
	 * pci_resource macros */
	for (i = count = 0; pci_resource_start(pdev, i); i++)
		if ((pci_resource_flags(pdev, i) & IORESOURCE_MEM) ||
		    (pci_resource_flags(pdev, i) & IORESOURCE_IO))
			count++;
	/* space for entry for IRQ is already in
	 * cm_partial_resource_list */
	resources_size = sizeof(struct cm_resource_list) +
		sizeof(struct cm_partial_resource_descriptor) * count;
	TRACE2("resources: %d, %d", count, resources_size);
	wd->resource_list = kmalloc(resources_size, GFP_KERNEL);
	if (!wd->resource_list) {
		WARNING("couldn't allocate memory");
		goto err_regions;
	}
	memset(wd->resource_list, 0, resources_size);
	wd->resource_list->count = 1;
	wd->resource_list->list[0].interface_type = PCIBus;
	/* bus_number is not used by WDM drivers */
	wd->resource_list->list[0].bus_number = pdev->bus->number;

	partial_resource_list =
		&wd->resource_list->list->partial_resource_list;
	partial_resource_list->version = 1;
	partial_resource_list->revision = 1;
	partial_resource_list->count = count + 1;

	for (i = count = 0; pci_resource_start(pdev, i); i++) {
		entry = &partial_resource_list->partial_descriptors[count];
		TRACE2("%d", count);
		if (pci_resource_flags(pdev, i) & IORESOURCE_MEM) {
			entry->type = CmResourceTypeMemory;
			entry->flags = CM_RESOURCE_MEMORY_READ_WRITE;
			entry->share = CmResourceShareDeviceExclusive;
		} else if (pci_resource_flags(pdev, i) & IORESOURCE_IO) {
			entry->type = CmResourceTypePort;
			entry->flags = CM_RESOURCE_PORT_IO;
			entry->share = CmResourceShareDeviceExclusive;
#if 0
		} else if (pci_resource_flags(pdev, i) & IORESOURCE_DMA) {
			/* it looks like no driver uses this resource */
			typeof(pci_resource_flags(pdev, 0)) flags;
			entry->type = CmResourceTypeDma;
			flags = pci_resource_flags(pdev, i);
			if (flags & IORESOURCE_DMA_TYPEA)
				entry->flags |= CM_RESOURCE_DMA_TYPE_A;
			else if (flags & IORESOURCE_DMA_TYPEB)
				entry->flags |= CM_RESOURCE_DMA_TYPE_B;
			else if (flags & IORESOURCE_DMA_TYPEF)
				entry->flags |= CM_RESOURCE_DMA_TYPE_F;
			if (flags & IORESOURCE_DMA_8BIT)
				entry->flags |= CM_RESOURCE_DMA_8;
			else if (flags & IORESOURCE_DMA_16BIT)
				entry->flags |= CM_RESOURCE_DMA_16;
			/* what about 32bit DMA? */
			else if (flags & IORESOURCE_DMA_8AND16BIT)
				entry->flags |= CM_RESOURCE_DMA_8_AND_16;
			if (flags & IORESOURCE_DMA_MASTER)
				entry->flags |= CM_RESOURCE_DMA_BUS_MASTER;
			entry->u.dma.channel = pci_resource_start(pdev, i);
			/* what should this be? */
			entry->u.dma.port = 1;
#endif
		} else
			continue;
		/* TODO: Add other resource types? */
		entry->u.generic.start =
			(ULONG_PTR)pci_resource_start(pdev, i);
		entry->u.generic.length = pci_resource_len(pdev, i);
		count++;
	}

	/* put IRQ resource at the end */
	entry = &partial_resource_list->partial_descriptors[count++];
	entry->type = CmResourceTypeInterrupt;
	entry->flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
	/* we assume all devices use shared IRQ */
	entry->share = CmResourceShareShared;
	/* as per documentation, interrupt level should be DIRQL, but
	 * examples from DDK as well some drivers, such as AR5211,
	 * RT8180L use interrupt level as interrupt vector also in
	 * NdisMRegisterInterrupt */
	entry->u.interrupt.level = pdev->irq;
	entry->u.interrupt.vector = pdev->irq;
	entry->u.interrupt.affinity = -1;

	TRACE2("resource list count %d, irq: %d",
	       partial_resource_list->count, pdev->irq);
	pci_set_drvdata(pdev, wd);
	EXIT1(return STATUS_SUCCESS);
err_regions:
	pci_release_regions(pdev);
err_enable:
	pci_disable_device(pdev);
	wd->pci.pdev = NULL;
	wd->pdo = NULL;
	EXIT1(return STATUS_FAILURE);
}

static void remove_pdo(struct device_object *pdo)
{
	struct wrap_device *wd = pdo->reserved;

	ntoskernel_exit_device(wd);
	if (wrap_is_pci_bus(wd->dev_bus)) {
		struct pci_dev *pdev = wd->pci.pdev;
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		wd->pci.pdev = NULL;
		pci_set_drvdata(pdev, NULL);
	} else if (wrap_is_usb_bus(wd->dev_bus)) {
#ifdef CONFIG_USB
		usb_exit_device(wd);
#endif
	}
	if (wd->resource_list)
		kfree(wd->resource_list);
	wd->resource_list = NULL;
	return;
}

wstdcall NTSTATUS IoSendIrpTopDev(struct device_object *dev_obj, ULONG major_fn,
				 ULONG minor_fn, struct io_stack_location *sl)
{
	NTSTATUS status;
	struct nt_event event;
	struct irp *irp;
	struct io_stack_location *irp_sl;
	struct device_object *top_dev = IoGetAttachedDeviceReference(dev_obj);

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top_dev, NULL, 0, NULL,
					   &event, NULL);
	irp->io_status.status = STATUS_NOT_IMPLEMENTED;
	irp->io_status.info = 0;
	irp_sl = IoGetNextIrpStackLocation(irp);
	if (sl)
		memcpy(irp_sl, sl, sizeof(*irp_sl));
	irp_sl->major_fn = major_fn;
	irp_sl->minor_fn = minor_fn;
	status = IoCallDriver(top_dev, irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode,
				      FALSE, NULL);
		status = irp->io_status.status;
	}
	ObDereferenceObject(top_dev);
	return status;
}

wstdcall NTSTATUS pdoDispatchDeviceControl(struct device_object *pdo,
					  struct  irp *irp)
{
	struct io_stack_location *irp_sl;
	NTSTATUS status;

	DUMP_IRP(irp);
	irp_sl = IoGetCurrentIrpStackLocation(irp);
#ifdef CONFIG_USB
	status = wrap_submit_irp(pdo, irp);
	IOTRACE("status: %08X", status);
	if (status != STATUS_PENDING)
		IoCompleteRequest(irp, IO_NO_INCREMENT);
#else
	status = irp->io_status.status = STATUS_NOT_IMPLEMENTED;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
#endif
	IOEXIT(return status);
}
WIN_FUNC_DECL(pdoDispatchDeviceControl,2)

wstdcall NTSTATUS pdoDispatchPnp(struct device_object *pdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct wrap_device *wd;
	NTSTATUS status;
#ifdef CONFIG_USB
	struct usbd_bus_interface_usbdi *usb_intf;
#endif

	irp_sl = IoGetCurrentIrpStackLocation(irp);
	TRACE2("%p %d:%d", pdo, irp_sl->major_fn, irp_sl->minor_fn);
	wd = pdo->reserved;
	switch (irp_sl->minor_fn) {
	case IRP_MN_START_DEVICE:
		status = start_pdo(pdo);
		break;
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_STOP_DEVICE:
	case IRP_MN_QUERY_REMOVE_DEVICE:
		status = STATUS_SUCCESS;
		break;
	case IRP_MN_REMOVE_DEVICE:
		remove_pdo(pdo);
		status = STATUS_SUCCESS;
		break;
	case IRP_MN_QUERY_INTERFACE:
#ifdef CONFIG_USB
		if (!wrap_is_usb_bus(wd->dev_bus)) {
			status = STATUS_NOT_IMPLEMENTED;
			break;
		}
		TRACE2("type: %x, size: %d, version: %d",
		       irp_sl->params.query_intf.type->data1,
		       irp_sl->params.query_intf.size,
		       irp_sl->params.query_intf.version);
		usb_intf = (struct usbd_bus_interface_usbdi *)
			irp_sl->params.query_intf.intf;
		usb_intf->Context = wd;
		usb_intf->InterfaceReference = USBD_InterfaceReference;
		usb_intf->InterfaceDereference = USBD_InterfaceDereference;
		usb_intf->GetUSBDIVersion = USBD_InterfaceGetUSBDIVersion;
		usb_intf->QueryBusTime = USBD_InterfaceQueryBusTime;
		usb_intf->SubmitIsoOutUrb = USBD_InterfaceSubmitIsoOutUrb;
		usb_intf->QueryBusInformation =
			USBD_InterfaceQueryBusInformation;
		if (irp_sl->params.query_intf.version >=
		    USB_BUSIF_USBDI_VERSION_1)
			usb_intf->IsDeviceHighSpeed =
				USBD_InterfaceIsDeviceHighSpeed;
		if (irp_sl->params.query_intf.version >=
		    USB_BUSIF_USBDI_VERSION_2)
			usb_intf->LogEntry = USBD_InterfaceLogEntry;
		status = STATUS_SUCCESS;
#else
		status = STATUS_NOT_IMPLEMENTED;
#endif
		break;
	default:
		TRACE2("fn %d not implemented", irp_sl->minor_fn);
		status = STATUS_SUCCESS;
		break;
	}
	irp->io_status.status = status;
	TRACE2("status: %08X", status);
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	IOEXIT(return status);
}
WIN_FUNC_DECL(pdoDispatchPnp,2)

wstdcall NTSTATUS pdoDispatchPower(struct device_object *pdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct wrap_device *wd;
	union power_state power_state;
	struct pci_dev *pdev;
	NTSTATUS status;

	irp_sl = IoGetCurrentIrpStackLocation(irp);
	wd = pdo->reserved;
	TRACE2("pdo: %p, fn: %d:%d, wd: %p",
	       pdo, irp_sl->major_fn, irp_sl->minor_fn, wd);
	switch (irp_sl->minor_fn) {
	case IRP_MN_WAIT_WAKE:
		/* TODO: this is not complete/correct */
		TRACE2("state: %d, completion: %p",
			  irp_sl->params.power.state.system_state,
			  irp_sl->completion_routine);
		IoMarkIrpPending(irp);
		status = STATUS_PENDING;
		break;
	case IRP_MN_SET_POWER:
		power_state = irp_sl->params.power.state;
		if (power_state.device_state == PowerDeviceD0) {
			TRACE2("resuming device %p", wd);
			if (wrap_is_pci_bus(wd->dev_bus)) {
				pdev = wd->pci.pdev;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
				pci_restore_state(pdev);
#else
				pci_restore_state(pdev, wd->pci.pci_state);
#endif
			} else { // usb device
#ifdef CONFIG_USB
				wrap_resume_urbs(wd);
#endif
			}
		} else {
			TRACE2("suspending device %p", wd);
			if (wrap_is_pci_bus(wd->dev_bus)) {
				pdev = wd->pci.pdev;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
				pci_save_state(pdev);
#else
				pci_save_state(pdev, wd->pci.pci_state);
#endif
			} else { // usb device
#ifdef CONFIG_USB
				wrap_suspend_urbs(wd);
#endif
			}
		}
		status = STATUS_SUCCESS;
		break;
	case IRP_MN_QUERY_POWER:
		status = STATUS_SUCCESS;
		break;
	default:
		TRACE2("fn %d not implemented", irp_sl->minor_fn);
		status = STATUS_SUCCESS;
		break;
	}
	irp->io_status.status = status;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}
WIN_FUNC_DECL(pdoDispatchPower,2)

NTSTATUS pnp_set_device_power_state(struct wrap_device *wd,
				    enum device_power_state state)
{
	NTSTATUS status;
	struct device_object *pdo;
	struct io_stack_location irp_sl;

	pdo = wd->pdo;
	IOTRACE("%p, %p", pdo, IoGetAttachedDevice(pdo));
	memset(&irp_sl, 0, sizeof(irp_sl));
	irp_sl.params.power.state.device_state = state;
	irp_sl.params.power.type = DevicePowerState;
	if (state > PowerDeviceD0) {
		status = IoSendIrpTopDev(pdo, IRP_MJ_POWER, IRP_MN_QUERY_POWER,
					 &irp_sl);
		if (status != STATUS_SUCCESS) {
			TRACE1("query of power to %d returns %08X",
			       state, status);
			EXIT1(return status);
		}
	}
	status = IoSendIrpTopDev(pdo, IRP_MJ_POWER, IRP_MN_SET_POWER, &irp_sl);
	if (status != STATUS_SUCCESS)
		WARNING("setting power to %d failed: %08X", state, status);
	EXIT1(return status);
}

NTSTATUS pnp_start_device(struct wrap_device *wd)
{
	struct device_object *fdo;
	struct device_object *pdo;
	struct io_stack_location irp_sl;
	NTSTATUS status;

	pdo = wd->pdo;
	/* TODO: for now we use same resources for both translated
	 * resources and raw resources */
	memset(&irp_sl, 0, sizeof(irp_sl));
	irp_sl.params.start_device.allocated_resources =
		wd->resource_list;
	irp_sl.params.start_device.allocated_resources_translated =
		wd->resource_list;
	status = IoSendIrpTopDev(pdo, IRP_MJ_PNP, IRP_MN_START_DEVICE, &irp_sl);
	fdo = IoGetAttachedDevice(pdo);
	if (status == STATUS_SUCCESS)
		fdo->drv_obj->drv_ext->count++;
	else
		WARNING("Windows driver couldn't initialize the device (%08X)",
			status);
	EXIT1(return status);
}

NTSTATUS pnp_stop_device(struct wrap_device *wd)
{
	struct device_object *pdo;
	NTSTATUS status;

	pdo = wd->pdo;
	status = IoSendIrpTopDev(pdo, IRP_MJ_PNP, IRP_MN_QUERY_STOP_DEVICE,
				 NULL);
	if (status != STATUS_SUCCESS)
		WARNING("status: %08X", status);
	/* for now we ignore query status */
	status = IoSendIrpTopDev(pdo, IRP_MJ_PNP, IRP_MN_STOP_DEVICE, NULL);
	if (status != STATUS_SUCCESS)
		WARNING("status: %08X", status);
	if (status != STATUS_SUCCESS)
		WARNING("status: %08X", status);
	EXIT2(return status);
}

NTSTATUS pnp_remove_device(struct wrap_device *wd)
{
	struct device_object *pdo, *fdo;
	struct driver_object *fdo_drv_obj;
	NTSTATUS status;

	pdo = wd->pdo;
	fdo = IoGetAttachedDevice(pdo);
	fdo_drv_obj = fdo->drv_obj;
	TRACE2("%p, %p, %p", pdo, fdo, fdo_drv_obj);
	status = IoSendIrpTopDev(pdo, IRP_MJ_PNP, IRP_MN_QUERY_REMOVE_DEVICE,
				 NULL);
	if (status != STATUS_SUCCESS)
		WARNING("status: %08X", status);

	status = IoSendIrpTopDev(pdo, IRP_MJ_PNP, IRP_MN_REMOVE_DEVICE, NULL);
	if (status != STATUS_SUCCESS)
		WARNING("status: %08X", status);
	/* TODO: should we use count in drv_ext or driver's Object
	 * header reference count to keep count of devices associated
	 * with a driver? */
	if (status == STATUS_SUCCESS)
		fdo_drv_obj->drv_ext->count--;
	TRACE1("count: %d", fdo_drv_obj->drv_ext->count);
	if (fdo_drv_obj->drv_ext->count < 0)
		WARNING("wrong count: %d", fdo_drv_obj->drv_ext->count);
	if (fdo_drv_obj->drv_ext->count == 0) {
		struct wrap_driver *wrap_driver;
		TRACE1("unloading driver: %p", fdo_drv_obj);
		wrap_driver =
			IoGetDriverObjectExtension(fdo_drv_obj,
					   (void *)WRAP_DRIVER_CLIENT_ID);
		if (fdo_drv_obj->unload)
			LIN2WIN1(fdo_drv_obj->unload, fdo_drv_obj);
		if (wrap_driver) {
			if (down_interruptible(&loader_mutex))
				WARNING("couldn't obtain loader_mutex");
			unload_wrap_driver(wrap_driver);
			up(&loader_mutex);
		} else
			ERROR("couldn't get wrap_driver");
		ObDereferenceObject(fdo_drv_obj);
	}
	IoDeleteDevice(pdo);
	unload_wrap_device(wd);
	EXIT1(return status);
}

WIN_FUNC_DECL(IoInvalidDeviceRequest,2)

static struct device_object *alloc_pdo(struct driver_object *drv_obj)
{
	struct device_object *pdo;
	NTSTATUS status ;
	int i;
	struct ansi_string ansi_name;
	struct unicode_string unicode_name;

	RtlInitAnsiString(&ansi_name, "NDISpdo");
	if (RtlAnsiStringToUnicodeString(&unicode_name, &ansi_name, TRUE) ==
	    STATUS_SUCCESS) {
		status = IoCreateDevice(drv_obj, 0, &unicode_name,
					FILE_DEVICE_UNKNOWN,
					FILE_AUTOGENERATED_DEVICE_NAME,
					FALSE, &pdo);
		RtlFreeUnicodeString(&unicode_name);
	} else {
		status = IoCreateDevice(drv_obj, 0, NULL,
					FILE_DEVICE_UNKNOWN,
					FILE_AUTOGENERATED_DEVICE_NAME,
					FALSE, &pdo);
	}
	TRACE1("%p, %d, %p", drv_obj, status, pdo);
	if (status != STATUS_SUCCESS)
		return NULL;
	/* dispatch routines are called as Windows functions */
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv_obj->major_func[i] = WIN_FUNC_PTR(IoInvalidDeviceRequest,2);
	drv_obj->major_func[IRP_MJ_INTERNAL_DEVICE_CONTROL] =
		WIN_FUNC_PTR(pdoDispatchDeviceControl,2);
	drv_obj->major_func[IRP_MJ_DEVICE_CONTROL] =
		WIN_FUNC_PTR(pdoDispatchDeviceControl,2);
	drv_obj->major_func[IRP_MJ_POWER] = WIN_FUNC_PTR(pdoDispatchPower,2);
	drv_obj->major_func[IRP_MJ_PNP] = WIN_FUNC_PTR(pdoDispatchPnp,2);
	return pdo;
}

static int wrap_pnp_start_device(struct wrap_device *wd)
{
	struct wrap_driver *driver;
	struct device_object *pdo;
	struct driver_object *pdo_drv_obj;

	ENTER1("wd: %p", wd);

	if (!((wrap_is_pci_bus(wd->dev_bus)) ||
	      (wrap_is_usb_bus(wd->dev_bus)))) {
		ERROR("bus type %d (%d) not supported",
		      WRAP_BUS(wd->dev_bus), wd->dev_bus);
		EXIT1(return -EINVAL);
	}
	driver = load_wrap_driver(wd);
	if (!driver)
		return -ENODEV;

	wd->driver = driver;
	wd->dev_bus = WRAP_DEVICE_BUS(driver->dev_type, WRAP_BUS(wd->dev_bus));
	TRACE1("dev type: %d, bus type: %d, %d", WRAP_DEVICE(wd->dev_bus),
	       WRAP_BUS(wd->dev_bus), wd->dev_bus);
	TRACE1("%d, %d", driver->dev_type, wrap_is_usb_bus(wd->dev_bus));
	/* first create pdo */
	if (wrap_is_pci_bus(wd->dev_bus))
		pdo_drv_obj = find_bus_driver("PCI");
	else // if (wrap_is_usb_bus(wd->dev_bus))
		pdo_drv_obj = find_bus_driver("USB");
	if (!pdo_drv_obj)
		return -EINVAL;
	pdo = alloc_pdo(pdo_drv_obj);
	if (!pdo)
		return -ENOMEM;
	wd->pdo = pdo;
	pdo->reserved = wd;
	if (WRAP_DEVICE(wd->dev_bus) == WRAP_NDIS_DEVICE) {
		if (init_ndis_driver(driver->drv_obj)) {
			IoDeleteDevice(pdo);
			return -EINVAL;
		}
	}
	TRACE1("%p", driver->drv_obj->drv_ext->add_device);
	if (driver->drv_obj->drv_ext->add_device(driver->drv_obj, pdo) !=
	    STATUS_SUCCESS) {
		IoDeleteDevice(pdo);
		return -ENOMEM;
	}
	if (pnp_start_device(wd) != STATUS_SUCCESS) {
		/* TODO: we need proper cleanup, to deallocate memory,
		 * for example */
		pnp_remove_device(wd);
		return -EINVAL;
	}
	return 0;
}

/*
 * This function should not be marked __devinit because PCI IDs are
 * added dynamically.
 */
int wrap_pnp_start_pci_device(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	struct load_device load_device;
	struct wrap_device *wd;

	ENTER1("called for %04x:%04x:%04x:%04x", pdev->vendor, pdev->device,
	       pdev->subsystem_vendor, pdev->subsystem_device);

	load_device.bus = WRAP_PCI_BUS;
	load_device.vendor = pdev->vendor;
	load_device.device = pdev->device;
	load_device.subvendor = pdev->subsystem_vendor;
	load_device.subdevice = pdev->subsystem_device;
	wd = load_wrap_device(&load_device);
	if (!wd)
		EXIT1(return -ENODEV);
	wd->pci.pdev = pdev;
	return wrap_pnp_start_device(wd);
}

void __devexit wrap_pnp_remove_pci_device(struct pci_dev *pdev)
{
	struct wrap_device *wd;

	wd = (struct wrap_device *)pci_get_drvdata(pdev);
	ENTER1("%p, %p", pdev, wd);
	if (!wd)
		EXIT1(return);
	pnp_remove_device(wd);
}

int wrap_pnp_suspend_pci_device(struct pci_dev *pdev, pm_message_t state)
{
	struct wrap_device *wd;

	wd = (struct wrap_device *)pci_get_drvdata(pdev);
	return pnp_set_device_power_state(wd, PowerDeviceD3);
}

int wrap_pnp_resume_pci_device(struct pci_dev *pdev)
{
	struct wrap_device *wd;

	wd = (struct wrap_device *)pci_get_drvdata(pdev);
	return pnp_set_device_power_state(wd, PowerDeviceD0);
}

#ifdef CONFIG_USB
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
int wrap_pnp_start_usb_device(struct usb_interface *intf,
			      const struct usb_device_id *usb_id)
#else
void *wrap_pnp_start_usb_device(struct usb_device *udev,
				unsigned int ifnum,
				const struct usb_device_id *usb_id)
#endif
{
	struct wrap_device *wd;
	int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	struct usb_device *udev = interface_to_usbdev(intf);
#else
	struct usb_interface *intf = usb_ifnum_to_if(udev, ifnum);
#endif
	ENTER1("%04x, %04x, %04x", udev->descriptor.idVendor,
	       udev->descriptor.idProduct, udev->descriptor.bDeviceClass);

	/* USB device (e.g., RNDIS) may have multiple interfaces;
	  initialize one interface only (is there a way to know which
	  of these interfaces is for network?) */

	if ((wd = get_wrap_device(udev, WRAP_USB_BUS))) {
		TRACE1("device already initialized: %p", wd);
		usb_set_intfdata(intf, NULL);
		ret = 0;
	} else {
		struct load_device load_device;

		load_device.bus = WRAP_USB_BUS;
		load_device.vendor = udev->descriptor.idVendor;
		load_device.device = udev->descriptor.idProduct;
		load_device.subvendor = 0;
		load_device.subdevice = 0;
		wd = load_wrap_device(&load_device);
		TRACE2("%p", wd);
		if (wd) {
			/* some devices (e.g., TI 4150, RNDIS) need
			 * full reset */
			ret = usb_reset_device(udev);
			if (ret)
				WARNING("reset failed: %d", ret);
			usb_set_intfdata(intf, wd);
			wd->usb.intf = intf;
			wd->usb.udev = udev;
			ret = wrap_pnp_start_device(wd);
		} else
			ret = -ENODEV;
	}

	TRACE2("ret: %d", ret);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	if (ret)
		EXIT1(return ret);
	else
		return 0;
#else
	if (ret)
		return NULL;
	else
		return wd;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
void __devexit wrap_pnp_remove_usb_device(struct usb_interface *intf)
{
	struct wrap_device *wd;

	wd = (struct wrap_device *)usb_get_intfdata(intf);
	ENTER1("%p, %p", intf, wd);
	if (wd == NULL)
		EXIT1(return);
	usb_set_intfdata(intf, NULL);
	pnp_remove_device(wd);
}

int wrap_pnp_suspend_usb_device(struct usb_interface *intf, pm_message_t state)
{
	struct wrap_device *wd;
	struct device_object *pdo;

	wd = usb_get_intfdata(intf);
	ENTER1("%p, %p", intf, wd);
	if (!wd)
		EXIT1(return 0);
	pdo = wd->pdo;
	if (pnp_set_device_power_state(wd, PowerDeviceD3))
		return -1;
	return 0;
}

int wrap_pnp_resume_usb_device(struct usb_interface *intf)
{
	struct wrap_device *wd;
	wd = usb_get_intfdata(intf);
	ENTER1("%p, %p", intf, wd);
	if (!wd)
		EXIT1(return 0);
	if (pnp_set_device_power_state(wd, PowerDeviceD0))
		return -1;
	return 0;
}

#else

void __devexit wrap_pnp_remove_usb_device(struct usb_device *udev, void *ptr)
{
	struct wrap_device *wd = ptr;
	struct usb_interface *intf;

	ENTER1("%p, %p", udev, wd);
	if (wd == NULL)
		EXIT1(return);
	intf = wd->usb.intf;
	pnp_remove_device(wd);
}
#endif

#endif // USB

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

#include "ndis.h"
#include "loader.h"
#include "wrapper.h"

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

KSPIN_LOCK loader_lock;
static struct ndis_device *ndis_devices;
static unsigned int num_ndis_devices;
struct nt_list ndis_drivers;
static struct pci_device_id *ndiswrapper_pci_devices;
static struct pci_driver ndiswrapper_pci_driver;
#if defined(CONFIG_USB)
static struct usb_device_id *ndiswrapper_usb_devices;
static struct usb_driver ndiswrapper_usb_driver;
#endif

extern int debug;

/* load driver for given device, if not already loaded */
static struct ndis_driver *ndiswrapper_load_driver(struct ndis_device *device)
{
	int err, found;
	struct ndis_driver *ndis_driver;
	KIRQL irql;

	TRACEENTER1("device: %04X:%04X:%04X:%04X", device->vendor,
		    device->device, device->subvendor, device->subdevice);
	found = 0;
	ndis_driver = NULL;
	irql = kspin_lock_irql(&loader_lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(ndis_driver, &ndis_drivers, list) {
		if (strcmp(ndis_driver->name, device->driver_name) == 0) {
			DBGTRACE1("driver %s already loaded",
				  ndis_driver->name);
			found = 1;
			break;
		}
	}
	kspin_unlock_irql(&loader_lock, irql);

	if (found)
		TRACEEXIT1(return ndis_driver);
	else {
		char *argv[] = {"loadndisdriver", 
#if defined DEBUG && DEBUG >= 1
				"1",
#else
				"0",
#endif
				UTILS_VERSION, device->driver_name,
				device->conf_file_name, NULL};
		char *env[] = {NULL};

		DBGTRACE1("loading driver %s", device->driver_name);
		err = call_usermodehelper("/sbin/loadndisdriver", argv, env
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
					  , 1
#endif
			);

		if (err) {
			ERROR("loadndiswrapper failed (%d); check system log "
			      "for messages from 'loadndisdriver'", err);
			TRACEEXIT1(return NULL);
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
		/* wait for the driver to load and initialize */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ);
#endif
		found = 0;
		irql = kspin_lock_irql(&loader_lock, DISPATCH_LEVEL);
		nt_list_for_each_entry(ndis_driver, &ndis_drivers, list) {
			if (strcmp(ndis_driver->name,
				   device->driver_name) == 0) {
				found = 1;
				break;
			}
		}
		kspin_unlock_irql(&loader_lock, irql);

		if (!found) {
			ERROR("couldn't load driver '%s'",
			      device->driver_name);
			TRACEEXIT1(return NULL);
		}

		DBGTRACE1("driver %s is loaded", ndis_driver->name);
	}
	TRACEEXIT1(return ndis_driver);
}

/*
 * Called by PCI-subsystem for each PCI-card found.
 *
 * This function should not be marked __devinit because ndiswrapper
 * adds PCI_id's dynamically.
 */
static int ndiswrapper_add_pci_device(struct pci_dev *pdev,
				      const struct pci_device_id *ent)
{
	int res = 0;
	struct ndis_device *device;
	struct ndis_driver *driver;
	struct wrapper_dev *wd;
	struct net_device *dev;
	struct device_object *pdo;
	struct driver_object *drv_obj;

	TRACEENTER1("ent: %p", ent);

	DBGTRACE1("called for %04x:%04x:%04x:%04x", pdev->vendor, pdev->device,
		  pdev->subsystem_vendor, pdev->subsystem_device);

	device = &ndis_devices[ent->driver_data];
	driver = ndiswrapper_load_driver(device);
	if (!driver)
		return -ENODEV;

	dev = init_netdev(&wd, device, driver);
	if (!dev) {
		ERROR("couldn't initialize network device");
		return -ENOMEM;
	}
	DBGTRACE1("");
	/* first create pdo */
	drv_obj = find_bus_driver("PCI");
	if (!drv_obj)
		goto err_bus_driver;
	wd->dev.dev_type = NDIS_PCI_BUS;
	wd->dev.pci = pdev;
	DBGTRACE1("");
	pdo = alloc_pdo(drv_obj);
	if (!pdo) {
		res = -ENODEV;
		goto err_bus_driver;
	}
	pdo->reserved = wd;
	wd->nmb->pdo = pdo;
	DBGTRACE1("driver: %p", pdo->drv_obj);

	/* this creates (empty) fdo */
	res = driver->drv_obj->drv_ext->add_device_func(driver->drv_obj,
							pdo);
	if (res != STATUS_SUCCESS)
		goto err_bus_driver;
	DBGTRACE1("fdo: %p", wd->nmb->fdo);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	SET_NETDEV_DEV(dev, &pdev->dev);
#endif

	DBGTRACE1("");
	pci_set_drvdata(pdev, wd);
	res = pci_enable_device(pdev);
	if (res) {
		ERROR("couldn't enable PCI device: %08x", res);
		goto err_bus_driver;
	}

	res = pci_request_regions(pdev, DRIVER_NAME);
	if (res) {
		ERROR("couldn't request PCI regions: %08x", res);
		goto err_enable;
	}

	res = pci_set_power_state(pdev, PCI_D0);

#ifdef CONFIG_X86_64
	/* 64-bit broadcom driver doesn't work if DMA is allocated
	 * from over 1GB */
	if (strcmp(device->driver_name, "netbc564") == 0) {
		if (pci_set_dma_mask(pdev, 0x3fffffff) ||
		    pci_set_consistent_dma_mask(pdev, 0x3fffffff))
			WARNING("DMA mask couldn't be set; this driver "
				"may not work with more than 1GB RAM");
	}
#endif
	if (ndiswrapper_start_device(wd)) {
		ERROR("couldn't start device");
		res = -EINVAL;
		goto err_regions;
	}
	atomic_inc(&driver->users);
	device->wd = wd;
	wd->ndis_device = device;
	TRACEEXIT1(return 0);

err_regions:
	pci_release_regions(pdev);
err_enable:
	pci_disable_device(pdev);
err_bus_driver:
	pci_set_drvdata(pdev, NULL);
	free_netdev(dev);
	TRACEEXIT1(return res);
}

/*
 * Remove one PCI-card.
 */
static void __devexit ndiswrapper_remove_pci_device(struct pci_dev *pdev)
{
	struct wrapper_dev *wd;

	TRACEENTER1("%p", pdev);

	wd = (struct wrapper_dev *)pci_get_drvdata(pdev);

	TRACEENTER1("%p", wd);

	if (!wd)
		TRACEEXIT1(return);
	ndiswrapper_stop_device(wd);
}

#ifdef CONFIG_USB
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
static int ndiswrapper_add_usb_device(struct usb_interface *intf,
				      const struct usb_device_id *usb_id)
#else
static void *ndiswrapper_add_usb_device(struct usb_device *udev,
					unsigned int ifnum,
					const struct usb_device_id *usb_id)
#endif
{
	int res = 0;
	struct ndis_device *device;
	struct ndis_driver *driver;
	struct wrapper_dev *wd;
	struct net_device *dev;
	struct device_object *pdo;
	struct driver_object *drv_obj;

	TRACEENTER1("vendor: %04x, product: %04x, id: %p",
		    usb_id->idVendor, usb_id->idProduct, usb_id);

	device = &ndis_devices[usb_id->driver_info];
	/* RNDIS devices have two interfaces, so prevent from
	 * initializing the device again, if it has already been
	 * initialized */
	if (device->wd) {
		DBGTRACE1("device is already loaded");
		TRACEEXIT1(return 0);
	}

	driver = ndiswrapper_load_driver(device);
	if (!driver)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
		return -ENODEV;
#else
		return NULL;
#endif
	dev = init_netdev(&wd, device, driver);
	if (!dev) {
		ERROR("couldn't initialize network device");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
		return -ENOMEM;
#else
		return NULL;
#endif
	}

	/* first create pdo */
	drv_obj = find_bus_driver("USB");
	if (!drv_obj)
		goto err_net_dev;
	wd->dev.dev_type = NDIS_USB_BUS;
	DBGTRACE1("");
	pdo = alloc_pdo(drv_obj);
	if (!pdo) {
		res = -ENODEV;
		goto err_net_dev;
	}
	pdo->reserved = wd;
	wd->nmb->pdo = pdo;

	DBGTRACE1("");
	/* this creates (empty) fdo */
	res = driver->drv_obj->drv_ext->add_device_func(driver->drv_obj,
							pdo);
	if (res != STATUS_SUCCESS)
		goto err_pdo;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	SET_NETDEV_DEV(dev, &intf->dev);

	wd->dev.usb.udev = interface_to_usbdev(intf);
	usb_set_intfdata(intf, wd);
	wd->dev.usb.intf = intf;
#else
	wd->dev.usb.udev = udev;
	wd->dev.usb.intf = usb_ifnum_to_if(udev, ifnum);
#endif

	TRACEENTER1("calling ndis init routine");

	if (ndiswrapper_start_device(wd)) {
		ERROR("couldn't start device");
		res = -EINVAL;
		goto err_add_dev;
	}

	atomic_inc(&driver->users);
	device->wd = wd;
	wd->ndis_device = device;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	TRACEEXIT1(return 0);
#else
	TRACEEXIT1(return wd);
#endif

err_add_dev:
	DeleteDevice(pdo);
err_pdo:
	IoDeleteDevice(pdo);
err_net_dev:
	free_netdev(dev);
	device->wd = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	TRACEEXIT1(return res);
#else
	TRACEEXIT1(return NULL);
#endif
}
#endif // CONFIG_USB

#ifdef CONFIG_USB
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
static void
ndiswrapper_remove_usb_device(struct usb_interface *intf)
{
	struct wrapper_dev *wd;

	TRACEENTER1("");
	wd = (struct wrapper_dev *)usb_get_intfdata(intf);
	if (!wd)
		TRACEEXIT1(return);

	if (!test_bit(HW_RMMOD, &wd->hw_status))
		miniport_surprise_remove(wd);
	wd->dev.usb.intf = NULL;
	usb_set_intfdata(intf, NULL);
	ndiswrapper_stop_device(wd);
}
#else
static void
ndiswrapper_remove_usb_device(struct usb_device *udev, void *ptr)
{
	struct wrapper_dev *wd = (struct wrapper_dev *)ptr;

	TRACEENTER1("");
	if (!wd || !wd->dev.usb.intf)
		TRACEEXIT1(return);
	if (!test_bit(HW_RMMOD, &wd->hw_status))
		miniport_surprise_remove(wd);
	wd->dev.usb.intf = NULL;
	atomic_dec(&wd->driver->users);
	if (wd->ndis_device)
		wd->ndis_device->wd = NULL;
	ndiswrapper_stop_device(wd);
}
#endif
#endif /* CONFIG_USB */

/* load the driver files from userspace. */
static int load_sys_files(struct ndis_driver *driver,
			  struct load_driver *load_driver)
{
	int i, err;

	TRACEENTER1("");

	DBGTRACE1("num_pe_images = %d", load_driver->nr_sys_files);
	DBGTRACE1("loading driver: %s", load_driver->name);
	memcpy(driver->name, load_driver->name, MAX_DRIVER_NAME_LEN);
	DBGTRACE1("driver: %s", driver->name);
	err = 0;
	driver->num_pe_images = 0;
	for (i = 0; i < load_driver->nr_sys_files; i++) {
		struct pe_image *pe_image;
		pe_image = &driver->pe_images[driver->num_pe_images];

		pe_image->name[MAX_DRIVER_NAME_LEN-1] = 0;
		memcpy(pe_image->name, load_driver->sys_files[i].name,
		       MAX_DRIVER_NAME_LEN);
		DBGTRACE1("image size: %lu bytes",
			  (unsigned long)load_driver->sys_files[i].size);

#ifdef CONFIG_X86_64
#ifdef PAGE_KERNEL_EXECUTABLE
		pe_image->image =
			__vmalloc(load_driver->sys_files[i].size,
				  GFP_KERNEL | __GFP_HIGHMEM,
				  PAGE_KERNEL_EXECUTABLE);
#elif defined PAGE_KERNEL_EXEC
		pe_image->image =
			__vmalloc(load_driver->sys_files[i].size,
				  GFP_KERNEL | __GFP_HIGHMEM,
				  PAGE_KERNEL_EXEC);
#else
#error x86_64 should have either PAGE_KERNEL_EXECUTABLE or PAGE_KERNEL_EXEC
#endif
#else
		/* hate to play with kernel macros, but PAGE_KERNEL_EXEC is
		 * not available to modules! */
#ifdef cpu_has_nx
		if (cpu_has_nx)
			pe_image->image =
				__vmalloc(load_driver->sys_files[i].size,
					  GFP_KERNEL | __GFP_HIGHMEM,
					  __pgprot(__PAGE_KERNEL & ~_PAGE_NX));
		else
			pe_image->image =
				vmalloc(load_driver->sys_files[i].size);
#else
			pe_image->image =
				vmalloc(load_driver->sys_files[i].size);
#endif
#endif
		if (!pe_image->image) {
			ERROR("couldn't allocate memory");
			break;
		}
		DBGTRACE1("image is at %p", pe_image->image);

		if (copy_from_user(pe_image->image,
				   load_driver->sys_files[i].data,
				   load_driver->sys_files[i].size)) {
			ERROR("couldn't load file %s",
			      load_driver->sys_files[i].name);
			break;
		}
		pe_image->size = load_driver->sys_files[i].size;
		driver->num_pe_images++;
	}

	if (load_pe_images(driver->pe_images, driver->num_pe_images)) {
		ERROR("couldn't prepare driver '%s'", load_driver->name);
		err = -EINVAL;
	}

	if (driver->num_pe_images < load_driver->nr_sys_files || err) {
		for (i = 0; i < driver->num_pe_images; i++)
			if (driver->pe_images[i].image)
				vfree(driver->pe_images[i].image);
		driver->num_pe_images = 0;
		TRACEEXIT1(return -EINVAL);
	} else
		TRACEEXIT1(return 0);
}

/* load firmware files from userspace */
static int load_bin_files(struct ndis_driver *driver,
			  struct load_driver *load_driver)
{
	struct ndis_bin_file *bin_files;
	int i;

	TRACEENTER1("loading bin files for driver %s", load_driver->name);
	bin_files = kmalloc(load_driver->nr_bin_files * sizeof(*bin_files),
			    GFP_KERNEL);
	if (!bin_files) {
		ERROR("couldn't allocate memory");
		TRACEEXIT1(return -ENOMEM);
	}
	memset(bin_files, 0, load_driver->nr_bin_files * sizeof(*bin_files));

	driver->num_bin_files = 0;
	for (i = 0; i < load_driver->nr_bin_files; i++) {
		struct ndis_bin_file *bin_file = &bin_files[i];
		struct load_driver_file *load_bin_file =
			&load_driver->bin_files[i];

		memcpy(bin_file->name, load_bin_file->name,
		       MAX_DRIVER_NAME_LEN);
		bin_file->size = load_bin_file->size;
		bin_file->data = vmalloc(load_bin_file->size);
		if (!bin_file->data) {
			ERROR("cound't allocate memory");
			break;
		}
		if (copy_from_user(bin_file->data, load_bin_file->data,
				   load_bin_file->size)) {
			ERROR("couldn't load file %s", load_bin_file->name);
			break;
		}

		DBGTRACE2("loaded bin file %s", bin_file->name);
		driver->num_bin_files++;
	}
	if (driver->num_bin_files < load_driver->nr_bin_files) {
		for (i = 0; i < driver->num_bin_files; i++)
			vfree(bin_files[i].data);
		kfree(bin_files);
		driver->num_bin_files = 0;
		TRACEEXIT1(return -EINVAL);
	} else {
		driver->bin_files = bin_files;
		TRACEEXIT1(return 0);
	}
}

/* load settnigs for a device */
static int load_settings(struct ndis_driver *ndis_driver,
			 struct load_driver *load_driver)
{
	int i, nr_settings;
	struct ndis_device *ndis_device;
	KIRQL irql;

	TRACEENTER1("");

	ndis_device = NULL;
	irql = kspin_lock_irql(&loader_lock, DISPATCH_LEVEL);
	for (i = 0; i < num_ndis_devices; i++) {
		if (strcmp(ndis_devices[i].conf_file_name,
			   load_driver->conf_file_name) == 0) {
			ndis_device = &ndis_devices[i];
			break;
		}
	}
	kspin_unlock_irql(&loader_lock, irql);

	if (!ndis_device) {
		ERROR("conf file %s not found",
		      ndis_devices[i].conf_file_name);
		TRACEEXIT1(return -EINVAL);
	}

	nr_settings = 0;
	for (i = 0; i < load_driver->nr_settings; i++) {
		struct load_device_setting *load_setting =
			&load_driver->settings[i];
		struct device_setting *setting;

		setting = kmalloc(sizeof(*setting), GFP_KERNEL);
		if (!setting) {
			ERROR("couldn't allocate memory");
			break;
		}
		memset(setting, 0, sizeof(*setting));
		memcpy(setting->name, load_setting->name,
		       MAX_NDIS_SETTING_NAME_LEN);
		memcpy(setting->value, load_setting->value,
		       MAX_NDIS_SETTING_VALUE_LEN);
		DBGTRACE2("copied setting %s", load_setting->name);
		setting->config_param.type = NDIS_CONFIG_PARAM_NONE;

		if (strcmp(setting->name, "ndis_version") == 0)
			memcpy(ndis_driver->version, setting->value,
			       sizeof(ndis_driver->version));
		irql = kspin_lock_irql(&loader_lock, DISPATCH_LEVEL);
		InsertTailList(&ndis_device->settings, &setting->list);
		kspin_unlock_irql(&loader_lock, irql);
		nr_settings++;
	}
	/* it is not a fatal error if some settings couldn't be loaded */
	if (nr_settings > 0)
		TRACEEXIT1(return 0);
	else
		TRACEEXIT1(return -EINVAL);
}

/* this function is called while holding load_lock spinlock */
static void unload_ndis_device(struct ndis_device *device)
{
	struct nt_list *cur;
	TRACEENTER1("unloading device %p (%04X:%04X:%04X:%04X), driver %s",
		    device, device->vendor, device->device, device->subvendor,
		    device->subdevice, device->driver_name);

	DBGTRACE3("%p", device->ndis_driver);
	if (!device->ndis_driver)
		TRACEEXIT1(return);
	while ((cur = RemoveHeadList(&device->settings))) {
		struct device_setting *setting;
		struct ndis_config_param *param;

		setting = container_of(cur, struct device_setting, list);
		param = &setting->config_param;
		if (param->type == NDIS_CONFIG_PARAM_STRING)
			RtlFreeUnicodeString(&param->data.ustring);
		kfree(setting);
	}
	TRACEEXIT1(return);
}

/* at the time this function is called, devices are deregistered, so
 * safe to remove the driver without any checks */
static void unload_ndis_driver(struct ndis_driver *driver)
{
	int i;
	struct driver_object *drv_obj;

	DBGTRACE1("freeing %d images", driver->num_pe_images);
	drv_obj = driver->drv_obj;
	for (i = 0; i < driver->num_pe_images; i++)
		if (driver->pe_images[i].image) {
			DBGTRACE1("freeing image at %p",
				  driver->pe_images[i].image);
			vfree(driver->pe_images[i].image);
		}

	DBGTRACE1("freeing %d bin files", driver->num_bin_files);
	for (i = 0; i < driver->num_bin_files; i++) {
		DBGTRACE1("freeing image at %p", driver->bin_files[i].data);
		vfree(driver->bin_files[i].data);
	}
	if (driver->bin_files)
		kfree(driver->bin_files);

//	IoDetachDevice(drv_obj->dev_obj);
	RtlFreeUnicodeString(&drv_obj->driver_name);
	/* this frees driver */
	free_custom_ext(drv_obj->drv_ext);
	kfree(drv_obj->drv_ext);
	kfree(drv_obj);
	TRACEEXIT1(return);
}

/* call the entry point of the driver */
static int start_driver(struct ndis_driver *driver)
{
	int i;
	NTSTATUS ret, res;
	struct driver_object *drv_obj;
	UINT (*entry)(struct driver_object *obj,
		      struct unicode_string *path) STDCALL;

	TRACEENTER1("");
	drv_obj = driver->drv_obj;
	for (ret = res = 0, i = 0; i < driver->num_pe_images; i++)
		/* dlls are already started by loader */
		if (driver->pe_images[i].type == IMAGE_FILE_EXECUTABLE_IMAGE) {
			entry = driver->pe_images[i].entry;
			drv_obj->driver_start = driver->pe_images[i].entry;
			drv_obj->driver_size = driver->pe_images[i].size;
			DBGTRACE1("entry: %p, %p, drv_obj: %p",
				  entry, *entry, drv_obj);
			res = LIN2WIN2(entry, drv_obj, &drv_obj->driver_name);
			ret |= res;
			DBGTRACE1("entry returns %08X", res);
			DBGTRACE1("driver version: %d.%d",
				  driver->miniport.major_version,
				  driver->miniport.minor_version);
			break;
		}

	if (ret) {
		ERROR("driver initialization failed: %08X", ret);
		RtlFreeUnicodeString(&drv_obj->driver_name);
		/* this frees ndis_driver */
		free_custom_ext(drv_obj->drv_ext);
		kfree(drv_obj->drv_ext);
		kfree(drv_obj);
		TRACEEXIT1(return -EINVAL);
	}

	TRACEEXIT1(return 0);
}

/*
 * add driver to list of loaded driver but make sure this driver is
 * not loaded before.
 */
static int add_driver(struct ndis_driver *driver)
{
	KIRQL irql;
	struct ndis_driver *tmp;

	TRACEENTER1("");
	irql = kspin_lock_irql(&loader_lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(tmp, &ndis_drivers, list) {
		if (strcmp(tmp->name, driver->name) == 0) {
			kspin_unlock_irql(&loader_lock, irql);
			ERROR("cannot add duplicate driver");
			TRACEEXIT1(return -EBUSY);
		}
	}
	InsertTailList(&ndis_drivers, &driver->list);
	kspin_unlock_irql(&loader_lock, irql);

	TRACEEXIT1(return 0);
}

/* load a driver from userspace and initialize it */
static int load_ndis_driver(struct load_driver *load_driver)
{
	struct driver_object *drv_obj;
	struct ansi_string ansi_reg;
	struct ndis_driver *ndis_driver = NULL;
	int i;

	TRACEENTER1("");
	drv_obj = kmalloc(sizeof(*drv_obj), GFP_KERNEL);
	if (!drv_obj) {
		ERROR("couldn't allocate memory");
		TRACEEXIT1(return -ENOMEM);
	}
	DBGTRACE1("drv_obj: %p", drv_obj);
	memset(drv_obj, 0, sizeof(*drv_obj));
	drv_obj->drv_ext = kmalloc(sizeof(*(drv_obj->drv_ext)), GFP_KERNEL);
	if (!drv_obj->drv_ext) {
		ERROR("couldn't allocate memory");
		kfree(drv_obj);
		TRACEEXIT1(return -ENOMEM);
	}
	memset(drv_obj->drv_ext, 0, sizeof(*(drv_obj->drv_ext)));
	drv_obj->drv_ext->add_device_func = AddDevice;
	InitializeListHead(&drv_obj->drv_ext->custom_ext);
	DBGTRACE1("");
	if (IoAllocateDriverObjectExtension(drv_obj,
					    (void *)CE_NDIS_DRIVER_CLIENT_ID,
					    sizeof(*ndis_driver),
					    (void **)&ndis_driver) !=
	    STATUS_SUCCESS)
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	DBGTRACE1("driver: %p", ndis_driver);
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv_obj->major_func[i] = IopPassIrpDown;
//	drv_obj->major_func[IRP_MJ_PNP] = fdoDispatchPnp;

	memset(ndis_driver, 0, sizeof(*ndis_driver));
	ndis_driver->bustype = -1;
	ndis_driver->drv_obj = drv_obj;
	ansi_reg.buf = "/tmp";
	ansi_reg.length = strlen(ansi_reg.buf);
	ansi_reg.max_length = ansi_reg.length + 1;
	if (RtlAnsiStringToUnicodeString(&drv_obj->driver_name,
					 &ansi_reg, 1) != STATUS_SUCCESS) {
		ERROR("couldn't initialize registry path");
		free_custom_ext(drv_obj->drv_ext);
		kfree(drv_obj->drv_ext);
		kfree(drv_obj);
		TRACEEXIT1(return -EINVAL);
	}
	DBGTRACE1("");
	if (load_sys_files(ndis_driver, load_driver) ||
	    load_bin_files(ndis_driver, load_driver) ||
	    load_settings(ndis_driver, load_driver) ||
	    start_driver(ndis_driver) ||
	    add_driver(ndis_driver)) {
		unload_ndis_driver(ndis_driver);
		TRACEEXIT1(return -EINVAL);
	} else {
		printk(KERN_INFO "%s: driver %s (%s) loaded\n",
		       DRIVER_NAME, ndis_driver->name, ndis_driver->version);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
		add_taint(TAINT_PROPRIETARY_MODULE);
		/* older kernels don't seem to have a way to set
		 * tainted information */
#endif
		TRACEEXIT1(return 0);
	}
}

/* register all devices (for all drivers) installed */
static int register_devices(struct load_devices *load_devices)
{
	int i, res, num_pci, num_usb;
	struct load_device *devices;

	devices = NULL;
	ndiswrapper_pci_devices = NULL;
#if defined(CONFIG_USB)
	ndiswrapper_usb_devices = NULL;
#endif
	ndis_devices = NULL;
	devices = vmalloc(load_devices->count * sizeof(struct load_device));
	if (!devices) {
		ERROR("couldn't allocate memory");
		TRACEEXIT1(return -ENOMEM);
	}

	if (copy_from_user(devices, load_devices->devices,
			   load_devices->count * sizeof(struct load_device))) {
		ERROR("couldn't copy from user space");
		goto err;
	}

	num_pci = num_usb = 0;
	for (i = 0; i < load_devices->count; i++)
		if (devices[i].bustype == NDIS_PCI_BUS)
			num_pci++;
		else if (devices[i].bustype == NDIS_USB_BUS)
			num_usb++;
		else
			WARNING("bus type %d is not valid",
				devices[i].bustype);
	num_ndis_devices = num_pci + num_usb;
	if (num_pci > 0) {
		ndiswrapper_pci_devices =
			kmalloc((num_pci + 1) * sizeof(struct pci_device_id),
				GFP_KERNEL);
		if (!ndiswrapper_pci_devices) {
			ERROR("couldn't allocate memory");
			goto err;
		}
		memset(ndiswrapper_pci_devices, 0,
		       (num_pci + 1) * sizeof(struct pci_device_id));
	}

#if defined(CONFIG_USB)
	if (num_usb > 0) {
		ndiswrapper_usb_devices =
			kmalloc((num_usb + 1) * sizeof(struct usb_device_id),
				GFP_KERNEL);
		if (!ndiswrapper_usb_devices) {
			ERROR("couldn't allocate memory");
			goto err;
		}
		memset(ndiswrapper_usb_devices, 0,
		       (num_usb + 1) * sizeof(struct usb_device_id));
	}
#endif

	ndis_devices = vmalloc(num_ndis_devices * sizeof(*ndis_devices));
	if (!ndis_devices) {
		ERROR("couldn't allocate memory");
		goto err;
	}

	memset(ndis_devices, 0, num_ndis_devices * sizeof(*ndis_devices));
	num_usb = num_pci = 0;
	for (i = 0; i < load_devices->count; i++) {
		struct load_device *device = &devices[i];
		struct ndis_device *ndis_device;

		ndis_device = &ndis_devices[num_pci + num_usb];

		InitializeListHead(&ndis_device->settings);
		memcpy(&ndis_device->driver_name, device->driver_name,
		       sizeof(ndis_device->driver_name));
		memcpy(&ndis_device->conf_file_name, device->conf_file_name,
		       sizeof(ndis_device->conf_file_name));
		ndis_device->bustype = device->bustype;

		ndis_device->vendor = device->vendor;
		ndis_device->device = device->device;
		ndis_device->subvendor = device->subvendor;
		ndis_device->subdevice = device->subdevice;

		if (device->bustype == NDIS_PCI_BUS) {
			ndiswrapper_pci_devices[num_pci].vendor =
				device->vendor;
			ndiswrapper_pci_devices[num_pci].device =
				device->device;
			if (device->subvendor == DEV_ANY_ID)
				ndiswrapper_pci_devices[num_pci].subvendor =
					PCI_ANY_ID;
			else
				ndiswrapper_pci_devices[num_pci].subvendor =
					device->subvendor;
			if (device->subdevice == DEV_ANY_ID)
				ndiswrapper_pci_devices[num_pci].subdevice =
					PCI_ANY_ID;
			else
				ndiswrapper_pci_devices[num_pci].subdevice =
					device->subdevice;
			ndiswrapper_pci_devices[num_pci].class = 0;
			ndiswrapper_pci_devices[num_pci].class_mask = 0;
			ndiswrapper_pci_devices[num_pci].driver_data =
				num_pci + num_usb;
			num_pci++;
			DBGTRACE1("pci device %d added", num_pci);
			DBGTRACE1("adding %04x:%04x:%04x:%04x to pci idtable",
				  device->vendor, device->device,
				  device->subvendor, device->subdevice);
#ifdef CONFIG_USB
		} else if (device->bustype == NDIS_USB_BUS) {
			ndiswrapper_usb_devices[num_usb].idVendor =
				device->vendor;
			ndiswrapper_usb_devices[num_usb].idProduct =
				device->device;
			ndiswrapper_usb_devices[num_usb].match_flags =
				USB_DEVICE_ID_MATCH_DEVICE;
			ndiswrapper_usb_devices[num_usb].driver_info =
				num_pci + num_usb;
			num_usb++;
			DBGTRACE1("usb device %d added", num_usb);
			DBGTRACE1("adding %04x:%04x to usb idtable",
				  device->vendor, device->device);
#endif
		} else {
			ERROR("bus type %d not supported", device->bustype);
		}
	}

	if (ndiswrapper_pci_devices) {
		memset(&ndiswrapper_pci_driver, 0,
			       sizeof(ndiswrapper_pci_driver));
		ndiswrapper_pci_driver.name = DRIVER_NAME;
		ndiswrapper_pci_driver.id_table = ndiswrapper_pci_devices;
		ndiswrapper_pci_driver.probe = ndiswrapper_add_pci_device;
		ndiswrapper_pci_driver.remove =
			__devexit_p(ndiswrapper_remove_pci_device);
		ndiswrapper_pci_driver.suspend = ndiswrapper_suspend_pci;
		ndiswrapper_pci_driver.resume = ndiswrapper_resume_pci;
		res = pci_register_driver(&ndiswrapper_pci_driver);
		if (res < 0) {
			ERROR("couldn't register ndiswrapper pci driver");
			goto err;
		}
	}
#ifdef CONFIG_USB
	if (ndiswrapper_usb_devices) {
		memset(&ndiswrapper_usb_driver, 0,
			       sizeof(ndiswrapper_usb_driver));
		ndiswrapper_usb_driver.owner = THIS_MODULE;
		ndiswrapper_usb_driver.name = DRIVER_NAME;
		ndiswrapper_usb_driver.id_table = ndiswrapper_usb_devices;
		ndiswrapper_usb_driver.probe = ndiswrapper_add_usb_device;
		ndiswrapper_usb_driver.disconnect =
			ndiswrapper_remove_usb_device;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
		ndiswrapper_usb_driver.suspend = ndiswrapper_suspend_usb;
		ndiswrapper_usb_driver.resume = ndiswrapper_resume_usb;
#endif
		res = usb_register(&ndiswrapper_usb_driver);
		if (res < 0) {
			ERROR("couldn't register ndiswrapper usb driver");
			goto err;
		}
	}
#endif

	vfree(devices);
	TRACEEXIT1(return 0);

err:
	if (ndis_devices)
		vfree(ndis_devices);
	ndis_devices = NULL;
#if defined(CONFIG_USB)
	if (ndiswrapper_usb_devices)
		kfree(ndiswrapper_usb_devices);
	ndiswrapper_usb_devices = NULL;
#endif
	if (ndiswrapper_pci_devices)
		kfree(ndiswrapper_pci_devices);
	ndiswrapper_pci_devices = NULL;
	if (devices)
		vfree(devices);
	TRACEEXIT1(return -EINVAL);
}

static int wrapper_ioctl(struct inode *inode, struct file *file,
			 unsigned int cmd, unsigned long arg)
{
	struct load_driver *load_driver;
	struct load_devices devices;
	int res;

	TRACEENTER1("cmd: %u (%lu, %lu)", cmd,
		    (unsigned long)NDIS_REGISTER_DEVICES,
		    (unsigned long)NDIS_LOAD_DRIVER);

	res = 0;
	switch (cmd) {
	case NDIS_REGISTER_DEVICES:
		DBGTRACE1("adding devices at %p", (void *)arg);
		res = copy_from_user(&devices, (void *)arg, sizeof(devices));
		if (!res)
			res = register_devices(&devices);
		if (res)
			TRACEEXIT1(return -EINVAL);
		TRACEEXIT1(return 0);
		break;
	case NDIS_LOAD_DRIVER:
		DBGTRACE1("loading driver at %p", (void *)arg);
		load_driver = vmalloc(sizeof(*load_driver));
		if (!load_driver)
			TRACEEXIT1(return -ENOMEM);
		res = copy_from_user(load_driver, (void *)arg,
				     sizeof(*load_driver));
		if (!res)
			res = load_ndis_driver(load_driver);
		vfree(load_driver);
		if (res)
			TRACEEXIT1(return -EINVAL);
		else
			TRACEEXIT1(return 0);
		break;
	default:
		ERROR("Unknown ioctl %u", cmd);
		TRACEEXIT1(return -EINVAL);
		break;
	}

	TRACEEXIT1(return 0);
}

static int wrapper_ioctl_release(struct inode *inode, struct file *file)
{
	TRACEENTER1("");
	return 0;
}

static struct file_operations wrapper_fops = {
	.owner          = THIS_MODULE,
	.ioctl		= wrapper_ioctl,
	.release	= wrapper_ioctl_release,
};

static struct miscdevice wrapper_misc = {
	.name   = DRIVER_NAME,
	.minor	= MISC_DYNAMIC_MINOR,
	.fops   = &wrapper_fops
};

int loader_init(void)
{
	int err;

	InitializeListHead(&ndis_drivers);
	kspin_lock_init(&loader_lock);
	if ((err = misc_register(&wrapper_misc)) < 0 ) {
		ERROR("couldn't register module (%d)", err);
		TRACEEXIT1(return err);
	}
	TRACEEXIT1(return 0);
}

void loader_exit(void)
{
	int i;
	struct nt_list *cur;

	TRACEENTER1("");
	misc_deregister(&wrapper_misc);

	for (i = 0; i < num_ndis_devices; i++)
		if (ndis_devices[i].wd)
			set_bit(HW_RMMOD, &ndis_devices[i].wd->hw_status);
#ifdef CONFIG_USB
	if (ndiswrapper_usb_devices) {
		usb_deregister(&ndiswrapper_usb_driver);
		kfree(ndiswrapper_usb_devices);
		ndiswrapper_usb_devices = NULL;
	}
#endif
	if (ndiswrapper_pci_devices) {
		pci_unregister_driver(&ndiswrapper_pci_driver);
		kfree(ndiswrapper_pci_devices);
		ndiswrapper_pci_devices = NULL;
	}
	kspin_lock(&loader_lock);
	if (ndis_devices) {
		for (i = 0; i < num_ndis_devices; i++)
			unload_ndis_device(&ndis_devices[i]);

		vfree(ndis_devices);
		ndis_devices = NULL;
	}

	while ((cur = RemoveHeadList(&ndis_drivers))) {
		struct ndis_driver *driver;

		driver = container_of(cur, struct ndis_driver, list);
		unload_ndis_driver(driver);
	}
	kspin_unlock(&loader_lock);
	TRACEEXIT1(return);
}

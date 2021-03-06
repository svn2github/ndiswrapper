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
#include "iw_ndis.h"
#include "wrapndis.h"
#include "pnp.h"
#include "loader.h"

#define MAX_ALLOCATED_NDIS_PACKETS 20
#define MAX_ALLOCATED_NDIS_BUFFERS 20

static workqueue_struct_t *ndis_wq;
static void ndis_worker(void *dummy);
static work_struct_t ndis_work;
static struct nt_list ndis_worker_list;
static NT_SPIN_LOCK ndis_work_list_lock;

extern struct semaphore loader_mutex;

/* ndis_init is called once when module is loaded */
int ndis_init(void)
{
	ndis_wq = create_singlethread_workqueue("ndis_wq");
	InitializeListHead(&ndis_worker_list);
	nt_spin_lock_init(&ndis_work_list_lock);
	initialize_work(&ndis_work, ndis_worker, NULL);

	return 0;
}

/* ndis_exit is called once when module is removed */
void ndis_exit(void)
{
	destroy_workqueue(ndis_wq);
	TRACEEXIT1(return);
}

/* ndis_exit_device is called for each handle */
void ndis_exit_device(struct wrap_ndis_device *wnd)
{
	struct wrap_device_setting *setting;
	DBGTRACE2("%p", wnd);
	/* TI driver doesn't call NdisMDeregisterInterrupt during halt! */
	if (wnd->ndis_irq)
		NdisMDeregisterInterrupt(wnd->ndis_irq);
	if (down_interruptible(&loader_mutex))
		WARNING("couldn't obtain loader_mutex");
	nt_list_for_each_entry(setting, &wnd->wd->settings, list) {
		struct ndis_configuration_parameter *param;
		param = setting->encoded;
		if (param) {
			if (param->type == NdisParameterString)
				RtlFreeUnicodeString(&param->data.string);
			ExFreePool(param);
			setting->encoded = NULL;
		}
	}
	up(&loader_mutex);
}

wstdcall void WIN_FUNC(NdisInitializeWrapper,4)
	(void **driver_handle, struct driver_object *driver,
	 struct unicode_string *reg_path, void *unused)
{
	TRACEENTER1("handle: %p, driver: %p", driver_handle, driver);
	*driver_handle = driver;
	TRACEEXIT1(return);
}

wstdcall void WIN_FUNC(NdisTerminateWrapper,2)
	(struct device_object *dev_obj, void *system_specific)
{
	TRACEEXIT1(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterMiniport,3)
	(struct driver_object *drv_obj,
	 struct miniport_char *miniport_char, UINT length)
{
	int min_length;
	struct wrap_driver *wrap_driver;
	struct wrap_ndis_driver *ndis_driver;

	min_length = ((char *)&miniport_char->co_create_vc) -
		((char *)miniport_char);

	TRACEENTER1("%p %p %d", drv_obj, miniport_char, length);

	if (miniport_char->major_version < 4) {
		ERROR("Driver is using ndis version %d which is too old.",
		      miniport_char->major_version);
		TRACEEXIT1(return NDIS_STATUS_BAD_VERSION);
	}

	if (length < min_length) {
		ERROR("Characteristics length %d is too small", length);
		TRACEEXIT1(return NDIS_STATUS_BAD_CHARACTERISTICS);
	}

	DBGTRACE1("%d.%d, %d, %u", miniport_char->major_version,
		  miniport_char->minor_version, length,
		  (u32)sizeof(struct miniport_char));
	wrap_driver =
		IoGetDriverObjectExtension(drv_obj,
					   (void *)WRAP_DRIVER_CLIENT_ID);
	if (!wrap_driver) {
		ERROR("couldn't get wrap_driver");
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	}
	if (IoAllocateDriverObjectExtension(
		    drv_obj, (void *)NDIS_DRIVER_CLIENT_ID,
		    sizeof(*ndis_driver), (void **)&ndis_driver) !=
	    STATUS_SUCCESS)
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	wrap_driver->ndis_driver = ndis_driver;
	TRACEENTER1("driver: %p", ndis_driver);
	memcpy(&ndis_driver->miniport, miniport_char,
	       length > sizeof(*miniport_char) ?
	       sizeof(*miniport_char) : length);

	DBG_BLOCK(2) {
		int i;
		void **func;
		char *miniport_funcs[] = {
			"query", "reconfig", "reset", "send", "setinfo",
			"tx_data", "return_packet", "send_packets",
			"alloc_complete", "co_create_vc", "co_delete_vc",
			"co_activate_vc", "co_deactivate_vc",
			"co_send_packets", "co_request",
			"cancel_send_packets", "pnp_event_notify",
			"shutdown",
		};
		func = (void **)&ndis_driver->miniport.query;
		for (i = 0; i < (sizeof(miniport_funcs) /
				 sizeof(miniport_funcs[0])); i++)
			DBGTRACE2("function '%s' is at %p",
				  miniport_funcs[i], func[i]);
	}
	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterDevice,6)
	(struct driver_object *drv_obj, struct unicode_string *dev_name,
	 struct unicode_string *link, void **funcs,
	 struct device_object **dev_obj, void **dev_obj_handle)
{
	NTSTATUS status;
	struct device_object *tmp;
	int i;

	TRACEENTER1("%p, %p, %p", drv_obj, dev_name, link);
	status = IoCreateDevice(drv_obj, 0, dev_name, FILE_DEVICE_NETWORK, 0,
				FALSE, &tmp);

	if (status != STATUS_SUCCESS)
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	if (link)
		status = IoCreateSymbolicLink(link, dev_name);
	if (status != STATUS_SUCCESS) {
		IoDeleteDevice(tmp);
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	}

	*dev_obj = tmp;
	*dev_obj_handle = *dev_obj;
	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++)
		if (funcs[i] && i != IRP_MJ_PNP && i != IRP_MJ_POWER) {
			drv_obj->major_func[i] = funcs[i];
			DBGTRACE1("mj_fn for 0x%x is at %p", i, funcs[i]);
		}
	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMDeregisterDevice,1)
	(struct device_object *dev_obj)
{
	TRACEENTER2("%p", dev_obj);
	IoDeleteDevice(dev_obj);
	return NDIS_STATUS_SUCCESS;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAllocateMemoryWithTag,3)
	(void **dest, UINT length, ULONG tag)
{
	void *res;
	res = ExAllocatePoolWithTag(NonPagedPool, length, tag);
	if (res) {
		*dest = res;
		TRACEEXIT4(return NDIS_STATUS_SUCCESS);
	} else
		TRACEEXIT4(return NDIS_STATUS_FAILURE);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAllocateMemory,4)
	(void **dest, UINT length, UINT flags, NDIS_PHY_ADDRESS highest_address)
{
	return NdisAllocateMemoryWithTag(dest, length, 0);
}

/* length_tag is either length or tag, depending on if
 * NdisAllocateMemory or NdisAllocateMemoryTag is used to allocate
 * memory */
wstdcall void WIN_FUNC(NdisFreeMemory,3)
	(void *addr, UINT length_tag, UINT flags)
{
	ExFreePool(addr);
}

noregparm void WIN_FUNC(NdisWriteErrorLogEntry,12)
	(struct driver_object *drv_obj, ULONG error, ULONG count, ...)
{
	va_list args;
	int i;
	ULONG code;

	va_start(args, count);
	ERROR("log: %08X, count: %d, return_address: %p",
	      error, count, __builtin_return_address(0));
	for (i = 0; i < count; i++) {
		code = va_arg(args, ULONG);
		ERROR("code: 0x%x", code);
	}
	va_end(args);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfiguration,3)
	(NDIS_STATUS *status, struct ndis_miniport_block **conf_handle,
	 struct ndis_miniport_block *handle)
{
	TRACEENTER2("%p", conf_handle);
	*conf_handle = handle;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenProtocolConfiguration,3)
	(NDIS_STATUS *status, void **confhandle,
	 struct unicode_string *section)
{
	TRACEENTER2("%p", confhandle);
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfigurationKeyByName,4)
	(NDIS_STATUS *status, void *handle,
	 struct unicode_string *key, void **subkeyhandle)
{
	struct ansi_string ansi;
	TRACEENTER2("");
	if (RtlUnicodeStringToAnsiString(&ansi, key, TRUE) == STATUS_SUCCESS) {
		DBGTRACE2("%s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*subkeyhandle = handle;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfigurationKeyByIndex,5)
	(NDIS_STATUS *status, void *handle, ULONG index,
	 struct unicode_string *key, void **subkeyhandle)
{
	TRACEENTER2("%u", index);
//	*subkeyhandle = handle;
	*status = NDIS_STATUS_FAILURE;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisCloseConfiguration,1)
	(void *handle)
{
	/* instead of freeing all configuration parameters as we are
	 * supposed to do here, we free them when the device is
	 * removed */
	TRACEENTER2("%p", handle);
	return;
}

wstdcall void WIN_FUNC(NdisOpenFile,5)
	(NDIS_STATUS *status, struct wrap_bin_file **file,
	 UINT *filelength, struct unicode_string *filename,
	 NDIS_PHY_ADDRESS highest_address)
{
	struct ansi_string ansi;
	struct wrap_bin_file *bin_file;

	TRACEENTER2("%p, %d, %llx, %p", status, *filelength,
		    highest_address, *file);
	if (RtlUnicodeStringToAnsiString(&ansi, filename, TRUE) !=
	    STATUS_SUCCESS) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT2(return);
	}
	DBGTRACE2("%s", ansi.buf);
	bin_file = get_bin_file(ansi.buf);
	if (bin_file) {
		*file = bin_file;
		*filelength = bin_file->size;
		*status = NDIS_STATUS_SUCCESS;
	} else
		*status = NDIS_STATUS_FILE_NOT_FOUND;

	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisMapFile,3)
	(NDIS_STATUS *status, void **mappedbuffer, struct wrap_bin_file *file)
{
	TRACEENTER2("%p", file);

	if (!file) {
		*status = NDIS_STATUS_ALREADY_MAPPED;
		TRACEEXIT2(return);
	}

	*status = NDIS_STATUS_SUCCESS;
	*mappedbuffer = file->data;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisUnmapFile,1)
	(struct wrap_bin_file *file)
{
	TRACEENTER2("%p", file);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisCloseFile,1)
	(struct wrap_bin_file *file)
{
	TRACEENTER2("%p", file);
	free_bin_file(file);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisGetSystemUpTime,1)
	(ULONG *ms)
{
	TRACEENTER5("");
	*ms = 1000 * jiffies / HZ;
	TRACEEXIT5(return);
}

wstdcall ULONG WIN_FUNC(NDIS_BUFFER_TO_SPAN_PAGES,1)
	(ndis_buffer *buffer)
{
	ULONG n, length;

	if (buffer == NULL)
		TRACEEXIT2(return 0);
	if (MmGetMdlByteCount(buffer) == 0)
		TRACEEXIT2(return 1);
	length = MmGetMdlByteCount(buffer);

#ifdef VT6655
	/* VIA VT6655 works with this bogus computation, but not with
	 * correct computation with SPAN_PAGES */
	if (1) {
		ULONG_PTR start, end;
		unsigned long ptr;

		ptr = (unsigned long)MmGetMdlVirtualAddress(buffer);
		start = ptr & (PAGE_SIZE - 1);
		end = (ptr + length + PAGE_SIZE - 1) & PAGE_MASK;
		n = (end - start) / PAGE_SIZE;
	}
#else
	n = SPAN_PAGES(MmGetMdlVirtualAddress(buffer), length);
#endif
	DBGTRACE4("%p, %p, %d, %d", buffer->startva, buffer->mappedsystemva,
		  length, n);
	TRACEEXIT3(return n);
}

wstdcall void WIN_FUNC(NdisGetBufferPhysicalArraySize,2)
	(ndis_buffer *buffer, UINT *arraysize)
{
	TRACEENTER3("%p", buffer);
	*arraysize = NDIS_BUFFER_TO_SPAN_PAGES(buffer);
	TRACEEXIT3(return);
}

static struct ndis_configuration_parameter *
ndis_encode_setting(struct wrap_device_setting *setting,
		    enum ndis_parameter_type type)
{
	struct ansi_string ansi;
	struct ndis_configuration_parameter *param;

	param = setting->encoded;
	if (param) {
		if (param->type == type)
			TRACEEXIT2(return param);
		if (param->type == NdisParameterString)
			RtlFreeUnicodeString(&param->data.string);
		setting->encoded = NULL;
	} else
		param = ExAllocatePoolWithTag(NonPagedPool, sizeof(*param), 0);
	if (!param) {
		ERROR("couldn't allocate memory");
		return NULL;
	}
	switch(type) {
	case NdisParameterInteger:
		param->data.integer = simple_strtol(setting->value, NULL, 0);
		DBGTRACE2("%u", (ULONG)param->data.integer);
		break;
	case NdisParameterHexInteger:
		param->data.integer = simple_strtol(setting->value, NULL, 16);
		DBGTRACE2("%u", (ULONG)param->data.integer);
		break;
	case NdisParameterString:
		RtlInitAnsiString(&ansi, setting->value);
		DBGTRACE2("'%s'", ansi.buf);
		if (RtlAnsiStringToUnicodeString(&param->data.string,
						 &ansi, TRUE)) {
			ExFreePool(param);
			TRACEEXIT2(return NULL);
		}
		break;
	default:
		ERROR("unknown type: %d", type);
		ExFreePool(param);
		return NULL;
	}
	param->type = type;
	setting->encoded = param;
	TRACEEXIT2(return param);
}

static int ndis_decode_setting(struct wrap_device_setting *setting,
			       struct ndis_configuration_parameter *param)
{
	struct ansi_string ansi;
	struct ndis_configuration_parameter *prev;

	TRACEENTER2("%p, %p", setting, param);
	prev = setting->encoded;
	if (prev && prev->type == NdisParameterString) {
		RtlFreeUnicodeString(&prev->data.string);
		setting->encoded = NULL;
	}
	switch(param->type) {
	case NdisParameterInteger:
		snprintf(setting->value, sizeof(u32), "%u", param->data.integer);
		setting->value[sizeof(ULONG)] = 0;
		break;
	case NdisParameterHexInteger:
		snprintf(setting->value, sizeof(u32), "%x", param->data.integer);
		setting->value[sizeof(ULONG)] = 0;
		break;
	case NdisParameterString:
		ansi.buf = setting->value;
		ansi.max_length = MAX_SETTING_VALUE_LEN;
		if ((RtlUnicodeStringToAnsiString(&ansi, &param->data.string,
						  FALSE) != STATUS_SUCCESS)
		    || ansi.length >= MAX_SETTING_VALUE_LEN) {
			TRACEEXIT1(return -1);
		}
		if (ansi.length == ansi.max_length)
			ansi.length--;
		setting->value[ansi.length] = 0;
		break;
	default:
		DBGTRACE2("unknown setting type: %d", param->type);
		return -1;
	}
	DBGTRACE2("setting changed %s='%s', %d", setting->name, setting->value,
		  ansi.length);
	return 0;
}

wstdcall void WIN_FUNC(NdisReadConfiguration,5)
	(NDIS_STATUS *status, struct ndis_configuration_parameter **param,
	 struct ndis_miniport_block *nmb, struct unicode_string *key,
	 enum ndis_parameter_type type)
{
	struct wrap_device_setting *setting;
	struct ansi_string ansi;
	char *keyname;
	int ret;

	TRACEENTER2("nmb: %p", nmb);
	ret = RtlUnicodeStringToAnsiString(&ansi, key, TRUE);
	if (ret || ansi.buf == NULL) {
		*param = NULL;
		*status = NDIS_STATUS_FAILURE;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return);
	}
	DBGTRACE3("%d, %s", type, ansi.buf);
	keyname = ansi.buf;

	if (down_interruptible(&loader_mutex))
		WARNING("couldn't obtain loader_mutex");
	nt_list_for_each_entry(setting, &nmb->wnd->wd->settings, list) {
		if (strnicmp(keyname, setting->name, ansi.length) == 0) {
			DBGTRACE2("setting %s='%s'", keyname, setting->value);
			up(&loader_mutex);
			*param = ndis_encode_setting(setting, type);
			if (*param)
				*status = NDIS_STATUS_SUCCESS;
			else
				*status = NDIS_STATUS_FAILURE;
			RtlFreeAnsiString(&ansi);
			DBGTRACE2("%d", *status);
			TRACEEXIT2(return);
		}
	}
	up(&loader_mutex);
	DBGTRACE2("setting %s not found (type:%d)", keyname, type);
	*status = NDIS_STATUS_FAILURE;
	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisWriteConfiguration,4)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 struct unicode_string *key, struct ndis_configuration_parameter *param)
{
	struct ansi_string ansi;
	char *keyname;
	struct wrap_device_setting *setting;

	TRACEENTER2("nmb: %p", nmb);
	if (RtlUnicodeStringToAnsiString(&ansi, key, TRUE)) {
		*status = NDIS_STATUS_FAILURE;
		TRACEEXIT2(return);
	}
	keyname = ansi.buf;
	DBGTRACE2("%s", keyname);

	if (down_interruptible(&loader_mutex))
		WARNING("couldn't obtain loader_mutex");
	nt_list_for_each_entry(setting, &nmb->wnd->wd->settings, list) {
		if (strnicmp(keyname, setting->name, ansi.length) == 0) {
			up(&loader_mutex);
			if (ndis_decode_setting(setting, param))
				*status = NDIS_STATUS_FAILURE;
			else
				*status = NDIS_STATUS_SUCCESS;
			RtlFreeAnsiString(&ansi);
			TRACEEXIT2(return);
		}
	}
	up(&loader_mutex);
	setting = kmalloc(sizeof(*setting), GFP_KERNEL);
	if (setting) {
		memset(setting, 0, sizeof(*setting));
		if (ansi.length == ansi.max_length)
			ansi.length--;
		memcpy(setting->name, keyname, ansi.length);
		setting->name[ansi.length] = 0;
		if (ndis_decode_setting(setting, param))
			*status = NDIS_STATUS_FAILURE;
		else {
			*status = NDIS_STATUS_SUCCESS;
			if (down_interruptible(&loader_mutex))
				WARNING("couldn't obtain loader_mutex");
			InsertTailList(&nmb->wnd->wd->settings, &setting->list);
			up(&loader_mutex);
		}
	} else
		*status = NDIS_STATUS_RESOURCES;

	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisInitializeString,2)
	(struct unicode_string *dest, UCHAR *src)
{
	struct ansi_string ansi;

	TRACEENTER2("");
	if (src == NULL) {
		dest->length = dest->max_length = 0;
		dest->buf = NULL;
	} else {
		RtlInitAnsiString(&ansi, src);
		RtlAnsiStringToUnicodeString(dest, &ansi, TRUE);
	}
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisInitAnsiString,2)
	(struct ansi_string *dst, CHAR *src)
{
	RtlInitAnsiString(dst, src);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisInitUnicodeString,2)
	(struct unicode_string *dest, const wchar_t *src)
{
	RtlInitUnicodeString(dest, src);
	return;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAnsiStringToUnicodeString,2)
	(struct unicode_string *dst, struct ansi_string *src)
{
	TRACEENTER2("");
	if (dst == NULL || src == NULL)
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	if (RtlAnsiStringToUnicodeString(dst, src, FALSE) == STATUS_SUCCESS)
		return NDIS_STATUS_SUCCESS;
	else
		return NDIS_STATUS_FAILURE;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisUnicodeStringToAnsiString,2)
	(struct ansi_string *dst, struct unicode_string *src)
{
	TRACEENTER2("");
	if (dst == NULL || src == NULL)
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	if (RtlUnicodeStringToAnsiString(dst, src, FALSE) == STATUS_SUCCESS)
		return NDIS_STATUS_SUCCESS;
	else
		return NDIS_STATUS_FAILURE;
}

wstdcall void WIN_FUNC(NdisMSetAttributesEx,5)
	(struct ndis_miniport_block *nmb, void *adapter_ctx,
	 UINT hangcheck_interval, UINT attributes, ULONG adaptortype)
{
	struct wrap_ndis_device *wnd;

	TRACEENTER2("%p, %p %d %08x, %d", nmb, adapter_ctx,
		    hangcheck_interval, attributes, adaptortype);
	wnd = nmb->wnd;
	nmb->adapter_ctx = adapter_ctx;

	if (attributes & NDIS_ATTRIBUTE_BUS_MASTER)
		pci_set_master(wnd->wd->pci.pdev);

	wnd->attributes = attributes;

	if (hangcheck_interval > 0)
		wnd->hangcheck_interval = 2 * hangcheck_interval * HZ;
	else
		wnd->hangcheck_interval = 2 * HZ;

	TRACEEXIT2(return);
}

wstdcall ULONG WIN_FUNC(NdisReadPciSlotInformation,5)
	(struct ndis_miniport_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrap_device *wd = nmb->wnd->wd;
	ULONG i;
	TRACEENTER4("%d", len);
	for (i = 0; i < len; i++)
		if (pci_read_config_byte(wd->pci.pdev, offset + i, &buf[i]) !=
		    PCIBIOS_SUCCESSFUL)
			break;
	TRACEEXIT4(return i);
}

wstdcall ULONG WIN_FUNC(NdisImmediateReadPciSlotInformation,5)
	(struct ndis_miniport_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	return NdisReadPciSlotInformation(nmb, slot, offset, buf, len);
}

wstdcall ULONG WIN_FUNC(NdisWritePciSlotInformation,5)
	(struct ndis_miniport_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrap_device *wd = nmb->wnd->wd;
	ULONG i;
	TRACEENTER4("%d", len);
	for (i = 0; i < len; i++)
		if (pci_write_config_byte(wd->pci.pdev, offset + i, buf[i]) !=
		    PCIBIOS_SUCCESSFUL)
			break;
	TRACEEXIT4(return i);
}

wstdcall void WIN_FUNC(NdisReadPortUchar,3)
	(struct ndis_miniport_block *nmb, ULONG port, char *data)
{
	*data = inb(port);
}

wstdcall void WIN_FUNC(NdisImmediateReadPortUchar,3)
	(struct ndis_miniport_block *nmb, ULONG port, char *data)
{
	*data = inb(port);
}

wstdcall void WIN_FUNC(NdisWritePortUchar,3)
	(struct ndis_miniport_block *nmb, ULONG port, char data)
{
	outb(data, port);
}

wstdcall void WIN_FUNC(NdisImmediateWritePortUchar,3)
	(struct ndis_miniport_block *nmb, ULONG port, char data)
{
	outb(data, port);
}

wstdcall void WIN_FUNC(NdisMQueryAdapterResources,4)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 NDIS_RESOURCE_LIST *resource_list, UINT *size)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	NDIS_RESOURCE_LIST *list;
	UINT resource_length;

	list = &wnd->wd->resource_list->list->partial_resource_list;
	resource_length = sizeof(struct cm_partial_resource_list) +
		sizeof(struct cm_partial_resource_descriptor) *
		(list->count - 1);
	DBGTRACE2("%p, %p,%d (%d), %p %d %d", wnd, resource_list, *size,
		  resource_length, &list->partial_descriptors[list->count-1],
		  list->partial_descriptors[list->count-1].u.interrupt.level,
		  list->partial_descriptors[list->count-1].u.interrupt.vector);
	if (*size < sizeof(*list)) {
		*size = resource_length;
		*status = NDIS_STATUS_BUFFER_TOO_SHORT;
	} else {
		ULONG count;
		if (*size >= resource_length) {
			*size = resource_length;
			count = list->count;
		} else {
			UINT n = sizeof(*list);
			count = 1;
			while (count++ < list->count && n < *size)
				n += sizeof(list->partial_descriptors);
			*size = n;
		}
		memcpy(resource_list, list, *size);
		resource_list->count = count;
		*status = NDIS_STATUS_SUCCESS;
	}
	TRACEEXIT2(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMPciAssignResources,3)
	(struct ndis_miniport_block *nmb, ULONG slot_number,
	 NDIS_RESOURCE_LIST **resources)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER2("%p, %p", wnd, wnd->wd->resource_list);
	*resources = &wnd->wd->resource_list->list->partial_resource_list;
	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMMapIoSpace,4)
	(void **virt, struct ndis_miniport_block *nmb,
	 NDIS_PHY_ADDRESS phy_addr, UINT len)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER2("%016llx, %d", phy_addr, len);
	*virt = MmMapIoSpace(phy_addr, len, MmCached);
	if (*virt == NULL) {
		ERROR("ioremap failed");
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	}
	wnd->mem_start = phy_addr;
	wnd->mem_end = phy_addr + len;
	DBGTRACE2("%p", *virt);
	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMUnmapIoSpace,3)
	(struct ndis_miniport_block *nmb, void *virt, UINT len)
{
	TRACEENTER2("%p, %d", virt, len);
	MmUnmapIoSpace(virt, len);
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisAllocateSpinLock,1)
	(struct ndis_spinlock *lock)
{
	DBGTRACE4("lock %p, %lu", lock, lock->klock);
	KeInitializeSpinLock(&lock->klock);
	lock->irql = PASSIVE_LEVEL;
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisFreeSpinLock,1)
	(struct ndis_spinlock *lock)
{
	DBGTRACE4("lock %p, %lu", lock, lock->klock);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisAcquireSpinLock,1)
	(struct ndis_spinlock *lock)
{
	DBGTRACE6("lock %p, %lu", lock, lock->klock);
	lock->irql = nt_spin_lock_irql(&lock->klock, DISPATCH_LEVEL);
	TRACEEXIT6(return);
}

wstdcall void WIN_FUNC(NdisReleaseSpinLock,1)
	(struct ndis_spinlock *lock)
{
	DBGTRACE6("lock %p, %lu", lock, lock->klock);
	nt_spin_unlock_irql(&lock->klock, lock->irql);
	TRACEEXIT6(return);
}

wstdcall void WIN_FUNC(NdisDprAcquireSpinLock,1)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	nt_spin_lock(&lock->klock);
	TRACEEXIT6(return);
}

wstdcall void WIN_FUNC(NdisDprReleaseSpinLock,1)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	nt_spin_unlock(&lock->klock);
	TRACEEXIT6(return);
}

wstdcall void WIN_FUNC(NdisInitializeReadWriteLock,1)
	(struct ndis_rw_lock *rw_lock)
{
	TRACEENTER3("%p", rw_lock);
	memset(rw_lock, 0, sizeof(*rw_lock));
	KeInitializeSpinLock(&rw_lock->u.s.klock);
	TRACEEXIT3(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMAllocateMapRegisters,5)
	(struct ndis_miniport_block *nmb, UINT dmachan,
	 NDIS_DMA_SIZE dmasize, ULONG basemap, ULONG max_buf_size)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER2("%p, %d %d %d %d",
		    wnd, dmachan, dmasize, basemap, max_buf_size);
	if (wnd->dma_map_count > 0) {
		WARNING("%s: map registers already allocated: %u",
			wnd->net_dev->name, wnd->dma_map_count);
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	}
	/* since memory for buffer is allocated with kmalloc, buffer
	 * is physically contiguous, so entire map will fit in one
	 * register */
	if (basemap > 64) {
		WARNING("Windows driver %s requesting too many (%u) "
			"map registers", wnd->wd->driver->name, basemap);
		/* As per NDIS, NDIS_STATUS_RESOURCES should be
		 * retrned, but with that Atheros PCI driver fails -
		 * for now tolerate it */
//		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	}

	wnd->dma_map_addr = kmalloc(basemap * sizeof(*(wnd->dma_map_addr)),
				    GFP_KERNEL);
	if (!wnd->dma_map_addr)
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	memset(wnd->dma_map_addr, 0, basemap * sizeof(*(wnd->dma_map_addr)));
	wnd->dma_map_count = basemap;
	DBGTRACE2("%u", wnd->dma_map_count);
	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMFreeMapRegisters,1)
	(struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	int i;

	TRACEENTER2("wnd: %p", wnd);
	if (wnd->dma_map_addr) {
		for (i = 0; i < wnd->dma_map_count; i++) {
			if (wnd->dma_map_addr[i])
				WARNING("%s: dma addr %p not freed by "
					"Windows driver", wnd->net_dev->name,
					(void *)wnd->dma_map_addr[i]);
		}
		kfree(wnd->dma_map_addr);
		wnd->dma_map_addr = NULL;
	} else
		WARNING("map registers already freed?");
	wnd->dma_map_count = 0;
	TRACEEXIT2(return);
}

wstdcall void WIN_FUNC(NdisMStartBufferPhysicalMapping,6)
	(struct ndis_miniport_block *nmb, ndis_buffer *buf,
	 ULONG index, BOOLEAN write_to_dev,
	 struct ndis_phy_addr_unit *phy_addr_array, UINT *array_size)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER3("%p, %p, %u, %u", wnd, buf, index, wnd->dma_map_count);
	if (wnd->use_sg_dma || !write_to_dev ||
	    index >= wnd->dma_map_count) {
		WARNING("invalid request: %d, %d, %d, %d", wnd->use_sg_dma,
			write_to_dev, index, wnd->dma_map_count);
		phy_addr_array[0].phy_addr = 0;
		phy_addr_array[0].length = 0;
		*array_size = 0;
		return;
	}
	if (wnd->dma_map_addr[index]) {
		DBGTRACE2("buffer %p at %d is already mapped: %lx", buf, index,
			  (unsigned long)wnd->dma_map_addr[index]);
//		*array_size = 1;
		return;
	}
	DBGTRACE3("%p, %p, %u", buf, MmGetSystemAddressForMdl(buf),
		  MmGetMdlByteCount(buf));
	DBG_BLOCK(4) {
		dump_bytes(__FUNCTION__, MmGetSystemAddressForMdl(buf),
			   MmGetMdlByteCount(buf));
	}
	wnd->dma_map_addr[index] = 
		PCI_DMA_MAP_SINGLE(wnd->wd->pci.pdev,
				   MmGetSystemAddressForMdl(buf),
				   MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);
	phy_addr_array[0].phy_addr = wnd->dma_map_addr[index];
	phy_addr_array[0].length = MmGetMdlByteCount(buf);
	DBGTRACE4("%Lx, %d, %d", phy_addr_array[0].phy_addr,
		  phy_addr_array[0].length, index);
	*array_size = 1;
}

wstdcall void WIN_FUNC(NdisMCompleteBufferPhysicalMapping,3)
	(struct ndis_miniport_block *nmb, ndis_buffer *buf, ULONG index)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER3("%p, %p %u (%u)", wnd, buf, index, wnd->dma_map_count);

	if (wnd->use_sg_dma)
		WARNING("buffer %p may have been unmapped already", buf);
	if (index >= wnd->dma_map_count) {
		ERROR("invalid map register (%u >= %u)",
		      index, wnd->dma_map_count);
		return;
	}
	DBGTRACE4("%lx", (unsigned long)wnd->dma_map_addr[index]);
	if (wnd->dma_map_addr[index]) {
		PCI_DMA_UNMAP_SINGLE(wnd->wd->pci.pdev, wnd->dma_map_addr[index],
				     MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);
		wnd->dma_map_addr[index] = 0;
	} else
		WARNING("map registers at %u not used", index);
}

wstdcall void WIN_FUNC(NdisMAllocateSharedMemory,5)
	(struct ndis_miniport_block *nmb, ULONG size,
	 BOOLEAN cached, void **virt, NDIS_PHY_ADDRESS *phys)
{
	dma_addr_t dma_addr;
	struct wrap_device *wd = nmb->wnd->wd;

	TRACEENTER3("size: %u, cached: %d", size, cached);
	*virt = PCI_DMA_ALLOC_COHERENT(wd->pci.pdev, size, &dma_addr);
	if (!*virt)
		WARNING("couldn't allocate %d bytes of %scached DMA memory",
			size, cached ? "" : "un-");
	*phys = dma_addr;
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisMFreeSharedMemory,5)
	(struct ndis_miniport_block *nmb, ULONG size, BOOLEAN cached,
	 void *virt, NDIS_PHY_ADDRESS addr)
{
	struct wrap_device *wd = nmb->wnd->wd;
	TRACEENTER3("");
	PCI_DMA_FREE_COHERENT(wd->pci.pdev, size, virt, addr);
	TRACEEXIT3(return);
}

wstdcall void alloc_shared_memory_async(void *arg1, void *arg2)
{
	struct wrap_ndis_device *wnd;
	struct alloc_shared_mem *alloc_shared_mem;
	struct miniport_char *miniport;
	void *virt;
	NDIS_PHY_ADDRESS phys;
	KIRQL irql;

	wnd = arg1;
	alloc_shared_mem = arg2;
	miniport = &wnd->wd->driver->ndis_driver->miniport;
	NdisMAllocateSharedMemory(wnd->nmb, alloc_shared_mem->size,
				  alloc_shared_mem->cached, &virt, &phys);
	irql = serialize_lock_irql(wnd);
	LIN2WIN5(miniport->alloc_complete, wnd->nmb, virt,
		 &phys, alloc_shared_mem->size, alloc_shared_mem->ctx);
	serialize_unlock_irql(wnd, irql);
	kfree(alloc_shared_mem);
}
WIN_FUNC_DECL(alloc_shared_memory_async,2)

wstdcall NDIS_STATUS WIN_FUNC(NdisMAllocateSharedMemoryAsync,4)
	(struct ndis_miniport_block *nmb, ULONG size, BOOLEAN cached,
	 void *ctx)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct alloc_shared_mem *alloc_shared_mem;

	TRACEENTER3("wnd: %p", wnd);
	alloc_shared_mem = kmalloc(sizeof(*alloc_shared_mem), gfp_irql());
	if (!alloc_shared_mem) {
		WARNING("couldn't allocate memory");
		return NDIS_STATUS_FAILURE;
	}

	alloc_shared_mem->size = size;
	alloc_shared_mem->cached = cached;
	alloc_shared_mem->ctx = ctx;
	if (schedule_ntos_work_item(WIN_FUNC_PTR(alloc_shared_memory_async,2),
				    wnd, alloc_shared_mem))
		TRACEEXIT3(return NDIS_STATUS_FAILURE);
	TRACEEXIT3(return NDIS_STATUS_PENDING);
}

/* Some drivers allocate NDIS_BUFFER (aka MDL) very often; instead of
 * allocating and freeing with kernel functions, we chain them into
 * ndis_buffer_pool. When an MDL is freed, it is added to the list of
 * free MDLs. When allocated, we first check if there is one in free
 * list and if so just return it; otherwise, we allocate a new one and
 * return that. This reduces memory fragmentation. Windows DDK says
 * that the driver itself shouldn't check what is returned in
 * pool_handle, presumably because buffer pools are not used in
 * XP. However, as long as driver follows rest of the semantics - that
 * it should indicate maximum number of MDLs used with num_descr and
 * pass the same pool_handle in other buffer functions, this should
 * work. Sadly, though, NdisFreeBuffer doesn't pass the pool_handle,
 * so we use 'process' field of MDL to store pool_handle. */

wstdcall void WIN_FUNC(NdisAllocateBufferPool,3)
	(NDIS_STATUS *status, struct ndis_buffer_pool **pool_handle,
	 UINT num_descr)
{
	struct ndis_buffer_pool *pool;

	TRACEENTER1("buffers: %d", num_descr);
	pool = kmalloc(sizeof(*pool), gfp_irql());
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT3(return);
	}
	nt_spin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->free_descr = NULL;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	DBGTRACE1("pool: %p, num_descr: %d", pool, num_descr);
	TRACEEXIT1(return);
}

wstdcall void WIN_FUNC(NdisAllocateBuffer,5)
	(NDIS_STATUS *status, ndis_buffer **buffer,
	 struct ndis_buffer_pool *pool, void *virt, UINT length)
{
	ndis_buffer *descr;

	TRACEENTER4("pool: %p, allocated: %d",
		    pool, pool->num_allocated_descr);
	if (!pool) {
		*status = NDIS_STATUS_FAILURE;
		TRACEEXIT4(return);
	}
	DBG_BLOCK(2) {
		if (pool->num_allocated_descr > pool->max_descr)
			WARNING("pool %p is full: %d(%d)", pool,
				pool->num_allocated_descr, pool->max_descr);
	}
	descr = atomic_remove_list_head(pool->free_descr, oldhead->next);
	/* TODO: make sure this mdl can map given buffer */
	if (descr) {
		typeof(descr->flags) flags = descr->flags;
		MmInitializeMdl(descr, virt, length);
//		MmBuildMdlForNonPagedPool(descr);
		descr->flags |= MDL_SOURCE_IS_NONPAGED_POOL;
		if (flags & MDL_CACHE_ALLOCATED)
			descr->flags |= MDL_CACHE_ALLOCATED;
	} else {
		descr = allocate_init_mdl(virt, length);
		if (!descr) {
			WARNING("couldn't allocate buffer");
			*status = NDIS_STATUS_FAILURE;
			TRACEEXIT4(return);
		}
		DBGTRACE4("allocated buffer %p for %p, %d",
			  descr, virt, length);
		atomic_inc_var(pool->num_allocated_descr);
		MmBuildMdlForNonPagedPool(descr);
	}
//	descr->flags |= MDL_ALLOCATED_FIXED_SIZE |
//		MDL_MAPPED_TO_SYSTEM_VA | MDL_PAGES_LOCKED;
	descr->pool = pool;
	*buffer = descr;
	*status = NDIS_STATUS_SUCCESS;
	DBGTRACE4("buffer: %p", descr);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisFreeBuffer,1)
	(ndis_buffer *descr)
{
	struct ndis_buffer_pool *pool;

	TRACEENTER4("descr: %p", descr);
	if (!descr || !descr->pool) {
		ERROR("invalid buffer");
		TRACEEXIT4(return);
	}
	pool = descr->pool;
	if (pool->num_allocated_descr > MAX_ALLOCATED_NDIS_BUFFERS) {
		/* NB NB NB: set mdl's 'pool' field to NULL before
		 * calling free_mdl; otherwise free_mdl calls
		 * NdisFreeBuffer causing deadlock (for spinlock) */
		atomic_dec_var(pool->num_allocated_descr);
		descr->pool = NULL;
		free_mdl(descr);
	} else
		atomic_insert_list_head(pool->free_descr, descr->next, descr);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisFreeBufferPool,1)
	(struct ndis_buffer_pool *pool)
{
	ndis_buffer *cur, *next;
	KIRQL irql;

	DBGTRACE3("pool: %p", pool);
	if (!pool) {
		WARNING("invalid pool");
		TRACEEXIT3(return);
	}
	irql = nt_spin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	cur = pool->free_descr;
	while (cur) {
		next = cur->next;
		cur->pool = NULL;
		free_mdl(cur);
		cur = next;
	}
	nt_spin_unlock_irql(&pool->lock, irql);
	kfree(pool);
	pool = NULL;
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisAdjustBufferLength,2)
	(ndis_buffer *buffer, UINT length)
{
	TRACEENTER4("%p, %d", buffer, length);
	buffer->bytecount = length;
}

wstdcall void WIN_FUNC(NdisQueryBuffer,3)
	(ndis_buffer *buffer, void **virt, UINT *length)
{
	TRACEENTER4("buffer: %p", buffer);
	if (virt)
		*virt = MmGetSystemAddressForMdl(buffer);
	*length = MmGetMdlByteCount(buffer);
	DBGTRACE4("%u", *length);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisQueryBufferSafe,4)
	(ndis_buffer *buffer, void **virt, UINT *length,
	 enum mm_page_priority priority)
{
	TRACEENTER4("%p, %p, %p, %d", buffer, virt, length, priority);
	if (virt)
		*virt = MmGetSystemAddressForMdlSafe(buffer, priority);
	*length = MmGetMdlByteCount(buffer);
	DBGTRACE4("%u", *length);
}

wstdcall void *WIN_FUNC(NdisBufferVirtualAddress,1)
	(ndis_buffer *buffer)
{
	TRACEENTER3("%p", buffer);
	return MmGetSystemAddressForMdl(buffer);
}

wstdcall ULONG WIN_FUNC(NdisBufferLength,1)
	(ndis_buffer *buffer)
{
	TRACEENTER3("%p", buffer);
	return MmGetMdlByteCount(buffer);
}

wstdcall void WIN_FUNC(NdisQueryBufferOffset,3)
	(ndis_buffer *buffer, UINT *offset, UINT *length)
{
	TRACEENTER3("%p", buffer);
	*offset = MmGetMdlByteOffset(buffer);
	*length = MmGetMdlByteCount(buffer);
	DBGTRACE3("%d, %d", *offset, *length);
}

wstdcall void WIN_FUNC(NdisUnchainBufferAtBack,2)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	ndis_buffer *b, *btail;

	TRACEENTER3("%p", packet);
	b = packet->private.buffer_head;
	if (!b) {
		/* no buffer in packet */
		*buffer = NULL;
		TRACEEXIT3(return);
	}
	btail = packet->private.buffer_tail;
	*buffer = btail;
	if (b == btail) {
		/* one buffer in packet */
		packet->private.buffer_head = NULL;
		packet->private.buffer_tail = NULL;
	} else {
		while (b->next != btail)
			b = b->next;
		packet->private.buffer_tail = b;
		b->next = NULL;
	}
	packet->private.valid_counts = FALSE;
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisUnchainBufferAtFront,2)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	TRACEENTER3("%p", packet);
	if (packet->private.buffer_head == NULL) {
		/* no buffer in packet */
		*buffer = NULL;
		TRACEEXIT3(return);
	}

	*buffer = packet->private.buffer_head;
	if (packet->private.buffer_head == packet->private.buffer_tail) {
		/* one buffer in packet */
		packet->private.buffer_head = NULL;
		packet->private.buffer_tail = NULL;
	} else
		packet->private.buffer_head = (*buffer)->next;

	packet->private.valid_counts = FALSE;
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisGetFirstBufferFromPacketSafe,6)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	ndis_buffer *b = packet->private.buffer_head;

	TRACEENTER3("%p(%p)", packet, b);
	*first_buffer = b;
	if (b) {
		*first_buffer_va = MmGetSystemAddressForMdlSafe(b, priority);
		*first_buffer_length = *total_buffer_length =
			MmGetMdlByteCount(b);
		for (b = b->next; b; b = b->next)
			*total_buffer_length += MmGetMdlByteCount(b);
	} else {
		*first_buffer_va = NULL;
		*first_buffer_length = 0;
		*total_buffer_length = 0;
	}
	DBGTRACE3("%p, %d, %d", *first_buffer_va, *first_buffer_length,
		  *total_buffer_length);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisGetFirstBufferFromPacket,6)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	NdisGetFirstBufferFromPacketSafe(packet, first_buffer,
					 first_buffer_va, first_buffer_length,
					 total_buffer_length,
					 NormalPagePriority);
}

wstdcall void WIN_FUNC(NdisAllocatePacketPoolEx,5)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT overflowsize, UINT proto_rsvd_length)
{
	struct ndis_packet_pool *pool;

	TRACEENTER3("buffers: %d, length: %d", num_descr, proto_rsvd_length);
	pool = kmalloc(sizeof(*pool), gfp_irql());
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT3(return);
	}
	memset(pool, 0, sizeof(*pool));
	nt_spin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->num_used_descr = 0;
	pool->free_descr = NULL;
	pool->proto_rsvd_length = proto_rsvd_length;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	DBGTRACE3("pool: %p", pool);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisAllocatePacketPool,4)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT proto_rsvd_length)
{
	NdisAllocatePacketPoolEx(status, pool_handle, num_descr, 0,
				 proto_rsvd_length);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisFreePacketPool,1)
	(struct ndis_packet_pool *pool)
{
	struct ndis_packet *packet, *next;
	KIRQL irql;

	TRACEENTER3("pool: %p", pool);
	if (!pool) {
		WARNING("invalid pool");
		TRACEEXIT3(return);
	}
	irql = nt_spin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	packet = pool->free_descr;
	while (packet) {
		next = (NDIS_PACKET_OOB_DATA(packet))->next;
		kfree(packet);
		packet = next;
	}
	pool->num_allocated_descr = 0;
	pool->num_used_descr = 0;
	pool->free_descr = NULL;
	nt_spin_unlock_irql(&pool->lock, irql);
	kfree(pool);
	TRACEEXIT3(return);
}

wstdcall UINT WIN_FUNC(NdisPacketPoolUsage,1)
	(struct ndis_packet_pool *pool)
{
	TRACEEXIT4(return pool->num_used_descr);
}

wstdcall void WIN_FUNC(NdisAllocatePacket,3)
	(NDIS_STATUS *status, struct ndis_packet **packet,
	 struct ndis_packet_pool *pool)
{
	struct ndis_packet *ndis_packet;
	int packet_length;

	TRACEENTER4("pool: %p", pool);
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT4(return);
	}
	DBG_BLOCK(2) {
		if (pool->num_used_descr >= pool->max_descr)
			WARNING("pool %p is full: %d(%d)", pool,
				pool->num_used_descr, pool->max_descr);
	}
	/* packet has space for 1 byte in protocol_reserved field */
	packet_length = sizeof(*ndis_packet) - 1 + pool->proto_rsvd_length +
		sizeof(struct ndis_packet_oob_data);
	ndis_packet =
		atomic_remove_list_head(pool->free_descr,
					(NDIS_PACKET_OOB_DATA(oldhead))->next);
	if (!ndis_packet) {
		ndis_packet = kmalloc(packet_length, gfp_irql());
		if (!ndis_packet) {
			WARNING("couldn't allocate packet");
			*status = NDIS_STATUS_RESOURCES;
			return;
		}
		atomic_inc_var(pool->num_allocated_descr);
	}
	DBGTRACE3("packet: %p", ndis_packet);
	atomic_inc_var(pool->num_used_descr);
	memset(ndis_packet, 0, packet_length);
	ndis_packet->private.oob_offset = packet_length -
		sizeof(struct ndis_packet_oob_data);
	ndis_packet->private.packet_flags = fPACKET_ALLOCATED_BY_NDIS;
//		| NDIS_PROTOCOL_ID_TCP_IP;
	ndis_packet->private.pool = pool;
	*packet = ndis_packet;
	*status = NDIS_STATUS_SUCCESS;
	DBGTRACE4("packet: %p, pool: %p", ndis_packet, pool);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisDprAllocatePacket,3)
	(NDIS_STATUS *status, struct ndis_packet **packet,
	 struct ndis_packet_pool *pool)
{
	NdisAllocatePacket(status, packet, pool);
}

wstdcall void WIN_FUNC(NdisFreePacket,1)
	(struct ndis_packet *descr)
{
	struct ndis_packet_pool *pool;

	TRACEENTER3("packet: %p, pool: %p", descr, descr->private.pool);
	pool = descr->private.pool;
	if (!pool) {
		ERROR("pool for descriptor %p is invalid", descr);
		TRACEEXIT4(return);
	}
	atomic_dec_var(pool->num_used_descr);
	if (pool->num_allocated_descr > MAX_ALLOCATED_NDIS_PACKETS) {
		kfree(descr);
		atomic_dec_var(pool->num_allocated_descr);
	} else {
		struct ndis_packet_oob_data *oob_data;
		oob_data = NDIS_PACKET_OOB_DATA(descr);
		atomic_insert_list_head(pool->free_descr, oob_data->next, descr);
	}
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisCopyFromPacketToPacketSafe,7)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied,
	 enum mm_page_priority priority)
{
	UINT dst_n, src_n, n, left;
	ndis_buffer *dst_buf;
	ndis_buffer *src_buf;

	TRACEENTER4("");
	if (!dst || !src) {
		*num_copied = 0;
		TRACEEXIT4(return);
	}

	dst_buf = dst->private.buffer_head;
	src_buf = src->private.buffer_head;

	if (!dst_buf || !src_buf) {
		*num_copied = 0;
		TRACEEXIT4(return);
	}
	dst_n = MmGetMdlByteCount(dst_buf) - dst_offset;
	src_n = MmGetMdlByteCount(src_buf) - src_offset;

	n = min(src_n, dst_n);
	n = min(n, num_to_copy);
	memcpy(MmGetSystemAddressForMdl(dst_buf) + dst_offset,
	       MmGetSystemAddressForMdl(src_buf) + src_offset,
	       n);

	left = num_to_copy - n;
	while (left > 0) {
		src_offset += n;
		dst_offset += n;
		dst_n -= n;
		src_n -= n;
		if (dst_n == 0) {
			dst_buf = dst_buf->next;
			if (!dst_buf)
				break;
			dst_n = MmGetMdlByteCount(dst_buf);
			dst_offset = 0;
		}
		if (src_n == 0) {
			src_buf = src_buf->next;
			if (!src_buf)
				break;
			src_n = MmGetMdlByteCount(src_buf);
			src_offset = 0;
		}

		n = min(src_n, dst_n);
		n = min(n, left);
		memcpy(MmGetSystemAddressForMdl(dst_buf) + dst_offset,
		       MmGetSystemAddressForMdl(src_buf) + src_offset,
		       n);
		left -= n;
	}
	*num_copied = num_to_copy - left;
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisCopyFromPacketToPacket,6)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied)
{
	NdisCopyFromPacketToPacketSafe(dst, dst_offset, num_to_copy,
				       src, src_offset, num_copied,
				       NormalPagePriority);
	return;
}

wstdcall void WIN_FUNC(NdisIMCopySendPerPacketInfo,2)
	(struct ndis_packet *dst, struct ndis_packet *src)
{
	struct ndis_packet_oob_data *dst_oob, *src_oob;
	dst_oob = NDIS_PACKET_OOB_DATA(dst);
	src_oob = NDIS_PACKET_OOB_DATA(src);
	memcpy(&dst_oob->extension, &src_oob->extension,
	       sizeof(dst_oob->extension));
	return;
}

wstdcall void WIN_FUNC(NdisSend,3)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 struct ndis_packet *packet)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct miniport_char *miniport;
	KIRQL irql;

	miniport = &wnd->wd->driver->ndis_driver->miniport;
	if (miniport->send_packets) {
		irql = serialize_lock_irql(wnd);
		LIN2WIN3(miniport->send_packets, wnd->nmb->adapter_ctx,
			 &packet, 1);
		serialize_unlock_irql(wnd, irql);
		if (deserialized_driver(wnd))
			*status = NDIS_STATUS_PENDING;
		else {
			struct ndis_packet_oob_data *oob_data;
			oob_data = NDIS_PACKET_OOB_DATA(packet);
			*status = oob_data->status;
			switch (*status) {
			case NDIS_STATUS_SUCCESS:
				free_tx_packet(wnd, packet, *status);
				break;
			case NDIS_STATUS_PENDING:
				break;
			case NDIS_STATUS_RESOURCES:
				atomic_dec_var(wnd->tx_ok);
				break;
			case NDIS_STATUS_FAILURE:
			default:
				free_tx_packet(wnd, packet, *status);
				break;
			}
		}
	} else {
		irql = serialize_lock_irql(wnd);
		*status = LIN2WIN3(miniport->send, wnd->nmb->adapter_ctx,
				   packet, 0);
		serialize_unlock_irql(wnd, irql);
		switch (*status) {
		case NDIS_STATUS_SUCCESS:
			free_tx_packet(wnd, packet, *status);
			break;
		case NDIS_STATUS_PENDING:
			break;
		case NDIS_STATUS_RESOURCES:
			atomic_dec_var(wnd->tx_ok);
			break;
		case NDIS_STATUS_FAILURE:
		default:
			free_tx_packet(wnd, packet, *status);
			break;
		}
	}
	TRACEEXIT3(return);
}

wstdcall void wrap_miniport_timer(struct kdpc *kdpc, void *ctx, void *arg1,
				  void *arg2)
{
	struct ndis_miniport_timer *timer;
	struct ndis_miniport_block *nmb;

	timer = ctx;
	TRACEENTER5("timer: %p, func: %p, ctx: %p, nmb: %p",
		    timer, timer->func, timer->ctx, timer->nmb);
	nmb = timer->nmb;
	/* already called at DISPATCH_LEVEL */
	if (!deserialized_driver(nmb->wnd))
		serialize_lock(nmb->wnd);
	LIN2WIN4(timer->func, kdpc, timer->ctx, kdpc->arg1, kdpc->arg2);
	if (!deserialized_driver(nmb->wnd))
		serialize_unlock(nmb->wnd);
	TRACEEXIT5(return);
}
WIN_FUNC_DECL(wrap_miniport_timer,4)

wstdcall void WIN_FUNC(NdisMInitializeTimer,4)
	(struct ndis_miniport_timer *timer, struct ndis_miniport_block *nmb,
	 DPC func, void *ctx)
{
	TRACEENTER4("timer: %p, func: %p, ctx: %p, nmb: %p",
		    timer, func, ctx, nmb);
	timer->func = func;
	timer->ctx = ctx;
	timer->nmb = nmb;
//	KeInitializeDpc(&timer->kdpc, func, ctx);
	KeInitializeDpc(&timer->kdpc, WIN_FUNC_PTR(wrap_miniport_timer,4),
			timer);
	wrap_init_timer(&timer->nt_timer, NotificationTimer, &timer->kdpc, nmb);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisMSetPeriodicTimer,2)
	(struct ndis_miniport_timer *timer, UINT period_ms)
{
	unsigned long expires = MSEC_TO_HZ(period_ms) + 1;

	TRACEENTER4("%p, %u, %ld", timer, period_ms, expires);
	wrap_set_timer(&timer->nt_timer, expires, expires, NULL);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisMCancelTimer,2)
	(struct ndis_miniport_timer *timer, BOOLEAN *canceled)
{
	TRACEENTER4("%p", timer);
	*canceled = KeCancelTimer(&timer->nt_timer);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisInitializeTimer,3)
	(struct ndis_timer *timer, void *func, void *ctx)
{
	TRACEENTER4("%p, %p, %p", timer, func, ctx);
	KeInitializeDpc(&timer->kdpc, func, ctx);
	wrap_init_timer(&timer->nt_timer, NotificationTimer, &timer->kdpc, NULL);
	TRACEEXIT4(return);
}

/* NdisMSetTimer is a macro that calls NdisSetTimer with
 * ndis_miniport_timer typecast to ndis_timer */

wstdcall void WIN_FUNC(NdisSetTimer,2)
	(struct ndis_timer *timer, UINT duetime_ms)
{
	unsigned long expires = MSEC_TO_HZ(duetime_ms) + 1;

	TRACEENTER4("%p, %p, %u, %ld", timer, timer->nt_timer.wrap_timer,
		    duetime_ms, expires);
	wrap_set_timer(&timer->nt_timer, expires, 0, NULL);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisCancelTimer,2)
	(struct ndis_timer *timer, BOOLEAN *canceled)
{
	TRACEENTER4("%p", timer);
	*canceled = KeCancelTimer(&timer->nt_timer);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisReadNetworkAddress,4)
	(NDIS_STATUS *status, void **addr, UINT *len,
	 struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct ndis_configuration_parameter *param;
	struct unicode_string key;
	struct ansi_string ansi;
	int ret;

	TRACEENTER1("");
	RtlInitAnsiString(&ansi, "NetworkAddress");
	*len = 0;
	*status = NDIS_STATUS_FAILURE;
	if (RtlAnsiStringToUnicodeString(&key, &ansi, TRUE) != STATUS_SUCCESS)
		TRACEEXIT1(return);

	NdisReadConfiguration(status, &param, nmb, &key, NdisParameterString);
	RtlFreeUnicodeString(&key);

	if (*status == NDIS_STATUS_SUCCESS) {
		int int_mac[ETH_ALEN];
		ret = RtlUnicodeStringToAnsiString(&ansi, &param->data.string,
						   TRUE);
		if (ret != NDIS_STATUS_SUCCESS)
			TRACEEXIT1(return);

		ret = sscanf(ansi.buf, MACSTR, MACINTADR(int_mac));
		if (ret != ETH_ALEN)
			ret = sscanf(ansi.buf, MACSTRSEP, MACINTADR(int_mac));
		RtlFreeAnsiString(&ansi);
		if (ret == ETH_ALEN) {
			int i;
			for (i = 0; i < ETH_ALEN; i++)
				wnd->mac[i] = int_mac[i];
			printk(KERN_INFO "%s: %s ethernet device " MACSTRSEP
			       "\n", wnd->net_dev->name, DRIVER_NAME,
			       MAC2STR(wnd->mac));
			*len = ETH_ALEN;
			*addr = wnd->mac;
			*status = NDIS_STATUS_SUCCESS;
		}
	}

	TRACEEXIT1(return);
}

wstdcall void WIN_FUNC(NdisMRegisterAdapterShutdownHandler,3)
	(struct ndis_miniport_block *nmb, void *ctx, void *func)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER1("%p", func);
	wnd->wd->driver->ndis_driver->miniport.shutdown = func;
	wnd->shutdown_ctx = ctx;
}

wstdcall void WIN_FUNC(NdisMDeregisterAdapterShutdownHandler,1)
	(struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	wnd->wd->driver->ndis_driver->miniport.shutdown = NULL;
	wnd->shutdown_ctx = NULL;
}

static void ndis_irq_handler(unsigned long data)
{
	struct wrap_ndis_device *wnd = (struct wrap_ndis_device *)data;
	struct miniport_char *miniport;

	miniport = &wnd->wd->driver->ndis_driver->miniport;
	if_serialize_lock(wnd);
	LIN2WIN1(miniport->handle_interrupt, wnd->nmb->adapter_ctx);
	if (miniport->enable_interrupt)
		LIN2WIN1(miniport->enable_interrupt, wnd->nmb->adapter_ctx);
	if_serialize_unlock(wnd);
}

irqreturn_t ndis_isr(int irq, void *data ISR_PT_REGS_PARAM_DECL)
{
	struct wrap_ndis_device *wnd = data;
	struct miniport_char *miniport;
	BOOLEAN recognized, queue_handler;

	miniport = &wnd->wd->driver->ndis_driver->miniport;
	/* this spinlock should be shared with NdisMSynchronizeWithInterrupt
	 */
	nt_spin_lock(&wnd->ndis_irq->lock);
	if (wnd->ndis_irq->req_isr)
		LIN2WIN3(miniport->isr, &recognized, &queue_handler,
			 wnd->nmb->adapter_ctx);
	else { //if (miniport->disable_interrupt)
		LIN2WIN1(miniport->disable_interrupt, wnd->nmb->adapter_ctx);
		/* it is not shared interrupt, so handler must be called */
		recognized = queue_handler = TRUE;
	}
	nt_spin_unlock(&wnd->ndis_irq->lock);
	if (recognized) {
		if (queue_handler)
			tasklet_schedule(&wnd->irq_tasklet);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterInterrupt,7)
	(struct ndis_irq *ndis_irq, struct ndis_miniport_block *nmb,
	 UINT vector, UINT level, BOOLEAN req_isr,
	 BOOLEAN shared, enum kinterrupt_mode mode)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER1("%p, vector:%d, level:%d, req_isr:%d, shared:%d, mode:%d",
		    ndis_irq, vector, level, req_isr, shared, mode);

	ndis_irq->irq.irq = vector;
	ndis_irq->wnd = wnd;
	ndis_irq->req_isr = req_isr;
	if (shared && !req_isr)
		WARNING("shared but dynamic interrupt!");
	ndis_irq->shared = shared;
	nt_spin_lock_init(&ndis_irq->lock);
	wnd->ndis_irq = ndis_irq;
	tasklet_init(&wnd->irq_tasklet, ndis_irq_handler, (unsigned long)wnd);
	if (request_irq(vector, ndis_isr, req_isr ? SA_SHIRQ : 0,
			wnd->net_dev->name, wnd)) {
		printk(KERN_WARNING "%s: request for IRQ %d failed\n",
		       DRIVER_NAME, vector);
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	}
	ndis_irq->enabled = 1;
	printk(KERN_INFO "%s: using IRQ %d\n", DRIVER_NAME, vector);
	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMDeregisterInterrupt,1)
	(struct ndis_irq *ndis_irq)
{
	struct wrap_ndis_device *wnd;

	TRACEENTER1("%p", ndis_irq);

	if (!ndis_irq)
		TRACEEXIT1(return);
	wnd = ndis_irq->wnd;
	if (!wnd)
		TRACEEXIT1(return);

	free_irq(ndis_irq->irq.irq, wnd);
	tasklet_kill(&wnd->irq_tasklet);
	ndis_irq->enabled = 0;
	ndis_irq->wnd = NULL;
	wnd->ndis_irq = NULL;
	TRACEEXIT1(return);
}

wstdcall BOOLEAN WIN_FUNC(NdisMSynchronizeWithInterrupt,3)
	(struct ndis_irq *ndis_irq, void *func, void *ctx)
{
	BOOLEAN ret;
	BOOLEAN (*sync_func)(void *ctx) wstdcall;
	unsigned long flags;

	TRACEENTER6("%p %p", func, ctx);
	sync_func = func;
	nt_spin_lock_irqsave(&ndis_irq->lock, flags);
	ret = LIN2WIN1(sync_func, ctx);
	nt_spin_unlock_irqrestore(&ndis_irq->lock, flags);
	DBGTRACE6("ret: %d", ret);
	TRACEEXIT6(return ret);
}

/* called via function pointer; but 64-bit RNDIS driver calls directly */
wstdcall void WIN_FUNC(NdisMIndicateStatus,4)
	(struct ndis_miniport_block *nmb, NDIS_STATUS status,
	 void *buf, UINT len)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct ndis_status_indication *si;
	struct ndis_auth_req *auth_req;
	struct ndis_radio_status_indication *radio_status;

	TRACEENTER2("status=0x%x len=%d", status, len);
	switch (status) {
	case NDIS_STATUS_MEDIA_DISCONNECT:
		netif_carrier_off(wnd->net_dev);
		set_bit(LINK_STATUS_CHANGED, &wnd->wrap_ndis_pending_work);
		schedule_wrap_work(&wnd->wrap_ndis_work);
		break;
	case NDIS_STATUS_MEDIA_CONNECT:
		netif_carrier_on(wnd->net_dev);
		set_bit(LINK_STATUS_CHANGED, &wnd->wrap_ndis_pending_work);
		schedule_wrap_work(&wnd->wrap_ndis_work);
		break;
	case NDIS_STATUS_MEDIA_SPECIFIC_INDICATION:
		if (!buf)
			break;
		si = buf;
		DBGTRACE2("status_type=%d", si->status_type);

		switch (si->status_type) {
		case Ndis802_11StatusType_Authentication:
			buf = (char *)buf + sizeof(*si);
			len -= sizeof(*si);
			while (len > 0) {
				auth_req = (struct ndis_auth_req *)buf;
				DBGTRACE1(MACSTRSEP, MAC2STR(auth_req->bssid));
				if (auth_req->flags & 0x01)
					DBGTRACE2("reqauth");
				if (auth_req->flags & 0x02)
					DBGTRACE2("keyupdate");
				if (auth_req->flags & 0x06)
					DBGTRACE2("pairwise_error");
				if (auth_req->flags & 0x0E)
					DBGTRACE2("group_error");
				/* TODO: report to wpa_supplicant */
				len -= auth_req->length;
				buf = (char *)buf + auth_req->length;
			}
			break;
		case Ndis802_11StatusType_MediaStreamMode:
			break;
#ifdef CONFIG_NET_RADIO
		case Ndis802_11StatusType_PMKID_CandidateList:
		{
			u8 *end;
			unsigned long i;
			struct ndis_pmkid_candidate_list *cand;

			cand = buf + sizeof(struct ndis_status_indication);
			if (len < sizeof(struct ndis_status_indication) +
			    sizeof(struct ndis_pmkid_candidate_list) ||
				cand->version != 1) {
				WARNING("Unrecognized PMKID_CANDIDATE_LIST"
					" ignored");
				TRACEEXIT1(return);
			}

			end = (u8 *)buf + len;
			DBGTRACE2("PMKID_CANDIDATE_LIST ver %ld num_cand %ld",
				  cand->version, cand->num_candidates);
			for (i = 0; i < cand->num_candidates; i++) {
#if WIRELESS_EXT > 17
				struct iw_pmkid_cand pcand;
				union iwreq_data wrqu;
#endif
				struct ndis_pmkid_candidate *c =
					&cand->candidates[i];
				if ((u8 *)(c + 1) > end) {
					DBGTRACE2("Truncated "
						  "PMKID_CANDIDATE_LIST");
					break;
				}
				DBGTRACE2("%ld: " MACSTRSEP " 0x%lx",
					  i, MAC2STR(c->bssid), c->flags);
#if WIRELESS_EXT > 17
				memset(&pcand, 0, sizeof(pcand));
				if (c->flags & 0x01)
					pcand.flags |= IW_PMKID_CAND_PREAUTH;
				pcand.index = i;
				memcpy(pcand.bssid.sa_data, c->bssid, ETH_ALEN);

				memset(&wrqu, 0, sizeof(wrqu));
				wrqu.data.length = sizeof(pcand);
				wireless_send_event(wnd->net_dev, IWEVPMKIDCAND,
						    &wrqu, (u8 *)&pcand);
#endif
			}
			break;
		}
		case Ndis802_11StatusType_RadioState:
			radio_status = buf;
			if (radio_status->radio_state ==
			    Ndis802_11RadioStatusOn)
				INFO("radio is turned on");
			else if (radio_status->radio_state ==
				 Ndis802_11RadioStatusHardwareOff)
				INFO("radio is turned off by hardware");
			else if (radio_status->radio_state ==
				 Ndis802_11RadioStatusSoftwareOff)
				INFO("radio is turned off by software");
			break;
#endif
		default:
			/* is this RSSI indication? */
			DBGTRACE2("unknown indication: %x", si->status_type);
			break;
		}
		break;
	default:
		DBGTRACE2("unknown status: %08X", status);
		break;
	}

	TRACEEXIT2(return);
}

/* called via function pointer; but 64-bit RNDIS driver calls directly */
wstdcall void WIN_FUNC(NdisMIndicateStatusComplete,1)
	(struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER2("%p", wnd);
	schedule_wrap_work(&wnd->wrap_ndis_work);
	if (wnd->tx_ok)
		schedule_wrap_work(&wnd->tx_work);
}

wstdcall void return_packet(void *arg1, void *arg2)
{
	struct wrap_ndis_device *wnd;
	struct ndis_packet *packet;
	struct miniport_char *miniport;
	KIRQL irql;

	wnd = arg1;
	packet = arg2;
	TRACEENTER4("%p, %p", wnd, packet);
	miniport = &wnd->wd->driver->ndis_driver->miniport;
	irql = serialize_lock_irql(wnd);
	LIN2WIN2(miniport->return_packet, wnd->nmb->adapter_ctx, packet);
	serialize_unlock_irql(wnd, irql);
	TRACEEXIT4(return);
}
WIN_FUNC_DECL(return_packet,2)

/* called via function pointer */
wstdcall void NdisMIndicateReceivePacket(struct ndis_miniport_block *nmb,
					 struct ndis_packet **packets,
					 UINT nr_packets)
{
	struct wrap_ndis_device *wnd;
	ndis_buffer *buffer;
	struct ndis_packet *packet;
	struct sk_buff *skb;
	UINT i, length, total_length;
	struct ndis_packet_oob_data *oob_data;
	void *virt;

	TRACEENTER3("%p, %d", nmb, nr_packets);
	wnd = nmb->wnd;
	for (i = 0; i < nr_packets; i++) {
		packet = packets[i];
		if (!packet) {
			WARNING("empty packet ignored");
			continue;
		}
		wnd->net_dev->last_rx = jiffies;
		/* get total number of bytes in packet */
		NdisGetFirstBufferFromPacketSafe(packet, &buffer, &virt,
						 &length, &total_length,
						 NormalPagePriority);
		DBGTRACE3("%d, %d", length, total_length);
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		skb = dev_alloc_skb(total_length);
		if (skb) {
			while (buffer) {
				memcpy_skb(skb, MmGetSystemAddressForMdl(buffer),
					   MmGetMdlByteCount(buffer));
				buffer = buffer->next;
			}
			skb->dev = wnd->net_dev;
			skb->protocol = eth_type_trans(skb, wnd->net_dev);
			pre_atomic_add(wnd->stats.rx_bytes, total_length);
			atomic_inc_var(wnd->stats.rx_packets);
			netif_rx(skb);
		} else {
			WARNING("couldn't allocate skb; packet dropped");
			atomic_inc_var(wnd->stats.rx_dropped);
		}

		/* serialized drivers check the status upon return
		 * from this function */
		if (!deserialized_driver(wnd)) {
			oob_data->status = NDIS_STATUS_SUCCESS;
			continue;
		}

		/* if a deserialized driver sets
		 * NDIS_STATUS_RESOURCES, then it reclaims the packet
		 * upon return from this function */
		if (oob_data->status == NDIS_STATUS_RESOURCES)
			continue;

		assert(oob_data->status == NDIS_STATUS_SUCCESS);
		/* deserialized driver doesn't check the status upon
		 * return from this function; we need to call
		 * MiniportReturnPacket later for this packet. Calling
		 * MiniportReturnPacket from here is not correct - the
		 * driver doesn't expect it (at least Centrino driver
		 * crashes) */
		schedule_ntos_work_item(WIN_FUNC_PTR(return_packet,2),
					wnd, packet);
	}
	TRACEEXIT3(return);
}

/* called via function pointer */
wstdcall void NdisMSendComplete(struct ndis_miniport_block *nmb,
				struct ndis_packet *packet, NDIS_STATUS status)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER4("%p, %08X", packet, status);
	if (deserialized_driver(wnd))
		free_tx_packet(wnd, packet, status);
	else {
		struct ndis_packet_oob_data *oob_data;
		NDIS_STATUS pkt_status;
		TRACEENTER3("%p, %08x", packet, status);
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		switch ((pkt_status = xchg(&oob_data->status, status))) {
		case NDIS_STATUS_NOT_RECOGNIZED:
			free_tx_packet(wnd, packet, status);
			break;
		case NDIS_STATUS_PENDING:
		case 0:
			break;
		default:
			WARNING("%p: invalid status: %08X", packet, pkt_status);
			break;
		}
		/* In case a serialized driver has earlier requested a
		 * pause by returning NDIS_STATUS_RESOURCES during
		 * MiniportSend(Packets), wakeup tx worker now.
		 */
		if (wnd->tx_ok == 0) {
			atomic_inc_var(wnd->tx_ok);
			DBGTRACE3("%d, %d", wnd->tx_ring_start,
				  wnd->tx_ring_end);
			schedule_wrap_work(&wnd->tx_work);
		}
	}
	TRACEEXIT3(return);
}

/* called via function pointer */
wstdcall void NdisMSendResourcesAvailable(struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER3("");
	DBGTRACE3("%d, %d", wnd->tx_ring_start, wnd->tx_ring_end);
	atomic_inc_var(wnd->tx_ok);
	schedule_wrap_work(&wnd->tx_work);
	TRACEEXIT3(return);
}

/* called via function pointer (by NdisMEthIndicateReceive macro); the
 * first argument is nmb->eth_db */
wstdcall void EthRxIndicateHandler(struct ndis_miniport_block *nmb, void *rx_ctx,
				   char *header1, char *header, UINT header_size,
				   void *look_ahead, UINT look_ahead_size,
				   UINT packet_size)
{
	struct sk_buff *skb = NULL;
	struct wrap_ndis_device *wnd;
	unsigned int skb_size = 0;
	KIRQL irql;
	struct ndis_packet_oob_data *oob_data;

	TRACEENTER3("nmb = %p, rx_ctx = %p, buf = %p, size = %d, buf = %p, "
		    "size = %d, packet = %d", nmb, rx_ctx, header, header_size,
		    look_ahead, look_ahead_size, packet_size);

	wnd = nmb->wnd;
	DBGTRACE3("wnd = %p", wnd);
	if (!wnd) {
		ERROR("nmb is NULL");
		TRACEEXIT3(return);
	}
	wnd->net_dev->last_rx = jiffies;

	if (look_ahead_size < packet_size) {
		struct ndis_packet *packet;
		struct miniport_char *miniport;
		unsigned int bytes_txed;
		NDIS_STATUS res;

		NdisAllocatePacket(&res, &packet, wnd->tx_packet_pool);
		if (res != NDIS_STATUS_SUCCESS) {
			atomic_inc_var(wnd->stats.rx_dropped);
			TRACEEXIT3(return);
		}
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		miniport = &wnd->wd->driver->ndis_driver->miniport;
		irql = serialize_lock_irql(wnd);
		res = LIN2WIN6(miniport->tx_data, packet, &bytes_txed, nmb,
			       rx_ctx, look_ahead_size, packet_size);
		serialize_unlock_irql(wnd, irql);
		DBGTRACE3("%d, %d, %d", header_size, look_ahead_size,
			  bytes_txed);
		if (res == NDIS_STATUS_SUCCESS) {
			ndis_buffer *buffer;
			skb = dev_alloc_skb(header_size + look_ahead_size +
					    bytes_txed);
			if (skb) {
				memcpy_skb(skb, header, header_size);
				memcpy_skb(skb, look_ahead, look_ahead_size);
				buffer = packet->private.buffer_head;
				while (buffer) {
					memcpy_skb(skb,
						   MmGetSystemAddressForMdl(buffer),
						   MmGetMdlByteCount(buffer));
					buffer = buffer->next;
				}
				skb_size = header_size+look_ahead_size +
					bytes_txed;
				NdisFreePacket(packet);
			}
		} else if (res == NDIS_STATUS_PENDING) {
			/* driver will call td_complete */
			oob_data->look_ahead = kmalloc(look_ahead_size,
						       GFP_ATOMIC);
			if (!oob_data->look_ahead) {
				NdisFreePacket(packet);
				ERROR("packet dropped");
				atomic_inc_var(wnd->stats.rx_dropped);
				TRACEEXIT3(return);
			}
			memcpy(oob_data->header, header,
			       sizeof(oob_data->header));
			memcpy(oob_data->look_ahead, look_ahead,
			       look_ahead_size);
			oob_data->look_ahead_size = look_ahead_size;
			TRACEEXIT3(return);
		} else {
			WARNING("packet dropped: %08X", res);
			NdisFreePacket(packet);
			atomic_inc_var(wnd->stats.rx_dropped);
			TRACEEXIT3(return);
		}
	} else {
		skb_size = header_size + packet_size;
		skb = dev_alloc_skb(skb_size);
		if (skb) {
			memcpy_skb(skb, header, header_size);
			memcpy_skb(skb, look_ahead, packet_size);
		}
	}

	if (skb) {
		skb->dev = wnd->net_dev;
		skb->protocol = eth_type_trans(skb, wnd->net_dev);
		pre_atomic_add(wnd->stats.rx_bytes, skb_size);
		atomic_inc_var(wnd->stats.rx_packets);
		netif_rx(skb);
	} else {
		ERROR("couldn't allocate skb; packet dropped");
		atomic_inc_var(wnd->stats.rx_dropped);
	}

	TRACEEXIT3(return);
}

/* called via function pointer */
wstdcall void NdisMTransferDataComplete(struct ndis_miniport_block *nmb,
					struct ndis_packet *packet,
					NDIS_STATUS status, UINT bytes_txed)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct sk_buff *skb;
	unsigned int skb_size;
	struct ndis_packet_oob_data *oob_data;
	ndis_buffer *buffer;

	TRACEENTER3("wnd = %p, packet = %p, bytes_txed = %d",
		    wnd, packet, bytes_txed);
	if (!packet) {
		WARNING("illegal packet");
		TRACEEXIT3(return);
	}
	wnd->net_dev->last_rx = jiffies;
	oob_data = NDIS_PACKET_OOB_DATA(packet);
	skb_size = sizeof(oob_data->header) + oob_data->look_ahead_size +
		bytes_txed;
	skb = dev_alloc_skb(skb_size);
	if (!skb) {
		kfree(oob_data->look_ahead);
		NdisFreePacket(packet);
		ERROR("couldn't allocate skb; packet dropped");
		atomic_inc_var(wnd->stats.rx_dropped);
		TRACEEXIT3(return);
	}
	memcpy_skb(skb, oob_data->header, sizeof(oob_data->header));
	memcpy_skb(skb, oob_data->look_ahead, oob_data->look_ahead_size);
	buffer = packet->private.buffer_head;
	while (buffer) {
		memcpy_skb(skb, MmGetSystemAddressForMdl(buffer),
			   MmGetMdlByteCount(buffer));
		buffer = buffer->next;
	}
	kfree(oob_data->look_ahead);
	NdisFreePacket(packet);
	skb->dev = wnd->net_dev;
	skb->protocol = eth_type_trans(skb, wnd->net_dev);
	pre_atomic_add(wnd->stats.rx_bytes, skb_size);
	atomic_inc_var(wnd->stats.rx_packets);
	netif_rx(skb);
}

/* called via function pointer */
wstdcall void EthRxComplete(struct ndis_miniport_block *nmb)
{
	DBGTRACE3("");
}

wstdcall void NdisMQueryInformationComplete(struct ndis_miniport_block *nmb,
					    NDIS_STATUS status)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER2("nmb: %p, wnd: %p, %08X", nmb, wnd, status);
	wnd->ndis_comm_status = status;
	wnd->ndis_comm_done = 1;
	wake_up(&wnd->ndis_comm_wq);
	TRACEEXIT2(return);
}

wstdcall void NdisMSetInformationComplete(struct ndis_miniport_block *nmb,
					  NDIS_STATUS status)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER2("status = %08X", status);

	wnd->ndis_comm_status = status;
	wnd->ndis_comm_done = 1;
	wake_up(&wnd->ndis_comm_wq);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisMSleep,1)
	(ULONG us)
{
	unsigned long delay;

	TRACEENTER4("%p: us: %u", current, us);
	delay = USEC_TO_HZ(us);
	sleep_hz(delay);
	DBGTRACE4("%p: done", current);
	TRACEEXIT4(return);
}

wstdcall void WIN_FUNC(NdisGetCurrentSystemTime,1)
	(LARGE_INTEGER *time)
{
	*time = ticks_1601();
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterIoPortRange,4)
	(void **virt, struct ndis_miniport_block *nmb, UINT start, UINT len)
{
	TRACEENTER3("%08x %08x", start, len);
	*virt = (void *)(ULONG_PTR)start;
	return NDIS_STATUS_SUCCESS;
}

wstdcall void WIN_FUNC(NdisMDeregisterIoPortRange,4)
	(struct ndis_miniport_block *nmb, UINT start, UINT len, void* virt)
{
	TRACEENTER1("%08x %08x", start, len);
}

wstdcall LONG WIN_FUNC(NdisInterlockedDecrement,1)
	(LONG *val)
{
	return InterlockedDecrement(val);
}

wstdcall LONG WIN_FUNC(NdisInterlockedIncrement,1)
	(LONG *val)
{
	return InterlockedIncrement(val);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedInsertHeadList,3)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertHeadList(head, entry, &lock->klock);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedInsertTailList,3)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertTailList(head, entry, &lock->klock);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedRemoveHeadList,2)
	(struct nt_list *head, struct ndis_spinlock *lock)
{
	return ExInterlockedRemoveHeadList(head, &lock->klock);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMInitializeScatterGatherDma,3)
	(struct ndis_miniport_block *nmb, BOOLEAN dma_size, ULONG max_phy_map)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	TRACEENTER2("dma_size=%d, maxtransfer=%u", dma_size, max_phy_map);
#ifdef CONFIG_X86_64
	if (dma_size != NDIS_DMA_64BITS)
		ERROR("DMA size is not 64-bits");
#endif
	wnd->use_sg_dma = TRUE;
	return NDIS_STATUS_SUCCESS;
}

wstdcall ULONG WIN_FUNC(NdisMGetDmaAlignment,1)
	(struct ndis_miniport_block *nmb)
{
	TRACEENTER3("");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	return dma_get_cache_alignment();
#else
	return L1_CACHE_BYTES;
#endif
}

wstdcall CHAR WIN_FUNC(NdisSystemProcessorCount,0)
	(void)
{
	return NR_CPUS;
}

wstdcall void WIN_FUNC(NdisInitializeEvent,1)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeInitializeEvent(&ndis_event->nt_event, NotificationEvent, 0);
}

wstdcall BOOLEAN WIN_FUNC(NdisWaitEvent,2)
	(struct ndis_event *ndis_event, UINT ms)
{
	LARGE_INTEGER ticks;
	NTSTATUS res;

	TRACEENTER3("%p %u", ndis_event, ms);
	ticks = -((LARGE_INTEGER)ms * TICKSPERMSEC);
	res = KeWaitForSingleObject(&ndis_event->nt_event, 0, 0, TRUE,
				    ms == 0 ? NULL : &ticks);
	if (res == STATUS_SUCCESS)
		TRACEEXIT3(return TRUE);
	else
		TRACEEXIT3(return FALSE);
}

wstdcall void WIN_FUNC(NdisSetEvent,1)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeSetEvent(&ndis_event->nt_event, 0, 0);
}

wstdcall void WIN_FUNC(NdisResetEvent,1)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeResetEvent(&ndis_event->nt_event);
}

/* called via function pointer */
wstdcall void NdisMResetComplete(struct ndis_miniport_block *nmb,
				 NDIS_STATUS status, BOOLEAN address_reset)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER3("status: %08X, %u", status, address_reset);
	wnd->ndis_comm_status = status;
	wnd->ndis_comm_done = 1 + address_reset;
	wake_up(&wnd->ndis_comm_wq);
	TRACEEXIT3(return);
}

static void ndis_worker(void *dummy)
{
	KIRQL irql;
	struct ndis_work_entry *ndis_work_entry;
	struct nt_list *ent;
	struct ndis_work_item *ndis_work_item;

	WORKENTER("");
	while (1) {
		irql = nt_spin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
		ent = RemoveHeadList(&ndis_worker_list);
		nt_spin_unlock_irql(&ndis_work_list_lock, irql);
		if (!ent)
			break;
		ndis_work_entry = container_of(ent, struct ndis_work_entry,
					       list);
		ndis_work_item = ndis_work_entry->ndis_work_item;
		WORKTRACE("%p: %p, %p", ndis_work_item,
			  ndis_work_item->func, ndis_work_item->ctx);
		LIN2WIN2(ndis_work_item->func, ndis_work_item,
			 ndis_work_item->ctx);
		WORKTRACE("%p done", ndis_work_item);
		kfree(ndis_work_entry);
	}
	WORKEXIT(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisScheduleWorkItem,1)
	(struct ndis_work_item *ndis_work_item)
{
	struct ndis_work_entry *ndis_work_entry;
	KIRQL irql;

	TRACEENTER3("%p", ndis_work_item);
	ndis_work_entry = kmalloc(sizeof(*ndis_work_entry), gfp_irql());
	if (!ndis_work_entry)
		BUG();
	ndis_work_entry->ndis_work_item = ndis_work_item;
	irql = nt_spin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
	InsertTailList(&ndis_worker_list, &ndis_work_entry->list);
	nt_spin_unlock_irql(&ndis_work_list_lock, irql);
	WORKTRACE("scheduling %p", ndis_work_item);
	schedule_ndis_work(&ndis_work);
	TRACEEXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMGetDeviceProperty,6)
	(struct ndis_miniport_block *nmb, void **phy_dev, void **func_dev,
	 void **next_dev, void **alloc_res, void**trans_res)
{
	TRACEENTER2("nmb: %p, phy_dev = %p, func_dev = %p, next_dev = %p, "
		    "alloc_res = %p, trans_res = %p", nmb, phy_dev, func_dev,
		    next_dev, alloc_res, trans_res);
	if (phy_dev)
		*phy_dev = nmb->pdo;
	if (func_dev)
		*func_dev = nmb->fdo;
	if (next_dev)
		*next_dev = nmb->next_device;
}

wstdcall void WIN_FUNC(NdisMRegisterUnloadHandler,2)
	(struct driver_object *drv_obj, void *unload)
{
	if (drv_obj)
		drv_obj->unload = unload;
	return;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMQueryAdapterInstanceName,2)
	(struct unicode_string *name, struct ndis_miniport_block *nmb)
{
	struct wrap_ndis_device *wnd = nmb->wnd;
	struct ansi_string ansi;

	if (wrap_is_pci_bus(wnd->wd->dev_bus))
		RtlInitAnsiString(&ansi, "PCI Ethernet Adapter");
	else
		RtlInitAnsiString(&ansi, "USB Ethernet Adapter");

	if (RtlAnsiStringToUnicodeString(name, &ansi, TRUE))
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	else
		TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall ULONG WIN_FUNC(NdisReadPcmciaAttributeMemory,4)
	(struct ndis_miniport_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	TODO();
	return 0;
}

wstdcall ULONG WIN_FUNC(NdisWritePcmciaAttributeMemory,4)
	(struct ndis_miniport_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	TODO();
	return 0;
}

wstdcall void WIN_FUNC(NdisMCoIndicateReceivePacket,3)
	(struct ndis_miniport_block *nmb, struct ndis_packet **packets,
	 UINT nr_packets)
{
	TRACEENTER3("nmb = %p", nmb);
	NdisMIndicateReceivePacket(nmb, packets, nr_packets);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisMCoSendComplete,3)
	(NDIS_STATUS status, struct ndis_miniport_block *nmb,
	 struct ndis_packet *packet)
{
	TRACEENTER3("%08x", status);
	NdisMSendComplete(nmb, packet, status);
	TRACEEXIT3(return);
}

wstdcall void WIN_FUNC(NdisMCoRequestComplete,3)
	(NDIS_STATUS status, struct ndis_miniport_block *nmb,
	 struct ndis_request *ndis_request)
{
	struct wrap_ndis_device *wnd = nmb->wnd;

	TRACEENTER3("%08X", status);
	wnd->ndis_comm_status = status;
	wnd->ndis_comm_done = 1;
	wake_up(&wnd->ndis_comm_wq);
	TRACEEXIT3(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMSetMiniportSecondary,2)
	(struct ndis_miniport_block *nmb2, struct ndis_miniport_block *nmb1)
{
	TRACEENTER3("%p, %p", nmb1, nmb2);
	TODO();
	TRACEEXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMPromoteMiniport,1)
	(struct ndis_miniport_block *nmb)
{
	TRACEENTER3("%p", nmb);
	TODO();
	TRACEEXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMCoActivateVcComplete,3)
	(NDIS_STATUS status, void *handle, void *params)
{
	TODO();
}

wstdcall void WIN_FUNC(NdisMCoDeactivateVcComplete,2)
	(NDIS_STATUS status, void *handle)
{
	TODO();
}

wstdcall void WIN_FUNC(NdisMRemoveMiniport,1)
	(void *handle)
{
	TODO();
}

#include "ndis_exports.h"

void init_nmb_functions(struct ndis_miniport_block *nmb)
{
	nmb->rx_packet = WIN_FUNC_PTR(NdisMIndicateReceivePacket,3);
	nmb->send_complete = WIN_FUNC_PTR(NdisMSendComplete,3);
	nmb->send_resource_avail = WIN_FUNC_PTR(NdisMSendResourcesAvailable,1);
	nmb->status = WIN_FUNC_PTR(NdisMIndicateStatus,4);
	nmb->status_complete = WIN_FUNC_PTR(NdisMIndicateStatusComplete,1);
	nmb->query_complete = WIN_FUNC_PTR(NdisMQueryInformationComplete,2);
	nmb->set_complete = WIN_FUNC_PTR(NdisMSetInformationComplete,2);
	nmb->reset_complete = WIN_FUNC_PTR(NdisMResetComplete,3);
	nmb->eth_rx_indicate = WIN_FUNC_PTR(EthRxIndicateHandler,8);
	nmb->eth_rx_complete = WIN_FUNC_PTR(EthRxComplete,1);
	nmb->td_complete = WIN_FUNC_PTR(NdisMTransferDataComplete,4);
}

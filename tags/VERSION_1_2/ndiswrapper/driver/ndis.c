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

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/wireless.h>
#include <net/iw_handler.h>

#include "ndis.h"
#include "iw_ndis.h"
#include "wrapper.h"

extern struct list_head ndis_drivers;
extern KSPIN_LOCK ntoskernel_lock;

static struct list_head wd_ctx_list;

static struct work_struct ndis_work;
static struct list_head ndis_work_list;
static KSPIN_LOCK ndis_work_list_lock;

static KSPIN_LOCK wrap_ndis_packet_lock;
static struct nt_list wrap_ndis_packet_list;

static void ndis_worker(void *data);
static void free_wd_ctx(struct wrapper_dev *wd);

/* Some drivers allocate all NDIS_PACKETs they need at the beginning
 * and others allocate them quite often - every time a packet is
 * sent/received. We use cache pool for this to avoid memory
 * fragmentation, just like MDLs */
static kmem_cache_t *packet_cache;

/* ndis_init is called once when module is loaded */
int ndis_init(void)
{
	/* only one worker is used for all drivers */
	INIT_WORK(&ndis_work, &ndis_worker, NULL);
	INIT_LIST_HEAD(&ndis_work_list);
	INIT_LIST_HEAD(&wd_ctx_list);
	kspin_lock_init(&ndis_work_list_lock);
	kspin_lock_init(&wrap_ndis_packet_lock);
	InitializeListHead(&wrap_ndis_packet_list);
	packet_cache = kmem_cache_create("ndis_packet",
					 sizeof(struct wrap_ndis_packet),
					 0, 0, NULL, NULL);
	if (!packet_cache) {
		ERROR("couldn't allocate packet cache");
		return -ENOMEM;
	}
	return 0;
}

/* ndis_exit is called once when module is removed */
void ndis_exit(void)
{
	KIRQL irql;

	if (packet_cache) {
		irql = kspin_lock_irql(&wrap_ndis_packet_lock, DISPATCH_LEVEL);
		if (!IsListEmpty(&wrap_ndis_packet_list)) {
			struct nt_list *cur;
			ERROR("Windows driver didn't free all packets; "
			      "freeing them now");
			while ((cur =
				RemoveHeadList(&wrap_ndis_packet_list))) {
				struct wrap_ndis_packet *p;
				p = container_of(cur, struct wrap_ndis_packet,
						 list);
				DBGTRACE3("feeing packet %p", &p->ndis_packet);
				kmem_cache_free(packet_cache, p);
			}
		}
		kspin_unlock_irql(&wrap_ndis_packet_lock, irql);
		kmem_cache_destroy(packet_cache);
		packet_cache = NULL;
	}
	return;
}

/* ndis_exit_device is called for each handle */
void ndis_exit_device(struct wrapper_dev *wd)
{
	/* TI driver doesn't call NdisMDeregisterInterrupt during halt! */
	if (wd->ndis_irq)
		NdisMDeregisterInterrupt(wd->ndis_irq);
	free_wd_ctx(wd);
	if (wd->pci_resources)
		vfree(wd->pci_resources);
	wd->pci_resources = NULL;
}

/* remove all 'wd X ctx' pairs for the given handle */
static void free_wd_ctx(struct wrapper_dev *wd)
{
	struct list_head *cur, *tmp;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	list_for_each_safe(cur, tmp, &wd_ctx_list) {
		struct wd_ctx_entry *wd_ctx =
			list_entry(cur, struct wd_ctx_entry, list);
		if (wd_ctx->wd == wd) {
			list_del(&wd_ctx->list);
			kfree(wd_ctx);
		}
	}
	INIT_LIST_HEAD(&wd_ctx_list);
	kspin_unlock_irql(&ntoskernel_lock, irql);
	return;
}

/* Called from the driver entry. */
STDCALL void WRAP_EXPORT(NdisInitializeWrapper)
	(void **driver_handle, struct driver_object *driver,
	 struct unicode_string *reg_path, void *unused)
{
	TRACEENTER1("handle: %p, driver: %p", driver_handle, driver);
	*driver_handle = driver;
	TRACEEXIT1(return);
}

STDCALL void WRAP_EXPORT(NdisTerminateWrapper)
	(struct device_object *dev_obj, void *SystemSpecific1)
{
	TRACEEXIT1(return);
}

/* Register a miniport with NDIS. Called from driver entry */
STDCALL NDIS_STATUS WRAP_EXPORT(NdisMRegisterMiniport)
	(struct driver_object *drv_obj,
	 struct miniport_char *miniport_char, UINT char_len)
{
	int i, min_length;
	void **func;
	struct ndis_driver *driver;
	char *miniport_funcs[] = {
		"query",
		"reconfig",
		"reset",
		"send",
		"setinfo",
		"tx_data",
		"return_packet",
		"send_packets",
		"alloc_complete",
		"co_create_vc",
		"co_delete_vc",
		"co_activate_vc",
		"co_deactivate_vc",
		"co_send_packets",
		"co_request",
		"cancel_send_packets",
		"pnp_event_notify",
		"adapter_shutdown",
	};

	min_length = ((char *)&miniport_char->co_create_vc) -
		((char *)miniport_char);

	TRACEENTER1("%p %p %d", drv_obj, miniport_char, char_len);

	if (miniport_char->majorVersion < 4) {
		ERROR("Driver is using ndis version %d which is too old.",
		      miniport_char->majorVersion);
		TRACEEXIT1(return NDIS_STATUS_BAD_VERSION);
	}

	if (char_len < min_length) {
		ERROR("Characteristics length %d is too small",
		      char_len);
		TRACEEXIT1(return NDIS_STATUS_BAD_CHARACTERISTICS);
	}

	DBGTRACE1("Version %d.%d", miniport_char->majorVersion,
		  miniport_char->minorVersion);
	DBGTRACE1("Len: %08x:%u", char_len, (u32)sizeof(struct miniport_char));

	driver = IoGetDriverObjectExtension(drv_obj,
					    (void *)CE_NDIS_DRIVER_CLIENT_ID);
	TRACEENTER1("driver: %p", driver);
	if (!driver) {
		ERROR("couldn't find ndis_driver - bug in %s?", DRIVER_NAME);
		TRACEEXIT1(return -EINVAL);
	}
	drv_obj->drv_ext->add_device_func = ndis_add_device;
	memcpy(&driver->miniport, miniport_char,
	       char_len > sizeof(*miniport_char) ?
	       sizeof(*miniport_char) : char_len);

	i = 0;
	func = (void **)&driver->miniport.query;
	while (i < sizeof(miniport_funcs) / sizeof(miniport_funcs[0])) {
		DBGTRACE2("miniport function '%s' is at %p",
			  miniport_funcs[i], func[i]);
		i++;
	}

	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisAllocateMemory)
	(void **dest, UINT length, UINT flags,
	 NDIS_PHY_ADDRESS highest_address)
{
	TRACEENTER3("length = %u, flags = %08X", length, flags);
	if (length <= KMALLOC_THRESHOLD) {
		if (current_irql() < DISPATCH_LEVEL)
			*dest = kmalloc(length, GFP_KERNEL);
		else
			*dest = kmalloc(length, GFP_ATOMIC);
	} else if (flags & NDIS_MEMORY_CONTIGUOUS) {
		WARNING("Allocating %u bytes of physically "
		       "contiguous memory may fail", length);
		*dest = kmalloc(length, GFP_KERNEL);
	} else {
		if (current_irql() == DISPATCH_LEVEL)
			ERROR("Windows driver allocating too big a block"
			      " at DISPATCH_LEVEL: %d", length);
		*dest = vmalloc(length);
	}

	if (*dest)
		TRACEEXIT3(return NDIS_STATUS_SUCCESS);
	DBGTRACE3("Allocatemem failed size=%d", length);
	TRACEEXIT3(return NDIS_STATUS_FAILURE);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisAllocateMemoryWithTag)
	(void **dest, UINT length, ULONG tag)
{
	NDIS_STATUS res;

	TRACEENTER3("dest = %p, *dest = %p", dest, *dest);
	res = NdisAllocateMemory(dest, length, 0, 0);
	return res;
}

STDCALL void WRAP_EXPORT(NdisFreeMemory)
	(void *addr, UINT length, UINT flags)
{
	struct ndis_work_entry *ndis_work_entry;
	struct ndis_free_mem_work_item *free_mem;
	KIRQL irql;

	TRACEENTER3("addr = %p, flags = %08X", addr, flags);

	if (!addr)
		TRACEEXIT3(return);

	if (length <= KMALLOC_THRESHOLD)
		kfree(addr);
	else if (flags & NDIS_MEMORY_CONTIGUOUS)
		kfree(addr);
	else {
		if (!in_interrupt()) {
			vfree(addr);
			TRACEEXIT3(return);
		}
		/* Centrino 2200 driver calls this function when in
		 * ad-hoc mode in interrupt context when length >
		 * KMALLOC_THRESHOLD, which implies that vfree is
		 * called in interrupt context, which is not
		 * correct. So we use worker for it */
		ndis_work_entry = kmalloc(sizeof(*ndis_work_entry),
					  GFP_ATOMIC);
		BUG_ON(!ndis_work_entry);

		ndis_work_entry->type = NDIS_FREE_MEM_WORK_ITEM;
		free_mem = &ndis_work_entry->entry.free_mem_work_item;

		free_mem->addr = addr;
		free_mem->length = length;
		free_mem->flags = flags;

		irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
		list_add_tail(&ndis_work_entry->list, &ndis_work_list);
		kspin_unlock_irql(&ndis_work_list_lock, irql);

		schedule_work(&ndis_work);
	}

	TRACEEXIT3(return);
}

/*
 * This function should not be STDCALL because it's a variable args function.
 */
NOREGPARM void WRAP_EXPORT(NdisWriteErrorLogEntry)
	(struct driver_object *drv_obj, unsigned int error, ULONG count,
	 unsigned int p1)
{
	ERROR("log: %08X, count: %d (%08x), return address: %p",
	      error, count, p1, __builtin_return_address(0));
	return;
}

STDCALL void WRAP_EXPORT(NdisOpenConfiguration)
	(NDIS_STATUS *status, struct ndis_miniport_block **conf_handle,
	 struct ndis_miniport_block *handle)
{
	TRACEENTER2("confHandle: %p", conf_handle);
	*conf_handle = handle;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisOpenProtocolConfiguration)
	(NDIS_STATUS *status, void **confhandle,
	 struct unicode_string *section)
{
	TRACEENTER2("confHandle: %p", confhandle);
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisOpenConfigurationKeyByName)
	(NDIS_STATUS *status, void *handle,
	 struct unicode_string *key, void **subkeyhandle)
{
	TRACEENTER2("%s", "");
	*subkeyhandle = handle;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisOpenConfigurationKeyByIndex)
	(NDIS_STATUS *status, void *handle, ULONG index,
	 struct unicode_string *key, void **subkeyhandle)
{
	TRACEENTER2("%s", "");
	*subkeyhandle = handle;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisCloseConfiguration)
	(void *handle)
{
	TRACEENTER2("handle: %p", handle);
	return;
}

STDCALL void WRAP_EXPORT(NdisOpenFile)
	(NDIS_STATUS *status, struct ndis_bin_file **filehandle,
	 UINT *filelength, struct unicode_string *filename,
	 NDIS_PHY_ADDRESS highest_address)
{
	struct ansi_string ansi;
	struct list_head *cur, *tmp;
	struct ndis_bin_file *file;

	TRACEENTER2("status = %p, filelength = %p, *filelength = %d, "
		    "high = %llx, filehandle = %p, *filehandle = %p",
		    status, filelength, *filelength,
		    highest_address, filehandle, *filehandle);

	ansi.buf = kmalloc(MAX_STR_LEN, GFP_KERNEL);
	if (!ansi.buf) {
		*status = NDIS_STATUS_RESOURCES;
		return;
	}
	ansi.buf[MAX_STR_LEN-1] = 0;
	ansi.buflen = MAX_STR_LEN;

	if (RtlUnicodeStringToAnsiString(&ansi, filename, 0)) {
		*status = NDIS_STATUS_RESOURCES;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return);
	}
	DBGTRACE2("Filename: %s", ansi.buf);

	/* Loop through all drivers and all files to find the requested file */
	list_for_each_safe(cur, tmp, &ndis_drivers) {
		struct ndis_driver *driver;
		int i;

		driver = list_entry(cur, struct ndis_driver, list);
		for (i = 0; i < driver->num_bin_files; i++) {
			int n;
			file = &driver->bin_files[i];
			DBGTRACE2("considering %s", file->name);
			n = min(strlen(file->name), strlen(ansi.buf));
			if (strnicmp(file->name, ansi.buf, n) == 0) {
				*filehandle = file;
				*filelength = file->size;
				*status = NDIS_STATUS_SUCCESS;
				RtlFreeAnsiString(&ansi);
				TRACEEXIT2(return);
			}
		}
	}
	*status = NDIS_STATUS_FILE_NOT_FOUND;
	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisMapFile)
	(NDIS_STATUS *status, void **mappedbuffer,
	 struct ndis_bin_file *filehandle)
{
	TRACEENTER2("handle: %p", filehandle);

	if (!filehandle) {
		*status = NDIS_STATUS_ALREADY_MAPPED;
		TRACEEXIT2(return);
	}

	*status = NDIS_STATUS_SUCCESS;
	*mappedbuffer = filehandle->data;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisUnmapFile)
	(struct ndis_bin_file *filehandle)
{
	TRACEENTER2("handle: %p", filehandle);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisCloseFile)
	(struct ndis_bin_file *filehandle)
{
	TRACEENTER2("handle: %p", filehandle);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisGetSystemUpTime)
	(ULONG *systemuptime)
{
	TRACEENTER5("");
	*systemuptime = 1000 * jiffies / HZ;
	TRACEEXIT5(return);
}

/* called as macro */
STDCALL ULONG WRAP_EXPORT(NDIS_BUFFER_TO_SPAN_PAGES)
	(ndis_buffer *buffer)
{
	ULONG_PTR start;
	ULONG n;

	TRACEENTER4("%s", "");

	if (buffer == NULL)
		return 0;

	if (MmGetMdlByteCount(buffer) == 0)
		return 1;

	start = (ULONG_PTR)(MmGetMdlVirtualAddress(buffer));
	n = SPAN_PAGES(start, MmGetMdlByteCount(buffer));
	DBGTRACE4("pages = %u", n);
	TRACEEXIT3(return n);
}

STDCALL void WRAP_EXPORT(NdisGetBufferPhysicalArraySize)
	(ndis_buffer *buffer, UINT *arraysize)
{
	TRACEENTER3("Buffer: %p", buffer);
	*arraysize = NDIS_BUFFER_TO_SPAN_PAGES(buffer);
	TRACEEXIT3(return);
}

static int ndis_encode_setting(struct device_setting *setting,
			       int device_setting_type)
{
	struct ansi_string ansi;
	struct ndis_config_param *param;

	TRACEENTER2("type = %d", device_setting_type);
	if (setting->config_param.type == device_setting_type)
		return NDIS_STATUS_SUCCESS;

	switch(device_setting_type) {
	case NDIS_CONFIG_PARAM_INT:
		setting->config_param.data.intval =
			simple_strtol(setting->value, NULL, 0);
		DBGTRACE1("value = %u",
			  (ULONG)setting->config_param.data.intval);
		break;
	case NDIS_CONFIG_PARAM_HEXINT:
		setting->config_param.data.intval =
			simple_strtol(setting->value, NULL, 16);
		DBGTRACE2("value = %u",
			  (ULONG)setting->config_param.data.intval);
		break;
	case NDIS_CONFIG_PARAM_STRING:
		ansi.buflen = ansi.len = strlen(setting->value);
		ansi.buf = setting->value;
		DBGTRACE2("setting value = %s", ansi.buf);
		param = &setting->config_param;
		if (param->data.ustring.buf)
			RtlFreeUnicodeString(&param->data.ustring);
		if (RtlAnsiStringToUnicodeString(&param->data.ustring,
						 &ansi, 1))
			TRACEEXIT1(return NDIS_STATUS_FAILURE);
		break;
	default:
		return NDIS_STATUS_FAILURE;
	}
	setting->config_param.type = device_setting_type;
	return NDIS_STATUS_SUCCESS;
}

static int ndis_decode_setting(struct device_setting *setting,
			       struct ndis_config_param *val)
{
	struct ansi_string ansi;

	if (setting->config_param.type == NDIS_CONFIG_PARAM_STRING &&
	    setting->config_param.data.ustring.buf)
		RtlFreeUnicodeString(&setting->config_param.data.ustring);

	switch(val->type) {
	case NDIS_CONFIG_PARAM_INT:
		snprintf(setting->value, sizeof(u32), "%u", val->data.intval);
		setting->value[sizeof(ULONG)] = 0;
		break;
	case NDIS_CONFIG_PARAM_HEXINT:
		snprintf(setting->value, sizeof(u32), "%x", val->data.intval);
		setting->value[sizeof(ULONG)] = 0;
		break;
	case NDIS_CONFIG_PARAM_STRING:
		ansi.buf = setting->value;
		ansi.buflen = MAX_STR_LEN;
		if (RtlUnicodeStringToAnsiString(&ansi, &val->data.ustring, 0)
		    || ansi.len >= MAX_STR_LEN) {
			TRACEEXIT1(return NDIS_STATUS_FAILURE);
		}
		memcpy(setting->value, ansi.buf, ansi.len);
		DBGTRACE2("value = %s", setting->value);
		setting->value[ansi.len] = 0;
		break;
	default:
		DBGTRACE2("unknown setting type: %d", val->type);
		return NDIS_STATUS_FAILURE;
	}
	setting->config_param.type = NDIS_CONFIG_PARAM_NONE;
	return NDIS_STATUS_SUCCESS;
}

STDCALL void WRAP_EXPORT(NdisReadConfiguration)
	(NDIS_STATUS *status, struct ndis_config_param **dest,
	 struct ndis_miniport_block *nmb, struct unicode_string *key,
	 enum ndis_config_param_type type)
{
	struct device_setting *setting;
	struct ansi_string ansi;
	char *keyname;
	int ret;
	struct wrapper_dev *wd;

	TRACEENTER2("nmb: %p", nmb);
	wd = nmb->wd;
	ret = RtlUnicodeStringToAnsiString(&ansi, key, 1);
	DBGTRACE3("rtl func returns: %d", ret);
	if (ret) {
		*dest = NULL;
		*status = NDIS_STATUS_FAILURE;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return);
	}
	DBGTRACE3("wd: %p, string: %s", wd, ansi.buf);
	keyname = ansi.buf;

	list_for_each_entry(setting, &wd->ndis_device->settings, list) {
		if (stricmp(keyname, setting->name) == 0) {
			DBGTRACE2("setting found %s=%s",
				 keyname, setting->value);

			*status = ndis_encode_setting(setting, type);
			if (*status == NDIS_STATUS_SUCCESS)
				*dest = &setting->config_param;
			RtlFreeAnsiString(&ansi);
			DBGTRACE2("status = %d", *status);
			TRACEEXIT2(return);
		}
	}

	DBGTRACE2("setting %s not found (type:%d)", keyname, type);

	*status = NDIS_STATUS_FAILURE;
	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisWriteConfiguration)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 struct unicode_string *key, struct ndis_config_param *param)
{
	struct ansi_string ansi;
	char *keyname;
	struct device_setting *setting;
	struct wrapper_dev *wd;

	TRACEENTER2("nmb: %p", nmb);
	wd = nmb->wd;
	if (RtlUnicodeStringToAnsiString(&ansi, key, 1)) {
		*status = NDIS_STATUS_FAILURE;
		TRACEEXIT2(return);
	}
	keyname = ansi.buf;
	DBGTRACE2("key = %s", keyname);

	list_for_each_entry(setting, &wd->ndis_device->settings, list) {
		if (strcmp(keyname, setting->name) == 0) {
			*status = ndis_decode_setting(setting, param);
			DBGTRACE2("setting changed %s=%s",
				 keyname, setting->value);
			RtlFreeAnsiString(&ansi);
			TRACEEXIT2(return);
		}
	}

	if ((setting = kmalloc(sizeof(*setting), GFP_KERNEL)) == NULL) {
		*status = NDIS_STATUS_RESOURCES;
		RtlFreeAnsiString(&ansi);
		TRACEEXIT2(return);
	}
	memset(setting, 0, sizeof(*setting));
	memcpy(setting->name, keyname, ansi.len);
	setting->name[ansi.len] = 0;
	*status = ndis_decode_setting(setting, param);
	if (*status == NDIS_STATUS_SUCCESS)
		list_add(&setting->list, &wd->ndis_device->settings);
	else {
		kfree(setting->name);
		kfree(setting);
	}
	RtlFreeAnsiString(&ansi);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisInitializeString)
	(struct unicode_string *dest, UCHAR *src)
{
	struct ansi_string ansi;

	TRACEENTER2("%s", "");
	ansi.len = ansi.buflen = strlen(src);
	ansi.buf = src;
	if (RtlAnsiStringToUnicodeString(dest, &ansi, 1))
		DBGTRACE2("%s", "failed");
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisInitAnsiString)
	(struct ansi_string *dst, CHAR *src)
{
	RtlInitAnsiString(dst, src);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisInitString)
	(struct ansi_string *dst, CHAR *src)
{
	RtlInitString(dst, src);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisInitUnicodeString)
	(struct unicode_string *dest, const wchar_t *src)
{
	TRACEEXIT2(return RtlInitUnicodeString(dest, src));
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisAnsiStringToUnicodeString)
	(struct unicode_string *dst, struct ansi_string *src)
{
	int dup;

	TRACEENTER2("%s", "");
	if (dst == NULL || src == NULL)
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	if (dst->buf == NULL)
		dup = 1;
	else
		dup = 0;
	TRACEEXIT2(return RtlAnsiStringToUnicodeString(dst, src, dup));
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisUnicodeStringToAnsiString)
	(struct ansi_string *dst, struct unicode_string *src)
{
	int dup;

	TRACEENTER2("%s", "");
	if (dst == NULL || src == NULL)
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	if (dst->buf == NULL)
		dup = 1;
	else
		dup = 0;
	TRACEEXIT2(return RtlUnicodeStringToAnsiString(dst, src, dup));
}

/*
 * Called by driver from the init callback.
 * The adapter_ctx should be supplied to most other callbacks so we save
 * it in out handle. Some functions are called only with adapter_ctx, but
 * we also need handle in them, so we store handle X adapter_ctx map in
 * a global list.
 */
STDCALL void WRAP_EXPORT(NdisMSetAttributesEx)
	(struct ndis_miniport_block *nmb, void *adapter_ctx,
	 UINT hangcheck_interval, UINT attributes, ULONG adaptortype)
{
	struct wd_ctx_entry *wd_ctx;
	KIRQL irql;
	struct wrapper_dev *wd;

	TRACEENTER2("%p, %p %d %08x, %d", nmb, adapter_ctx,
		    hangcheck_interval, attributes, adaptortype);
	wd = nmb->wd;
	/* FIXME: is it possible to have duplicate ctx's? */
	wd_ctx = kmalloc(sizeof(*wd_ctx), GFP_KERNEL);
	if (wd_ctx) {
		wd_ctx->wd = wd;
		wd_ctx->ctx = adapter_ctx;
		/* ntoskernel_lock is not meant for use here, but since this
		 * function is called during initialization only,
		 * no harm abusing it */
		irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
		list_add(&wd_ctx->list, &wd_ctx_list);
		kspin_unlock_irql(&ntoskernel_lock, irql);
	}

	if (attributes & NDIS_ATTRIBUTE_BUS_MASTER)
		pci_set_master(wd->dev.pci);

	if (!(attributes & NDIS_ATTRIBUTE_DESERIALIZE)) {
		DBGTRACE2("serialized driver");
		set_bit(ATTR_SERIALIZED, &wd->attributes);
	}

	if (attributes & NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK)
		set_bit(ATTR_SURPRISE_REMOVE, &wd->attributes);

	if (!(attributes & NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND))
		set_bit(ATTR_HALT_ON_SUSPEND, &wd->attributes);

	if (wd->hangcheck_interval >= 0) {
		/* less than 3 seconds seem to be problematic */
		if (hangcheck_interval > 2)
			wd->hangcheck_interval = 2 * hangcheck_interval * HZ;
		else
			wd->hangcheck_interval = 3 * HZ;
	}

	wd->nmb->adapter_ctx = adapter_ctx;
	TRACEEXIT2(return);
}

static struct wrapper_dev *ctx_to_wd(void *ctx)
{
	struct wd_ctx_entry *wd_ctx;
	KIRQL irql;

	irql = kspin_lock_irql(&ntoskernel_lock, DISPATCH_LEVEL);
	list_for_each_entry(wd_ctx, &wd_ctx_list, list) {
		if (wd_ctx->ctx == ctx) {
			kspin_unlock_irql(&ntoskernel_lock, irql);
			return wd_ctx->wd;
		}
	}
	kspin_unlock_irql(&ntoskernel_lock, irql);

	return NULL;
}

STDCALL ULONG WRAP_EXPORT(NdisReadPciSlotInformation)
	(struct ndis_miniport_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrapper_dev *wd = nmb->wd;
	int i;
	TRACEENTER3("%d", len);
	for (i = 0; i < len; i++)
		pci_read_config_byte(wd->dev.pci, offset+i, &buf[i]);

	TRACEEXIT3(return len);
}

STDCALL ULONG WRAP_EXPORT(NdisWritePciSlotInformation)
	(struct ndis_miniport_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrapper_dev *wd = nmb->wd;
	int i;
	TRACEENTER3("%d", len);
	for (i = 0; i < len; i++)
		pci_write_config_byte(wd->dev.pci, offset+i, buf[i]);

	TRACEEXIT3(return len);
}

STDCALL void WRAP_EXPORT(NdisMQueryAdapterResources)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 struct ndis_resource_list *resource_list, UINT *size)
{
	int i;
	int len = 0;
	struct wrapper_dev *wd = nmb->wd;

	/* FIXME: do USB drivers call this? */
	struct pci_dev *pci_dev = wd->dev.pci;
	struct ndis_resource_entry *entry;
	TRACEENTER2("wd: %p. buf: %p, len: %d. IRQ:%d", wd,
		    resource_list, *size, pci_dev->irq);

	resource_list->version = 1;

	/* Put all memory and port resources */
	i = 0;
	while (pci_resource_start(pci_dev, i)) {
		entry = &resource_list->list[len++];
		if (pci_resource_flags(pci_dev, i) & IORESOURCE_MEM) {
			entry->type = 3;
			entry->flags = 0;

		} else if (pci_resource_flags(pci_dev, i) & IORESOURCE_IO) {
			entry->type = 1;
			entry->flags = 1;
		}

		entry->share = 0;
		entry->u.generic.start =
			(ULONG_PTR)pci_resource_start(pci_dev, i);
		entry->u.generic.length = pci_resource_len(pci_dev, i);

		i++;
	}

	/* Put IRQ resource */
	entry = &resource_list->list[len++];
	entry->type = 2;
	entry->share = 0;
	entry->flags = 0;
	entry->u.interrupt.level = pci_dev->irq;
	entry->u.interrupt.vector = pci_dev->irq;
	entry->u.interrupt.affinity = -1;

	resource_list->length = len;
	*size = (char *) (&resource_list->list[len]) - (char *)resource_list;
	*status = NDIS_STATUS_SUCCESS;


	DBGTRACE2("resource list v%d.%d len %d, size=%d",
		  resource_list->version, resource_list->revision,
		  resource_list->length, *size);

	for (i = 0; i < len; i++) {
		DBGTRACE2("resource: %d: %Lx %d, %d",
			  resource_list->list[i].type,
			  resource_list->list[i].u.generic.start,
			  resource_list->list[i].u.generic.length,
			  resource_list->list[i].flags);
	}
	TRACEEXIT2(return);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMPciAssignResources)
	(struct ndis_miniport_block *nmb, ULONG slot_number,
	 struct ndis_resource_list **resources)
{
	UINT size;
	NDIS_STATUS status;
	struct wrapper_dev *wd = nmb->wd;

	TRACEENTER2("%p", wd);
	size = sizeof(struct ndis_resource_list) +
		sizeof(struct ndis_resource_entry) * MAX_NDIS_PCI_RESOURCES;
	wd->pci_resources = vmalloc(size);
	if (!wd->pci_resources) {
		ERROR("couldn't allocate memory");
		TRACEEXIT2(return NDIS_STATUS_SUCCESS);
	}
	NdisMQueryAdapterResources(&status, nmb, wd->pci_resources, &size);
	*resources = wd->pci_resources;
	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMMapIoSpace)
	(void **virt, struct ndis_miniport_block *nmb,
	 NDIS_PHY_ADDRESS phy_addr, UINT len)
{
	struct wrapper_dev *wd = nmb->wd;

	TRACEENTER2("%016llx, %d", phy_addr, len);
	*virt = ioremap(phy_addr, len);
	if (*virt == NULL) {
		ERROR("%s", "ioremap failed");
		TRACEEXIT2(return NDIS_STATUS_FAILURE);
	}

	wd->mem_start = phy_addr;
	wd->mem_end = phy_addr + len -1;

	DBGTRACE2("ioremap successful %p", *virt);
	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL void WRAP_EXPORT(NdisMUnmapIoSpace)
	(struct ndis_miniport_block *nmb, void *virtaddr, UINT len)
{
	TRACEENTER2("%p, %d", virtaddr, len);
	iounmap(virtaddr);
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisAllocateSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER4("lock %p", lock);

	KeInitializeSpinLock(&lock->klock);
	lock->irql = PASSIVE_LEVEL;
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisFreeSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER4("lock %p", lock);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisAcquireSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	lock->irql = kspin_lock_irql(&lock->klock, DISPATCH_LEVEL);
	TRACEEXIT6(return);
}

STDCALL void WRAP_EXPORT(NdisReleaseSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	kspin_unlock_irql(&lock->klock, lock->irql);
	TRACEEXIT6(return);
}

STDCALL void WRAP_EXPORT(NdisDprAcquireSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	kspin_lock(&lock->klock);
	TRACEEXIT6(return);
}

STDCALL void WRAP_EXPORT(NdisDprReleaseSpinLock)
	(struct ndis_spinlock *lock)
{
	TRACEENTER6("lock %p", lock);
	kspin_unlock(&lock->klock);
	TRACEEXIT6(return);
}

STDCALL void WRAP_EXPORT(NdisInitializeReadWriteLock)
	(struct ndis_rw_lock *rw_lock)
{
	TRACEENTER3("%p", rw_lock);
	memset(rw_lock, 0, sizeof(*rw_lock));
	KeInitializeSpinLock(&rw_lock->u.s.klock);
	TRACEEXIT3(return);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMAllocateMapRegisters)
	(struct ndis_miniport_block *nmb, UINT dmachan,
	 NDIS_DMA_SIZE dmasize, ULONG basemap, ULONG size)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER2("%d %d %d %d", dmachan, dmasize, basemap, size);

//	if (basemap > 64)
//		return NDIS_STATUS_RESOURCES;

	if (wd->map_count > 0) {
		DBGTRACE2("%s: map registers already allocated: %u",
			  wd->net_dev->name, wd->map_count);
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	}

	wd->map_count = basemap;
	wd->map_dma_addr = kmalloc(basemap * sizeof(dma_addr_t),
				       GFP_KERNEL);
	if (!wd->map_dma_addr)
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	memset(wd->map_dma_addr, 0, basemap * sizeof(dma_addr_t));

	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL void WRAP_EXPORT(NdisMFreeMapRegisters)
	(struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER2("wd: %p", wd);

	if (wd->map_dma_addr != NULL)
		kfree(wd->map_dma_addr);
	wd->map_count = 0;
	TRACEEXIT2(return);
}

STDCALL void WRAP_EXPORT(NdisMAllocateSharedMemory)
	(struct ndis_miniport_block *nmb, ULONG size,
	 BOOLEAN cached, void **virt, NDIS_PHY_ADDRESS *phys)
{
	dma_addr_t p;
	void *v;
	struct wrapper_dev *wd = nmb->wd;

	TRACEENTER3("map count: %d, size: %u, cached: %d",
		    wd->map_count, size, cached);

//	if (wd->map_dma_addr == NULL)
//		ERROR("%s: DMA map address is not set!\n", __FUNCTION__);
	/* FIXME: do USB drivers call this? */
	v = PCI_DMA_ALLOC_COHERENT(wd->dev.pci, size, &p);
	if (!v) {
		ERROR("Failed to allocate DMA coherent memory. "
		      "Windows driver requested %d bytes of "
		      "%scached memory\n", size, cached ? "" : "un-");
	}

	*virt = v;
	*phys = p;
	DBGTRACE3("allocated shared memory: %p", v);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMAllocateSharedMemoryAsync)
	(struct ndis_miniport_block *nmb, ULONG size, BOOLEAN cached,
	 void *ctx)
{
	struct wrapper_dev *wd = nmb->wd;
	struct ndis_work_entry *ndis_work_entry;
	struct ndis_alloc_mem_work_item *alloc_mem;
	KIRQL irql;

	TRACEENTER3("wd: %p", wd);
	ndis_work_entry = kmalloc(sizeof(*ndis_work_entry), GFP_ATOMIC);
	if (!ndis_work_entry)
		return NDIS_STATUS_FAILURE;

	ndis_work_entry->type = NDIS_ALLOC_MEM_WORK_ITEM;
	ndis_work_entry->wd = wd;

	alloc_mem = &ndis_work_entry->entry.alloc_mem_work_item;
	alloc_mem->size = size;
	alloc_mem->cached = cached;
	alloc_mem->ctx = ctx;

	irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
	list_add_tail(&ndis_work_entry->list, &ndis_work_list);
	kspin_unlock_irql(&ndis_work_list_lock, irql);

	schedule_work(&ndis_work);
	TRACEEXIT3(return NDIS_STATUS_PENDING);
}

STDCALL void WRAP_EXPORT(NdisMFreeSharedMemory)
	(struct ndis_miniport_block *nmb, ULONG size, BOOLEAN cached,
	 void *virt, NDIS_PHY_ADDRESS addr)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%s", "");
	/* FIXME: do USB drivers call this? */
	PCI_DMA_FREE_COHERENT(wd->dev.pci, size, virt, addr);
	TRACEEXIT3(return);
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
STDCALL void WRAP_EXPORT(NdisAllocateBufferPool)
	(NDIS_STATUS *status, struct ndis_buffer_pool **pool_handle,
	 UINT num_descr)
{
	struct ndis_buffer_pool *pool;

	TRACEENTER3("buffers: %d", num_descr);
	pool = kmalloc(sizeof(*pool), GFP_ATOMIC);
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT3(return);
	}
	kspin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->free_descr = NULL;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisAllocateBuffer)
	(NDIS_STATUS *status, ndis_buffer **buffer,
	 struct ndis_buffer_pool *pool, void *virt, UINT length)
{
	ndis_buffer *descr;
	KIRQL irql;

	TRACEENTER3("pool: %p, allocated: %d",
		    pool, pool->num_allocated_descr);
	if (!pool) {
		*status = NDIS_STATUS_FAILURE;
		TRACEEXIT4(return);
	}
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	if (pool->num_allocated_descr < pool->max_descr) {
		if (pool->free_descr) {
			typeof(descr->flags) flags;
			descr = pool->free_descr;
			pool->free_descr = descr->next;
			flags = descr->flags;
			memset(descr, 0, sizeof(*descr));
			MmInitializeMdl(descr, virt, length);
			if (flags & MDL_CACHE_ALLOCATED)
				descr->flags = MDL_CACHE_ALLOCATED;
		} else
			descr = allocate_init_mdl(virt, length);
	} else
		descr = NULL;

	if (descr) {
		/* NdisFreeBuffer doesn't pass pool, so we store pool
		 * in unused field 'process' */
		descr->process = pool;
		pool->num_allocated_descr++;
		*status = NDIS_STATUS_SUCCESS;
		DBGTRACE3("allocated buffer %p for %p", descr, virt);
	} else
		*status = NDIS_STATUS_FAILURE;

	*buffer = descr;
	kspin_unlock_irql(&pool->lock, irql);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisFreeBuffer)
	(ndis_buffer *descr)
{
	struct ndis_buffer_pool *pool;
	KIRQL irql;

	TRACEENTER3("buffer: %p", descr);
	pool = descr->process;
	if (!pool) {
		ERROR("pool for descriptor %p is invalid", descr);
		TRACEEXIT3(return);
	}
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	descr->next = pool->free_descr;
	pool->free_descr = descr;
	pool->num_allocated_descr--;
	kspin_unlock_irql(&pool->lock, irql);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisFreeBufferPool)
	(struct ndis_buffer_pool *pool)
{
	ndis_buffer *cur, *prev;
	KIRQL irql;

	DBGTRACE3("pool: %p", pool);
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	cur = pool->free_descr;
	while (cur) {
		prev = cur;
		cur = cur->next;
		prev->process = NULL;
		free_mdl(prev);
	}
	kspin_unlock_irql(&pool->lock, irql);
	kfree(pool);
	pool = NULL;
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisAdjustBufferLength)
	(ndis_buffer *buffer, UINT length)
{
	TRACEENTER4("%p", buffer);
	if (buffer)
		buffer->bytecount = length;
	else
		ERROR("invalid buffer");
}

STDCALL void WRAP_EXPORT(NdisQueryBuffer)
	(ndis_buffer *buffer, void **virt, UINT *length)
{
	TRACEENTER3("buffer: %p", buffer);
	if (virt)
		*virt = MmGetMdlVirtualAddress(buffer);
	if (length)
		*length = MmGetMdlByteCount(buffer);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisQueryBufferSafe)
	(ndis_buffer *buffer, void **virt, UINT *length,
	 enum mm_page_priority priority)
{
	TRACEENTER3("%p, %p, %p", buffer, virt, length);
	if (virt)
		*virt = MmGetMdlVirtualAddress(buffer);
	if (length)
		*length = MmGetMdlByteCount(buffer);
}

STDCALL void *WRAP_EXPORT(NdisBufferVirtualAddress)
	(ndis_buffer *buffer)
{
	TRACEENTER3("%p", buffer);
	return MmGetMdlVirtualAddress(buffer);
}

STDCALL ULONG WRAP_EXPORT(NdisBufferLength)
	(ndis_buffer *buffer)
{
	TRACEENTER3("%p", buffer);
	return MmGetMdlByteCount(buffer);
}

STDCALL void WRAP_EXPORT(NdisAllocatePacketPool)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT rsvlen)
{
	struct ndis_packet_pool *pool;

	TRACEENTER3("buffers: %d", num_descr);
	pool = kmalloc(sizeof(*pool), GFP_ATOMIC);
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT3(return);
	}
	kspin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->free_descr = NULL;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisAllocatePacketPoolEx)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT overflowsize, UINT rsvlen)
{
	TRACEENTER3("");
	NdisAllocatePacketPool(status, pool_handle, num_descr, rsvlen);
	TRACEEXIT3(return);
}

STDCALL UINT WRAP_EXPORT(NdisPacketPoolUsage)
	(struct ndis_packet_pool *pool)
{
	UINT i;
	KIRQL irql;

	TRACEENTER4("");
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	i = pool->num_allocated_descr;
	kspin_unlock_irql(&pool->lock, irql);
	TRACEEXIT4(return i);
}

struct ndis_packet *allocate_ndis_packet(void)
{
	struct ndis_packet *ndis_packet;
	struct wrap_ndis_packet *wrap_ndis_packet;
	KIRQL irql;

	wrap_ndis_packet = kmem_cache_alloc(packet_cache, GFP_ATOMIC);
	if (wrap_ndis_packet) {
		irql = kspin_lock_irql(&wrap_ndis_packet_lock, DISPATCH_LEVEL);
		InsertHeadList(&wrap_ndis_packet_list,
			       &wrap_ndis_packet->list);
		kspin_unlock_irql(&wrap_ndis_packet_lock, irql);
		ndis_packet = &wrap_ndis_packet->ndis_packet;
		memset(ndis_packet, 0, sizeof(*ndis_packet));
		ndis_packet->private.oob_offset =
			offsetof(struct ndis_packet, oob_data);
		ndis_packet->private.packet_flags = fPACKET_ALLOCATED_BY_NDIS;
		return ndis_packet;
	} else
		return NULL;
}

void free_ndis_packet(struct ndis_packet *packet)
{
	struct wrap_ndis_packet *wrap_ndis_packet;
	KIRQL irql;

	packet->private.buffer_head = NULL;
	packet->private.valid_counts = FALSE;
	wrap_ndis_packet = container_of(packet, struct wrap_ndis_packet,
					ndis_packet);
	irql = kspin_lock_irql(&wrap_ndis_packet_lock, DISPATCH_LEVEL);
	RemoveEntryList(&wrap_ndis_packet->list);
	kspin_unlock_irql(&wrap_ndis_packet_lock, irql);
	kmem_cache_free(packet_cache, wrap_ndis_packet);
}

STDCALL void WRAP_EXPORT(NdisAllocatePacket)
	(NDIS_STATUS *status, struct ndis_packet **packet,
	 struct ndis_packet_pool *pool)
{
	struct ndis_packet *descr;
	KIRQL irql;

	TRACEENTER3("pool: %p", pool);
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		TRACEEXIT3(return);
	}
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	if (pool->num_allocated_descr < pool->max_descr) {
		if (pool->free_descr) {
			descr = pool->free_descr;
			pool->free_descr = descr->next;
			memset(descr, 0, sizeof(*descr));
			descr->private.oob_offset =
				offsetof(struct ndis_packet, oob_data);
			descr->private.packet_flags =
				fPACKET_ALLOCATED_BY_NDIS;
		} else
			descr = allocate_ndis_packet();
	} else
		descr = NULL;

	if (descr) {
		pool->num_allocated_descr++;
		descr->private.pool = pool;
		*status = NDIS_STATUS_SUCCESS;
	} else
		*status = NDIS_STATUS_RESOURCES;
	*packet = descr;
	kspin_unlock_irql(&pool->lock, irql);
	DBGTRACE3("packet: %p, pool: %p", descr, pool);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisDprAllocatePacket)
	(NDIS_STATUS *status, struct ndis_packet **packet,
	 struct ndis_packet_pool *pool)
{
	NdisAllocatePacket(status, packet, pool);
}

STDCALL void WRAP_EXPORT(NdisFreePacket)
	(struct ndis_packet *descr)
{
	struct ndis_packet_pool *pool;
	KIRQL irql;

	TRACEENTER3("packet: %p, pool: %p", descr, descr->private.pool);
	pool = descr->private.pool;
	if (!pool) {
		ERROR("pool for descriptor %p is invalid", descr);
		TRACEEXIT3(return);
	}
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	descr->private.buffer_head = NULL;
	descr->private.valid_counts = FALSE;
	descr->next = pool->free_descr;
	pool->free_descr = descr;
	pool->num_allocated_descr--;
	kspin_unlock_irql(&pool->lock, irql);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisFreePacketPool)
	(struct ndis_packet_pool *pool)
{
	struct ndis_packet *cur, *prev;
	KIRQL irql;

	TRACEENTER3("pool: %p", pool);
	irql = kspin_lock_irql(&pool->lock, DISPATCH_LEVEL);
	cur = pool->free_descr;
	while (cur) {
		prev = cur;
		cur = cur->next;
		free_ndis_packet(prev);
	}
	kspin_unlock_irql(&pool->lock, irql);
	kfree(pool);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisSend)
	(NDIS_STATUS *status, struct ndis_miniport_block *nmb,
	 struct ndis_packet *packet)
{
	struct wrapper_dev *wd = nmb->wd;
	KIRQL irql;
	struct miniport_char *miniport = &wd->driver->miniport;

	if (miniport->send_packets) {
		struct ndis_packet *packets[1];

		packets[0] = packet;
		irql = raise_irql(DISPATCH_LEVEL);
		LIN2WIN3(miniport->send_packets, wd->nmb->adapter_ctx,
			 packets, 1);
		lower_irql(irql);
		if (test_bit(ATTR_SERIALIZED, &wd->attributes)) {
			*status = packet->oob_data.status;
			switch (*status) {
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
				break;
			}
		} else {
			*status = NDIS_STATUS_PENDING;
		}
	} else {
		irql = raise_irql(DISPATCH_LEVEL);
		*status = LIN2WIN3(miniport->send, wd->nmb->adapter_ctx,
				   packet, 0);
		lower_irql(irql);
		switch (*status) {
		case NDIS_STATUS_SUCCESS:
			sendpacket_done(wd, packet);
			break;
		case NDIS_STATUS_PENDING:
			break;
		case NDIS_STATUS_RESOURCES:
			wd->send_ok = 0;
			break;
		case NDIS_STATUS_FAILURE:
			break;
		}
	}
	TRACEEXIT3(return);
}

/* FIXME: not sure if NDIS timers are synchronization or notification
 * events, but we have to initialize the event's wait_list so that
 * when timer expires, the handler can reference it */
STDCALL void WRAP_EXPORT(NdisMInitializeTimer)
	(struct ndis_miniport_timer *timer_handle,
	 struct ndis_miniport_block *nmb, void *func, void *ctx)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER4("%s", "");
	KeInitializeDpc(&timer_handle->kdpc, func, ctx);
	KeInitializeEvent((struct kevent *)&timer_handle->ktimer,
			  NotificationEvent, FALSE);
	wrapper_init_timer(&timer_handle->ktimer, wd, &timer_handle->kdpc);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisInitializeTimer)
	(struct ndis_timer *timer_handle, void *func, void *ctx)
{
	TRACEENTER4("%p, %p, %p", timer_handle, func, ctx);
	KeInitializeDpc(&timer_handle->kdpc, func, ctx);
	KeInitializeEvent((struct kevent *)&timer_handle->ktimer,
			  NotificationEvent, FALSE);
	wrapper_init_timer(&timer_handle->ktimer, NULL, &timer_handle->kdpc);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisSetTimer)
	(struct ndis_timer *timer_handle, UINT ms)
{
	unsigned long expires = ms * HZ / 1000;

	TRACEENTER4("%p, %u", timer_handle, ms);
	wrapper_set_timer(timer_handle->ktimer.wrapper_timer, expires, 0,
			  NULL);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisMSetPeriodicTimer)
	(struct ndis_miniport_timer *timer_handle, UINT ms)
{
	unsigned long expires = ms * HZ / 1000;
	unsigned long repeat = ms * HZ / 1000;

	TRACEENTER4("%p, %u", timer_handle, ms);
	wrapper_set_timer(timer_handle->ktimer.wrapper_timer, expires, repeat,
			  NULL);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisMCancelTimer)
	(struct ndis_miniport_timer *timer_handle, BOOLEAN *canceled)
{
	TRACEENTER4("%s", "");
	wrapper_cancel_timer(timer_handle->ktimer.wrapper_timer, canceled);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisCancelTimer)
	(struct ndis_timer *timer_handle, BOOLEAN *canceled)
{
	TRACEENTER4("%s", "");
	wrapper_cancel_timer(timer_handle->ktimer.wrapper_timer, canceled);
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisReadNetworkAddress)
	(NDIS_STATUS *status, void **addr, UINT *len,
	 struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	struct ndis_config_param *setting;
	struct unicode_string key;
	struct ansi_string ansi;
	int ret;

	TRACEENTER1("%s", "");
	ansi.buf = "mac_address";
	ansi.buflen = strlen(ansi.buf);
	ansi.len = ansi.buflen;

	*len = 0;
	*status = NDIS_STATUS_FAILURE;
	if (RtlAnsiStringToUnicodeString(&key, &ansi, 1) !=
	    NDIS_STATUS_SUCCESS)
		TRACEEXIT1(return);

	NdisReadConfiguration(status, &setting, nmb, &key,
			      NDIS_CONFIG_PARAM_STRING);
	RtlFreeUnicodeString(&key);

	if (*status == NDIS_STATUS_SUCCESS) {
		int int_mac[ETH_ALEN];
		ret = RtlUnicodeStringToAnsiString(&ansi,
						   &setting->data.ustring, 1);
		if (ret != NDIS_STATUS_SUCCESS)
			TRACEEXIT1(return);

		ret = sscanf(ansi.buf, MACSTR, MACINTADR(int_mac));
		RtlFreeAnsiString(&ansi);
		if (ret == ETH_ALEN) {
			int i;
			for (i = 0; i < ETH_ALEN; i++)
				wd->mac[i] = int_mac[i];
			printk(KERN_INFO "%s: %s ethernet device " MACSTR "\n",
			       wd->net_dev->name, DRIVER_NAME,
			       MAC2STR(wd->mac));
			*len = ETH_ALEN;
			*addr = wd->mac;
			*status = NDIS_STATUS_SUCCESS;
		}
	}

	TRACEEXIT1(return);
}

STDCALL void WRAP_EXPORT(NdisMRegisterAdapterShutdownHandler)
	(struct ndis_miniport_block *nmb, void *ctx, void *func)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER1("sp:%p", get_sp());
	wd->driver->miniport.adapter_shutdown = func;
	wd->shutdown_ctx = ctx;
}

STDCALL void WRAP_EXPORT(NdisMDeregisterAdapterShutdownHandler)
	(struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER1("sp:%p", get_sp());
	wd->driver->miniport.adapter_shutdown = NULL;
	wd->shutdown_ctx = NULL;
}

/* bottom half of the irq handler */
static void ndis_irq_bh(void *data)
{
	struct ndis_irq *ndis_irq = (struct ndis_irq *)data;
	struct wrapper_dev *wd = ndis_irq->wd;
	struct miniport_char *miniport = &wd->driver->miniport;
	KIRQL irql;

	/* Dpcs run at DISPATCH_LEVEL */
	irql = raise_irql(DISPATCH_LEVEL);
	if (ndis_irq->enabled) {
		LIN2WIN1(miniport->handle_interrupt, wd->nmb->adapter_ctx);
		if (miniport->enable_interrupts)
			LIN2WIN1(miniport->enable_interrupts,
				 wd->nmb->adapter_ctx);
	}
	lower_irql(irql);
}

/* Top half of the irq handler */
static irqreturn_t ndis_irq_th(int irq, void *data, struct pt_regs *pt_regs)
{
	int recognized = 0;
	int handled = 0;
	struct ndis_irq *ndis_irq = (struct ndis_irq *)data;
	struct wrapper_dev *wd;
	struct miniport_char *miniport;
	unsigned long flags;

	if (!ndis_irq || !ndis_irq->wd)
		return IRQ_NONE;
	wd = ndis_irq->wd;
	miniport = &wd->driver->miniport;
	/* this spinlock should be shared with NdisMSynchronizeWithInterrupt
	 */
	kspin_lock_irqsave(&ndis_irq->lock, flags);
	if (ndis_irq->req_isr)
		LIN2WIN3(miniport->isr, &recognized, &handled,
			 wd->nmb->adapter_ctx);
	else { //if (miniport->disable_interrupts)
		LIN2WIN1(miniport->disable_interrupts, wd->nmb->adapter_ctx);
		/* it is not shared interrupt, so handler must be called */
		recognized = handled = 1;
	}
	kspin_unlock_irqrestore(&ndis_irq->lock, flags);

	if (recognized && handled)
		schedule_work(&wd->irq_work);

	if (recognized)
		return IRQ_HANDLED;

	return IRQ_NONE;
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMRegisterInterrupt)
	(struct ndis_irq *ndis_irq, struct ndis_miniport_block *nmb,
	 UINT vector, UINT level, BOOLEAN req_isr,
	 BOOLEAN shared, enum kinterrupt_mode mode)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER1("%p, vector:%d, level:%d, req_isr:%d, shared:%d, "
		    "mode:%d sp:%p", ndis_irq, vector, level, req_isr,
		    shared, mode, get_sp());

	ndis_irq->irq.irq = vector;
	ndis_irq->wd = wd;
	ndis_irq->req_isr = req_isr;
	if (shared && !req_isr)
		WARNING("%s", "shared but dynamic interrupt!");
	ndis_irq->shared = shared;
	kspin_lock_init(&ndis_irq->lock);

	INIT_WORK(&wd->irq_work, &ndis_irq_bh, ndis_irq);
	if (request_irq(vector, ndis_irq_th, shared? SA_SHIRQ : 0,
			"ndiswrapper", ndis_irq)) {
		printk(KERN_WARNING "%s: request for irq %d failed\n",
		       DRIVER_NAME, vector);
		TRACEEXIT1(return NDIS_STATUS_RESOURCES);
	}
	ndis_irq->enabled = 1;
	printk(KERN_INFO "%s: using irq %d\n", DRIVER_NAME, vector);
	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

STDCALL void WRAP_EXPORT(NdisMDeregisterInterrupt)
	(struct ndis_irq *ndis_irq)
{
	struct wrapper_dev *wd;

	TRACEENTER1("%p", ndis_irq);

	if (!ndis_irq)
		TRACEEXIT1(return);
	wd = ndis_irq->wd;
	if (!wd)
		TRACEEXIT1(return);

	ndis_irq->enabled = 0;
	/* flush irq_bh workqueue; calling it before enabled=0 will
	 * crash since some drivers (Centrino at least) don't expect
	 * irq hander to be called anymore */
	/* cancel_delayed_work is probably better, but 2.4 kernels
	 * don't have equivalent function
	 */
#if LINUX_KERNEL_VERSION >= KERNEL_VERSION(2,6,0)
	flush_scheduled_work();
#else
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ/10);
#endif
	free_irq(ndis_irq->irq.irq, ndis_irq);
	ndis_irq->wd = NULL;
	wd->ndis_irq = NULL;
	TRACEEXIT1(return);
}

STDCALL BOOLEAN WRAP_EXPORT(NdisMSynchronizeWithInterrupt)
	(struct ndis_irq *ndis_irq, void *func, void *ctx)
{
	unsigned char ret;
	unsigned char (*sync_func)(void *ctx) STDCALL;
	unsigned long flags;

	TRACEENTER6("%p %p %p\n", ndis_irq, func, ctx);

	if (func == NULL || ctx == NULL)
		TRACEEXIT6(return 0);

	sync_func = func;
	kspin_lock_irqsave(&ndis_irq->lock, flags);
	ret = LIN2WIN1(sync_func, ctx);
	kspin_unlock_irqrestore(&ndis_irq->lock, flags);

	DBGTRACE6("sync_func returns %u", ret);
	TRACEEXIT6(return ret);
}

/* called via function pointer */
STDCALL void
NdisMIndicateStatus(struct ndis_miniport_block *nmb, NDIS_STATUS status,
		    void *buf, UINT len)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER2("status=0x%x len=%d", status, len);
	if (status == NDIS_STATUS_MEDIA_DISCONNECT) {
		wd->link_status = 0;
		wd->send_ok = 0;
		set_bit(LINK_STATUS_CHANGED, &wd->wrapper_work);
	}
	if (status == NDIS_STATUS_MEDIA_CONNECT) {
		wd->link_status = 1;
		wd->send_ok = 1;
		set_bit(LINK_STATUS_CHANGED, &wd->wrapper_work);
	}

	if (status == NDIS_STATUS_MEDIA_SPECIFIC_INDICATION && buf) {
		struct ndis_status_indication *si = buf;
		struct ndis_auth_req *auth_req;
		struct ndis_radio_status_indication *radio_status;

		DBGTRACE2("status_type=%d", si->status_type);

		switch (si->status_type) {
		case Ndis802_11StatusType_Authentication:
			buf = (char *)buf + sizeof(*si);
			len -= sizeof(*si);
			while (len > 0) {
				auth_req = (struct ndis_auth_req *)buf;
				DBGTRACE1(MACSTR, MAC2STR(auth_req->bssid));
				if (auth_req->flags & 0x01)
					DBGTRACE2("%s", "reqauth");
				if (auth_req->flags & 0x02)
					DBGTRACE2("%s", "keyupdate");
				if (auth_req->flags & 0x06)
					DBGTRACE2("%s", "pairwise_error");
				if (auth_req->flags & 0x0E)
					DBGTRACE2("%s", "group_error");
				/* TODO: report to wpa_supplicant */
				len -= auth_req->length;
				buf = (char *)buf + auth_req->length;
			}
			break;
		case Ndis802_11StatusType_MediaStreamMode:
			break;
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
			DBGTRACE2("PMKID_CANDIDATE_LIST ver %ld "
				  "num_cand %ld",
				  cand->version, cand->num_candidates);
			for (i = 0; i < cand->num_candidates; i++) {
				struct ndis_pmkid_candidate *c =
					&cand->candidates[i];
				if ((u8 *)(c + 1) > end) {
					DBGTRACE2("Truncated "
						    "PMKID_CANDIDATE_LIST");
					break;
				}
				DBGTRACE2("%ld: " MACSTR " 0x%lx",
					  i, MAC2STR(c->bssid), c->flags);
#if WIRELESS_EXT > 17
				{
					struct iw_pmkid_cand pcand;
					union iwreq_data wrqu;
					memset(&pcand, 0, sizeof(pcand));
					if (c->flags & 0x01)
						pcand.flags |=
							IW_PMKID_CAND_PREAUTH;
					pcand.index = i;
					memcpy(pcand.bssid.sa_data, c->bssid,
					       ETH_ALEN);

					memset(&wrqu, 0, sizeof(wrqu));
					wrqu.data.length = sizeof(pcand);
					wireless_send_event(wd->net_dev,
							    IWEVPMKIDCAND,
							    &wrqu,
							    (u8 *)&pcand);
				}
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
		}
	}

	TRACEEXIT1(return);
}

/* called via function pointer */
STDCALL void NdisMIndicateStatusComplete(struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("");
	schedule_work(&wd->wrapper_worker);
	if (wd->send_ok)
		schedule_work(&wd->xmit_work);
}

/* called via function pointer */
STDCALL void
NdisMIndicateReceivePacket(struct ndis_miniport_block *nmb,
			   struct ndis_packet **packets, UINT nr_packets)
{
	struct wrapper_dev *wd = nmb->wd;
	ndis_buffer *buffer;
	struct ndis_packet *packet;
	struct sk_buff *skb;
	int i;
	struct ndis_work_entry *ndis_work_entry;
	KIRQL irql;

	TRACEENTER3("%s", "");
	for (i = 0; i < nr_packets; i++) {
		packet = packets[i];
		if (!packet) {
			WARNING("%s", "Skipping empty packet on receive");
			continue;
		}

		buffer = packet->private.buffer_head;

		skb = dev_alloc_skb(MmGetMdlByteCount(buffer));
		if (skb) {
			skb->dev = wd->net_dev;
			eth_copy_and_sum(skb, MmGetMdlVirtualAddress(buffer),
					 MmGetMdlByteCount(buffer), 0);
			skb_put(skb, MmGetMdlByteCount(buffer));
			skb->protocol = eth_type_trans(skb, wd->net_dev);
			wd->stats.rx_bytes += MmGetMdlByteCount(buffer);
			wd->stats.rx_packets++;
			netif_rx(skb);
		} else
			wd->stats.rx_dropped++;

		/* serialized drivers check the status upon return
		 * from this function */
		if (test_bit(ATTR_SERIALIZED, &wd->attributes)) {
			packet->oob_data.status = NDIS_STATUS_SUCCESS;
			continue;
		}

		/* if a deserialized driver sets
		 * NDIS_STATUS_RESOURCES, then it reclaims the packet
		 * upon return from this function: it doesn't matter
		 * what value we set in the status */
		if (packet->oob_data.status == NDIS_STATUS_RESOURCES) {
			packet->oob_data.status = NDIS_STATUS_SUCCESS;
			DBGTRACE3("low on resources");
			continue;
		}

		if (packet->oob_data.status != NDIS_STATUS_SUCCESS)
			WARNING("invalid packet status %08X",
				packet->oob_data.status);
		/* deserialized driver doesn't check the status upon
		 * return from this function; we need to call
		 * MiniportReturnPacket later for this packet. Calling
		 * MiniportReturnPacket from here is not correct - the
		 * driver doesn't expect it (at least Centrino driver
		 * crashes) */
		packet->oob_data.status = NDIS_STATUS_PENDING;
		ndis_work_entry = kmalloc(sizeof(*ndis_work_entry),
					  GFP_ATOMIC);
		if (!ndis_work_entry) {
			ERROR("couldn't allocate memory");
			continue;
		}
		ndis_work_entry->type = NDIS_RETURN_PACKET_WORK_ITEM;
		ndis_work_entry->wd = wd;
		ndis_work_entry->entry.return_packet = packet;

		irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
		list_add_tail(&ndis_work_entry->list, &ndis_work_list);
		kspin_unlock_irql(&ndis_work_list_lock, irql);
	}
	schedule_work(&ndis_work);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisMCoIndicateReceivePacket)
	(struct ndis_miniport_block *nmb, struct ndis_packet **packets,
	 UINT nr_packets)
{
	TRACEENTER3("nmb = %p", nmb);
	NdisMIndicateReceivePacket(nmb, packets, nr_packets);
	TRACEEXIT3(return);
}

/* called via function pointer */
STDCALL void
NdisMSendComplete(struct ndis_miniport_block *nmb, struct ndis_packet *packet,
		  NDIS_STATUS status)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%08x", status);
	sendpacket_done(wd, packet);
	/* In case a serialized driver has requested a pause by returning
	 * NDIS_STATUS_RESOURCES we need to give the send-code a kick again.
	 */
	wd->send_ok = 1;
	schedule_work(&wd->xmit_work);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisMCoSendComplete)
	(NDIS_STATUS status, struct ndis_miniport_block *nmb,
	 struct ndis_packet *packet)
{
	TRACEENTER3("%08x", status);
	NdisMSendComplete(nmb, packet, status);
	TRACEEXIT3(return);
}

/* called via function pointer */
STDCALL void
NdisMSendResourcesAvailable(struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%s", "");
	/* sending packets immediately seem to result in NDIS_STATUS_FAILURE,
	   so wait for a while before sending the packet again */
	mdelay(5);
	wd->send_ok = 1;
	schedule_work(&wd->xmit_work);
	TRACEEXIT3(return);
}

/* called via function pointer (by NdisMEthIndicateReceive macro) */
STDCALL void
EthRxIndicateHandler(void *adapter_ctx, void *rx_ctx, char *header1,
		     char *header, UINT header_size, void *look_ahead,
		     UINT look_ahead_size, UINT packet_size)
{
	struct sk_buff *skb = NULL;
	struct wrapper_dev *wd = ctx_to_wd(rx_ctx);
	unsigned int skb_size = 0;
	KIRQL irql;

	TRACEENTER3("adapter_ctx = %p, rx_ctx = %p, buf = %p, size = %d, "
		    "buf = %p, size = %d, packet = %d",
		    adapter_ctx, rx_ctx, header, header_size, look_ahead,
		    look_ahead_size, packet_size);

	DBGTRACE3("wd = %p", wd);
	if (!wd)
		TRACEEXIT3(return);

	if (look_ahead_size < packet_size) {
		struct ndis_packet *packet;
		struct miniport_char *miniport;
		unsigned int res, bytes_txed;

		packet = allocate_ndis_packet();
		if (!packet) {
			wd->stats.rx_dropped++;
			TRACEEXIT3(return);
		}
		packet->private.pool = NULL;

		miniport = &wd->driver->miniport;
		irql = raise_irql(DISPATCH_LEVEL);
		res = LIN2WIN6(miniport->tx_data, packet, &bytes_txed,
			       adapter_ctx, rx_ctx, look_ahead_size,
			       packet_size);
		lower_irql(irql);
		if (res == NDIS_STATUS_SUCCESS) {
			ndis_buffer *buffer;
			skb = dev_alloc_skb(header_size+look_ahead_size+
					    bytes_txed);
			if (skb) {
				memcpy(skb->data, header, header_size);
				memcpy(skb->data+header_size, look_ahead,
				       look_ahead_size);
				buffer = packet->private.buffer_head;
				memcpy(skb->data+header_size+look_ahead_size,
				       MmGetMdlVirtualAddress(buffer),
				       bytes_txed);
				skb_size = header_size+look_ahead_size+
					bytes_txed;
				free_ndis_packet(packet);
			}
		} else if (res == NDIS_STATUS_PENDING) {
			/* driver will call td_complete */
			packet->look_ahead = kmalloc(look_ahead_size,
						     GFP_ATOMIC);
			if (!packet->look_ahead) {
				free_ndis_packet(packet);
				wd->stats.rx_dropped++;
				TRACEEXIT3(return);
			}
			memcpy(&packet->header, header,
			       sizeof(packet->header));
			memcpy(packet->look_ahead, look_ahead,
			       look_ahead_size);
			packet->look_ahead_size = look_ahead_size;
		} else {
			free_ndis_packet(packet);
			wd->stats.rx_dropped++;
			TRACEEXIT3(return);
		}
	} else {
		skb_size = header_size+packet_size;
		skb = dev_alloc_skb(skb_size);
		if (skb) {
			memcpy(skb->data, header, header_size);
			memcpy(skb->data+header_size, look_ahead, packet_size);
		}
	}

	if (skb && skb_size > 0) {
		skb->dev = wd->net_dev;
		skb_put(skb, skb_size);
		skb->protocol = eth_type_trans(skb, wd->net_dev);
		wd->stats.rx_bytes += skb_size;
		wd->stats.rx_packets++;
		netif_rx(skb);
	} else
		wd->stats.rx_dropped++;

	TRACEEXIT3(return);
}

/* called via function pointer */
STDCALL void
NdisMTransferDataComplete(struct ndis_miniport_block *nmb,
			  struct ndis_packet *packet,
			  NDIS_STATUS status, UINT bytes_txed)
{
	struct wrapper_dev *wd = nmb->wd;
	struct sk_buff *skb;
	unsigned int skb_size;

	TRACEENTER3("wd = %p, packet = %p, bytes_txed = %d",
		    wd, packet, bytes_txed);

	if (!packet) {
		WARNING("%s", "illegal packet");
		TRACEEXIT3(return);
	}

	skb_size = sizeof(packet->header) + packet->look_ahead_size +
		bytes_txed;

	skb = dev_alloc_skb(skb_size);
	if (!skb) {
		kfree(packet->look_ahead);
		free_ndis_packet(packet);
		wd->stats.rx_dropped++;
		TRACEEXIT3(return);
	}

	skb->dev = wd->net_dev;
	memcpy(skb->data, packet->header, sizeof(packet->header));
	memcpy(skb->data + sizeof(packet->header), packet->look_ahead,
	       packet->look_ahead_size);
	memcpy(skb->data + sizeof(packet->header) + packet->look_ahead_size,
	       MmGetMdlVirtualAddress(packet->private.buffer_head),
	       bytes_txed);
	kfree(packet->look_ahead);
	free_ndis_packet(packet);
	skb_put(skb, skb_size);
	skb->protocol = eth_type_trans(skb, wd->net_dev);
	wd->stats.rx_bytes += skb_size;
	wd->stats.rx_packets++;
	netif_rx(skb);
}

/* called via function pointer */
STDCALL void
EthRxComplete(struct ndis_miniport_block *nmb)
{
	DBGTRACE3("%s", "");
}

/* Called via function pointer if query returns NDIS_STATUS_PENDING */
STDCALL void
NdisMQueryInformationComplete(struct ndis_miniport_block *nmb,
			      NDIS_STATUS status)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%08X", status);

	wd->ndis_comm_res = status;
	wd->ndis_comm_done = 1;
	wake_up(&wd->ndis_comm_wq);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisMCoRequestComplete)
	(NDIS_STATUS status, struct ndis_miniport_block *nmb,
	 struct ndis_request *ndis_request)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%08X", status);

	wd->ndis_comm_res = status;
	wd->ndis_comm_done = 1;
	wake_up(&wd->ndis_comm_wq);
	TRACEEXIT3(return);
}

/* Called via function pointer if setinfo returns NDIS_STATUS_PENDING */
STDCALL void
NdisMSetInformationComplete(struct ndis_miniport_block *nmb,
			    NDIS_STATUS status)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("status = %08X", status);

	wd->ndis_comm_res = status;
	wd->ndis_comm_done = 1;
	wake_up(&wd->ndis_comm_wq);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisMSleep)
	(ULONG us)
{
	TRACEENTER4("us: %u", us);
	if (us > 0) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * us / 1000000);
		DBGTRACE4("%s", "woke up");
	}
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisGetCurrentSystemTime)
	(LARGE_INTEGER *time)
{
	*time = ticks_1601();
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMRegisterIoPortRange)
	(void **virt, struct ndis_miniport_block *nmb, UINT start, UINT len)
{
	TRACEENTER3("%08x %08x", start, len);
	*virt = (void *)(ULONG_PTR)start;
	return NDIS_STATUS_SUCCESS;
}

STDCALL void WRAP_EXPORT(NdisMDeregisterIoPortRange)
	(struct ndis_miniport_block *nmb, UINT start, UINT len, void* virt)
{
	TRACEENTER1("%08x %08x", start, len);
}

STDCALL LONG WRAP_EXPORT(NdisInterlockedDecrement)
	(LONG *val)
{
	return InterlockedDecrement(FASTCALL_ARGS_1(val));
}

STDCALL LONG WRAP_EXPORT(NdisInterlockedIncrement)
	(LONG *val)
{
	return InterlockedIncrement(FASTCALL_ARGS_1(val));
}

STDCALL struct nt_list *WRAP_EXPORT(NdisInterlockedInsertHeadList)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertHeadList(FASTCALL_ARGS_3(head, entry,
							   &lock->klock));
}

STDCALL struct nt_list *WRAP_EXPORT(NdisInterlockedInsertTailList)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertTailList(FASTCALL_ARGS_3(head, entry,
							   &lock->klock));
}

STDCALL struct nt_list *WRAP_EXPORT(NdisInterlockedRemoveHeadList)
	(struct nt_list *head, struct ndis_spinlock *lock)
{
	return ExInterlockedRemoveHeadList(FASTCALL_ARGS_2(head,
							   &lock->klock));
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMInitializeScatterGatherDma)
	(struct ndis_miniport_block *nmb, UCHAR dma_size, ULONG max_phy_map)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER2("dma_size=%d, maxtransfer=%u", dma_size, max_phy_map);
#ifdef CONFIG_X86_64
	if (dma_size != NDIS_DMA_64BITS)
		ERROR("DMA size is not 64-bits");
#endif
	wd->use_sg_dma = 1;
	return NDIS_STATUS_SUCCESS;
}

STDCALL ULONG WRAP_EXPORT(NdisMGetDmaAlignment)
	(struct ndis_miniport_block *nmb)
{
	TRACEENTER3("%s", "");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	return dma_get_cache_alignment();
#else
	return L1_CACHE_BYTES;
#endif
}

STDCALL void WRAP_EXPORT(NdisQueryBufferOffset)
	(ndis_buffer *buffer, UINT *offset, UINT *length)
{
	TRACEENTER3("%s", "");
	*offset = MmGetMdlByteOffset(buffer);
	*length = MmGetMdlByteCount(buffer);
}

STDCALL CHAR WRAP_EXPORT(NdisSystemProcessorCount)
	(void)
{
	return NR_CPUS;
}

STDCALL void WRAP_EXPORT(NdisInitializeEvent)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeInitializeEvent(&ndis_event->kevent, NotificationEvent, 0);
}

STDCALL BOOLEAN WRAP_EXPORT(NdisWaitEvent)
	(struct ndis_event *ndis_event, UINT ms)
{
	LARGE_INTEGER ticks;
	NTSTATUS res;

	TRACEENTER3("%p %u", ndis_event, ms);
	ticks = -((LARGE_INTEGER)ms * TICKSPERMSEC);
	res = KeWaitForSingleObject(&ndis_event->kevent, 0, 0, TRUE,
				    ms == 0 ? NULL : &ticks);
	if (res == STATUS_SUCCESS)
		TRACEEXIT3(return TRUE);
	else
		TRACEEXIT3(return FALSE);
}

STDCALL void WRAP_EXPORT(NdisSetEvent)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeSetEvent(&ndis_event->kevent, 0, 0);
}

STDCALL void WRAP_EXPORT(NdisResetEvent)
	(struct ndis_event *ndis_event)
{
	TRACEENTER3("%p", ndis_event);
	KeResetEvent(&ndis_event->kevent);
}

/* called via function pointer */
STDCALL void
NdisMResetComplete(struct ndis_miniport_block *nmb, NDIS_STATUS status,
		   BOOLEAN address_reset)
{
	struct wrapper_dev *wd = nmb->wd;

	TRACEENTER3("status: %08X, reset status: %u", status,
		    address_reset);

	wd->ndis_comm_res = status;
	wd->reset_status = status;
	wd->ndis_comm_done = 1;
	wake_up(&wd->ndis_comm_wq);
	TRACEEXIT3(return);
}

/* one worker for all drivers/handles */
static void ndis_worker(void *data)
{
	struct ndis_work_entry *ndis_work_entry;
	struct ndis_sched_work_item *sched_work_item;
	struct ndis_alloc_mem_work_item *alloc_mem;
	struct ndis_free_mem_work_item *free_mem;
	struct ndis_io_work_item *io_work_item;
	struct ndis_packet *packet;
	struct wrapper_dev *wd;
	struct miniport_char *miniport;
	void *virt;
	NDIS_PHY_ADDRESS phys;
	KIRQL irql;

	TRACEENTER3("%s", "");

	while (1) {
		irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
		if (list_empty(&ndis_work_list))
			ndis_work_entry = NULL;
		else {
			ndis_work_entry =
				list_entry(ndis_work_list.next,
					   struct ndis_work_entry, list);
			list_del(&ndis_work_entry->list);
		}
		kspin_unlock_irql(&ndis_work_list_lock, irql);

		if (!ndis_work_entry) {
			DBGTRACE3("%s", "No more work");
			break;
		}

		wd = ndis_work_entry->wd;
		switch (ndis_work_entry->type) {
		case NDIS_SCHED_WORK_ITEM:
			sched_work_item =
				ndis_work_entry->entry.sched_work_item;

			DBGTRACE3("Calling work at %p with parameter %p",
				  sched_work_item->func,
				  sched_work_item->ctx);
			LIN2WIN2(sched_work_item->func, sched_work_item,
				 sched_work_item->ctx);
			break;

		case NDIS_IO_WORK_ITEM:
			io_work_item =
				ndis_work_entry->entry.io_work_item;

			DBGTRACE3("Calling work at %p with parameter %p",
				  io_work_item->func, io_work_item->ctx);
			LIN2WIN2(io_work_item->func,
				io_work_item->device_object,
				io_work_item->ctx);
			break;

		case NDIS_ALLOC_MEM_WORK_ITEM:
			alloc_mem =
				&ndis_work_entry->entry.alloc_mem_work_item;
			DBGTRACE3("Allocating %scached memory of length %ld",
				  alloc_mem->cached ? "" : "un-",
				  alloc_mem->size);
			miniport = &wd->driver->miniport;
			NdisMAllocateSharedMemory(wd->nmb, alloc_mem->size,
						  alloc_mem->cached,
						  &virt, &phys);
			irql = raise_irql(DISPATCH_LEVEL);
			LIN2WIN5(miniport->alloc_complete, wd->nmb, virt,
				 &phys, alloc_mem->size, alloc_mem->ctx);
			lower_irql(irql);
			break;

		case NDIS_FREE_MEM_WORK_ITEM:
			free_mem = &ndis_work_entry->entry.free_mem_work_item;
			DBGTRACE3("Freeing memory of size %d, flags %d at %p",
				  free_mem->length, free_mem->flags,
				  free_mem->addr);
			if (free_mem->addr)
				vfree(free_mem->addr);
			break;

		case NDIS_RETURN_PACKET_WORK_ITEM:
			packet = ndis_work_entry->entry.return_packet;
			miniport = &wd->driver->miniport;
			irql = raise_irql(DISPATCH_LEVEL);
			LIN2WIN2(miniport->return_packet,
				 wd->nmb->adapter_ctx, packet);
			lower_irql(irql);
			break;

		default:
			ERROR("unknown ndis work item: %d",
			      ndis_work_entry->type);
			break;
		}
		kfree(ndis_work_entry);
	}
	TRACEEXIT3(return);
}

STDCALL struct ndis_io_work_item *WRAP_EXPORT(IoAllocateWorkItem)
	(void *device_object)
{
	struct ndis_io_work_item *io_work_item;

	TRACEENTER3("%p", device_object);
	io_work_item = kmalloc(sizeof(*io_work_item), GFP_ATOMIC);
	if (!io_work_item)
		TRACEEXIT3(return NULL);

	io_work_item->device_object = device_object;
	TRACEEXIT3(return io_work_item);
}

STDCALL void WRAP_EXPORT(IoFreeWorkItem)
	(struct ndis_io_work_item *io_work_item)
{
	kfree(io_work_item);
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(IoQueueWorkItem)
	(struct ndis_io_work_item *io_work_item, void *func,
	 enum work_queue_type queue_type, void *ctx)
{
	struct ndis_work_entry *ndis_work_entry;
	KIRQL irql;

	TRACEENTER3("%s", "");
	if (io_work_item == NULL) {
		ERROR("%s", "io_work_item is NULL; item not queued");
		return;
	}

	ndis_work_entry = kmalloc(sizeof(*ndis_work_entry), GFP_ATOMIC);
	if (!ndis_work_entry)
		BUG();

	ndis_work_entry->type = NDIS_IO_WORK_ITEM;
	io_work_item->func = func;
	io_work_item->ctx = ctx;
	ndis_work_entry->entry.io_work_item = io_work_item;

	irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
	list_add_tail(&ndis_work_entry->list, &ndis_work_list);
	kspin_unlock_irql(&ndis_work_list_lock, irql);

	schedule_work(&ndis_work);
	TRACEEXIT3(return);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisScheduleWorkItem)
	(struct ndis_sched_work_item *ndis_sched_work_item)
{
	struct ndis_work_entry *ndis_work_entry;
	KIRQL irql;

	TRACEENTER3("%s", "");
	/* this function is called from irq_bh by realtek driver */
	ndis_work_entry = kmalloc(sizeof(*ndis_work_entry), GFP_ATOMIC);
	if (!ndis_work_entry)
		BUG();

	ndis_work_entry->type = NDIS_SCHED_WORK_ITEM;
	ndis_work_entry->entry.sched_work_item = ndis_sched_work_item;

	irql = kspin_lock_irql(&ndis_work_list_lock, DISPATCH_LEVEL);
	list_add_tail(&ndis_work_entry->list, &ndis_work_list);
	kspin_unlock_irql(&ndis_work_list_lock, irql);

	schedule_work(&ndis_work);
	TRACEEXIT3(return NDIS_STATUS_SUCCESS);
}

STDCALL void WRAP_EXPORT(NdisUnchainBufferAtBack)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	ndis_buffer *b, *btail;

	TRACEENTER3("%p", packet);
	if (packet == NULL || buffer == NULL)
		return;
	b = packet->private.buffer_head;
	if (!b) {
		/* no buffer in packet */
		*buffer = NULL;
		TRACEEXIT3(return);
	}
	btail = packet->private.buffer_tail;
	*buffer = btail;
	packet->private.valid_counts = FALSE;
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
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisUnchainBufferAtFront)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	TRACEENTER3("%p", packet);
	if (buffer == NULL)
		return;
	if (packet == NULL) {
		/* no buffer in packet */
		*buffer = NULL;
		TRACEEXIT3(return);
	}

	packet->private.valid_counts = FALSE;
	*buffer = packet->private.buffer_head;
	if (packet->private.buffer_head == packet->private.buffer_tail) {
		/* one buffer in packet */
		packet->private.buffer_head = NULL;
		packet->private.buffer_tail = NULL;
	} else
		packet->private.buffer_head = (*buffer)->next;

	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisGetFirstBufferFromPacketSafe)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	ndis_buffer *b = packet->private.buffer_head;

	TRACEENTER3("%p", b);
	*first_buffer = b;
	if (b) {
		*first_buffer_va = MmGetMdlVirtualAddress(b);
		*first_buffer_length = *total_buffer_length =
			MmGetMdlByteCount(b);
		for (b = b->next; b != NULL; b = b->next)
			*total_buffer_length += MmGetMdlByteCount(b);
	} else {
		*first_buffer_va = NULL;
		*first_buffer_length = 0;
		*total_buffer_length = 0;
	}
	TRACEEXIT3(return);
}

STDCALL void WRAP_EXPORT(NdisGetFirstBufferFromPacket)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	NdisGetFirstBufferFromPacketSafe(packet, first_buffer,
					 first_buffer_va, first_buffer_length,
					 total_buffer_length,
					 NormalPagePriority);
}

STDCALL void WRAP_EXPORT(NdisCopyFromPacketToPacketSafe)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied,
	 enum mm_page_priority priority)
{
	UINT dst_left, src_left, left, done;
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
	dst_left = MmGetMdlByteCount(dst_buf) - dst_offset;
	src_left = MmGetMdlByteCount(src_buf) - src_offset;

	left = min(src_left, dst_left);
	left = min(left, num_to_copy);
	memcpy(MmGetMdlVirtualAddress(dst_buf) + dst_offset,
	       MmGetMdlVirtualAddress(src_buf) + src_offset, left);

	done = num_to_copy - left;
	while (done > 0) {
		if (left == dst_left) {
			dst_buf = dst_buf->next;
			if (!dst_buf)
				break;
			dst_left = MmGetMdlByteCount(dst_buf);
		} else
			dst_left -= left;
		if (left == src_left) {
			src_buf = src_buf->next;
			if (!src_buf)
				break;
			src_left = MmGetMdlByteCount(src_buf);
		} else
			src_left -= left;

		left = min(src_left, dst_left);
		left = min(left, done);
		memcpy(MmGetMdlVirtualAddress(dst_buf),
		       MmGetMdlVirtualAddress(src_buf), left);
		done -= left;
	}
	*num_copied = num_to_copy - done;
	TRACEEXIT4(return);
}

STDCALL void WRAP_EXPORT(NdisCopyFromPacketToPacket)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied)
{
	NdisCopyFromPacketToPacketSafe(dst, dst_offset, num_to_copy,
				       src, src_offset, num_copied,
				       NormalPagePriority);
	return;
}

STDCALL void WRAP_EXPORT(NdisMStartBufferPhysicalMapping)
	(struct ndis_miniport_block *nmb, ndis_buffer *buf,
	 ULONG phy_map_reg, BOOLEAN write_to_dev,
	 struct ndis_phy_addr_unit *phy_addr_array, UINT *array_size)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("phy_map_reg: %u", phy_map_reg);

	if (!write_to_dev) {
		ERROR( "dma from device not supported (%d)", write_to_dev);
		*array_size = 0;
		return;
	}

	if (phy_map_reg > wd->map_count) {
		ERROR("map_register too big (%u > %u)",
		      phy_map_reg, wd->map_count);
		*array_size = 0;
		return;
	}

	if (wd->map_dma_addr[phy_map_reg] != 0) {
//		ERROR("map register already used (%lu)", phy_map_reg);
		*array_size = 1;
		return;
	}

	// map buffer
	/* FIXME: do USB drivers call this? */
	phy_addr_array[0].phy_addr =
		PCI_DMA_MAP_SINGLE(wd->dev.pci,
				   MmGetMdlVirtualAddress(buf),
				   MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);
	phy_addr_array[0].length = MmGetMdlByteCount(buf);

	*array_size = 1;

	// save mapping index
	wd->map_dma_addr[phy_map_reg] = phy_addr_array[0].phy_addr;
}

STDCALL void WRAP_EXPORT(NdisMCompleteBufferPhysicalMapping)
	(struct ndis_miniport_block *nmb, ndis_buffer *buf,
	 ULONG phy_map_reg)
{
	struct wrapper_dev *wd = nmb->wd;
	TRACEENTER3("%p %u (%u)", wd, phy_map_reg, wd->map_count);

	if (phy_map_reg > wd->map_count) {
		ERROR("map_register too big (%u > %u)",
		      phy_map_reg, wd->map_count);
		return;
	}

	if (wd->map_dma_addr[phy_map_reg] == 0) {
//		ERROR("map register not used (%lu)", phy_map_reg);
		return;
	}

	// unmap buffer
	/* FIXME: do USB drivers call this? */
	PCI_DMA_UNMAP_SINGLE(wd->dev.pci,
			     wd->map_dma_addr[phy_map_reg],
			     MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);

	// clear mapping index
	wd->map_dma_addr[phy_map_reg] = 0;
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMRegisterDevice)
	(struct ndis_miniport_block *nmb, struct unicode_string *dev_name,
	 struct unicode_string *sym_name, void **funcs,
	 void **dev_object, void **dev_handle)
{
	TRACEENTER1("nmb: %p", nmb);
	*dev_object = nmb->fdo;
	*dev_handle = nmb;
	TRACEEXIT1(return NDIS_STATUS_SUCCESS);
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMDeregisterDevice)
	(struct ndis_miniport_block *nmb)
{
	return NDIS_STATUS_SUCCESS;
}

STDCALL void WRAP_EXPORT(NdisMGetDeviceProperty)
	(struct ndis_miniport_block *nmb, void **phy_dev, void **func_dev,
	 void **next_dev, void **alloc_res, void**trans_res)
{
	TRACEENTER2("nmb: %p, phy_dev = %p, func_dev = %p, next_dev = %p, "
		    "alloc_res = %p, trans_res = %p", phy_dev, func_dev,
		    nmb, next_dev, alloc_res, trans_res);
	if (phy_dev)
		*phy_dev = nmb->pdo;
	if (func_dev)
		*func_dev = nmb->fdo;
	if (next_dev)
		*next_dev = nmb->next_device;
}

STDCALL void WRAP_EXPORT(NdisMRegisterUnloadHandler)
	(struct driver_object *drv_obj, void *unload)
{
	if (drv_obj)
		drv_obj->driver_unload = unload;
	return;
}

STDCALL NDIS_STATUS WRAP_EXPORT(NdisMQueryAdapterInstanceName)
	(struct unicode_string *name, struct ndis_miniport_block *nmb)
{
	struct wrapper_dev *wd = nmb->wd;
	struct ansi_string ansi_string;

	if (wd->driver->bustype == NDIS_PCI_BUS)
		ansi_string.buf = "PCI Ethernet Adapter";
	else
		ansi_string.buf = "USB Ethernet Adapter";
	ansi_string.buflen = ansi_string.len = strlen(ansi_string.buf);
	if (RtlAnsiStringToUnicodeString(name, &ansi_string, 1))
		TRACEEXIT2(return NDIS_STATUS_RESOURCES);
	else
		TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL ULONG WRAP_EXPORT(NdisReadPcmciaAttributeMemory)
	(struct ndis_miniport_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	UNIMPL();
	return 0;
}

STDCALL ULONG WRAP_EXPORT(NdisWritePcmciaAttributeMemory)
	(struct ndis_miniport_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	UNIMPL();
	return 0;
}

 /* Unimplemented...*/
STDCALL void WRAP_EXPORT(NdisMSetAttributes)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(EthFilterDprIndicateReceiveComplete)
	(void){UNIMPL();}
STDCALL void WRAP_EXPORT(EthFilterDprIndicateReceive)(void){UNIMPL();}
STDCALL void WRAP_EXPORT(NdisMRemoveMiniport)(void) { UNIMPL(); }
//STDCALL void RndisMSendComplete(void) { UNIMPL(); }
//STDCALL void RndisMInitializeWrapper(void) { UNIMPL(); }
STDCALL void WRAP_EXPORT(RndisMIndicateReceive)(void) { UNIMPL(); }

STDCALL void WRAP_EXPORT(NdisMCoActivateVcComplete)(void){UNIMPL();}

STDCALL void WRAP_EXPORT(NdisMCoDeactivateVcComplete)(void)
{
	UNIMPL();
	return;
}

#include "ndis_exports.h"

STDCALL NTSTATUS ndis_add_device(struct driver_object *drv_obj,
				 struct device_object *pdo)
{
	struct device_object *fdo;
	struct ndis_miniport_block *nmb;
	NTSTATUS ret;

	TRACEENTER2("%p, %p", drv_obj, pdo);
	ret = IoCreateDevice(drv_obj, sizeof(*nmb), NULL,
			     FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (ret != STATUS_SUCCESS)
		TRACEEXIT2(return ret);
	nmb = fdo->dev_ext;
	nmb->fdo = fdo;
	nmb->pdo = pdo;
	DBGTRACE2("nmb: %p, fdo: %p", nmb, nmb->fdo);
	nmb->wd = (struct wrapper_dev *)pdo->wd;
	nmb->wd->nmb = nmb;
	nmb->next_device = IoAttachDeviceToDeviceStack(fdo, pdo);
	KeInitializeSpinLock(&nmb->lock);
	nmb->rx_packet = WRAP_FUNC_PTR(NdisMIndicateReceivePacket);
	nmb->send_complete = WRAP_FUNC_PTR(NdisMSendComplete);
	nmb->send_resource_avail =
		WRAP_FUNC_PTR(NdisMSendResourcesAvailable);
	nmb->status = WRAP_FUNC_PTR(NdisMIndicateStatus);
	nmb->status_complete = WRAP_FUNC_PTR(NdisMIndicateStatusComplete);
	nmb->query_complete = WRAP_FUNC_PTR(NdisMQueryInformationComplete);
	nmb->set_complete = WRAP_FUNC_PTR(NdisMSetInformationComplete);
	nmb->reset_complete = WRAP_FUNC_PTR(NdisMResetComplete);
	nmb->eth_rx_indicate = WRAP_FUNC_PTR(EthRxIndicateHandler);
	nmb->eth_rx_complete = WRAP_FUNC_PTR(EthRxComplete);
	nmb->td_complete = WRAP_FUNC_PTR(NdisMTransferDataComplete);
	nmb->wd->driver->miniport.adapter_shutdown = NULL;

	TRACEEXIT2(return STATUS_SUCCESS);
}


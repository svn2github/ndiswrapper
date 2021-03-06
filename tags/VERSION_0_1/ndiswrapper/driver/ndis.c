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
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/types.h>

#include "ndis.h"

extern int image_offset;


int getSp(void)
{
	volatile int i;
	asm("movl %esp,(%esp,1)");
	return i;
}


int unicodeToStr(char *dst, struct ustring *src, int dstlen)
{
	char *buf = src->buf;
	int i = 0;
	while(buf[0] || buf[1]) {
		if(i >= dstlen)
		{
			printk(KERN_ERR "%s failed. Buffer to small\n", __FUNCTION__);
			return -1;	
		}
		dst[i++] = buf[0];
		buf +=2;
	}
	dst[i] = 0;
	return 0;
}


/*
 * 
 *
 * Called from the driver entry.
 */
STDCALL void NdisInitializeWrapper(struct ndis_handle **ndis_handle,
	                           void *SystemSpecific1,
				   void *SystemSpecific2,
				   void *SystemSpecific3)
{
	DBGTRACE("%s handle=%08x, SS1=%08x, SS2=%08x\n", __FUNCTION__, (int)ndis_handle, (int)SystemSpecific1, (int)SystemSpecific2);
	*ndis_handle = (struct ndis_handle*) SystemSpecific1;
}

STDCALL void NdisTerminateWrapper(struct ndis_handle *ndis_handle,
	                          void *SystemSpecific1)
{
	DBGTRACE("%s\n", __FUNCTION__ );
}

/*
 * Register a miniport with NDIS. 
 *
 * Called from driver entry
 */
STDCALL int NdisMRegisterMiniport(struct ndis_driver *ndis_driver,
	                          struct miniport_char *miniport_char,
	                          unsigned int char_len)
{
	DBGTRACE("%s driver: %08x\n", __FUNCTION__, (int)ndis_driver);

	if(miniport_char->majorVersion < 4)
	{
		return NDIS_STATUS_BAD_VERSION;
	}

	if(char_len < sizeof(struct miniport_char))
	{
		return NDIS_STATUS_BAD_CHAR;
	}

	printk(KERN_INFO "Version %d.%d\n", miniport_char->majorVersion, miniport_char->minorVersion);
	DBGTRACE("Len: %08x:%08x\n", char_len, sizeof(struct miniport_char));
	memcpy(&ndis_driver->miniport_char, miniport_char, sizeof(struct miniport_char));

	return NDIS_STATUS_SUCCESS;
}

/*
 * Allocate mem.
 *
 */
STDCALL unsigned int NdisAllocateMemory(void **dest,
	                                unsigned int length,
					unsigned int flags,
					unsigned int highest_addr)
{
	*dest = (void*) kmalloc(length, GFP_KERNEL);
	if(*dest)
		return NDIS_STATUS_SUCCESS;
	return NDIS_STATUS_FAILIURE;
}

/*
 * Allocate mem.
 *
 * Debug version?
 */
STDCALL unsigned int NdisAllocateMemoryWithTag(void **dest,
	                                       unsigned int length,
					       unsigned int tag)
{
	return NdisAllocateMemory(dest, length, 0, 0);
}

/*
 * Free mem.
 */
STDCALL void NdisFreeMemory(void *adr, unsigned int length, unsigned int flags)
{
	kfree(adr);
}


/*
 * Log an error.
 *
 * This function should not be STDCALL because it's a variable args function. 
 */
void NdisWriteErrorLogEntry(struct ndis_handle *handle,
	                    unsigned int error,
			    unsigned int length,
			    unsigned int p1)
{
	printk(KERN_ERR "%s: error: %08x, %d %d\n", __FUNCTION__, (int)error, (int) length, (int)p1);
}


STDCALL void NdisOpenConfiguration(unsigned int *status,
	                           void **confhandle,
				   struct ndis_handle *handle)
{
	DBGTRACE("%s: Handle: %08x\n", __FUNCTION__, (int) handle);
	*confhandle = (void*) handle;
	*status = NDIS_STATUS_SUCCESS;
}

STDCALL void NdisCloseConfiguration(void *confhandle)
{
	DBGTRACE("%s: confhandle: %08x\n", __FUNCTION__, (int) confhandle);
}


struct internal_parameters
{
	char *name;
	struct ndis_setting_val val;
};

struct internal_parameters internal_parameters[] = { 
	{
		.name = "NdisVersion",
		.val = {0, 0x00050000}
	},

	{
		.name = "Environment",
		.val = {0, 1}
	},

	{
		.name = "BusType",
		.val = {0, 5}
	},
	{
		.name = 0,
		.val= {0,0}
	}
};


STDCALL void NdisReadConfiguration(unsigned int *status,
                                   struct ndis_setting_val **dest,
				   struct ndis_handle *handle, struct ustring *key,
				   unsigned int type)
{
	struct ndis_setting *setting;

	char keyname[1024];
	int i;
	unicodeToStr(keyname, key, sizeof(keyname));

	/* Search built in keys */

	for(i = 0; internal_parameters[i].name; i++)
	{
		if(strcmp(keyname, internal_parameters[i].name) == 0)
		{
			DBGTRACE("%s: Builting found value for %s\n", __FUNCTION__, keyname);
			
			*dest = &internal_parameters[i].val;
			*status = NDIS_STATUS_SUCCESS;
			return;
		}
	}

	/* Search parameters from inf-file */
	list_for_each_entry(setting, &handle->driver->settings, list)
	{
		if(strcmp(keyname, setting->name) == 0)
		{
			DBGTRACE("%s: From inf found value for %s: %d\n", __FUNCTION__, keyname, setting->val.data);

			*dest =& setting->val;
			*status = NDIS_STATUS_SUCCESS;
			return;
		}
	}

	
	DBGTRACE(KERN_INFO "%s: Key not found type:%d. key:%s\n", __FUNCTION__, type, keyname);

	*dest = (struct ndis_setting_val*)0;
	*status = NDIS_STATUS_FAILIURE;
}


/*
 * Called by driver from the init callback.
 *
 * The adapter_ctx should be supplied to most other callbacks so we save
 * it in out handle.
 *
 */ 
STDCALL void NdisMSetAttributesEx(struct ndis_handle *handle,
                                  void* adapter_ctx,
				  unsigned int hangchecktime,
				  unsigned int attributes,
				  unsigned int adaptortype)
{
	DBGTRACE("%s, %08x, %08x %d %08x, %d\n", __FUNCTION__, (int)handle, (int)adapter_ctx, hangchecktime, attributes, adaptortype);
	if(attributes & 8)
	{
		pci_set_master(handle->pci_dev);
	}
	
	handle->adapter_ctx = adapter_ctx;
}

/*
 * Read information from the PCI config area
 *
 */  
STDCALL unsigned int NdisReadPciSlotInformation(struct ndis_handle *handle,
                                                unsigned int slot,
						unsigned int offset,
						char *buf,
						unsigned int len)
{
	int i;
	for(i = 0; i < len; i++)
	{
		pci_read_config_byte(handle->pci_dev, offset+i, &buf[i]);
	}
	return len;
}


/*
 * Write information to the PCI config area
 *
 */  
STDCALL unsigned int NdisWritePciSlotInformation(struct ndis_handle *handle,
                                                 unsigned int slot,
						 unsigned int offset,
						 char *buf,
						 unsigned int len)
{
	int i;
	for(i = 0; i < len; i++)
	{
		pci_write_config_byte(handle->pci_dev, offset+i, buf[i]);
	}
	return len;
}


/*
 * Read information about IRQ and other resources
 *
 * - This needs to be more general, and I'm not sure it's correct..
 * - IOPort resources are not handled.
 */
STDCALL void NdisMQueryAdapterResources(unsigned int *status,
                                        struct ndis_handle *handle,
					struct ndis_resource_list *resource_list,
					unsigned int *size)
{
	int i;
	int len = 0;
	struct pci_dev *pci_dev = handle->pci_dev;
	struct ndis_resource_entry *entry;
	DBGTRACE("%s handle: %08x. buf: %08x, len: %d. IRQ:%d\n", __FUNCTION__, (int)handle, (int)resource_list, *size, pci_dev->irq);

	resource_list->version = 1;
	resource_list->revision = 0;

	/* Put all memory and port resources */
	i = 0;
	while(pci_resource_start(pci_dev, i))
	{
		entry = &resource_list->list[len++];
		entry->type = 3;
		entry->share = 0;

		//Param 2 and 3 seems to be swapped...investigate...
		entry->param1 = pci_resource_start(pci_dev, i);		
		entry->param3 =0;
		entry->param2 = pci_resource_len(pci_dev, i);		
		i++;
	}

	/* Put IRQ resource */
	entry = &resource_list->list[len++];
	entry->type = 2;
	entry->share = 0;
	entry->flags = 0;
	entry->param1 = pci_dev->irq; //Level
	entry->param2 = pci_dev->irq; //Vector
	entry->param3 = -1;  //affinity

	
	resource_list->length = len;
	*size = (char*) (&resource_list->list[len]) - (char*)resource_list;
	*status = NDIS_STATUS_SUCCESS;
	{
		/* Now dump for debugging... */
		DBGTRACE("resource list v%d.%d len %d, size=%d\n", resource_list->version, resource_list->revision, resource_list->length, *size);

		for(i = 0; i < len; i++)
		{
			DBGTRACE("Resource: %d: %08x %08x %08x\n", resource_list->list[i].type, resource_list->list[i].param1, resource_list->list[i].param2, resource_list->list[i].param3); 
		}

		
	}
	return;
}


/*
 * Just like ioremap
 */
STDCALL unsigned int NdisMMapIoSpace(void **virt,
                                     struct ndis_handle *handle,
				     unsigned int phys,
				     unsigned int len)
{
	DBGTRACE("%s: %08x, %d\n", __FUNCTION__, (int)phys, len);
	*virt = ioremap(phys, len);
	if(*virt == NULL) {
		printk(KERN_ERR "IORemap failed\n");
		return NDIS_STATUS_FAILIURE;
	}
	
	handle->mem_start = phys;
	handle->mem_end = phys + len -1;
	DBGTRACE("ioremap successful %08x\n", (int)*virt);
	return NDIS_STATUS_SUCCESS;
}

/*
 * Just like iounmap
 */
STDCALL void NdisMUnmapIoSpace(struct ndis_handle *handle,
                               void *virtaddr,
			       unsigned int len)
{
	DBGTRACE("%s: %08x, %d\n", __FUNCTION__, (int)virtaddr, len);
	iounmap(virtaddr);
}


STDCALL void NdisAllocateSpinLock(spinlock_t **lock)
{
	*lock = kmalloc(sizeof(spinlock_t), GFP_KERNEL);
	if(*lock)
		**lock = SPIN_LOCK_UNLOCKED;
}

STDCALL void NdisFreeSpinLock(spinlock_t **lock)
{
	if(*lock)
		kfree(*lock);
	*lock = NULL;
}

STDCALL void NdisAcquireSpinLock(spinlock_t **lock)
{
	spin_lock(*lock);	
}

STDCALL void NdisReleaseSpinLock(spinlock_t **lock)
{
	spin_unlock(*lock);	
}


STDCALL unsigned int NdisMAllocateMapRegisters(struct ndis_handle *handle,
                                               unsigned int dmachan,
					       unsigned char dmasize,
					       unsigned int basemap,
					       unsigned int size)
{
	DBGTRACE("%s: %d %d %d %d\n", __FUNCTION__, dmachan, dmasize, basemap, size);
	return NDIS_STATUS_SUCCESS;
}

STDCALL void NdisMFreeMapRegisters(void *handle)
{
	DBGTRACE("%s: %08x\n", __FUNCTION__, (int)handle);
}


STDCALL void NdisMAllocateSharedMemory(struct ndis_handle *handle,
                                       unsigned int size,
				       char cached,
				       void **virt,
				       struct ndis_phy_address *phys)
{
	dma_addr_t p;

	void *v = pci_alloc_consistent(handle->pci_dev, size, &p);  
	if(!v)
	{
		printk(KERN_ERR "failed to allocate shared mem\n");
	}

	*(char**)virt = v;
	phys->low = (unsigned int)p;
	phys->high = 0;
}

STDCALL void NdisMFreeSharedMemory(struct ndis_handle *handle,
                                   unsigned int size,
				   char cached,
				   void *virt,
				   unsigned int physlow,
				   unsigned int physhigh)
{
	pci_free_consistent(handle->pci_dev, size, virt, physlow);
}


STDCALL void NdisAllocateBufferPool(unsigned int *status,
                                    unsigned int *poolhandle,
				    unsigned int size)
{
	DBGTRACE("%s: size=%d. \n", __FUNCTION__ , size);
	*poolhandle = 0xa000fff8;
	*status = NDIS_STATUS_SUCCESS;
}

STDCALL void NdisFreeBufferPool(void *poolhandle)
{
	DBGTRACE("%s: %08x\n", __FUNCTION__, (int)poolhandle);
}


STDCALL void NdisAllocateBuffer(unsigned int *status,
                                void **buffer,
				void *poolhandle,
				void *virt,
				unsigned int len)
{
	struct ndis_buffer *my_buffer = kmalloc(sizeof(struct ndis_buffer), GFP_KERNEL);
	if(!my_buffer)
	{
		*status = NDIS_STATUS_FAILIURE;
		return;
	}
	
	memset(my_buffer, 0, sizeof(struct ndis_buffer));

	my_buffer->data = virt;
	my_buffer->next = 0;
	my_buffer->len = len;

	*buffer = my_buffer;
	
	*status = NDIS_STATUS_SUCCESS;

}

STDCALL void NdisFreeBuffer(void *buffer)
{
	if(buffer)
	{
		memset(buffer, 0, sizeof(struct ndis_buffer));
		kfree(buffer);
	}
}
STDCALL void NdisAdjustBufferLength(struct ndis_buffer *buf, unsigned int len)
{
	buf->len = len;
}
STDCALL void NdisQueryBuffer(struct ndis_buffer *buf, void **adr, unsigned int *len)
{
	*adr = buf->data;
	*len = buf->len;
}


STDCALL void NdisAllocatePacketPool(unsigned int *status,
                                    unsigned int *poolhandle,
				    unsigned int size,
				    unsigned int rsvlen)
{
	DBGTRACE("%s: size=%d\n", __FUNCTION__, size);
	*poolhandle = 0xa000fff4;
	*status = NDIS_STATUS_SUCCESS;
}

STDCALL void NdisFreePacketPool(void *poolhandle)
{
	DBGTRACE("%s: %08x\n", __FUNCTION__, (int)poolhandle);
}

STDCALL void NdisAllocatePacket(unsigned int *status, struct ndis_packet **packet_out, void *poolhandle)
{
	struct ndis_packet *packet = (struct ndis_packet*) kmalloc(sizeof(struct ndis_packet), GFP_KERNEL);
	if(!packet)
	{
		printk(KERN_ERR "%s failed\n", __FUNCTION__);
		*packet_out = NULL;
		*status = NDIS_STATUS_FAILIURE;
		return;
	}
	memset(packet, 0, sizeof(struct ndis_packet));
	packet->oob_offset = (int)(&packet->timesent1) - (int)packet;

	*packet_out = packet;
	*status = NDIS_STATUS_SUCCESS;	
}

STDCALL void NdisFreePacket(void *packet)
{
	if(packet)
	{
		memset(packet, 0, sizeof(struct ndis_packet));
		kfree(packet);
	}
}



/*
 * Bottom half of the timer function.
 */
void ndis_timer_handler_bh(void *data)
{
	struct ndis_timer *timer = (struct ndis_timer*) data;
	STDCALL void (*func)(void *res1, void *data, void *res3, void *res4) = timer->func;
	func(0, timer->ctx, 0, 0);

	if(timer->repeat)
	{
		timer->timer.expires = jiffies + timer->repeat;
		add_timer(&timer->timer);
	}
}


/*
 * Top half of the timer function.
 */
void ndis_timer_handler(unsigned long data)
{
	struct ndis_timer *timer = (struct ndis_timer*) data;
	schedule_work(&timer->bh);
}


STDCALL void NdisMInitializeTimer(struct ndis_timer **timer_handle,
                                  struct ndis_handle *handle,
				  void *func,
				  void *ctx)
{
	struct ndis_timer *timer;
	DBGTRACE("%s: %08x %08x, %08x, %08x\n", __FUNCTION__ , (int)timer_handle, (int)handle, (int)func, (int)ctx);
	timer = kmalloc(sizeof(struct ndis_timer), GFP_KERNEL);
	if(!timer)
		timer_handle = NULL;

	init_timer(&timer->timer);
	timer->timer.data = (unsigned long) timer;
	timer->timer.function = &ndis_timer_handler;
	timer->func = func;
	timer->ctx = ctx;
	INIT_WORK(&timer->bh, &ndis_timer_handler_bh, timer);
	*timer_handle = timer;
	DBGTRACE("Allocated timer at %08x\n", (int)timer);
}


/*
 * Start a one shot timer.
 */
STDCALL void NdisSetTimer(struct ndis_timer **timer_handle, unsigned int ms)
{
	struct ndis_timer *ndis_timer = *timer_handle;
	ndis_timer->timer.expires = jiffies + (ms * HZ) / 1000;
	ndis_timer->repeat = 0;
	add_timer(&ndis_timer->timer);
}

/*
 * Start a repeated timer.
 */
STDCALL void NdisMSetPeriodicTimer(struct ndis_timer **timer_handle,
                                   unsigned int ms)
{

	struct ndis_timer *ndis_timer = *timer_handle;
	DBGTRACE("%s %08x, %d\n", __FUNCTION__, (int)ndis_timer, ms);
	ndis_timer->timer.expires = jiffies + (ms * HZ) / 1000;
	ndis_timer->repeat = (ms * HZ) / 1000;
	add_timer(&ndis_timer->timer);
}

/*
 * Cancel a pending timer
 */
STDCALL void NdisMCancelTimer(struct ndis_timer **timer_handle, char *canceled)
{
	(*timer_handle)->repeat = 0;
	*canceled = del_timer_sync(&(*timer_handle)->timer);
}


/*
 * The driver asks ndis what mac it should use. If this
 * function returns failiure it will use it's default mac.
 */
STDCALL void NdisReadNetworkAddress(unsigned int *status,
                                    char * adr,
				    unsigned int *len,
				    void *conf_handle)
{
	*len = 0;
	*status = NDIS_STATUS_FAILIURE;
}


STDCALL void NdisMRegisterAdapterShutdownHandler(struct ndis_handle *handle,
                                                 void *ctx,
						 void *func)
{
	DBGTRACE("%s sp:%08x\n", __FUNCTION__ , getSp());
}

STDCALL void NdisMDeregisterAdapterShutdownHandler(struct ndis_handle *handle)
{
	DBGTRACE("%s sp:%08x\n", __FUNCTION__ , getSp());
}


/*
 *  bottom half of the irq handler
 *
 */
void ndis_irq_bh(void *data)
{
	struct ndis_handle *handle = (struct ndis_handle *) data;
	handle->driver->miniport_char.handle_interrupt(handle->adapter_ctx);
}

/*
 *  Top half of the irq handler
 *
 */
irqreturn_t ndis_irq_th(int irq, void *data, struct pt_regs *pt_regs)
{
	int handeled = 0;
	int more_work = 0;

	struct ndis_irq *irqhandle = (struct ndis_irq *) data;
	struct ndis_handle *handle = irqhandle->handle; 

	spin_lock(&irqhandle->spinlock);
	handle->driver->miniport_char.isr(&handeled, &more_work, handle->adapter_ctx);
	spin_unlock(&irqhandle->spinlock);

	if(more_work)
		schedule_work(&handle->irq_bh);
	
	if(handeled)
		return IRQ_HANDLED;
	return IRQ_NONE;
}


/*
 * Register an irq
 *
 */
STDCALL unsigned int NdisMRegisterInterrupt(struct ndis_irq **ndis_irq_ptr,
                                            struct ndis_handle *handle,
					    unsigned int vector,
					    unsigned int level,
					    char req_isr,
					    char shared,
					    unsigned int mode)
{
	struct ndis_irq *ndis_irq; 
	DBGTRACE("%s. %08x, vector:%d, level:%d, req_isr:%d, shared:%d, mode:%d sp:%08x\n", __FUNCTION__, (int)ndis_irq_ptr, vector, level, req_isr, shared, mode, (int)getSp());

	*ndis_irq_ptr = (struct ndis_irq*) kmalloc(sizeof(struct ndis_irq), GFP_KERNEL);
	
	if(!*ndis_irq_ptr)
		return NDIS_STATUS_FAILIURE;

	ndis_irq = *ndis_irq_ptr;
	handle->irq = vector;

	ndis_irq->spinlock = SPIN_LOCK_UNLOCKED;
	ndis_irq->irq = vector;
	ndis_irq->handle = handle;
	if(request_irq(vector, ndis_irq_th, SA_SHIRQ, "ndiswrapper", ndis_irq))
	{
		kfree(ndis_irq);
		return NDIS_STATUS_FAILIURE;
	}
	INIT_WORK(&handle->irq_bh, &ndis_irq_bh, handle);
	return NDIS_STATUS_SUCCESS;
}

/*
 * Deregister an irq
 *
 */
STDCALL void NdisMDeregisterInterrupt(struct ndis_irq **ndis_irq_ptr)
{
	struct ndis_irq *ndis_irq = *ndis_irq_ptr;
	DBGTRACE("%s: %08x %d %08x\n", __FUNCTION__, (int)ndis_irq, ndis_irq->irq, (int)ndis_irq->handle);

	if(ndis_irq)
	{
		free_irq(ndis_irq->irq, ndis_irq);
		kfree(ndis_irq);
	}
}



/*
 * Run func synchorinized with the isr.
 *
 */
typedef unsigned int (*sync_func_t)(void *ctx);
STDCALL char NdisMSynchronizeWithInterrupt(struct ndis_irq *interrupt,
                                           STDCALL sync_func_t func,
					   void *ctx)
{
	unsigned int ret;
	unsigned long flags;
	DBGTRACE("%s: %08x %08x %08x\n", __FUNCTION__, (int) interrupt, (int) func, (int) ctx);
	spin_lock_irqsave(&interrupt->spinlock, flags);
	ret = func(ctx);
	spin_unlock_irqrestore(&interrupt->spinlock, flags);
	DBGTRACE("%s: Past func\n", __FUNCTION__);
	return ret;
}


/*
 * This function is not called in a format way.
 * It's called using a macro that referenced the opaque miniport-handler
 *
 */
STDCALL void NdisIndicateStatus(struct ndis_handle *handle, unsigned int status, void *buf, unsigned int len)
{
	DBGTRACE("%s %08x\n", __FUNCTION__, status);
}

/*
 *
 *
 * Called via function pointer.
 */
STDCALL void NdisIndicateStatusComplete(struct ndis_handle *handle)
{
	DBGTRACE("%s\n", __FUNCTION__);
}

/*
 *
 *
 * Called via function pointer.
 */
STDCALL void NdisMIndicateReceivePacket(struct ndis_handle *handle, struct ndis_packet **packets, unsigned int nr_packets)
{
	struct ndis_buffer *buffer;
	struct ndis_packet *packet;
	struct sk_buff *skb;
	int i;
	for(i = 0; i < nr_packets; i++)
	{
		packet = packets[i];
		buffer = packet->buffer_head;

		skb = dev_alloc_skb (buffer->len);
		if(skb)
		{
			skb->dev = handle->net_dev;
		
			eth_copy_and_sum(skb, buffer->data, buffer->len, 0);
			skb_put(skb, buffer->len);
			skb->protocol = eth_type_trans (skb, handle->net_dev);
			netif_rx(skb);
		}
		handle->driver->miniport_char.return_packet(handle->adapter_ctx,  packet);
	}
}

/*
 *
 *
 * Called via function pointer.
 */
STDCALL void NdisMSendComplete(struct ndis_handle *handle, struct ndis_packet *packet, unsigned int status)
{
	kfree(packet->buffer_head);
	kfree(packet);
}

STDCALL unsigned long NDIS_BUFFER_TO_SPAN_PAGES(void *buffer)
{
	DBGTRACE("%s\n", __FUNCTION__ );
	return 1;
}


/* Unimplemented...*/
STDCALL void NdisInitAnsiString(void *src, void *dst) { printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
STDCALL void NdisOpenConfigurationKeyByName(unsigned int *status, void *handle, void *key, void *subkeyhandle){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
STDCALL void NdisWriteConfiguration(unsigned int *status, void *handle, void *keyword, void *val){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
STDCALL unsigned int NdisAnsiStringToUnicodeString(void *dst, void *src){printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );return 0;}
STDCALL void NdisQueryBufferOffset(void *buffer, unsigned int offset, unsigned int length){printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ ); }
STDCALL void NdisMGetDeviceProperty(void *handle, void **p1, void **p2, void **p3, void**p4, void**p5){printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
STDCALL unsigned long NdisWritePcmciaAttributeMemory(void *handle, unsigned int offset, void *buffer, unsigned int length){printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );return 0;}
STDCALL unsigned long NdisReadPcmciaAttributeMemory(void *handle, unsigned int offset, void *buffer, unsigned int length){printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );return 0;}

void NdisMRegisterIoPortRange(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisInterlockedDecrement(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisGetCurrentSystemTime(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisMDeregisterIoPortRange(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisWaitEvent(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisDprAcquireSpinLock(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisDprReleaseSpinLock(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisInterlockedIncrement(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisSetEvent(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisMInitializeScatterGatherDma(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisSystemProcessorCount(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisInitializeEvent(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisMGetDmaAlignment(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}
void NdisUnicodeStringToAnsiString(void){ printk(KERN_ERR "%s --UNIMPLEMENTED--\n", __FUNCTION__ );}


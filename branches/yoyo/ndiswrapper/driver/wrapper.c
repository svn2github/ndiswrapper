#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
 

#include <asm/uaccess.h>

#include "wrapper.h"
#include "loader.h"
#include "ndis.h"
#include "ndis_funcs.h"

static int pci_vendor = 0x14e4;
static int pci_device = 0x4301;

static struct net_device *thedev;

static int wrapper_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);

extern int image_offset;

static struct file_operations wrapper_fops = {
	.owner          = THIS_MODULE,
	.ioctl		= wrapper_ioctl,
};

static struct miscdevice wrapper_misc = {
	.name   = "ndiswrapper",
	.fops   = &wrapper_fops
};



void call_init(struct ndis_handle *handle)
{
	__u32 res;
	__u32 selected_medium;
	__u32 mediumtypes[] = {0,1,2,3,4,5,6,7,8,9,10,11,12};
	printk("Calling init at %08x rva(%08x). sp:%08x\n", (int)handle->miniport_char.init, (int)handle->miniport_char.init - image_offset, getSp());
	handle->miniport_char.init(&res, &selected_medium, mediumtypes, 13, handle, handle);
	printk("past init sp:%08x %08x\n\n\n", getSp(), res);
}

void call_halt(struct ndis_handle *handle)
{
	printk("Calling halt at %08x rva(%08x). sp:%08x\n", (int)handle->miniport_char.halt, (int)handle->miniport_char.halt - image_offset, getSp());
	handle->miniport_char.halt(handle->adapter_ctx);
	printk("past halt sp:%08x\n\n\n", getSp());
}

void call_query(struct ndis_handle *handle)
{
	__u32 res;
	__u32 written;
	__u32 needed;
	unsigned char mac[10];
	printk("Calling query at %08x rva(%08x). sp:%08x\n", (int)handle->miniport_char.query, (int)handle->miniport_char.query - image_offset, getSp());
	res = handle->miniport_char.query(handle->adapter_ctx, 0x01010102, &mac[0], 10, &written, &needed);
	printk("past query sp:%08x %08x\n\n\n", getSp(), res);
	printk("mac:%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

struct packed essid_req
{
	unsigned int len;
	char essid[32];
};


int set_essid(struct ndis_handle *handle, char *name)
{
	unsigned int res, written, needed;
	struct essid_req req;
		
	if(strlen(name) > 32)
		req.len = 32;
	else
		req.len = strlen(name);

	memset(&req.essid, 0, 32);
	memcpy(&req.essid, name, req.len);


	res = handle->miniport_char.setinfo(handle->adapter_ctx, 0x0d010102, (char*)&req, 36, &written, &needed);
	printk("set essid status %08x, %d, %d\n", res, written, needed);
	return res;
}


void test_query(struct ndis_handle *handle)
{
/*
	__u32 res;
	__u32 written;
	__u32 needed;
	int i;
	unsigned char data[1024] = {0,};
	unsigned int oid;

	
	data[0] = 4;
	data[1] = 0;
	data[2] = 0;
	data[3] = 0;
	data[4] = 0;
	data[5] = 'd';
	data[6] = '1';
	data[7] = '1';
	data[8] = 'b';
	
	res = handle->miniport_char.setinfo(handle->adapter_ctx, 0x0d010102, &data[0], 36, &written, &needed);
	printk("res %08x\n", res);
	
	res = handle->miniport_char.query(handle->adapter_ctx, 0x0d010102, &data[0], 1024, &written, &needed);
	printk("Query res: %08x, written %d %d\n", res, written, needed);
	if(res == 0)
	{
		printk("data:");
		for(i = 0; i < written; i++)
		{
			printk("%02x ", data[i]);
		}	
	
		printk("\n");

		if(written == 4)
		{
			printk("%d, 0x%08x\n", *(int*)data, *(int*)data);
		}
		printk("\n");
//0x0d010102
		res = handle->miniport_char.setinfo(handle->adapter_ctx, 0x0d010118, &data[0], 4, &written, &needed);
		printk("res %08x\n", res);
	}

*/
}



unsigned int call_entry(struct ndis_handle *handle)
{
	int res;
	char regpath[] = {'a', 0, 'b', 0, 0, 0};
	printk("Calling entry at %08x rva(%08x). sp:%08x\n", (int)handle->entry, (int)handle->entry - image_offset, getSp());
	res = handle->entry((void*)handle, regpath);
	printk("Past entry: Version: %d.%d. sp:%08x\n\n\n", handle->miniport_char.majorVersion, handle->miniport_char.minorVersion, getSp());

	/* Dump addresses of driver suppoled callbacks */
	{
		int i;
		int *adr = (int*) &handle->miniport_char.CheckForHangTimer;
		char *name[] = {
				"CheckForHangTimer",
				"DisableInterruptHandler",
				"EnableInterruptHandler",
				"halt",
				"HandleInterruptHandler",
				"init",
				"ISRHandler",
				"query",
				"ReconfigureHandler",
				"ResetHandler",
				"SendHandler",
				"SetInformationHandler",
				"TransferDataHandler",
				"ReturnPacketHandler",
				"SendPacketsHandler",
				"AllocateCompleteHandler",
/*
				"CoCreateVcHandler",
				"CoDeleteVcHandler",	
				"CoActivateVcHandler",
				"CoDeactivateVcHandler",
				"CoSendPacketsHandler",
				"CoRequestHandler"
*/
		};

		
		for(i = 0; i < 16; i++)
		{
			printk("%08x (rva %08x):%s\n", adr[i], adr[i]?adr[i] - image_offset:0, name[i]); 
		}
	}
	return res;
}


static int ndis_open (struct net_device *dev)
{
	printk("%s\n", __FUNCTION__);
	return 0;
}

static int ndis_close (struct net_device *dev)
{
	printk("%s\n", __FUNCTION__);
	return 0;
}
static struct net_device_stats *ndis_get_stats (struct net_device *dev)
{
	struct ndis_handle *handle = dev->priv;
	printk("%s\n", __FUNCTION__);
	return &handle->stats;
}

static int ndis_ioctl (struct net_device *dev, struct ifreq *rq, int cmd)
{
	printk("%s\n", __FUNCTION__);
	return -EOPNOTSUPP;
}


static int ndis_start_xmit (struct sk_buff *skb, struct net_device *dev)
{
	printk("%s\n", __FUNCTION__);
	return 0;
}


static int setup_dev(struct net_device *dev)
{
	struct ndis_handle *handle = dev->priv;

	unsigned int res;
	unsigned int written;
	unsigned int needed;
	int i;
	unsigned char mac[6];

	printk("Calling query at %08x rva(%08x). sp:%08x\n", (int)handle->miniport_char.query, (int)handle->miniport_char.query - image_offset, getSp());
	res = handle->miniport_char.query(handle->adapter_ctx, 0x01010102, &mac[0], 1024, &written, &needed);
	printk("past query sp:%08x %08x\n\n\n", getSp(), res);
	printk("mac:%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if(res)
	{
		printk("Unable to get MAC-addr from driver\n");
		return -1;
	}
	
	
	dev->open = ndis_open;
	dev->hard_start_xmit = ndis_start_xmit;
	dev->stop = ndis_close;
	dev->get_stats = ndis_get_stats;
	dev->do_ioctl = ndis_ioctl;

	
	for(i = 0; i < 6; i++)
	{
		dev->dev_addr[i] = mac[i];
	}
	dev->irq = handle->irq;
	dev->mem_start = handle->mem_start;		
	dev->mem_end = handle->mem_end;		

	return register_netdev(dev);
}





static int load_ndis_driver(int size, char *src)
{
	void *entry;
	struct ndis_handle *handle;
	struct net_device *dev;
	
	printk("Putting driver size %d\n", size);
	dev = alloc_etherdev(sizeof *handle);
	if(!dev)
	{
		printk("Unable to alloc etherdev\n");
		goto out_nomem;
	}
	handle = dev->priv;
	handle->net_dev = dev;

	handle->image = vmalloc(size);
	if(!handle->image)
	{
		printk("Unable to allocate mem for driver\n");
		goto out_vmalloc;
	}
	copy_from_user(handle->image, src, size);

	handle->pci_dev = pci_find_device(pci_vendor, pci_device, handle->pci_dev);
	if(!handle->pci_dev)
	{
		printk("PCI device %04x:%04x not found\n", pci_vendor, pci_device);
		goto out_baddriver;
	}
		
	if(prepare_coffpe_image(&entry, handle->image, size))
	{
		printk("Unable to prepare driver\n");		
		goto out_baddriver;
	}


	printk("size: %08x\n", sizeof(handle->fill1));
	/* Poision this because it may contain function pointers */
	memset(&handle->fill1, 0x11, sizeof(handle->fill1));
	memset(&handle->fill2, 0x11, sizeof(handle->fill2));
	memset(&handle->fill3, 0x11, sizeof(handle->fill3));

	handle->indicate_receive_packet = &NdisMIndicateReceivePacket;
	handle->send_complete = &NdisMSendComplete;
	handle->indicate_status = &NdisIndicateStatus;	
	handle->indicate_status_complete = &NdisIndicateStatusComplete;	
	
	handle->entry = entry;

	
	printk("Image is at %08x\n", (int)handle->image);
	printk("Handle is at: %08x.\n", (int)handle);

	if(call_entry(handle))
	{
		printk("Driver entry return error\n");
		goto out_baddriver;

	}

	call_init(handle);
	//test_query(handle);
	if(setup_dev(dev))
	{
		printk("Unable to set up driver\n");
		goto out_baddriver;
	}
	thedev = dev;
	
	return 0;

out_baddriver:
	vfree(handle->image);
	free_netdev(dev);
	return -EINVAL;	

out_vmalloc:
	vfree(handle->image);
	free_netdev(dev);
	return -ENOMEM;	
	
out_nomem:
	return -ENOMEM;
}


static int wrapper_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	size_t size;

	switch(cmd) {
	case WDIOC_PUTDRIVER:
		size = *(size_t*)arg;
		return load_ndis_driver(size, (char*) (arg+4));
		break;
	case WDIOC_TEST:
		if(thedev)
			set_essid(thedev->priv, "d11b");
		break;
		
	default:
		return -EINVAL;
		break;
	}	

	return 0;
}

void test(void)
{
	struct pci_dev *dev = NULL;
	struct resource *resource;
	int i;
	dev = pci_find_device(pci_vendor, pci_device, dev);
	if(dev)
	{
		printk("IRQ: %d\n", dev->irq);
		for(i = 0; i < DEVICE_COUNT_RESOURCE; i++)
		{
			resource = &dev->resource[i];
//			if(resource->name == NULL)
//				break;
			printk("Resource: %s, %08lx, %08lx, %08lx\n", resource->name, resource->start, resource->end, resource->flags);
		}

		printk("\n");
		for(i = 0; i < DEVICE_COUNT_IRQ; i++)
		{
			resource = &dev->irq_resource[i];
//			if(resource->name == NULL)
//				break;
			printk("IRQ-Resource: %s, %08lx, %08lx, %08lx\n", resource->name, resource->start, resource->end, resource->flags);
		}

		printk("\n");
		for(i = 0; i < DEVICE_COUNT_DMA; i++)
		{
			resource = &dev->dma_resource[i];
//			if(resource->name == NULL)
//				break;
			printk("DMA-Resource: %s, %08lx, %08lx, %08lx\n", resource->name, resource->start, resource->end, resource->flags);
		}
	}
}


static int __init wrapper_init(void)
{
	int err;
        if ( (err = misc_register(&wrapper_misc)) < 0 ) {
                printk(KERN_ERR "misc_register failed\n");
		return err;
        }
	//test();
	
	return 0;
}

static void __exit wrapper_exit(void)
{
	struct ndis_handle *handle;
	if(thedev)
	{
		handle = thedev->priv;
		unregister_netdev(thedev);
		call_halt(handle);

		if(handle->image)
		{
			vfree(handle->image);
		}
		free_netdev(thedev);
	}
	misc_deregister(&wrapper_misc);
}

module_init(wrapper_init);
module_exit(wrapper_exit);

MODULE_LICENSE("GPL");

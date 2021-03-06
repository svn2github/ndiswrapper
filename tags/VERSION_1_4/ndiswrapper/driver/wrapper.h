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

#ifndef WRAPPER_H
#define WRAPPER_H

#include "ndis.h"

NDIS_STATUS miniport_reset(struct wrapper_dev *wd);
NDIS_STATUS miniport_query_info_needed(struct wrapper_dev *wd,
				       ndis_oid oid, void *buf,
				       ULONG bufsize, ULONG *needed);
NDIS_STATUS miniport_query_info(struct wrapper_dev *wd, ndis_oid oid,
				void *buf, ULONG bufsize);
NDIS_STATUS miniport_set_info(struct wrapper_dev *wd, ndis_oid oid,
			      void *buf, ULONG bufsize);
NDIS_STATUS miniport_query_int(struct wrapper_dev *wd, ndis_oid oid,
			       void *data);
NDIS_STATUS miniport_set_int(struct wrapper_dev *wd, ndis_oid oid,
			     ULONG data);
NDIS_STATUS miniport_init(struct wrapper_dev *wd);
NDIS_STATUS miniport_surprise_remove(struct wrapper_dev *wd);
NDIS_STATUS miniport_set_pm_state(struct wrapper_dev *wd,
				     enum ndis_pm_state);
void miniport_halt(struct wrapper_dev *wd);
void hangcheck_add(struct wrapper_dev *wd);
void hangcheck_del(struct wrapper_dev *wd);
void sendpacket_done(struct wrapper_dev *wd, struct ndis_packet *packet);
int ndiswrapper_suspend_pci(struct pci_dev *pdev, pm_message_t state);
int ndiswrapper_resume_pci(struct pci_dev *pdev);

int ndiswrapper_suspend_usb(struct usb_interface *intf, pm_message_t state);
int ndiswrapper_resume_usb(struct usb_interface *intf);

NTSTATUS ndiswrapper_start_device(struct wrapper_dev *wd);
void ndiswrapper_stop_device(struct wrapper_dev *wd);
void ndiswrapper_remove_device(struct wrapper_dev *wd);
int ndis_reinit(struct wrapper_dev *wd);
int setup_device(struct net_device *dev);

void check_capa(struct wrapper_dev *wd);

struct net_device *ndis_init_netdev(struct wrapper_dev **pwd,
				    struct ndis_device *device,
				    struct ndis_driver *driver);

struct iw_statistics *get_wireless_stats(struct net_device *dev);
#endif /* WRAPPER_H */

/*
 *  Copyright (C) 2006-2007 Giridhar Pemmasani
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

#ifndef _IW_NDIS_H_
#define _IW_NDIS_H_

#include "ndis.h"

#define	WL_NOISE	-96	/* typical noise level in dBm */
#define	WL_SIGMAX	-32	/* typical maximum signal level in dBm */

NDIS_STATUS set_infra_mode(struct ndis_device *wnd,
			   enum ndis_dot11_bss_type mode);
NDIS_STATUS set_auth_algo(struct ndis_device *wnd,
			  enum ndis_dot11_auth_algorithm algo_id);
NDIS_STATUS set_cipher_algo(struct ndis_device *wnd,
			    enum ndis_dot11_cipher_algorithm algo_id);
extern const struct iw_handler_def ndis_handler_def;

#define PRIV_RESET			SIOCIWFIRSTPRIV+16
#define PRIV_POWER_PROFILE		SIOCIWFIRSTPRIV+17
#define PRIV_SET_PHY_ID			SIOCIWFIRSTPRIV+18
#define PRIV_SET_NIC_POWER		SIOCIWFIRSTPRIV+19
#define PRIV_CONNECT			SIOCIWFIRSTPRIV+20
#define PRIV_SCAN			SIOCIWFIRSTPRIV+21

/* these have to match what is in wpa_supplicant */

enum wpa_alg { WPA_ALG_NONE, WPA_ALG_WEP, WPA_ALG_TKIP, WPA_ALG_CCMP };
enum wpa_cipher { CIPHER_NONE, CIPHER_WEP40, CIPHER_TKIP, CIPHER_CCMP,
		  CIPHER_WEP104 };
enum wpa_key_mgmt { KEY_MGMT_802_1X, KEY_MGMT_PSK, KEY_MGMT_NONE,
		    KEY_MGMT_802_1X_NO_WPA, KEY_MGMT_WPA_NONE };

#endif // IW_NDIS_H

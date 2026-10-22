// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2009 - 2014 Espressif System. */
#include "linux/types.h"
#include "linux/kernel.h"
#include <linux/ieee80211.h>
#include <net/mac80211.h>
#include <linux/skbuff.h>

#include <net/tcp.h>
#include <linux/ip.h>
#include <asm/checksum.h>

#include "esp_pub.h"
#include "esp_utils.h"
#include "esp_wmac.h"
#include "esp_debug.h"

/*
 * Convert IEEE channel number to MHz frequency.
 */
u32 esp_ieee2mhz(u8 chan)
{
	if (chan == 14)
		return 2484;

	if (chan < 14)
		return 2407 + chan * 5;
	else
		return 2512 + ((chan - 15) * 20);
}

enum {
	ESP_RATE_1_LONG = 0x0,
	ESP_RATE_2_LONG = 0x1,
	ESP_RATE_2_SHORT = 0x5,
	ESP_RATE_5_SHORT = 0x6,
	ESP_RATE_5_LONG = 0x2,
	ESP_RATE_11_SHORT = 0x7,
	ESP_RATE_11_LONG = 0x3,
	ESP_RATE_6 = 0xb,
	ESP_RATE_9 = 0xf,
	ESP_RATE_12 = 0xa,
	ESP_RATE_18 = 0xe,
	ESP_RATE_24 = 0x9,
	ESP_RATE_36 = 0xd,
	ESP_RATE_48 = 0x8,
	ESP_RATE_54 = 0xc,
};

static const u8 esp_rate_table[] = {
	ESP_RATE_1_LONG, ESP_RATE_2_SHORT, ESP_RATE_5_SHORT, ESP_RATE_11_SHORT,
	ESP_RATE_6,	 ESP_RATE_9,	   ESP_RATE_12,	     ESP_RATE_18,
	ESP_RATE_24,	 ESP_RATE_36,	   ESP_RATE_48,	     ESP_RATE_54,
};

s8 esp_wmac_rate2idx(u8 rate)
{
	int i;

	if (rate == ESP_RATE_2_LONG)
		return 1;
	if (rate == ESP_RATE_5_LONG)
		return 2;
	if (rate == ESP_RATE_11_LONG)
		return 3;

	for (i = 0; i < ARRAY_SIZE(esp_rate_table); i++) {
		if (rate == esp_rate_table[i])
			return i;
	}

	esp_dbg(ESP_DBG_ERROR, "%s unknown rate 0x%02x\n", __func__, rate);

	return 0;
}

bool esp_wmac_rxsec_error(u8 error)
{
	return (error >= RX_SECOV_ERR && error <= RX_SECFIFO_TIMEOUT) ||
	       (error >= RX_WEPICV_ERR && error <= RX_WAPIMIC_ERR);
}

int esp_cipher2alg(int cipher)
{
	if (cipher == WLAN_CIPHER_SUITE_TKIP)
		return ALG_TKIP;

	if (cipher == WLAN_CIPHER_SUITE_CCMP)
		return ALG_CCMP;

	if (cipher == WLAN_CIPHER_SUITE_WEP40 ||
	    cipher == WLAN_CIPHER_SUITE_WEP104)
		return ALG_WEP;

	if (cipher == WLAN_CIPHER_SUITE_AES_CMAC)
		return ALG_AES_CMAC;

	return -1;
}

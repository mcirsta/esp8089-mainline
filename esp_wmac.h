/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2011-2012 Espressif System. */
#ifndef _ESP_WMAC_H_
#define _ESP_WMAC_H_

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/unaligned.h>

#define ESP_RX_CTRL_LEN 12

/* Host representation of the firmware's three little-endian RX words. */
struct esp_mac_rx_ctrl {
	s8 rssi;
	s8 noise_floor;
	u8 rate;
	u8 sig_mode;
	u8 mcs;
	u8 stbc;
	u8 sgi;
	u8 rxend_state;
	u8 ampdu_cnt;
	u8 channel;
	bool aggregation;
	u16 legacy_length;
	u16 ht_length;
};

static inline void esp_rx_ctrl_decode(const void *data,
				      struct esp_mac_rx_ctrl *ctrl)
{
	const u8 *bytes = data;
	u32 word0 = get_unaligned_le32(bytes);
	u32 word1 = get_unaligned_le32(bytes + 4);
	u32 word2 = get_unaligned_le32(bytes + 8);

	ctrl->rssi = word0;
	ctrl->rate = (word0 >> 8) & 0xf;
	ctrl->sig_mode = (word0 >> 14) & 3;
	ctrl->legacy_length = (word0 >> 16) & 0xfff;
	ctrl->mcs = word1 & 0x7f;
	ctrl->ht_length = (word1 >> 8) & 0xffff;
	ctrl->aggregation = !!(word1 & BIT(27));
	ctrl->stbc = (word1 >> 28) & 3;
	ctrl->sgi = !!(word1 & BIT(31));
	ctrl->rxend_state = word2;
	ctrl->ampdu_cnt = word2 >> 8;
	ctrl->channel = (word2 >> 16) & 0xf;
	ctrl->noise_floor = word2 >> 24;
}

/* The firmware fills the descriptor after receiving a TX MPDU. */
struct esp_tx_ampdu_entry {
	__le32 data;
};

#define RX_PYH_ERR_MIN 0x42
#define RX_AGC_ERR_MIN 0x42
#define RX_AGC_ERR_MAX 0x47
#define RX_OFDM_ERR_MIN 0x50
#define RX_OFDM_ERR_MAX 0x58
#define RX_CCK_ERR_MIN 0x59
#define RX_CCK_ERR_MAX 0x5F
#define RX_ABORT 0x80
#define RX_SF_ERR 0x40
#define RX_FCS_ERR 0x41
#define RX_AHBOV_ERR 0xC0
#define RX_BUFOV_ERR 0xC1
#define RX_BUFINV_ERR 0xC2
#define RX_AMPDUSF_ERR 0xC3
#define RX_AMPDUBUFOV_ERR 0xC4
#define RX_MACBBFIFOOV_ERR 0xC5
#define RX_RPBM_ERR 0xC6
#define RX_BTFORCE_ERR 0xC7
#define RX_SECOV_ERR 0xE1
#define RX_SECPROT_ERR0 0xE2
#define RX_SECPROT_ERR1 0xE3
#define RX_SECKEY_ERR 0xE4
#define RX_SECCRLEN_ERR 0xE5
#define RX_SECFIFO_TIMEOUT 0xE6
#define RX_WEPICV_ERR 0xF0
#define RX_TKIPICV_ERR 0xF4
#define RX_TKIPMIC_ERR 0xF5
#define RX_CCMPMIC_ERR 0xF8
#define RX_WAPIMIC_ERR 0xFC

s8 esp_wmac_rate2idx(u8 rate);
bool esp_wmac_rxsec_error(u8 error);

#endif /* _ESP_WMAC_H_ */

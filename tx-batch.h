/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP_TX_BATCH_H
#define ESP_TX_BATCH_H

#include <linux/overflow.h>
#include "esp_wmac.h"
#include "sip2_common.h"

static inline bool esp_tx_data_fits(u32 payload, bool ampdu, u16 block,
				    int credits, unsigned int reserved)
{
	u32 bytes, blocks;
	u32 header = ALIGN(sizeof(struct sip_hdr), 4);

	if (ampdu)
		header += ALIGN(sizeof(struct esp_tx_ampdu_entry), 4);
	if (!payload || !block || credits < 0 || (unsigned int)credits < reserved ||
	    check_add_overflow(payload, header, &bytes))
		return false;
	blocks = bytes / block + !!(bytes % block);
	return blocks <= credits - reserved;
}

#endif

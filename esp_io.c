// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2009 - 2014 Espressif System. */
#include <linux/mmc/sdio_func.h>
#include <linux/unaligned.h>
#include "esp_sif.h"
#include "esp_sip.h"
#include "slc_host_register.h"
#include "esp_debug.h"

int esp_common_read(struct esp_pub *epub, u8 *buf, u32 len, int sync,
		    bool noround)
{
	if (sync)
		return sif_lldesc_read_sync(epub, buf, len);
	else
		return sif_lldesc_read_raw(epub, buf, len, noround);
}

int esp_common_write(struct esp_pub *epub, u8 *buf, u32 len, int sync)
{
	if (sync)
		return sif_lldesc_write_sync(epub, buf, len);
	else
		return sif_lldesc_write_raw(epub, buf, len);
}

int esp_common_read_with_addr(struct esp_pub *epub, u32 addr, u8 *buf, u32 len,
			      int sync)
{
	if (sync) {
		return sif_io_sync(epub, addr, buf, len,
				   SIF_FROM_DEVICE | SIF_SYNC | SIF_BYTE_BASIS |
					   SIF_INC_ADDR);
	} else {
		return sif_io_raw(epub, addr, buf, len,
				  SIF_FROM_DEVICE | SIF_BYTE_BASIS |
					  SIF_INC_ADDR);
	}
}

int esp_common_write_with_addr(struct esp_pub *epub, u32 addr, u8 *buf, u32 len,
			       int sync)
{
	if (sync) {
		return sif_io_sync(epub, addr, buf, len,
				   SIF_TO_DEVICE | SIF_SYNC | SIF_BYTE_BASIS |
					   SIF_INC_ADDR);
	} else {
		return sif_io_raw(epub, addr, buf, len,
				  SIF_TO_DEVICE | SIF_BYTE_BASIS |
					  SIF_INC_ADDR);
	}
}

int esp_common_readbyte_with_addr(struct esp_pub *epub, u32 addr, u8 *buf,
				  int sync)
{
	int res;

	if (sync)
		sif_lock_bus(epub);
	*buf = sdio_io_readb(epub, addr, &res);
	if (sync)
		sif_unlock_bus(epub);
	return res;
}

int esp_common_writebyte_with_addr(struct esp_pub *epub, u32 addr, u8 buf,
				   int sync)
{
	int res;

	if (sync)
		sif_lock_bus(epub);
	sdio_io_writeb(epub, buf, addr, &res);
	if (sync)
		sif_unlock_bus(epub);
	return res;
}

int sif_read_reg_window(struct esp_pub *epub, unsigned int reg_addr, u8 *value)
{
	u8 *p_tbuf = NULL;
	int ret = 0;
	int retry = 20;

	reg_addr >>= 2;
	if (reg_addr > 0x1f)
		return -1;

	p_tbuf = kzalloc(4, GFP_KERNEL);
	if (!p_tbuf)
		return -ENOMEM;

	p_tbuf[0] = 0x80 | (reg_addr & 0x1f);

	ret = esp_common_write_with_addr(epub, SLC_HOST_WIN_CMD, p_tbuf, 1,
					 ESP_SIF_NOSYNC);

	if (ret == 0) {
		do {
			if (retry < 20)
				mdelay(10);
			retry--;
			ret = esp_common_read_with_addr(epub, SLC_HOST_STATE_W0,
							p_tbuf, 4,
							ESP_SIF_NOSYNC);
		} while (retry > 0 && ret != 0);
	}

	if (ret == 0)
		memcpy(value, p_tbuf, 4);

	kfree(p_tbuf);
	return ret;
}

int sif_write_reg_window(struct esp_pub *epub, unsigned int reg_addr, u8 *value)
{
	u8 *p_tbuf = NULL;
	int ret = 0;

	reg_addr >>= 2;
	if (reg_addr > 0x1f)
		return -1;

	p_tbuf = kzalloc(8, GFP_KERNEL);
	if (!p_tbuf)
		return -ENOMEM;
	memcpy(p_tbuf, value, 4);
	p_tbuf[4] = 0xc0 | (reg_addr & 0x1f);

	ret = esp_common_write_with_addr(epub, SLC_HOST_CONF_W5, p_tbuf, 5,
					 ESP_SIF_NOSYNC);

	kfree(p_tbuf);
	return ret;
}

int sif_ack_target_read_err(struct esp_pub *epub)
{
	u32 value[1];
	int ret;

	ret = sif_read_reg_window(epub, SLC_RX_LINK, (u8 *)value);
	if (ret)
		return ret;
	value[0] |= SLC_RXLINK_START;
	ret = sif_write_reg_window(epub, SLC_RX_LINK, (u8 *)value);
	return ret;
}

int sif_had_io_enable(struct esp_pub *epub)
{
	u32 *p_tbuf = NULL;
	int ret;

	p_tbuf = kzalloc_obj(u32, GFP_KERNEL);
	if (!p_tbuf)
		return -ENOMEM;

	*p_tbuf = SLC_TXEOF_ENA | (0x4 << SLC_FIFO_MAP_ENA_S) |
		  SLC_TX_DUMMY_MODE | SLC_HDA_MAP_128K |
		  (0xFE << SLC_TX_PUSH_IDLE_NUM_S);
	ret = sif_write_reg_window(epub, SLC_BRIDGE_CONF, (u8 *)p_tbuf);

	if (ret)
		goto _err;

	*p_tbuf = 0x30;
	ret = esp_common_write_with_addr((epub), SLC_HOST_CONF_W4 + 1,
					 (u8 *)p_tbuf, 1, ESP_SIF_NOSYNC);

	if (ret)
		goto _err;
	*p_tbuf = 0x1;
	ret = esp_common_write_with_addr((epub), SLC_HOST_CONF_W3, (u8 *)p_tbuf,
					 1, ESP_SIF_NOSYNC);

_err:
	kfree(p_tbuf);
	return ret;
}

int sif_interrupt_target(struct esp_pub *epub, u8 index)
{
	u8 low_byte = BIT(index);

	return esp_common_writebyte_with_addr(epub, SLC_HOST_CONF_W4 + 2,
					      low_byte, ESP_SIF_NOSYNC);
}

int check_target_id(struct esp_pub *epub)
{
	struct esp_sdio_ctrl *ctrl = EPUB_TO_CTRL(epub);
	u8 id[4], conf[4] = {};
	int ret;

	sif_lock_bus(epub);
	ret = esp_common_read_with_addr(epub, SLC_HOST_ID, id, sizeof(id),
					ESP_SIF_NOSYNC);
	if (ret)
		goto out;
	ctrl->target_id = get_unaligned_le32(id);
	switch (ctrl->target_id) {
	case 0x100:
		ctrl->slc_window_end_addr = 0x20000;
		break;
	case 0x600:
		ctrl->slc_window_end_addr = 0x1f800;
		ret = esp_common_write_with_addr(epub, SLC_HOST_CONF_W1, conf,
						 sizeof(conf), ESP_SIF_NOSYNC);
		break;
	default:
		ret = -ENODEV;
	}
out:
	sif_unlock_bus(epub);
	return ret;
}

u32 sif_get_blksz(struct esp_pub *epub)
{
	return EPUB_TO_CTRL(epub)->slc_blk_sz;
}

u32 sif_get_target_id(struct esp_pub *epub)
{
	return EPUB_TO_CTRL(epub)->target_id;
}

/* The MMC core calls this threaded handler with the SDIO host claimed. */
void sif_halt_transport(struct esp_pub *epub, int error)
{
	__le32 mask = 0;

	atomic_set(&epub->wl.off, 1);
	sip_stop(epub->sip);
	if (test_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags))
		ieee80211_stop_queues(epub->hw);
	/* Called with the host claimed; mask a pending level IRQ before exit. */
	esp_common_write_with_addr(epub, SLC_HOST_INT_ENA, (u8 *)&mask,
				   sizeof(mask), ESP_SIF_NOSYNC);
	dev_err_ratelimited(epub->dev, "Transport halted (%d); requesting recovery\n",
			    error);
	esp_recovery_request(epub, error);
}

void sif_dsr(struct sdio_func *func)
{
	struct esp_sdio_ctrl *ctrl = sdio_get_drvdata(func);
	struct slc_host_regs *regs = &ctrl->slc_regs;
	int ret;

	atomic64_inc(&ctrl->epub->stats[ESP_RX_IRQ]);
	ret = esp_common_read_with_addr(ctrl->epub, REG_SLC_HOST_BASE + 8,
					(u8 *)regs, sizeof(*regs),
					ESP_SIF_NOSYNC);
	if (ret) {
		dev_err_ratelimited(&func->dev,
				    "Interrupt register read failed: %d\n",
				    ret);
		sif_halt_transport(ctrl->epub, ret);
		return;
	}
	if (le32_to_cpu(regs->intr_raw) & SLC_HOST_RX_ST)
		esp_dsr(ctrl->epub);
}

struct slc_host_regs *sif_get_regs(struct esp_pub *epub)
{
	return &EPUB_TO_CTRL(epub)->slc_regs;
}

void sif_disable_target_interrupt(struct esp_pub *epub)
{
	sif_lock_bus(epub);
	memset(EPUB_TO_CTRL(epub)->dma_buffer, 0x00, sizeof(u32));
	esp_common_write_with_addr(epub, SLC_HOST_INT_ENA,
				   EPUB_TO_CTRL(epub)->dma_buffer, sizeof(u32),
				   ESP_SIF_NOSYNC);

	sif_unlock_bus(epub);

	mdelay(1);

	sif_lock_bus(epub);
	sif_interrupt_target(epub, 7);
	sif_unlock_bus(epub);
}

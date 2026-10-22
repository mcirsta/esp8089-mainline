// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2010 -2013 Espressif System. */
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <net/mac80211.h>
#include <linux/time.h>
#include <linux/pm.h>
#include <linux/sizes.h>

#include "esp_pub.h"
#include "esp_sif.h"
#include "esp_sip.h"
#include "esp_debug.h"
#include "slc_host_register.h"
#include "esp_ctrl.h"
#include "esp_file.h"

#define ESP_DMA_IBUFSZ SZ_64K

static int esdio_power_off(struct esp_sdio_ctrl *sctrl);
static int esdio_power_on(struct esp_sdio_ctrl *sctrl);

void sif_lock_bus(struct esp_pub *epub)
{
	sdio_claim_host(EPUB_TO_FUNC(epub));
}

void sif_unlock_bus(struct esp_pub *epub)
{
	sdio_release_host(EPUB_TO_FUNC(epub));
}

u8 sdio_io_readb(struct esp_pub *epub, int addr, int *res)
{
	struct esp_sdio_ctrl *sctrl = NULL;
	struct sdio_func *func = NULL;

	sctrl = (struct esp_sdio_ctrl *)epub->sif;
	func = sctrl->func;

	if (func->num == 0)
		return sdio_f0_readb(func, addr, res);
	else
		return sdio_readb(func, addr, res);
}

void sdio_io_writeb(struct esp_pub *epub, u8 value, int addr, int *res)
{
	struct esp_sdio_ctrl *sctrl = NULL;
	struct sdio_func *func = NULL;

	sctrl = (struct esp_sdio_ctrl *)epub->sif;
	func = sctrl->func;

	if (func->num == 0)
		sdio_f0_writeb(func, value, addr, res);
	else
		sdio_writeb(func, value, addr, res);
}

/* The SDIO host claim serializes the DMA-safe bounce buffer. */
static int esp_sdio_transfer(struct esp_pub *epub, u32 addr, u8 *buf, u32 len,
			     u32 wire_len, u32 flags, bool claim)
{
	struct esp_sdio_ctrl *ctrl = EPUB_TO_CTRL(epub);
	struct sdio_func *func = ctrl->func;
	bool write = flags & SIF_TO_DEVICE;
	int ret;

	if (!buf || !len || len > wire_len || wire_len > ESP_DMA_IBUFSZ)
		return -EINVAL;
	if (claim)
		sdio_claim_host(func);
	if (write) {
		esp_sdio_prepare_tx(ctrl->dma_buffer, ESP_DMA_IBUFSZ, buf, len,
				    wire_len);
		if (flags & SIF_FIXED_ADDR)
			ret = sdio_writesb(func, addr, ctrl->dma_buffer,
					   wire_len);
		else
			ret = sdio_memcpy_toio(func, addr, ctrl->dma_buffer,
					       wire_len);
	} else {
		if (flags & SIF_FIXED_ADDR)
			ret = sdio_readsb(func, ctrl->dma_buffer, addr,
					  wire_len);
		else
			ret = sdio_memcpy_fromio(func, ctrl->dma_buffer, addr,
						 wire_len);
		if (!ret && buf != ctrl->dma_buffer)
			memcpy(buf, ctrl->dma_buffer, len);
	}
	if (claim)
		sdio_release_host(func);
	return ret;
}

int sif_io_raw(struct esp_pub *epub, u32 addr, u8 *buf, u32 len, u32 flags)
{
	return esp_sdio_transfer(epub, addr, buf, len, len, flags, false);
}

int sif_io_sync(struct esp_pub *epub, u32 addr, u8 *buf, u32 len, u32 flags)
{
	return esp_sdio_transfer(epub, addr, buf, len, len, flags, true);
}

/* The mailbox address uses the logical length; the SDIO bus may pad it.
 * Padding belongs to the DMA buffer, never to the caller's allocation.
 */
static int esp_sdio_mailbox(struct esp_pub *epub, u8 *buf, u32 len, bool write,
			    bool claim, bool noround)
{
	struct esp_sdio_ctrl *ctrl = EPUB_TO_CTRL(epub);
	u32 wire_len = len;
	u32 addr;

	if (ctrl->target_id == 0x600 && !noround) {
		if (len > ESP_DMA_IBUFSZ)
			return -EINVAL;
		wire_len = roundup(len, ctrl->slc_blk_sz);
	}
	if (!len || len > ctrl->slc_window_end_addr - 2)
		return -EINVAL;
	addr = ctrl->slc_window_end_addr - len - (write ? 0 : 2);
	return esp_sdio_transfer(epub, addr, buf, len, wire_len,
				 (write ? SIF_TO_DEVICE : SIF_FROM_DEVICE) |
					 SIF_BYTE_BASIS | SIF_INC_ADDR,
				 claim);
}

int sif_lldesc_read_sync(struct esp_pub *epub, u8 *buf, u32 len)
{
	return esp_sdio_mailbox(epub, buf, len, false, true, false);
}

int sif_lldesc_write_sync(struct esp_pub *epub, u8 *buf, u32 len)
{
	return esp_sdio_mailbox(epub, buf, len, true, true, false);
}

int sif_lldesc_read_raw(struct esp_pub *epub, u8 *buf, u32 len, bool noround)
{
	return esp_sdio_mailbox(epub, buf, len, false, false, noround);
}

int sif_lldesc_write_raw(struct esp_pub *epub, u8 *buf, u32 len)
{
	return esp_sdio_mailbox(epub, buf, len, true, false, false);
}

static int esdio_power_on(struct esp_sdio_ctrl *sctrl)
{
	int err = 0;

	if (!sctrl->off)
		return err;

	sdio_claim_host(sctrl->func);
	err = sdio_enable_func(sctrl->func);

	if (err) {
		esp_dbg(ESP_DBG_ERROR, "Unable to enable sdio func: %d\n", err);
		sdio_release_host(sctrl->func);
		return err;
	}

	sdio_release_host(sctrl->func);

	/* ensure device is up */
	usleep_range(5000, 6000);

	sctrl->off = false;

	return err;
}

static int esdio_power_off(struct esp_sdio_ctrl *sctrl)
{
	int err;

	if (sctrl->off)
		return 0;

	sdio_claim_host(sctrl->func);
	err = sdio_disable_func(sctrl->func);
	sdio_release_host(sctrl->func);

	if (err)
		return err;

	sctrl->off = true;

	return err;
}

int sif_enable_irq(struct esp_pub *epub)
{
	int err;
	struct esp_sdio_ctrl *sctrl = NULL;

	sctrl = (struct esp_sdio_ctrl *)epub->sif;

	sdio_claim_host(sctrl->func);

	err = sdio_claim_irq(sctrl->func, sif_dsr);

	if (err) {
		sdio_release_host(sctrl->func);
		return err;
	}

	if (!sip_set_state(epub->sip, SIP_BOOT)) {
		sdio_release_irq(sctrl->func);
		sdio_release_host(sctrl->func);
		return -ESHUTDOWN;
	}

	atomic_set(&sctrl->irq_installed, 1);

	sdio_release_host(sctrl->func);
	return 0;
}

void sif_disable_irq(struct esp_pub *epub)
{
	struct esp_sdio_ctrl *ctrl = EPUB_TO_CTRL(epub);

	if (!atomic_read(&ctrl->irq_installed))
		return;
	/* The IRQ handler retains the host claim until it returns. */
	sdio_claim_host(ctrl->func);
	sdio_release_irq(ctrl->func);
	atomic_set(&ctrl->irq_installed, 0);
	sdio_release_host(ctrl->func);
}

static void esp_sdio_remove(struct sdio_func *func)
{
	struct esp_sdio_ctrl *ctrl = sdio_get_drvdata(func);
	struct esp_pub *epub;

	if (!ctrl)
		return;
	epub = ctrl->epub;
	esp_recovery_stop(epub);
	if (epub->sip) {
		sip_detach(epub->sip);
		epub->sip = NULL;
	}
	esdio_power_off(ctrl);
	sdio_set_drvdata(func, NULL);
	esp_pub_dealloc_mac80211(epub);
}

static int esp_sdio_prepare(struct esp_sdio_ctrl *ctrl)
{
	int ret = esdio_power_on(ctrl);

	if (ret)
		return ret;
	sdio_claim_host(ctrl->func);
	ret = sdio_set_block_size(ctrl->func, ctrl->slc_blk_sz);
	sdio_release_host(ctrl->func);
	return ret;
}

int esp_sdio_boot(struct esp_pub *epub)
{
	struct esp_sdio_ctrl *ctrl = EPUB_TO_CTRL(epub);
	struct sdio_func *func = ctrl->func;
	int ret;

	ctrl->off = true;
	epub->sdio_state = ESP_SDIO_STATE_FIRST_INIT;
	sdio_claim_host(func);
	ret = mmc_hw_reset(func->card);
	sdio_release_host(func);
	if (ret)
		return ret;
	ret = esp_init_config(epub);
	if (ret)
		return ret;
	ret = esp_sdio_prepare(ctrl);
	if (ret)
		return ret;
	ret = check_target_id(epub);
	if (ret)
		return ret;
	ret = esp_pub_init_all(epub);
	if (ret)
		return ret;

	/* Stage one restarts the SDIO peripheral at its initial bus settings.
	 * Re-negotiate through the MMC core without cycling the card's RAM
	 * supply or modifying the host's capabilities or non-removable policy.
	 */
	sif_disable_irq(epub);
	flush_workqueue(epub->esp_wkq);
	msleep(100);
	sdio_claim_host(func);
	ret = mmc_sw_reset(func->card);
	sdio_release_host(func);
	if (ret)
		return ret;
	ctrl->off = true;
	ret = esp_sdio_prepare(ctrl);
	if (ret)
		return ret;
	epub->sdio_state = ESP_SDIO_STATE_SECOND_INIT;
	ret = esp_pub_init_all(epub);
	if (ret)
		return ret;
	return 0;
}

static int esp_sdio_probe(struct sdio_func *func,
			  const struct sdio_device_id *id)
{
	struct device *dev = &func->dev;
	struct esp_sdio_ctrl *ctrl;
	struct esp_pub *epub;
	int ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dma_buffer = devm_kmalloc(dev, ESP_DMA_IBUFSZ, GFP_KERNEL);
	if (!ctrl->dma_buffer)
		return -ENOMEM;
	epub = esp_pub_alloc_mac80211(dev);
	if (IS_ERR(epub))
		return PTR_ERR(epub);
	ctrl->epub = epub;
	ctrl->func = func;
	ctrl->id = id;
	ctrl->slc_blk_sz = SIF_SLC_BLOCK_SIZE;
	epub->sif = ctrl;
	sdio_set_drvdata(func, ctrl);
	func->enable_timeout = 1000;
	ret = esp_sdio_boot(epub);
	if (ret)
		goto error;
	dev_info(dev, "ESP8089 initialized through two firmware stages\n");
	return 0;
error:
	dev_err(dev, "initialization failed at stage %u: %d\n",
		epub->sdio_state, ret);
	esp_sdio_remove(func);
	return ret;
}

static const struct sdio_device_id esp_sdio_ids[] = { { SDIO_DEVICE(0x6666,
								    0x1111) },
						      {} };
MODULE_DEVICE_TABLE(sdio, esp_sdio_ids);

static struct sdio_driver esp_sdio_driver = {
	.name = "esp8089",
	.id_table = esp_sdio_ids,
	.probe = esp_sdio_probe,
	.remove = esp_sdio_remove,
};

module_sdio_driver(esp_sdio_driver);
MODULE_AUTHOR("Espressif Systems");
MODULE_DESCRIPTION("Espressif ESP8089 SDIO wireless LAN driver");
MODULE_LICENSE("GPL");

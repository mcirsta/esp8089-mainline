// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2010 - 2014 Espressif System. */
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/firmware.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <linux/time.h>
#include <linux/moduleparam.h>

#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_sif.h"
#include "esp_debug.h"
#include "esp_file.h"
#include "esp_wl.h"
#include "fw.h"

static int esp_download_fw(struct esp_pub *epub);

int esp_pub_init_all(struct esp_pub *epub)
{
	int ret = 0;

	/* completion for bootup event poll */
	reinit_completion(&epub->boot_completion);
	epub->boot_error = 0;
	atomic_set(&epub->ps.state, ESP_PM_OFF);
	if (epub->sdio_state == ESP_SDIO_STATE_FIRST_INIT) {
		if (!epub->sip) {
			epub->sip = sip_attach(epub);
			if (!epub->sip) {
				dev_err(epub->dev, "SIP allocation failed\n");
				return -ENOMEM;
			}
		} else {
			ret = sip_prepare_boot(epub->sip);
			if (ret)
				return ret;
		}
	} else {
		if (!sip_set_state(epub->sip, SIP_PREPARE_BOOT))
			return -ESHUTDOWN;
		atomic_set(&epub->sip->tx_credits, 0);
	}

	epub->sip->to_host_seq = 0;

	ret = esp_download_fw(epub);
	if (ret) {
		esp_dbg(ESP_DBG_ERROR, "download firmware failed\n");
		return ret;
	}

	esp_dbg(ESP_DBG_TRACE, "download firmware OK\n");

	epub->wait_reset = 0;
	ret = sif_enable_irq(epub);
	if (ret)
		return ret;

	if (epub->sdio_state == ESP_SDIO_STATE_SECOND_INIT) {
		ret = sip_poll_bootup_event(epub->sip);
	} else {
		ret = sip_poll_resetting_event(epub->sip);
		if (ret == 0) {
			sif_lock_bus(epub);
			sif_interrupt_target(epub, 7);
			sif_unlock_bus(epub);
		}
	}

	return ret;
}

void esp_dsr(struct esp_pub *epub)
{
	sip_rx(epub);
}

#define ESP_FW_NAME1 "espressif/esp8089-cal-v19.bin"
#define ESP_FW_NAME2 "espressif/esp8089-txr8-v1.bin"

static int esp_download_fw(struct esp_pub *epub)
{
	const struct firmware *fw;
	struct esp8089_fw_image image;
	struct sip_cmd_bootup bootcmd;
	const char *name;
	unsigned int i;
	int ret;

	if (epub->sdio_state == ESP_SDIO_STATE_FIRST_INIT)
		name = ESP_FW_NAME1;
	else
		name = ESP_FW_NAME2;

	ret = request_firmware(&fw, name, epub->dev);
	if (ret)
		return ret;
	ret = esp8089_fw_parse(fw->data, fw->size, &image);
	if (ret) {
		dev_err(epub->dev, "Invalid firmware image %s: %d\n", name,
			ret);
		goto out;
	}

	for (i = 0; i < image.nsegments; i++) {
		const struct esp8089_fw_segment *seg = &image.segments[i];

		ret = sip_write_memory(epub->sip, seg->addr, seg->data,
				       seg->len);
		if (ret)
			goto out;
	}
	bootcmd.boot_addr = cpu_to_le32(image.entry);
	ret = sip_send_cmd(epub->sip, SIP_CMD_BOOTUP, sizeof(bootcmd),
			   &bootcmd);
out:
	release_firmware(fw);
	return ret;
}

MODULE_FIRMWARE(ESP_FW_NAME1);
MODULE_FIRMWARE(ESP_FW_NAME2);

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP_RECOVERY_H
#define ESP_RECOVERY_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct esp_pub;

struct esp_recovery {
	spinlock_t lock; /* Serializes recovery requests, removal and restart state. */
	struct work_struct work;
	bool removing, pending, reconfiguring, disabled;
	int error;
	unsigned long window;
	u32 in_window, requests, boots, completed, failures;
};

void esp_recovery_init(struct esp_pub *epub);
void esp_recovery_stop(struct esp_pub *epub);
void esp_recovery_request(struct esp_pub *epub, int error);
void esp_recovery_complete(struct esp_pub *epub);
int esp_sdio_boot(struct esp_pub *epub);

#endif

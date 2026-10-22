/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP_POWER_H
#define ESP_POWER_H
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#define ESP_POWER_MAGIC 0x31525750U
#define ESP_POWER_COMMAND 29
#define ESP_POWER_EVENT 24
struct esp_sip;
struct esp_power {
	struct mutex lock; /* Entire request/reply transaction and drain. */
	struct completion done;
	wait_queue_head_t drain;
	u32 transaction, effective, factory;
	int status;
	bool pending, blocked;
};

void esp_power_init(struct esp_sip *sip);
int esp_power_set(struct esp_sip *sip, u32 qdbm);
int esp_power_get(struct esp_sip *sip, int *dbm);
int esp_power_negotiate(struct esp_sip *sip);
int esp_power_reply(struct esp_sip *sip, const u8 *buf, size_t len);
#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP8089_DEBUG_H
#define ESP8089_DEBUG_H
#include <linux/bug.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define ESSERT(condition) WARN_ON_ONCE(!(condition))
enum {
	ESP_DBG_ERROR = BIT(0),
	ESP_DBG_TRACE = BIT(1),
	ESP_DBG_LOG = BIT(2),
	ESP_DBG = BIT(3),
	ESP_SHOW = BIT(4),
	ESP_DBG_TXAMPDU = BIT(5),
	ESP_DBG_OP = BIT(6),
	ESP_DBG_PS = BIT(7),
	ESP_ATE = BIT(8),
	ESP_DBG_ALL = 0xffffffff
};

#define esp_dbg(mask, ...)                                             \
	do {                                                                \
		if ((mask) & ESP_DBG_ERROR)                                 \
			pr_err_ratelimited("esp8089: " __VA_ARGS__); \
		else                                                        \
			pr_debug("esp8089: " __VA_ARGS__);           \
	} while (0)
#endif

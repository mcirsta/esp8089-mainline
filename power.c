// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/visibility.h>
#include <linux/unaligned.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"

void esp_power_init(struct esp_sip *sip)
{
	mutex_init(&sip->power.lock);
	init_completion(&sip->power.done);
	init_waitqueue_head(&sip->power.drain);
	sip->power.blocked = true;
}

int esp_power_reply(struct esp_sip *sip, const u8 *buf, size_t len)
{
	struct esp_power *p = &sip->power;
	u32 effective, factory;
	int status;

	if (len != 24 || get_unaligned_le32(buf) != ESP_POWER_MAGIC ||
	    get_unaligned_le32(buf + 20))
		return -EBADMSG;
	if (!READ_ONCE(p->pending) || get_unaligned_le32(buf + 4) != p->transaction)
		return 0;
	status = (s32)get_unaligned_le32(buf + 8);
	effective = get_unaligned_le32(buf + 12);
	factory = get_unaligned_le32(buf + 16);
	if ((status && status != -EINVAL && status != -ERANGE) ||
	    factory > 82 || effective > factory)
		return -EBADMSG;
	p->status = status;
	p->effective = effective;
	p->factory = factory;
	complete(&p->done);
	return 0;
}

static int esp_power_set_internal(struct esp_sip *sip, u32 qdbm, bool boot)
{
	struct esp_power *p = &sip->power;
	struct esp_pub *epub = sip->epub;
	__le32 command[3];
	int ret;

	mutex_lock(&p->lock);
	if (atomic_read(&sip->state) == SIP_STOP ||
	    (!boot && atomic_read(&sip->state) != SIP_RUN)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	/* Preserve an accepted user's ceiling across a firmware restart. */
	if (!boot && qdbm <= 82)
		epub->power_limit_qdbm = qdbm;
	/* Wait for any aggregate already being packed to publish its packets. */
	mutex_lock(&epub->tx_mtx);
	WRITE_ONCE(p->blocked, true);
	mutex_unlock(&epub->tx_mtx);
	if (test_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags)) {
		ieee80211_stop_queues(epub->hw);
		atomic_set(&epub->txq_stopped, true);
	}
	/* The worker still sends control messages while data is paused. */
	if (!wait_event_timeout(p->drain,
				skb_queue_empty(&sip->reports.pending) ||
				atomic_read(&sip->state) == SIP_STOP, HZ)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (atomic_read(&sip->state) == SIP_STOP) {
		ret = -ESHUTDOWN;
		goto out;
	}
	command[0] = cpu_to_le32(ESP_POWER_MAGIC);
	command[1] = cpu_to_le32(++p->transaction);
	command[2] = cpu_to_le32(qdbm);
	reinit_completion(&p->done);
	WRITE_ONCE(p->pending, true);
	ret = sip_cmd(epub, ESP_POWER_COMMAND, (u8 *)command, sizeof(command));
	if (!ret && !wait_for_completion_timeout(&p->done, 2 * HZ))
		ret = -ETIMEDOUT;
	WRITE_ONCE(p->pending, false);
	if (atomic_read(&sip->state) == SIP_STOP)
		ret = -ESHUTDOWN;
	if (!ret)
		ret = p->status;
	if (!ret && qdbm != U32_MAX) {
		WRITE_ONCE(p->blocked, false);
		sip_trigger_txq_process(sip);
	}
out:
	/* A failed or ambiguous setting keeps host data paused. */
	mutex_unlock(&p->lock);
	/* link_info_changed cannot return an allocation error to mac80211.
	 * Restart with the requested ceiling instead of leaving data paused
	 * indefinitely after a transient control-buffer allocation failure.
	 */
	if (!boot && (ret == -ETIMEDOUT || ret == -ENOMEM))
		esp_recovery_request(epub, ret);
	return ret;
}

int esp_power_set(struct esp_sip *sip, u32 qdbm)
{
	return esp_power_set_internal(sip, qdbm, false);
}

int esp_power_get(struct esp_sip *sip, int *dbm)
{
	struct esp_power *p = &sip->power;
	int ret = 0;

	mutex_lock(&p->lock);
	if (atomic_read(&sip->state) != SIP_RUN || p->blocked)
		ret = -EAGAIN;
	else
		/* mac80211 reports integer dBm; never round above the ceiling. */
		*dbm = p->effective / 4;
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(esp_power_get);

int esp_power_negotiate(struct esp_sip *sip)
{
	u32 ceiling;
	int ret = esp_power_set_internal(sip, U32_MAX, true);

	if (ret)
		return ret;
	ceiling = min(sip->epub->power_limit_qdbm, sip->power.factory);
	ret = esp_power_set_internal(sip, ceiling, true);
	if (!ret)
		dev_info(sip->epub->dev, "PWR1 firmware: ceiling %u/4 dBm, factory %u/4 dBm\n",
			 sip->power.effective, sip->power.factory);
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-only
/* Firmware recovery keeps the wiphy, but never reuses uncertain key state. */
#include <kunit/visibility.h>
#include <linux/jiffies.h>
#include <linux/rtnetlink.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_sif.h"
#include "slc_host_register.h"

static void esp_recovery_fault_snapshot(struct esp_pub *epub)
{
	struct slc_host_regs regs;
	int ret;

	/* Offset 4 consumes credits. Read only the passive window starting at 8. */
	ret = esp_common_read_with_addr(epub, REG_SLC_HOST_BASE + 8,
					(u8 *)&regs, sizeof(regs), ESP_SIF_SYNC);
	if (!ret)
		dev_err(epub->dev, "Recovery snapshot: marker=%08x pc=%08x ra=%08x\n",
			le32_to_cpu(regs.config_w2), le32_to_cpu(regs.config_w3),
			le32_to_cpu(regs.config_w1));
	else
		dev_err(epub->dev, "Recovery snapshot unavailable: %d\n", ret);
}

static void esp_recovery_clear_mac(struct esp_pub *epub)
{
	unsigned int i;

	/* RTNL and wiphy locks exclude configuration callbacks. All RX/TX
	 * producers and readers have been drained by sip_quiesce(). mac80211
	 * owns the station/vif objects and will replay them during restart.
	 */
	epub->vif = NULL;
	epub->vif_slot = 0;
	epub->enodes_map = 0;
	memset(epub->enodes_maps, 0, sizeof(epub->enodes_maps));
	for (i = 0; i < ARRAY_SIZE(epub->enodes); i++)
		WRITE_ONCE(epub->enodes[i], NULL);
	epub->rxampdu_map = 0;
	memset(epub->rxampdu_node, 0, sizeof(epub->rxampdu_node));
	memset(epub->rxampdu_tid, 0, sizeof(epub->rxampdu_tid));
	memset(epub->hi_map, 0, sizeof(epub->hi_map));
	memset(epub->low_map, 0, sizeof(epub->low_map));
	atomic_set(&epub->wl.ptk_cnt, 0);
	atomic_set(&epub->wl.gtk_cnt, 0);
	eth_zero_addr(epub->wl.bssid);
	eth_zero_addr(epub->wl.req_bssid);
	epub->roc_flags = 0;
	clear_bit(ESP_WL_FLAG_CONNECT, &epub->wl.flags);
	clear_bit(ESP_WL_FLAG_STOP_TXQ, &epub->wl.flags);
}

static void esp_recovery_work(struct work_struct *work)
{
	struct esp_recovery *r = container_of(work, struct esp_recovery, work);
	struct esp_pub *epub = container_of(r, struct esp_pub, recovery);
	unsigned long flags;
	bool had_interface;
	int ret = -ESHUTDOWN, attempt;

	/* Never run on esp_wkq: quiescing must be able to drain RX/TX work. */
	rtnl_lock();
	wiphy_lock(epub->hw->wiphy);
	if (READ_ONCE(r->removing))
		goto out;
	had_interface = !!epub->vif;
	dev_warn(epub->dev, "Recovering firmware after error %d\n", r->error);
	sif_disable_irq(epub);
	esp_recovery_fault_snapshot(epub);
	sip_quiesce(epub->sip);
	esp_recovery_clear_mac(epub);
	if (time_after(jiffies, r->window + 60 * HZ)) {
		r->window = jiffies;
		r->in_window = 0;
	}
	if (++r->in_window > 3) {
		ret = -ELOOP;
		goto failed;
	}
	for (attempt = 0; attempt < 2; attempt++) {
		if (READ_ONCE(r->removing))
			goto out;
		r->boots++;
		ret = esp_sdio_boot(epub);
		if (!ret)
			break;
		dev_err(epub->dev, "Recovery boot %d failed: %d\n", attempt + 1, ret);
		sip_quiesce(epub->sip);
	}
	if (ret)
		goto failed;
	if (READ_ONCE(r->removing)) {
		sip_quiesce(epub->sip);
		goto out;
	}
	spin_lock_irqsave(&r->lock, flags);
	r->reconfiguring = had_interface;
	if (!had_interface) {
		r->pending = false;
		r->completed++;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	/* Restart does not call ->stop(). Hardware and every transport cursor
	 * have already been reset. ->add_interface marks old keys tainted and
	 * requests fresh authentication rather than trusting firmware PN state.
	 */
	ieee80211_restart_hw(epub->hw);
	dev_info(epub->dev, "Firmware reloaded; mac80211 restart requested\n");
	goto out;
failed:
	spin_lock_irqsave(&r->lock, flags);
	r->error = ret;
	r->failures++;
	r->disabled = true;
	r->pending = false;
	spin_unlock_irqrestore(&r->lock, flags);
	dev_err(epub->dev, "Recovery failed (%d); radio remains stopped\n", ret);
out:
	wiphy_unlock(epub->hw->wiphy);
	rtnl_unlock();
}

void esp_recovery_init(struct esp_pub *epub)
{
	spin_lock_init(&epub->recovery.lock);
	INIT_WORK(&epub->recovery.work, esp_recovery_work);
	epub->power_limit_qdbm = 80;
}

void esp_recovery_request(struct esp_pub *epub, int error)
{
	struct esp_recovery *r = &epub->recovery;
	struct esp_sip *sip = epub->sip;
	unsigned long flags;

	atomic_set(&epub->wl.off, 1);
	sip_stop(sip);
	WRITE_ONCE(sip->reports.failed, true);
	WRITE_ONCE(sip->power.blocked, true);
	WRITE_ONCE(sip->power.status, -ESHUTDOWN);
	complete_all(&sip->power.done);
	wake_up_all(&sip->power.drain);
	if (!test_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags)) {
		epub->boot_error = error;
		complete(&epub->boot_completion);
		return;
	}
	ieee80211_stop_queues(epub->hw);
	spin_lock_irqsave(&r->lock, flags);
	if (r->pending) {
		/* A transport error during a recovery boot must wake its waiter. */
		epub->boot_error = error;
		complete(&epub->boot_completion);
	} else if (!r->removing && !r->disabled) {
		r->pending = true;
		r->error = error;
		r->requests++;
		/* Queue under the same lock used by removal, so no request can
		 * enqueue after cancel_work_sync() has returned.
		 */
		queue_work(system_long_wq, &r->work);
	}
	spin_unlock_irqrestore(&r->lock, flags);
}

EXPORT_SYMBOL_IF_KUNIT(esp_recovery_request);

void esp_recovery_complete(struct esp_pub *epub)
{
	struct esp_recovery *r = &epub->recovery;
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (r->reconfiguring) {
		r->reconfiguring = false;
		r->pending = false;
		if (atomic_read(&epub->sip->state) == SIP_RUN) {
			r->completed++;
		} else {
			r->failures++;
			r->disabled = true;
			r->error = -ESHUTDOWN;
		}
	}
	spin_unlock_irqrestore(&r->lock, flags);
}

void esp_recovery_stop(struct esp_pub *epub)
{
	unsigned long flags;

	spin_lock_irqsave(&epub->recovery.lock, flags);
	epub->recovery.removing = true;
	spin_unlock_irqrestore(&epub->recovery.lock, flags);
	if (epub->sip) {
		atomic_set(&epub->wl.off, 1);
		sip_stop(epub->sip);
	}
	cancel_work_sync(&epub->recovery.work);
}

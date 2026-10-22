// SPDX-License-Identifier: GPL-2.0-only
/* Retained completion records from the TXR8 firmware interface. */
#include <linux/unaligned.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"
#include "esp_wmac.h"

static int report_query(struct esp_sip *sip, u32 flags)
{
	struct sk_buff_head *queue = &sip->epub->ctrlq;
	struct sk_buff *skb;
	__le32 query[] = { cpu_to_le32(ESP_TXR_MAGIC),
		cpu_to_le32(READ_ONCE(sip->reports.next)), cpu_to_le32(flags) };

	/* RX events can advance several records before the ordered TX worker
	 * runs. Update a queued request instead of consuming a credit per event.
	 */
	spin_lock_bh(&queue->lock);
	skb_queue_walk(queue, skb) {
		struct sip_hdr *hdr = (void *)skb->data;
		u8 *payload = skb->data + sizeof(*hdr);

		if (skb->len != sizeof(*hdr) + sizeof(query) ||
		    le32_to_cpu(hdr->c_cmdid) != ESP_TXR_COMMAND ||
		    get_unaligned_le32(payload) != ESP_TXR_MAGIC)
			continue;
		put_unaligned_le32(sip->reports.next, payload + 4);
		put_unaligned_le32(flags | get_unaligned_le32(payload + 8), payload + 8);
		atomic64_inc(&sip->reports.coalesced_queries);
		spin_unlock_bh(&queue->lock);
		return 0;
	}
	spin_unlock_bh(&queue->lock);
	return sip_cmd(sip->epub, ESP_TXR_COMMAND, (u8 *)query, sizeof(query));
}

static void report_progress(struct esp_reports *r)
{
	r->progress = jiffies;
	r->replay_delay_ms = 100;
	r->replay_at = r->progress + msecs_to_jiffies(r->replay_delay_ms);
}

static void report_poll(struct work_struct *work)
{
	struct esp_reports *r = container_of(to_delayed_work(work),
					   struct esp_reports, poll);
	struct esp_sip *sip = container_of(r, struct esp_sip, reports);

	if (atomic_read(&sip->state) == SIP_STOP)
		return;
	if (!skb_queue_empty(&r->pending)) {
		u32 flags = 1; /* Flush partial completion batches. */

		if (time_after(jiffies, r->progress + 5 * HZ)) {
			dev_err(sip->epub->dev, "TXR8 stalled; requesting firmware recovery\n");
			esp_recovery_request(sip->epub, -ETIMEDOUT);
			return;
		}
		/* Normal polling only flushes newly completed records. Replaying
		 * records already queued for USB/SDIO delivery causes a feedback
		 * loop when the host is slower than the radio (notably KASAN).
		 */
		if (time_after_eq(jiffies, r->replay_at)) {
			flags |= 2;
			atomic64_inc(&r->replay_requests);
			r->replay_delay_ms = min(r->replay_delay_ms * 2, 1000U);
			r->replay_at = jiffies + msecs_to_jiffies(r->replay_delay_ms);
		}
		report_query(sip, flags);
		sip_queue_report_poll(sip);
	}
}

void esp_reports_init(struct esp_sip *sip)
{
	init_completion(&sip->reports.ready);
	skb_queue_head_init(&sip->reports.pending);
	INIT_DELAYED_WORK(&sip->reports.poll, report_poll);
}

int esp_reports_negotiate(struct esp_sip *sip)
{
	int ret = report_query(sip, 6); /* Replay plus explicit capability reply. */

	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&sip->reports.ready, HZ))
		return -ETIMEDOUT;
	ieee80211_hw_set(sip->epub->hw, REPORTS_TX_ACK_STATUS);
	dev_info(sip->epub->dev, "TXR8 firmware negotiated, window %u\n",
		 sip->reports.capacity);
	return 0;
}

bool esp_reports_is_ack_only(struct esp_sip *sip, struct sk_buff *skb)
{
	const struct sip_hdr *hdr = (const void *)skb->data;

	if (!sip->reports.active || skb->len < sizeof(*hdr))
		return false;
	return esp_txr_ack_only(le32_to_cpu(hdr->c_cmdid),
			       skb->data + sizeof(*hdr), skb->len - sizeof(*hdr));
}

void esp_reports_packed_control(struct esp_sip *sip, struct sk_buff *skb)
{
	struct sip_hdr *hdr = (struct sip_hdr *)skb->data;
	const u8 *payload = skb->data + sizeof(*hdr);

	if (sip->reports.active && skb->len == sizeof(*hdr) + 12 &&
	    le32_to_cpu(hdr->c_cmdid) == ESP_TXR_COMMAND &&
	    get_unaligned_le32(payload) == ESP_TXR_MAGIC)
		sip->reports.acked = get_unaligned_le32(payload + 4);
}

void esp_reports_submit(struct esp_sip *sip, struct sk_buff *skb, u32 seq)
{
	struct esp_reports *r = &sip->reports;

	esp_tx_packet_info(skb)->sequence = seq;
	if (skb_queue_empty(&r->pending))
		report_progress(r);
	skb_queue_tail(&r->pending, skb);
	r->submitted++;
	sip_queue_report_poll(sip);
}

void esp_reports_abort(struct esp_sip *sip)
{
	struct sk_buff *skb;

	/* Unknown outcomes are freed without inventing a radio failure or ACK. */
	while ((skb = skb_dequeue(&sip->reports.pending))) {
		atomic64_inc(&sip->reports.lost);
		atomic_dec(&sip->tx_data_pkt_queued);
		ieee80211_free_txskb(sip->epub->hw, skb);
	}
	wake_up_all(&sip->power.drain);
}

static void report_complete(struct esp_sip *sip, struct sk_buff *skb,
			    u32 word)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_rate_status rate = {};
	struct ieee80211_tx_status status = { .skb = skb, .info = info };
	struct esp_reports *r = &sip->reports;
	struct esp_node *node;
	u32 retry = word & 0x801fffffU;
	u8 outcome = (word >> 21) & 7, phy;
	bool noack = word & BIT(24);
	bool known = retry & BIT(31);
	u16 attempts = retry;
	s8 idx;

	if (outcome == 1 && !noack) {
		rcu_read_lock();
		sip_maybe_start_tx_ba(sip, skb);
		rcu_read_unlock();
	}
	ieee80211_tx_info_clear_status(info);
	info->status.rates[0].idx = -1;
	if (outcome == 1) {
		if (noack) {
			info->flags |= IEEE80211_TX_STAT_NOACK_TRANSMITTED;
			atomic64_inc(&r->noack);
		} else {
			info->flags |= IEEE80211_TX_STAT_ACK;
			atomic64_inc(&r->ack);
		}
	} else {
		atomic64_inc(&r->fail);
	}
	/* Firmware counts DATA attempts before retry state/rate is advanced.
	 * Keep the per-rate history unspecified: only the final PHY is known.
	 */
	if (known) {
		atomic64_inc(&r->attempts_known);
		atomic64_add(attempts, &r->attempts_total);
		atomic64_add(attempts - 1, &r->retry_counter);
		phy = (retry >> 16) & 31;
	} else {
		atomic64_inc(&r->attempts_unknown);
	}
	if (known) {
		status.rates = &rate;
		status.n_rates = 1;
		rate.rate_idx.bw = RATE_INFO_BW_20;
		if (phy >= 16) {
			rate.rate_idx.flags = RATE_INFO_FLAGS_MCS;
			if (phy >= 24)
				rate.rate_idx.flags |= RATE_INFO_FLAGS_SHORT_GI;
			rate.rate_idx.mcs = (phy - 16) & 7;
		} else {
			idx = esp_wmac_rate2idx(phy == 4 ? 0 : phy);
			if (idx < 0) {
				status.n_rates = 0;
			} else {
				struct ieee80211_supported_band *band;

				band = &sip->epub->wl.sbands[NL80211_BAND_2GHZ];
				rate.rate_idx.legacy = band->bitrates[idx].bitrate;
			}
		}
	}
	/* Final rate is known; the per-rate attempt history remains unknown.
	 * try_count=0 deliberately leaves that history unspecified.
	 */
	/* Match ieee80211_tx_status_ni()'s process-context protection. The
	 * ordered worker excludes RX/status peers, but does not disable BHs.
	 */
	local_bh_disable();
	rcu_read_lock();
	node = esp_get_node_by_addr(sip->epub, hdr->addr1);
	if (node) {
		status.sta = node->sta;
		if (ieee80211_is_data(hdr->frame_control)) {
			spin_lock_bh(&sip->epub->tx_ampdu_lock);
			if (known)
				node->tx_retries += attempts - 1;
			else
				node->tx_attempts_unknown++;
			if (status.n_rates) {
				node->last_tx_rate = rate.rate_idx;
				node->last_tx_rate_valid = true;
			}
			spin_unlock_bh(&sip->epub->tx_ampdu_lock);
		}
	}
	atomic_dec(&sip->tx_data_pkt_queued);
	ieee80211_tx_status_ext(sip->epub->hw, &status);
	rcu_read_unlock();
	local_bh_enable();
}

int esp_reports_receive(struct esp_sip *sip, const u8 *buf, size_t len)
{
	struct esp_reports *r = &sip->reports;
	u32 base, count, i;
	int valid;

	atomic64_inc(&sip->epub->stats[ESP_FW_TX_REPORTS]);
	valid = esp_txr_payload_valid(buf, len);
	if (valid < 0)
		goto malformed;
	base = get_unaligned_le32(buf + 4);
	count = valid;
	if (!r->active) {
		if (base || count)
			goto malformed;
		r->capacity = get_unaligned_le32(buf + 48);
		r->active = true;
		complete(&r->ready);
		return 0;
	}
	if (get_unaligned_le32(buf + 48) != r->capacity)
		goto malformed;
	for (i = 0; i < count; i++) {
		const u8 *record = buf + 16 + i * 8;
		u32 seq = get_unaligned_le32(record);
		struct sk_buff *skb, *found = NULL;

		if ((s32)(base + i - r->next) < 0) {
			atomic64_inc(&r->duplicates);
			continue;
		}
		if (base + i != r->next)
			goto malformed;
		skb_queue_walk(&r->pending, skb) {
			if (esp_tx_packet_info(skb)->sequence == seq) {
				found = skb;
				break;
			}
		}
		if (!found) {
			atomic64_inc(&r->orphan);
			goto malformed;
		}
		skb_unlink(found, &r->pending);
		report_complete(sip, found, get_unaligned_le32(record + 4));
		atomic64_inc(&r->records);
		atomic64_inc(&sip->epub->stats[ESP_FW_TX_REPORTED_PKTS]);
		r->next++;
		report_progress(r);
	}
	if (count) {
		wake_up_all(&sip->power.drain);
		report_query(sip, 0);
		sip_trigger_txq_process(sip);
	}
	return 0;
malformed:
	atomic64_inc(&r->malformed);
	atomic64_inc(&sip->epub->stats[ESP_FW_TX_REPORT_ERRORS]);
	return -EBADMSG;
}

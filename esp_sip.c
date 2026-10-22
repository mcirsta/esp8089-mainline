// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2009 - 2014 Espressif System. */
#include <kunit/visibility.h>
#include <kunit/static_stub.h>
#include <linux/ieee80211.h>
#include <net/mac80211.h>
#include <net/cfg80211.h>
#include <linux/skbuff.h>
#include <linux/bitops.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>
#include <linux/completion.h>
#include <linux/timer.h>

#include "esp_mac80211.h"
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"
#include "esp_sif.h"
#include "esp_debug.h"
#include "slc_host_register.h"
#include "esp_wmac.h"
#include "esp_utils.h"
#include "tx-batch.h"

static const struct esp_mac_prefix esp_mac_prefix_table[] = {
	{ 0, { 0x18, 0xfe, 0x34 } },
	{ 1, { 0xac, 0xd0, 0x74 } },
	{ 255, { 0x18, 0xfe, 0x34 } },
};

static u8 tid_to_ac(u8 tid)
{
	if (tid == 0 || tid == 3)
		return WME_AC_BE;
	if (tid < 3)
		return WME_AC_BK;
	if (tid < 6)
		return WME_AC_VI;
	return WME_AC_VO;
}

#define esp_sip_dbg esp_dbg

#define SIP_STOP_QUEUE_THRESHOLD 48
#define SIP_RESUME_QUEUE_THRESHOLD 12

static void sip_recalc_credit_init(struct esp_sip *sip);

static int sip_recalc_credit_claim(struct esp_sip *sip, int force);

static void sip_recalc_credit_release(struct esp_sip *sip);

static struct sip_pkt *sip_get_ctrl_buf(struct esp_sip *sip,
					enum sip_buf_type bftype);

static void sip_reclaim_ctrl_buf(struct esp_sip *sip, struct sip_pkt *pkt,
				 enum sip_buf_type bftype);

static void sip_free_init_ctrl_buf(struct esp_sip *sip);

static int sip_pack_pkt(struct esp_sip *sip, struct sk_buff *skb, int *pm_state,
			bool control);

VISIBLE_IF_KUNIT struct sk_buff *sip_parse_data_rx_info(struct esp_sip *sip,
					      struct sk_buff *skb,
					      int pkt_len_enc, int buf_len,
					      struct esp_mac_rx_ctrl *mac_ctrl,
					      int *pulled_len);

static inline void sip_rx_pkt_enqueue(struct esp_sip *sip, struct sk_buff *skb);

static void sip_complete_tx(struct esp_sip *sip, bool success);

static void sip_update_tx_credits(struct esp_sip *sip, u16 recycled_credits);

static bool sip_rx_pkt_process(struct esp_sip *sip, struct sk_buff *skb);

static void sip_tx_status_report(struct esp_sip *sip, struct sk_buff *skb,
				 struct ieee80211_tx_info *tx_info,
				 bool success);

static bool check_ac_tid(u8 *pkt, u8 ac, u8 tid)
{
	struct ieee80211_hdr *wh = (struct ieee80211_hdr *)pkt;

	if (ieee80211_is_data_qos(wh->frame_control)) {
	} else if (ieee80211_is_mgmt(wh->frame_control)) {
	} else {
		if (ieee80211_is_ctl(wh->frame_control)) {
		} else {
			if (tid != 0 || ac != WME_AC_BE) {
				esp_sip_dbg(ESP_DBG_LOG, "444 ac:%u, tid:%u\n",
					    ac, tid);
				if (tid == 7 && ac == WME_AC_VO)
					return false;
			}
			return true; //hack to modify non-qos null data.
		}
	}

	return false;
}

static void sip_recalc_credit_timeout(struct timer_list *t)
{
	struct esp_sip *sip = timer_container_of(sip, t, credit_timer);

	if (atomic_read(&sip->state) == SIP_STOP)
		return;
	esp_dbg(ESP_DBG_ERROR, "rct");

	sip_recalc_credit_claim(sip, 1); /* recalc again */
}

static void sip_recalc_credit_init(struct esp_sip *sip)
{
	atomic_set(&sip->credit_status, RECALC_CREDIT_DISABLE); //set it disable
}

static int sip_recalc_credit_claim(struct esp_sip *sip, int force)
{
	int ret;

	if (atomic_read(&sip->credit_status) == RECALC_CREDIT_ENABLE &&
	    force == 0)
		return 1;

	atomic_set(&sip->credit_status, RECALC_CREDIT_ENABLE);
	ret = sip_send_recalc_credit(sip->epub);
	if (ret) {
		esp_dbg(ESP_DBG_ERROR, "%s error %d", __func__, ret);
		return ret;
	}
	/*setup a timer for handle the abs_credit not receive */
	sip_rearm_credit_timer(sip);

	esp_dbg(ESP_SHOW, "rcc");

	return ret;
}

static void sip_recalc_credit_release(struct esp_sip *sip)
{
	esp_dbg(ESP_SHOW, "rcr");

	if (atomic_read(&sip->credit_status) == RECALC_CREDIT_ENABLE) {
		atomic_set(&sip->credit_status, RECALC_CREDIT_DISABLE);
		timer_delete_sync(&sip->credit_timer);
	} else {
		esp_dbg(ESP_SHOW, "maybe bogus credit");
	}
}

static void sip_update_tx_credits(struct esp_sip *sip, u16 recycled_credits)
{
	esp_sip_dbg(ESP_DBG_TRACE, "%s:before add, credits is %d\n", __func__,
		    atomic_read(&sip->tx_credits));

	if (recycled_credits & 0x800) {
		atomic_set(&sip->tx_credits, (recycled_credits & 0x7ff));
		sip_recalc_credit_release(sip);
	} else {
		atomic_add(recycled_credits, &sip->tx_credits);
	}

	esp_sip_dbg(ESP_DBG_TRACE, "%s:after add %d, credits is %d\n", __func__,
		    recycled_credits, atomic_read(&sip->tx_credits));
}

static bool sip_tx_head_can_pack(struct esp_sip *sip)
{
	struct sk_buff_head *queue = &sip->epub->txq;
	struct sk_buff *skb;
	bool ready = false;
	unsigned int reserved = sip->credit_to_reserve + SIP_CTRL_CREDIT_RESERVE;
	int credits = atomic_read(&sip->tx_credits);
	bool ampdu;

	/* Producers can run in softirq context. Protect the peeked packet until
	 * its size/aggregation flag have been copied; the worker may dequeue it.
	 */
	spin_lock_bh(&queue->lock);
	skb = skb_peek(queue);
	if (skb) {
		ampdu = IEEE80211_SKB_CB(skb)->flags & IEEE80211_TX_CTL_AMPDU;
		ready = esp_tx_data_fits(skb->len, ampdu, sip->tx_blksz, credits, reserved);
	}
	spin_unlock_bh(&queue->lock);
	return ready;
}

void sip_trigger_txq_process(struct esp_sip *sip)
{
	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) == SIP_STOP)
		goto out;
	if (atomic_read(&sip->tx_credits) <=
	    sip->credit_to_reserve + SIP_CTRL_CREDIT_RESERVE ||
	    atomic_read(&sip->credit_status) == RECALC_CREDIT_ENABLE)
		goto out;

	if (sip_queue_may_resume(sip)) {
		/* wakeup upper queue only if we have sufficient credits */
		esp_sip_dbg(ESP_DBG_TRACE, "%s wakeup ieee80211 txq\n",
			    __func__);
		atomic_set(&sip->epub->txq_stopped, false);
		ieee80211_wake_queues(sip->epub->hw);
	} else if (atomic_read(&sip->epub->txq_stopped)) {
		esp_sip_dbg(ESP_DBG_TRACE, "%s can't wake txq, credits: %d\n",
			    __func__, atomic_read(&sip->tx_credits));
	}

	if (!skb_queue_empty(&sip->epub->txq) ||
	    !skb_queue_empty(&sip->epub->ctrlq)) {
		/* Credit updates and new data both use this predicate. A later
		 * credit update kicks again; commands retain their immediate path.
		 */
		if (skb_queue_empty(&sip->epub->ctrlq) &&
		    !sip_tx_head_can_pack(sip)) {
			atomic64_inc(&sip->epub->stats[ESP_TX_KICK_NO_ROOM]);
			goto out;
		}
		/* try to send out pkt already in sip queue once we have credits */
		esp_sip_dbg(ESP_DBG_TRACE, "%s resume sip txq\n", __func__);

		queue_work(sip->epub->esp_wkq, &sip->epub->tx_work);
	}
out:
	spin_unlock_bh(&sip->state_lock);
}

static bool sip_ampdu_occupy_buf(struct esp_sip *sip, u8 state)
{
	return !state || esp_wmac_rxsec_error(state) ||
	       (sip->dump_rpbm_err && state == RX_RPBM_ERR);
}

/* Parsing and allocations may sleep. Only the bounded hand-off to mac80211
 * and the network stack runs with BHs disabled. esp_wkq orders this against
 * TX status delivery; rx_mtx also excludes a second receive worker.
 */
static void sip_rx_deliver(struct esp_sip *sip, struct sk_buff_head *frames)
{
	struct sk_buff *frame;
	LIST_HEAD(list);

	if (skb_queue_empty(frames))
		return;
	local_bh_disable();
	rcu_read_lock();
	while ((frame = __skb_dequeue(frames))) {
		if (atomic_read(&sip->epub->wl.off))
			kfree_skb(frame);
		else
			ieee80211_rx_list(sip->epub->hw, NULL, frame, &list);
	}
	rcu_read_unlock();
	netif_receive_skb_list(&list);
	local_bh_enable();
}

static void sip_rx_queue_frame(struct esp_sip *sip, struct sk_buff_head *frames,
			       struct sk_buff *frame)
{
	__skb_queue_tail(frames, frame);
	/* Bound BH latency and the number of additional live frame allocations. */
	if (skb_queue_len(frames) >= 32)
		sip_rx_deliver(sip, frames);
}

VISIBLE_IF_KUNIT void sip_rx_ampdu(struct esp_sip *sip, struct sk_buff *view,
				   struct esp_mac_rx_ctrl *ctrl,
				   const __le32 *entries,
				   struct sk_buff_head *frames)
{
	struct sk_buff *frame;
	u32 i;
	int pulled;

	for (i = 0; i < ctrl->ampdu_cnt; i++) {
		u32 entry = le32_to_cpu(entries[i]);
		u8 state = entry & 0xff;
		u16 sublen = (entry >> 8) & 0xfff;

		/* Do not reconstruct bytes from a previous MPDU when
		 * early firmware reports a truncated aggregate.
		 */
		if (state == RX_ABORT && !sip->rxabort_fixed)
			break;
		if (!sip_ampdu_occupy_buf(sip, state))
			continue;
		ctrl->rxend_state = state;
		pulled = 0;
		frame = sip_parse_data_rx_info(sip, view, sublen - FCS_LEN,
					       0, ctrl, &pulled);
		if (!frame) {
			/* A rejected MPDU must not discard the valid tail when
			 * its storage boundary has already been consumed.
			 */
			if (pulled)
				continue;
			break;
		}
		if (state == 0 || state == RX_TKIPMIC_ERR)
			sip_rx_queue_frame(sip, frames, frame);
		else
			kfree_skb(frame);
	}
}
EXPORT_SYMBOL_IF_KUNIT(sip_rx_ampdu);

/* The transfer contains aligned SIP packets. Never trust lengths from the
 * device until they have been checked against both the transfer and packet.
 */
static bool sip_rx_pkt_process(struct esp_sip *sip, struct sk_buff *skb)
{
	struct sk_buff_head frames;
	struct sip_hdr *first;
	u32 credits, offset = 0, first_len;

	__skb_queue_head_init(&frames);
	if (skb->len < sizeof(*first))
		goto malformed;
	first = (void *)skb->data;
	credits = le32_to_cpu(first->h_credits);
	first_len = credits >> 12;
	if (first_len < sizeof(*first) || first_len > skb->len)
		goto malformed;
	if (credits & SIP_CREDITS_MASK)
		sip_update_tx_credits(sip, credits & SIP_CREDITS_MASK);
	first->h_credits = 0;
	first->len = cpu_to_le16(first_len);

	while (offset < skb->len) {
		struct sip_hdr *hdr = (void *)(skb->data + offset);
		struct sk_buff *view, *frame;
		struct esp_mac_rx_ctrl ctrl;
		u32 len, payload;
		int pulled = 0, pkt_len = 0, buf_len = 0;

		if (skb->len - offset < sizeof(*hdr))
			goto malformed;
		len = le16_to_cpu(hdr->len);
		if (len < sizeof(*hdr) || len > skb->len - offset || len & 3)
			goto malformed;
		if (le32_to_cpu(hdr->seq) != sip->rxseq)
			dev_dbg(sip->epub->dev, "RX sequence gap: %u -> %u\n",
				sip->rxseq, le32_to_cpu(hdr->seq));
		sip->rxseq = le32_to_cpu(hdr->seq) + 1;
		if (SIP_HDR_IS_CTRL(hdr)) {
			/* A control event must not overtake preceding received frames. */
			sip_rx_deliver(sip, &frames);
			if (sip_parse_events(sip, (void *)hdr))
				goto malformed;
			goto next;
		}
		if (!SIP_HDR_IS_DATA(hdr) && !SIP_HDR_IS_AMPDU(hdr))
			goto malformed;
		if (len < sizeof(*hdr) + ESP_RX_CTRL_LEN)
			goto malformed;
		esp_rx_ctrl_decode((u8 *)hdr + sizeof(*hdr), &ctrl);
		view = skb_clone(skb, GFP_KERNEL);
		if (!view)
			break;
		skb_pull(view, offset);
		skb_trim(view, len);
		skb_pull(view, sizeof(*hdr) + ESP_RX_CTRL_LEN);
		payload = view->len;
		if (SIP_HDR_IS_DATA(hdr)) {
			if (ctrl.aggregation) {
				kfree_skb(view);
				goto malformed;
			}
			pkt_len = (ctrl.sig_mode ? ctrl.ht_length :
						   ctrl.legacy_length) -
				  FCS_LEN;
			buf_len = payload;
			frame = sip_parse_data_rx_info(sip, view, pkt_len,
						       buf_len, &ctrl, &pulled);
			if (frame)
				sip_rx_queue_frame(sip, &frames, frame);
		} else {
			const __le32 *entries;
			u32 table;

			if (!ctrl.aggregation || !sip->rx_blksz) {
				kfree_skb(view);
				goto malformed;
			}
			table = len / sip->rx_blksz * sip->rx_blksz;
			if (table < sizeof(*hdr) + ESP_RX_CTRL_LEN ||
			    ctrl.ampdu_cnt > (len - table) / sizeof(*entries)) {
				kfree_skb(view);
				goto malformed;
			}
			atomic64_inc(&sip->epub->stats[ESP_RX_AGGREGATES]);
			entries = (void *)((u8 *)hdr + table);
			skb_trim(view, table - sizeof(*hdr) - ESP_RX_CTRL_LEN);
			sip_rx_ampdu(sip, view, &ctrl, entries, &frames);
		}
		kfree_skb(view);
next:
		offset += len;
	}
	sip_rx_deliver(sip, &frames);
	kfree_skb(skb);
	return false;
malformed:
	sip_rx_deliver(sip, &frames);
	atomic64_inc(&sip->epub->stats[ESP_RX_BAD_PACKETS]);
	dev_warn_ratelimited(sip->epub->dev, "Invalid RX transfer\n");
	kfree_skb(skb);
	return false;
}

static void _sip_rxq_process(struct esp_sip *sip)
{
	struct sk_buff *skb = NULL;

	while ((skb = skb_dequeue(&sip->rxq)))
		sip_rx_pkt_process(sip, skb);

	/* probably tx_credit is updated, try txq */
	sip_trigger_txq_process(sip);
}

static void sip_rxq_process(struct work_struct *work)
{
	struct esp_sip *sip =
		container_of(work, struct esp_sip, rx_process_work);
	if (!sip) {
		ESSERT(0);
		return;
	}

	if (unlikely(atomic_read(&sip->state) == SIP_SEND_INIT)) {
		sip_send_chip_init(sip);
		sip_set_state(sip, SIP_WAIT_BOOTUP);
		return;
	}

	mutex_lock(&sip->rx_mtx);
	_sip_rxq_process(sip);
	mutex_unlock(&sip->rx_mtx);
}

static inline void sip_rx_pkt_enqueue(struct esp_sip *sip, struct sk_buff *skb)
{
	skb_queue_tail(&sip->rxq, skb);
}

static inline struct sk_buff *sip_rx_pkt_dequeue(struct esp_sip *sip)
{
	return skb_dequeue(&sip->rxq);
}

int sip_rx(struct esp_pub *epub)
{
	struct sip_hdr *shdr = NULL;
	struct esp_sip *sip = epub->sip;
	int err = 0;
	struct sk_buff *first_skb = NULL;
	u8 *rx_buf = NULL;
	u32 rx_blksz;
	struct sk_buff *rx_skb = NULL;

	u32 first_sz;

	first_sz = le32_to_cpu(sif_get_regs(epub)->config_w0);
	if (!esp_rx_transfer_size_valid(first_sz)) {
		atomic64_inc(&epub->stats[ESP_RX_INVALID_SIZE]);
		dev_warn_ratelimited(epub->dev, "Invalid transfer size %u\n",
				     first_sz);
		sif_halt_transport(epub, -EBADMSG);
		return -EBADMSG;
	}

	/* Consume the pending transfer even after a sequence gap. Returning
	 * without draining it leaves the device's level interrupt asserted.
	 */
	sip->to_host_seq = le32_to_cpu(sif_get_regs(epub)->intr_raw) & 0xff;
	if (first_sz > 16384)
		dev_info_once(epub->dev,
			      "RX transfer exceeds 16 KiB: %u bytes\n",
			      first_sz);

	/* first read one block out, if we luck enough, that's it
	 *
	 *  To make design as simple as possible, we allocate skb(s)
	 *  separately for each sif read operation to avoid global
	 *  read_buf_pointe access.  It coule be optimized late.
	 */
	if (first_sz > atomic64_read(&epub->stats[ESP_RX_MAX_TRANSFER]))
		atomic64_set(&epub->stats[ESP_RX_MAX_TRANSFER], first_sz);
	rx_blksz = sif_get_blksz(epub);
	first_skb = __dev_alloc_skb(roundup(first_sz, rx_blksz), GFP_KERNEL);

	if (!first_skb) {
		/* Drain the transfer even under memory pressure, using the
		 * preallocated DMA buffer. Otherwise the level IRQ stays set.
		 */
		err = esp_common_read(epub, EPUB_TO_CTRL(epub)->dma_buffer,
				      first_sz, ESP_SIF_NOSYNC, false);
		atomic64_inc(&epub->stats[ESP_RX_FRAME_DROPS]);
		if (err)
			sif_halt_transport(epub, err);
		return err ?: -ENOMEM;
	}

	rx_buf = skb_put(first_skb, first_sz);
	esp_sip_dbg(ESP_DBG_LOG, "%s rx_buf ptr %p, first_sz %d\n", __func__,
		    rx_buf, first_sz);

	atomic64_inc(&epub->stats[ESP_RX_TRANSFERS]);
	atomic64_add(first_sz, &epub->stats[ESP_RX_BYTES]);

	err = esp_common_read(epub, rx_buf, first_sz, ESP_SIF_NOSYNC, false);

	if (unlikely(err)) {
		atomic64_inc(&epub->stats[ESP_RX_IO_ERRORS]);
		esp_dbg(ESP_DBG_ERROR, " %s first read err %d %d\n", __func__,
			err, sif_get_regs(epub)->config_w0);
		kfree_skb(first_skb);
		sif_halt_transport(epub, err);
		goto _err;
	}

	shdr = (struct sip_hdr *)rx_buf;
	if (SIP_HDR_IS_CTRL(shdr) && shdr->c_evtid == SIP_EVT_SLEEP) {
		atomic_set(&sip->epub->ps.state, ESP_PM_ON);
		esp_dbg(ESP_DBG_TRACE, "s\n");
	}

	{
		sip->to_host_seq++;
	}

	if ((le16_to_cpu(shdr->len) & 3) != 0) {
		esp_sip_dbg(ESP_DBG_ERROR, "%s shdr->len[%d] error\n", __func__,
			    shdr->len);
		kfree_skb(first_skb);
		err = -EIO;
		goto _err;
	}
	if (le16_to_cpu(shdr->len) != first_sz) {
		esp_sip_dbg(ESP_DBG_ERROR,
			    "%s shdr->len[%d]  first_size[%d] error\n",
			    __func__, shdr->len, first_sz);
		kfree_skb(first_skb);
		err = -EIO;
		goto _err;
	} else {
		skb_trim(first_skb, le16_to_cpu(shdr->len));
		esp_dbg(ESP_DBG_TRACE, " %s first_skb only\n", __func__);

		rx_skb = first_skb;
	}

	if (atomic_read(&sip->state) == SIP_STOP) {
		kfree_skb(rx_skb);
		esp_sip_dbg(ESP_DBG_ERROR, "%s when sip stopped\n", __func__);
		return 0;
	}

	sip_rx_pkt_enqueue(sip, rx_skb);
	sip_queue_work(sip, &sip->rx_process_work);

_err:
	return err;
}

int sip_post_init(struct esp_sip *sip, struct sip_evt_bootup2 *bevt)
{
	struct esp_pub *epub;

	u8 mac_id = bevt->mac_addr[0];
	int mac_index = 0;
	int i = 0;

	if (!sip) {
		ESSERT(0);
		return -EINVAL;
	}

	epub = sip->epub;

	sip->tx_aggr_write_ptr = sip->tx_aggr_buf;

	if ((le16_to_cpu(bevt->tx_blksz) & 3) ||
	    (le16_to_cpu(bevt->rx_blksz) & 3) ||
	    le16_to_cpu(bevt->tx_blksz) < 64 ||
	    le16_to_cpu(bevt->rx_blksz) < 64 ||
	    le16_to_cpu(bevt->tx_blksz) > 2048 ||
	    le16_to_cpu(bevt->rx_blksz) > 2048) {
		dev_err(epub->dev,
			"Invalid firmware block sizes: TX=%u RX=%u\n",
			le16_to_cpu(bevt->tx_blksz),
			le16_to_cpu(bevt->rx_blksz));
		return -EBADMSG;
	}
	sip->tx_blksz = le16_to_cpu(bevt->tx_blksz);
	sip->rx_blksz = le16_to_cpu(bevt->rx_blksz);
	sip->credit_to_reserve = bevt->credit_to_reserve;
	atomic64_set(&epub->stats[ESP_TX_CREDIT_BYTES], sip->tx_blksz);

	sip->dump_rpbm_err = (bevt->options & SIP_DUMP_RPBM_ERR);
	sip->rxabort_fixed = (bevt->options & SIP_RXABORT_FIXED);
	sip->support_bgscan = (bevt->options & SIP_SUPPORT_BGSCAN);

	sip->sendup_rpbm_pkt = sip->dump_rpbm_err && false;

	/* print out MAC addr... */
	memcpy(epub->mac_addr, bevt->mac_addr, ETH_ALEN);
	for (i = 0;
	     i < sizeof(esp_mac_prefix_table) / sizeof(struct esp_mac_prefix);
	     i++) {
		if (esp_mac_prefix_table[i].mac_index == mac_id) {
			mac_index = i;
			break;
		}
	}

	epub->mac_addr[0] = esp_mac_prefix_table[mac_index].mac_addr_prefix[0];
	epub->mac_addr[1] = esp_mac_prefix_table[mac_index].mac_addr_prefix[1];
	epub->mac_addr[2] = esp_mac_prefix_table[mac_index].mac_addr_prefix[2];

	atomic_set(&sip->noise_floor, (s16)le16_to_cpu(bevt->noise_floor));

	sip_recalc_credit_init(sip);

	esp_sip_dbg(ESP_DBG_TRACE, "%s tx_blksz %d rx_blksz %d mac addr %pM\n",
		    __func__, sip->tx_blksz, sip->rx_blksz, epub->mac_addr);

	return 0;
}

/* write pkts in aggr buf to target memory */
static void sip_write_pkts(struct esp_sip *sip, int pm_state, bool urgent_control)
{
	int tx_aggr_len = 0;
	struct sip_hdr *first_shdr = NULL;
	int err = 0;

	tx_aggr_len = sip->tx_aggr_write_ptr - sip->tx_aggr_buf;
	if (tx_aggr_len < sizeof(struct sip_hdr)) {
		dev_err(sip->epub->dev, "Invalid TX aggregate length %d\n",
			tx_aggr_len);
		ESSERT(0);
		return;
	}
	if ((tx_aggr_len & 0x3) != 0) {
		ESSERT(0);
		return;
	}

	first_shdr = (struct sip_hdr *)sip->tx_aggr_buf;

	/* Runtime data and plain cursor ACKs use normal credit recycling.
	 * Boot, explicit partial-flush polls and other control commands retain
	 * urgent requests, including when packed alongside runtime data.
	 */
	if ((!sip->reports.active || urgent_control) &&
	    atomic_read(&sip->tx_credits) <= SIP_CREDITS_LOW_THRESHOLD)
		first_shdr->fc[1] |= SIP_HDR_F_NEED_CRDT_RPT;

	/* Keep the aggregate and transport bookkeeping under one host claim. */

	sif_lock_bus(sip->epub);

	atomic64_inc(&sip->epub->stats[ESP_TX_TRANSFERS]);
	atomic64_add(tx_aggr_len, &sip->epub->stats[ESP_TX_BYTES]);

	err = esp_common_write(sip->epub, sip->tx_aggr_buf, tx_aggr_len,
			       ESP_SIF_NOSYNC);

	sip->tx_aggr_write_ptr = sip->tx_aggr_buf;
	sip->tx_tot_len = 0;

	if (err)
		sif_halt_transport(sip->epub, err);
	sif_unlock_bus(sip->epub);

	if (err) {
		atomic64_inc(&sip->epub->stats[ESP_TX_IO_ERRORS]);
		esp_sip_dbg(ESP_DBG_ERROR, "func %s err!!!!!!!!!: %d\n",
			    __func__, err);
		esp_reports_abort(sip);
	}
	sip_complete_tx(sip, !err);
}

/* setup sip header and tx info, copy pkt into aggr buf */
static int sip_pack_pkt(struct esp_sip *sip, struct sk_buff *skb, int *pm_state,
			bool control)
{
	struct ieee80211_tx_info *itx_info;
	struct sip_hdr *shdr;
	u32 tx_len = 0, offset = 0;
	bool is_data = !control;

	itx_info = IEEE80211_SKB_CB(skb);

	if (control) {
		shdr = (struct sip_hdr *)skb->data;
		esp_reports_packed_control(sip, skb);
		is_data = false;
		tx_len = skb->len;
	} else {
		struct ieee80211_hdr *wh = (struct ieee80211_hdr *)skb->data;
		const struct esp_tx_packet_info packet =
			*esp_tx_packet_info(skb);

		shdr = (void *)sip->tx_aggr_write_ptr;
		memset(shdr, 0, sizeof(*shdr));
		sip_hdr_set_type(&shdr->fc[0],
				 itx_info->flags & IEEE80211_TX_CTL_AMPDU ?
					 SIP_DATA_AMPDU :
					 SIP_DATA);
		shdr->d_flags = packet.enc_flags;
		shdr->d_hw_kid = packet.key_index | packet.vif_index << 7;
		sip_hdr_set_ifidx(&shdr->fc[0],
				  packet.vif_index << 3 | packet.sta_index);
		shdr->d_ac = skb_get_queue_mapping(skb);
		shdr->d_tid = skb->priority & IEEE80211_QOS_CTL_TAG1D_MASK;
		wh = (struct ieee80211_hdr *)skb->data;
		if (ieee80211_is_mgmt(wh->frame_control)) {
			/* addba/delba/bar may use different tid/ac */
			if (shdr->d_ac == WME_AC_VO)
				shdr->d_tid = 7;
			if (ieee80211_is_beacon(wh->frame_control)) {
				shdr->d_tid = 8;
				shdr->d_ac = 4;
			}
		}
		if (check_ac_tid(skb->data, shdr->d_ac, shdr->d_tid)) {
			shdr->d_ac = WME_AC_BE;
			shdr->d_tid = 0;
		}

		/* make sure data is start at 4 bytes aligned addr. */
		offset = roundup(sizeof(struct sip_hdr), 4);

		if (SIP_HDR_IS_AMPDU(shdr)) {
			memset(sip->tx_aggr_write_ptr + offset, 0,
			       sizeof(struct esp_tx_ampdu_entry));
			offset += roundup(sizeof(struct esp_tx_ampdu_entry), 4);
		}

		tx_len = offset + skb->len;
		shdr->len = cpu_to_le16(tx_len); /* actual len */

		esp_sip_dbg(ESP_DBG_TRACE, "%s offset %d skblen %d txlen %d\n",
			    __func__, offset, skb->len, tx_len);
	}

	shdr->seq = cpu_to_le32(sip->txseq);

	/* copy skb to aggr buf */
	if (skb_copy_bits(skb, 0, sip->tx_aggr_write_ptr + offset, skb->len))
		return -EINVAL;
	sip->txseq++;

	if (is_data) {
		spin_lock_bh(&sip->epub->tx_lock);
		sip->txdataseq = le32_to_cpu(shdr->seq);
		spin_unlock_bh(&sip->epub->tx_lock);
		/* Keep runtime packets until observed firmware radio completion.
		 * The transport-only fallback does not advertise TX ACK status.
		 */
		if (sip->reports.active)
			esp_reports_submit(sip, skb, le32_to_cpu(shdr->seq));
		else
			skb_queue_tail(&sip->epub->txdoneq, skb);
	} else {
		/* check pm state here */

		/* no need to hold ctrl skb */
		sip_free_ctrl_skbuff(sip, skb);
	}

	/* TBD: roundup here or whole aggr-buf */
	memset(sip->tx_aggr_write_ptr + tx_len, 0,
	       roundup(tx_len, sip->tx_blksz) - tx_len);
	tx_len = roundup(tx_len, sip->tx_blksz);

	sip->tx_aggr_write_ptr += tx_len;
	sip->tx_tot_len += tx_len;

	return 0;
}

static void sip_tx_status_report(struct esp_sip *sip, struct sk_buff *skb,
				 struct ieee80211_tx_info *tx_info,
				 bool success)
{
	u32 flags = tx_info->flags;

	ieee80211_tx_info_clear_status(tx_info);
	tx_info->status.rates[0].idx = -1;
	if (success && !(flags & IEEE80211_TX_CTL_NO_ACK))
		tx_info->flags |= IEEE80211_TX_STAT_ACK;
	if (success && (flags & IEEE80211_TX_CTL_NO_ACK))
		tx_info->flags |= IEEE80211_TX_STAT_NOACK_TRANSMITTED;

	if (success)
		sip_maybe_start_tx_ba(sip, skb);
	ieee80211_tx_status_ni(sip->epub->hw, skb);
}

void sip_maybe_start_tx_ba(struct esp_sip *sip, struct sk_buff *skb)
{
	if (atomic_read(&sip->state) == SIP_RUN &&
	    cfg80211_get_chandef_type(&sip->epub->hw->conf.chandef) !=
		    NL80211_CHAN_NO_HT) {
		struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
		struct ieee80211_hdr *wh = (struct ieee80211_hdr *)skb->data;

		if (ieee80211_is_data_qos(wh->frame_control)) {
			if (!(tx_info->flags & IEEE80211_TX_CTL_AMPDU)) {
				u8 tidno = ieee80211_get_qos_ctl(wh)[0] &
					   IEEE80211_QOS_CTL_TID_MASK;
				struct esp_node *node;
				struct esp_tx_tid *tid;
				struct ieee80211_sta *sta;

				node = esp_get_node_by_addr(sip->epub,
							    wh->addr1);
				if (!node)
					goto _exit;
				if (!node->sta)
					goto _exit;
				sta = node->sta;
				tid = &node->tid[tidno];
				spin_lock_bh(&sip->epub->tx_ampdu_lock);

				if (tid->state == ESP_TID_STATE_INIT &&
				    (tid_to_ac(tidno) != WME_AC_VO) &&
				    tid->cnt >= 10) {
					tid->state = ESP_TID_STATE_TRIGGER;
					esp_sip_dbg(ESP_DBG_ERROR,
						    "start tx ba session,addr:%pM,tid:%u\n",
						wh->addr1, tidno);
					spin_unlock_bh(&sip->epub->tx_ampdu_lock);
					ieee80211_start_tx_ba_session(sta,
								      tidno, 0);
				} else {
					if (tid->state == ESP_TID_STATE_INIT)
						tid->cnt++;
					else
						tid->cnt = 0;
					spin_unlock_bh(&sip->epub->tx_ampdu_lock);
				}
			}
		}
	}
_exit:
	return;
}

/*
 *  NB: this routine should be locked when calling
 */
void sip_txq_process(struct esp_pub *epub)
{
	struct sk_buff *skb;
	struct esp_sip *sip = epub->sip;
	u32 pkt_len = 0, tx_len = 0;
	int blknum = 0;
	bool queued_back = false;
	bool out_of_credits = false;
	struct ieee80211_tx_info *itx_info;
	int pm_state = 0;
	bool control = false;
	bool urgent_control = false, current_urgent;
	u32 data_count = 0, control_count = 0;

	for (;;) {
		skb = skb_dequeue(&epub->ctrlq);
		control = !!skb;
		if (!skb)
			skb = skb_dequeue(&epub->txq);
		if (!skb)
			break;
		if (!control && (esp_reports_full(&sip->reports) ||
				 READ_ONCE(sip->power.blocked))) {
			queued_back = true;
			out_of_credits = true;
			break;
		}
		/* cmd skb->len does not include sip_hdr too */
		pkt_len = skb->len;
		itx_info = IEEE80211_SKB_CB(skb);
		if (!control) {
			pkt_len += roundup(sizeof(struct sip_hdr), 4);
			if (itx_info->flags & IEEE80211_TX_CTL_AMPDU)
				pkt_len += roundup(sizeof(struct esp_tx_ampdu_entry), 4);
		}

		/* Firmware requires each SIP packet to begin at a block boundary. */

		pkt_len = roundup(pkt_len, sip->tx_blksz);
		blknum = pkt_len / sip->tx_blksz;
		esp_dbg(ESP_DBG_TRACE, "%s skb_len %d pkt_len %d blknum %d\n",
			__func__, skb->len, pkt_len, blknum);

		if (unlikely(atomic_read(&sip->credit_status) ==
			     RECALC_CREDIT_ENABLE)) { /* need recalc credit */
			struct sip_hdr *hdr = (struct sip_hdr *)skb->data;

			itx_info = IEEE80211_SKB_CB(skb);
			if (!(control &&
			      SIP_HDR_GET_TYPE(hdr->fc[0]) == SIP_CTRL &&
			      hdr->c_cmdid ==
				      cpu_to_le32(SIP_CMD_RECALC_CREDIT) &&
			      blknum <= atomic_read(&sip->tx_credits) -
						sip->credit_to_reserve)) {
				esp_dbg(ESP_DBG_ERROR, "%s recalc credits!\n",
					__func__);
				queued_back = true;
				out_of_credits = true;

				break;
			}
		} else { /* normal situation */
			if (unlikely(blknum > (atomic_read(&sip->tx_credits) -
					       sip->credit_to_reserve -
					       SIP_CTRL_CREDIT_RESERVE))) {
				itx_info = IEEE80211_SKB_CB(skb);
				if (control) { /* priv ctrl pkt */
					if (blknum >
					    atomic_read(&sip->tx_credits) -
						    sip->credit_to_reserve) {
						esp_dbg(ESP_DBG_TRACE,
							"%s cmd pkt out of credits!\n",
							__func__);
						queued_back = true;
						out_of_credits = true;

						break;
					}
				} else {
					esp_dbg(ESP_DBG_TRACE,
						"%s out of credits!\n",
						__func__);
					queued_back = true;
					out_of_credits = true;

					break;
				}
			}
		}
		tx_len += pkt_len;
		if (tx_len >= SIP_TX_AGGR_BUF_SIZE) {
			/* do we need to have limitation likemax 8 pkts in a row? */
			esp_dbg(ESP_DBG_TRACE,
				"%s too much pkts in one shot!\n", __func__);
			tx_len -= pkt_len;
			queued_back = true;

			break;
		}

		current_urgent = control && !esp_reports_is_ack_only(sip, skb);
		rcu_read_lock();
		if (sip_pack_pkt(sip, skb, &pm_state, control) != 0) {
			rcu_read_unlock();
			/* wrong pkt, won't send to target */
			tx_len -= pkt_len;
			if (control) {
				kfree_skb(skb);
			} else {
				atomic_dec(&sip->tx_data_pkt_queued);
				ieee80211_free_txskb(epub->hw, skb);
			}
			continue;
		}

		rcu_read_unlock();
		if (control) {
			control_count++;
			urgent_control |= current_urgent;
		} else {
			data_count++;
		}
		esp_sip_dbg(ESP_DBG_TRACE, "%s:before sub, credits is %d\n",
			    __func__, atomic_read(&sip->tx_credits));
		atomic_sub(blknum, &sip->tx_credits);
		esp_sip_dbg(ESP_DBG_TRACE,
			    "%s:after sub %d,credits remains %d\n", __func__,
			    blknum, atomic_read(&sip->tx_credits));
	}
	atomic64_add(data_count, &epub->stats[ESP_TX_DATA_PACKETS]);
	atomic64_add(control_count, &epub->stats[ESP_TX_CTRL_PACKETS]);

	if (queued_back)
		skb_queue_head(control ? &epub->ctrlq : &epub->txq, skb);

	if (atomic_read(&sip->state) == SIP_STOP) {
		queued_back = false;
		tx_len = 0;
		sip_complete_tx(sip, false);
		sip->tx_aggr_write_ptr = sip->tx_aggr_buf;
		sip->tx_tot_len = 0;
	}

	if (tx_len)
		sip_write_pkts(sip, pm_state, urgent_control);

	if (queued_back && !out_of_credits) {
		/* skb pending, do async process again */
		sip_trigger_txq_process(sip);
	}
}

static void sip_complete_tx(struct esp_sip *sip, bool success)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&sip->epub->txdoneq))) {
		rcu_read_lock();
		sip_tx_status_report(sip, skb, IEEE80211_SKB_CB(skb), success);
		rcu_read_unlock();
		atomic_dec(&sip->tx_data_pkt_queued);
	}
}

/*  parse mac_rx_ctrl and return length */
static int sip_parse_mac_rx_info(struct esp_sip *sip,
				 struct esp_mac_rx_ctrl *mac_ctrl,
				 struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx_status = NULL;
	struct ieee80211_hdr *hdr = (void *)skb->data;

	rx_status = IEEE80211_SKB_RXCB(skb);
	memset(rx_status, 0, sizeof(*rx_status));
	if (mac_ctrl->channel < 1 || mac_ctrl->channel > 14 ||
	    (mac_ctrl->sig_mode && mac_ctrl->mcs > 7))
		return -EINVAL;
	rx_status->freq = esp_ieee2mhz(mac_ctrl->channel);

	rx_status->signal =
		mac_ctrl->rssi +
		mac_ctrl->noise_floor; /* snr actually, need to offset noise floor e.g. -85 */

	rx_status->antenna = 0; /* one antenna for now */
	rx_status->band = NL80211_BAND_2GHZ;
	/* Firmware retains the IV, but removes WEP/TKIP ICVs, the TKIP
	 * Michael MIC and the CCMP MIC. Keep the IV for mac80211 replay checks.
	 * Crypto trailers must not be trimmed a second time by mac80211.
	 */
	if (ieee80211_has_protected(hdr->frame_control))
		rx_status->flag = RX_FLAG_DECRYPTED | RX_FLAG_MMIC_STRIPPED |
				  RX_FLAG_MIC_STRIPPED | RX_FLAG_ICV_STRIPPED;
	if (mac_ctrl->sig_mode) {
		rx_status->encoding = RX_ENC_HT;
		rx_status->rate_idx = mac_ctrl->mcs;
		rx_status->enc_flags |= mac_ctrl->stbc << RX_ENC_FLAG_STBC_SHIFT;
		if (mac_ctrl->sgi)
			rx_status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
	} else {
		rx_status->rate_idx = esp_wmac_rate2idx(mac_ctrl->rate);
	}
	if (mac_ctrl->rxend_state == RX_FCS_ERR)
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;

	/* mac80211 trusts DECRYPTED and skips the WEP/TKIP ICV check.
	 * Reject every hardware security error here, including non-aggregated
	 * RX. TKIP Michael failures must reach its countermeasure handler.
	 */
	if (esp_wmac_rxsec_error(mac_ctrl->rxend_state) &&
	    mac_ctrl->rxend_state != RX_TKIPMIC_ERR)
		return -EBADMSG;
	if (mac_ctrl->rxend_state == RX_TKIPMIC_ERR)
		rx_status->flag |= RX_FLAG_MMIC_ERROR;

	/* Neither a missing key nor plaintext means the IV was stripped.
	 * mac80211 must still select a key, reject unexpected plaintext and
	 * validate retained packet numbers. The old 3.0-era EAPOL workaround
	 * bypassed those checks and mislabeled control-port encryption status.
	 */

	return 0;
}

/*
 * for one MPDU (including subframe in AMPDU)
 *
 */
VISIBLE_IF_KUNIT struct sk_buff *sip_parse_data_rx_info(struct esp_sip *sip,
					      struct sk_buff *skb,
					      int pkt_len_enc, int buf_len,
					      struct esp_mac_rx_ctrl *mac_ctrl,
					      int *pulled_len)
{
	struct sk_buff *frame;
	struct ieee80211_hdr *hdr = (void *)skb->data;
	int pkt_len;

	if (pkt_len_enc < 10 || pkt_len_enc > 4095 || skb->len < 10) {
		dev_warn_ratelimited(sip->epub->dev,
				     "Invalid RX frame size %d, remaining %u\n",
				     pkt_len_enc, skb->len);
		goto drop;
	}
	if (mac_ctrl->aggregation) {
		pkt_len = pkt_len_enc;
		if (ieee80211_has_protected(hdr->frame_control))
			pkt_len -= 8; /* firmware removed CCMP MIC */
		buf_len = ALIGN(pkt_len, 4);
	} else {
		pkt_len = buf_len - 3 + ((pkt_len_enc - 1) & 3);
	}
	if (pkt_len < 10 || pkt_len > pkt_len_enc || pkt_len > buf_len ||
	    buf_len > skb->len ||
	    pkt_len < ieee80211_hdrlen(hdr->frame_control))
		goto drop;
	frame = dev_alloc_skb(pkt_len);
	if (!frame)
		goto drop;
	/* Copy only stored bytes. Reconstructing the on-air crypto trailers
	 * leaves eight bogus payload bytes for TKIP with MMIC_STRIPPED set.
	 */
	skb_put_data(frame, skb->data, pkt_len);
	skb_pull(skb, buf_len);
	*pulled_len += buf_len;
	if (sip_parse_mac_rx_info(sip, mac_ctrl, frame)) {
		kfree_skb(frame);
		goto drop;
	}
	return frame;
drop:
	atomic64_inc(&sip->epub->stats[ESP_RX_FRAME_DROPS]);
	return NULL;
}
EXPORT_SYMBOL_IF_KUNIT(sip_parse_data_rx_info);

struct esp_sip *sip_attach(struct esp_pub *epub)
{
	struct esp_sip *sip = NULL;
	struct sip_pkt *pkt = NULL;
	int i;
	int po = 0;

	sip = kzalloc_obj(struct esp_sip, GFP_KERNEL);
	if (!sip)
		goto _err_sip;
	po = get_order(SIP_TX_AGGR_BUF_SIZE);
	sip->tx_aggr_buf = (u8 *)__get_free_pages(GFP_KERNEL, po);
	if (!sip->tx_aggr_buf)
		goto _err_aggr;

	timer_setup(&sip->credit_timer, sip_recalc_credit_timeout, 0);
	spin_lock_init(&sip->lock);
	spin_lock_init(&sip->state_lock);

	INIT_LIST_HEAD(&sip->free_ctrl_txbuf);
	INIT_LIST_HEAD(&sip->free_ctrl_rxbuf);

	for (i = 0; i < SIP_CTRL_BUF_N; i++) {
		pkt = kzalloc_obj(struct sip_pkt, GFP_KERNEL);

		if (!pkt)
			goto _err_pkt;

		pkt->buf_begin = kzalloc(SIP_CTRL_BUF_SZ, GFP_KERNEL);

		if (!pkt->buf_begin) {
			kfree(pkt);
			pkt = NULL;
			goto _err_pkt;
		}

		pkt->buf_len = SIP_CTRL_BUF_SZ;
		pkt->buf = pkt->buf_begin;

		if (i < SIP_CTRL_TXBUF_N)
			list_add_tail(&pkt->list, &sip->free_ctrl_txbuf);
		else
			list_add_tail(&pkt->list, &sip->free_ctrl_rxbuf);
	}

	mutex_init(&sip->rx_mtx);
	skb_queue_head_init(&sip->rxq);
	INIT_WORK(&sip->rx_process_work, sip_rxq_process);

	sip->epub = epub;
	esp_reports_init(sip);
	esp_power_init(sip);
	atomic_set(&sip->noise_floor, -96);

	atomic_set(&sip->state, SIP_INIT);
	atomic_set(&sip->tx_credits, 0);

	if (!sip->rawbuf) {
		sip->rawbuf = kzalloc(SIP_BOOT_BUF_SIZE, GFP_KERNEL);
		if (!sip->rawbuf)
			goto _err_pkt;
	}

	atomic_set(&sip->state, SIP_PREPARE_BOOT);

	return sip;

_err_pkt:
	sip_free_init_ctrl_buf(sip);

	if (sip->tx_aggr_buf) {
		po = get_order(SIP_TX_AGGR_BUF_SIZE);
		free_pages((unsigned long)sip->tx_aggr_buf, po);
		sip->tx_aggr_buf = NULL;
	}
_err_aggr:
	kfree(sip);
_err_sip:
	return NULL;
}

static void sip_free_init_ctrl_buf(struct esp_sip *sip)
{
	struct sip_pkt *pkt, *tpkt;

	list_for_each_entry_safe(pkt, tpkt, &sip->free_ctrl_txbuf, list) {
		list_del(&pkt->list);
		kfree(pkt->buf_begin);
		kfree(pkt);
	}

	list_for_each_entry_safe(pkt, tpkt, &sip->free_ctrl_rxbuf, list) {
		list_del(&pkt->list);
		kfree(pkt->buf_begin);
		kfree(pkt);
	}
}

void sip_quiesce(struct esp_sip *sip)
{
	struct esp_pub *epub = sip->epub;
	struct sk_buff *skb;

	atomic_set(&epub->wl.off, 1);
	sip_stop(sip);
	WRITE_ONCE(sip->reports.failed, true);
	WRITE_ONCE(sip->power.blocked, true);
	WRITE_ONCE(sip->power.status, -ESHUTDOWN);
	complete_all(&sip->power.done);
	wake_up_all(&sip->power.drain);

	sif_disable_target_interrupt(epub);
	sif_disable_irq(epub);
	/* Drain TX callbacks that observed wl.off before STOP was published. */
	synchronize_net();
	cancel_delayed_work_sync(&sip->reports.poll);
	timer_delete_sync(&sip->credit_timer);
	cancel_work_sync(&sip->rx_process_work);
	cancel_work_sync(&epub->tx_work);

	/* External power RPCs must finish before reusing their completions. */
	mutex_lock(&sip->power.lock);
	mutex_unlock(&sip->power.lock);
	esp_reports_abort(sip);
	skb_queue_purge(&sip->rxq);
	while ((skb = skb_dequeue(&epub->txq)))
		ieee80211_free_txskb(epub->hw, skb);
	skb_queue_purge(&epub->ctrlq);
	while ((skb = skb_dequeue(&epub->txdoneq)))
		ieee80211_free_txskb(epub->hw, skb);
	atomic_set(&sip->tx_data_pkt_queued, 0);
}

int sip_prepare_boot(struct esp_sip *sip)
{
	struct esp_reports *r = &sip->reports;

	/* Called only after quiescing. Retain locks, work objects and cumulative
	 * statistics; reset protocol state belonging to the discarded firmware.
	 */
	if (!sip->rawbuf) {
		sip->rawbuf = kzalloc(SIP_BOOT_BUF_SIZE, GFP_KERNEL);
		if (!sip->rawbuf)
			return -ENOMEM;
	}
	r->active = false;
	r->failed = false;
	r->submitted = 0;
	r->acked = 0;
	r->next = 0;
	r->capacity = 0;
	r->progress = jiffies;
	r->replay_at = 0;
	r->replay_delay_ms = 100;
	reinit_completion(&r->ready);
	sip->rxseq = 0;
	sip->txseq = 0;
	sip->txdataseq = 0;
	sip->to_host_seq = 0;
	atomic_set(&sip->tx_credits, 0);
	atomic_set(&sip->tx_ask_credit_update, 0);
	atomic_set(&sip->credit_status, RECALC_CREDIT_DISABLE);
	atomic_set(&sip->rx_handling, 0);
	atomic_set(&sip->data_tx_stopped, 0);
	atomic_set(&sip->tx_stopped, 0);
	sip->tx_aggr_write_ptr = sip->tx_aggr_buf;
	sip->tx_aggr_lastpkt_ptr = NULL;
	sip->tx_tot_len = 0;
	sip->rx_tot_len = 0;
	sip->tx_blksz = 0;
	sip->rx_blksz = 0;
	sip->credit_to_reserve = 0;
	WRITE_ONCE(sip->power.pending, false);
	WRITE_ONCE(sip->power.blocked, true);
	spin_lock_bh(&sip->state_lock);
	atomic_set(&sip->state, SIP_PREPARE_BOOT);
	spin_unlock_bh(&sip->state_lock);
	return 0;
}

void sip_detach(struct esp_sip *sip)
{
	struct esp_pub *epub;

	if (!sip)
		return;
	epub = sip->epub;
	sip_quiesce(sip);

	timer_shutdown_sync(&sip->credit_timer);
	if (test_and_clear_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags)) {
		dev_info(epub->dev, "Unregistering mac80211\n");
		ieee80211_unregister_hw(epub->hw);
	}
	/* unregister_hw waits for TX callers that raced the shutdown flag. */
	cancel_work_sync(&epub->tx_work);
	skb_queue_purge(&sip->rxq);
	while (!skb_queue_empty(&epub->txq)) {
		struct sk_buff *skb = skb_dequeue(&epub->txq);

		ieee80211_free_txskb(epub->hw, skb);
	}
	skb_queue_purge(&epub->ctrlq);
	atomic_set(&sip->tx_data_pkt_queued, 0);
	while (!skb_queue_empty(&epub->txdoneq)) {
		struct sk_buff *skb = skb_dequeue(&epub->txdoneq);

		ieee80211_free_txskb(epub->hw, skb);
	}
	sip_free_init_ctrl_buf(sip);
	mutex_destroy(&sip->rx_mtx);
	kfree(sip->rawbuf);
	free_pages((unsigned long)sip->tx_aggr_buf,
		   get_order(SIP_TX_AGGR_BUF_SIZE));
	kfree(sip);
}

int sip_write_memory(struct esp_sip *sip, u32 addr, const u8 *buf, u32 len)
{
	struct sip_cmd_write_memory *cmd;
	struct sip_hdr *chdr;
	u32 remains, hdrs, bufsize, copied;
	u32 loadaddr;
	const u8 *src;
	int err = 0;
	u32 *t = NULL;

	if (!sip || !sip->rawbuf || !buf || !len)
		return -EINVAL;

	memset(sip->rawbuf, 0, SIP_BOOT_BUF_SIZE);

	chdr = (struct sip_hdr *)sip->rawbuf;
	sip_hdr_set_type(&chdr->fc[0], SIP_CTRL);
	chdr->c_cmdid = cpu_to_le32(SIP_CMD_WRITE_MEMORY);

	remains = len;
	hdrs = sizeof(struct sip_hdr) + sizeof(struct sip_cmd_write_memory);

	while (remains) {
		src = &buf[len - remains];
		loadaddr = addr + (len - remains);

		if (remains < (SIP_BOOT_BUF_SIZE - hdrs)) {
			/* aligned with 4 bytes */
			bufsize = roundup(remains, 4);
			memset(sip->rawbuf + hdrs, 0, bufsize);
			remains = 0;
		} else {
			bufsize = SIP_BOOT_BUF_SIZE - hdrs;
			remains -= bufsize;
		}

		chdr->len = cpu_to_le16(bufsize + hdrs);
		chdr->seq = cpu_to_le32(sip->txseq++);
		cmd = (struct sip_cmd_write_memory *)(sip->rawbuf +
						      SIP_CTRL_HDR_LEN);
		cmd->len = cpu_to_le32(bufsize);
		cmd->addr = cpu_to_le32(loadaddr);
		copied = min_t(u32, len - (loadaddr - addr), bufsize);
		memcpy(sip->rawbuf + hdrs, src, copied);

		t = (u32 *)sip->rawbuf;
		esp_dbg(ESP_DBG_TRACE,
			"%s t0: 0x%08x t1: 0x%08x t2:0x%08x loadaddr 0x%08x\n",
			__func__, t[0], t[1], t[2], loadaddr);

		err = esp_common_write(sip->epub, sip->rawbuf,
				       le16_to_cpu(chdr->len), ESP_SIF_SYNC);

		if (err) {
			esp_dbg(ESP_DBG_ERROR, "%s send buffer failed\n",
				__func__);
			return err;
		}
		mdelay(1);
	}

	return err;
}

int sip_send_cmd(struct esp_sip *sip, int cid, u32 cmdlen, void *cmd)
{
	struct sip_hdr *chdr;
	struct sip_pkt *pkt = NULL;
	int ret = 0;

	pkt = sip_get_ctrl_buf(sip, SIP_TX_CTRL_BUF);

	if (!pkt)
		return -ENOMEM;

	chdr = (struct sip_hdr *)pkt->buf_begin;
	chdr->len = cpu_to_le16(SIP_CTRL_HDR_LEN + cmdlen);
	chdr->seq = cpu_to_le32(sip->txseq++);
	chdr->c_cmdid = cpu_to_le32(cid);

	if (cmd) {
		memset(pkt->buf, 0, cmdlen);
		memcpy(pkt->buf, (u8 *)cmd, cmdlen);
	}

	esp_dbg(ESP_DBG_TRACE, "cid %d, len %u, seq %u\n", chdr->c_cmdid,
		chdr->len, chdr->seq);

	esp_dbg(ESP_DBG_TRACE, "c1 0x%08x   c2 0x%08x\n", *(u32 *)&pkt->buf[0],
		*(u32 *)&pkt->buf[4]);

	ret = esp_common_write(sip->epub, pkt->buf_begin,
			       le16_to_cpu(chdr->len), ESP_SIF_SYNC);

	if (ret)
		esp_dbg(ESP_DBG_ERROR, "%s send cmd %d failed\n", __func__,
			cid);

	sip_reclaim_ctrl_buf(sip, pkt, SIP_TX_CTRL_BUF);

	/*
	 *  Hack here: reset tx/rx seq before target ram code is up...
	 */
	if (cid == SIP_CMD_BOOTUP) {
		sip->rxseq = 0;
		sip->txseq = 0;
		sip->txdataseq = 0;
	}

	return ret;
}

struct sk_buff *sip_alloc_ctrl_skbuf(struct esp_sip *sip, u16 len, u32 cid)
{
	struct sip_hdr *si = NULL;
	struct sk_buff *skb = NULL;

	KUNIT_STATIC_STUB_REDIRECT(sip_alloc_ctrl_skbuf, sip, len, cid);

	/* Configuration callbacks can arrive after a failed recovery. They
	 * must not allocate or warn against the discarded firmware's geometry.
	 */
	if (atomic_read(&sip->state) == SIP_STOP ||
	    len < sizeof(struct sip_hdr) || len > sip->tx_blksz)
		return NULL;

	/* no need to reserve space for net stack */
	/* Credit timer and aggregation callbacks can run in atomic context. */
	skb = __dev_alloc_skb(len, GFP_ATOMIC);

	if (!skb)
		return NULL;

	skb_put_zero(skb, len);

	si = (struct sip_hdr *)skb->data;
	memset(si, 0, sizeof(struct sip_hdr));
	sip_hdr_set_type(&si->fc[0], SIP_CTRL);
	si->len = cpu_to_le16(len);
	si->c_cmdid = cpu_to_le32(cid);

	return skb;
}
EXPORT_SYMBOL_IF_KUNIT(sip_alloc_ctrl_skbuf);

void sip_free_ctrl_skbuff(struct esp_sip *sip, struct sk_buff *skb)
{
	memset(IEEE80211_SKB_CB(skb), 0, sizeof(struct ieee80211_tx_info));
	kfree_skb(skb);
}

static struct sip_pkt *sip_get_ctrl_buf(struct esp_sip *sip,
					enum sip_buf_type bftype)
{
	struct sip_pkt *pkt = NULL;
	struct list_head *bflist;
	struct sip_hdr *chdr;

	bflist = (bftype == SIP_TX_CTRL_BUF) ? &sip->free_ctrl_txbuf :
					       &sip->free_ctrl_rxbuf;

	spin_lock_bh(&sip->lock);

	if (list_empty(bflist)) {
		spin_unlock_bh(&sip->lock);
		return NULL;
	}

	pkt = list_first_entry(bflist, struct sip_pkt, list);
	list_del(&pkt->list);
	spin_unlock_bh(&sip->lock);

	if (bftype == SIP_TX_CTRL_BUF) {
		chdr = (struct sip_hdr *)pkt->buf_begin;
		sip_hdr_set_type(&chdr->fc[0], SIP_CTRL);
		pkt->buf = pkt->buf_begin + SIP_CTRL_HDR_LEN;
	} else {
		pkt->buf = pkt->buf_begin;
	}

	return pkt;
}

static void sip_reclaim_ctrl_buf(struct esp_sip *sip, struct sip_pkt *pkt,
				 enum sip_buf_type bftype)
{
	struct list_head *bflist = NULL;

	if (bftype == SIP_TX_CTRL_BUF)
		bflist = &sip->free_ctrl_txbuf;
	else if (bftype == SIP_RX_CTRL_BUF)
		bflist = &sip->free_ctrl_rxbuf;
	else
		return;

	pkt->buf = pkt->buf_begin;

	spin_lock_bh(&sip->lock);
	list_add_tail(&pkt->list, bflist);
	spin_unlock_bh(&sip->lock);
}

int sip_poll_bootup_event(struct esp_sip *sip)
{
	int ret = 0;

	esp_dbg(ESP_DBG_TRACE, "polling bootup event...\n");

	ret = wait_for_completion_timeout(&sip->epub->boot_completion, 2 * HZ);

	esp_dbg(ESP_DBG_TRACE, "******time remain****** = [%d]\n", ret);
	if (ret <= 0) {
		esp_dbg(ESP_DBG_ERROR, "bootup event timeout\n");
		return -ETIMEDOUT;
	}

	if (sip->epub->boot_error)
		return sip->epub->boot_error;
	ret = esp_reports_negotiate(sip);
	if (ret)
		return ret;
	ret = esp_power_negotiate(sip);
	if (ret)
		return ret;
	if (!test_bit(ESP_WL_FLAG_HW_REGISTERED, &sip->epub->wl.flags)) {
		ret = esp_register_mac80211(sip->epub);
		if (ret)
			return ret;
	}

	if (!sip_set_state(sip, SIP_RUN))
		return -ESHUTDOWN;
	esp_dbg(ESP_DBG_TRACE, "target booted up\n");

	return ret;
}

int sip_poll_resetting_event(struct esp_sip *sip)
{
	int ret = 0;

	esp_dbg(ESP_DBG_TRACE, "polling resetting event...\n");

	ret = wait_for_completion_timeout(&sip->epub->boot_completion, 10 * HZ);

	esp_dbg(ESP_DBG_TRACE, "******time remain****** = [%d]\n", ret);
	if (ret <= 0) {
		esp_dbg(ESP_DBG_ERROR, "resetting event timeout\n");
		return -ETIMEDOUT;
	}
	if (sip->epub->boot_error)
		return sip->epub->boot_error;

	esp_dbg(ESP_DBG_TRACE, "target resetting %d %p\n", ret,
		&sip->epub->boot_completion);

	return 0;
}

bool sip_queue_need_stop(struct esp_sip *sip)
{
	return atomic_read(&sip->tx_data_pkt_queued) >=
		       SIP_STOP_QUEUE_THRESHOLD ||
	       (atomic_read(&sip->tx_credits) < 8 &&
		atomic_read(&sip->tx_data_pkt_queued) >=
			SIP_STOP_QUEUE_THRESHOLD / 4 * 3);
}

bool sip_queue_may_resume(struct esp_sip *sip)
{
	return atomic_read(&sip->epub->txq_stopped) &&
	       !READ_ONCE(sip->epub->recovery.pending) &&
	       !READ_ONCE(sip->power.blocked) &&
	       !READ_ONCE(sip->reports.failed) &&
	       !test_bit(ESP_WL_FLAG_STOP_TXQ, &sip->epub->wl.flags) &&
	       ((atomic_read(&sip->tx_credits) >= 16 &&
		 atomic_read(&sip->tx_data_pkt_queued) <
			 SIP_RESUME_QUEUE_THRESHOLD * 2) ||
		atomic_read(&sip->tx_data_pkt_queued) <
			SIP_RESUME_QUEUE_THRESHOLD);
}

int sip_cmd_enqueue(struct esp_sip *sip, struct sk_buff *skb, int prior)
{
	int ret = 0;

	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) == SIP_STOP) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (prior == ENQUEUE_PRIOR_HEAD)
		skb_queue_head(&sip->epub->ctrlq, skb);
	else
		skb_queue_tail(&sip->epub->ctrlq, skb);
	queue_work(sip->epub->esp_wkq, &sip->epub->tx_work);
out:
	spin_unlock_bh(&sip->state_lock);
	if (ret)
		kfree_skb(skb);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(sip_cmd_enqueue);

void sip_tx_data_pkt_enqueue(struct esp_pub *epub, struct sk_buff *skb)
{
	struct esp_sip *sip = epub->sip;

	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) == SIP_STOP) {
		spin_unlock_bh(&sip->state_lock);
		ieee80211_free_txskb(epub->hw, skb);
		return;
	}
	/* Publish the count before the worker can dequeue and complete it. */
	atomic_inc(&sip->tx_data_pkt_queued);
	skb_queue_tail(&epub->txq, skb);
	if (sip_queue_need_stop(sip)) {
		ieee80211_stop_queues(epub->hw);
		atomic_set(&epub->txq_stopped, true);
	}
	spin_unlock_bh(&sip->state_lock);
}

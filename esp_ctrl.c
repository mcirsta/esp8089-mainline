// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2009 - 2014 Espressif System. */
#include <net/mac80211.h>
#include <net/cfg80211.h>
#include <linux/skbuff.h>
#include <linux/bitops.h>
#include <linux/firmware.h>

#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"
#include "esp_sif.h"
#include "esp_debug.h"
#include "esp_wmac.h"
#include "esp_utils.h"
#include "esp_wl.h"
#include "esp_file.h"
#include <linux/unaligned.h>

static void esp_tx_ba_session_op(struct esp_sip *sip, struct esp_node *node,
				 enum trc_ampdu_state state, u8 tid)
{
	struct esp_tx_tid *txtid = &node->tid[tid];
	bool stop = false, start = false, operational = false;

	spin_lock_bh(&sip->epub->tx_ampdu_lock);
	if (state == TRC_TX_AMPDU_STOPPED &&
	    txtid->state == ESP_TID_STATE_OPERATIONAL) {
		txtid->state = ESP_TID_STATE_WAIT_STOP;
		stop = true;
	} else if (state == TRC_TX_AMPDU_OPERATIONAL) {
		if (txtid->state == ESP_TID_STATE_STOP) {
			txtid->state = ESP_TID_STATE_TRIGGER;
			start = true;
		} else if (txtid->state == ESP_TID_STATE_OPERATIONAL) {
			operational = true;
		}
	}
	spin_unlock_bh(&sip->epub->tx_ampdu_lock);

	if (stop)
		ieee80211_stop_tx_ba_session(node->sta, tid);
	else if (start)
		ieee80211_start_tx_ba_session(node->sta, tid, 0);
	else if (operational)
		sip_send_ampdu_action(sip->epub, SIP_AMPDU_TX_OPERATIONAL,
				      node->sta->addr, tid, node->ifidx, 0);
}

int sip_parse_events(struct esp_sip *sip, u8 *buf)
{
	struct sip_hdr *hdr = (struct sip_hdr *)buf;
	size_t payload, needed = 0;

	if (le16_to_cpu(hdr->len) < SIP_CTRL_HDR_LEN)
		return -EBADMSG;
	payload = le16_to_cpu(hdr->len) - SIP_CTRL_HDR_LEN;
	switch (hdr->c_evtid) {
	case SIP_EVT_TX_STATUS:
		needed = sizeof(__le32);
		break;
	case SIP_EVT_BOOTUP:
		needed = sizeof(struct sip_evt_bootup2);
		break;
	case SIP_EVT_SCAN_RESULT:
		needed = sizeof(struct sip_evt_scan_report);
		break;
	case SIP_EVT_ROC:
		needed = sizeof(struct sip_evt_roc);
		break;
	case SIP_EVT_TRC_AMPDU:
		needed = sizeof(struct sip_evt_trc_ampdu);
		break;
	case SIP_EVT_NOISEFLOOR:
		needed = sizeof(struct sip_evt_noisefloor);
		break;
	}
	if (payload < needed)
		return -EBADMSG;

	switch (hdr->c_evtid) {
	case ESP_TXR_EVENT:
		return esp_reports_receive(sip, buf + SIP_CTRL_HDR_LEN, payload);
	case ESP_POWER_EVENT:
		return esp_power_reply(sip, buf + SIP_CTRL_HDR_LEN, payload);
	case SIP_EVT_TARGET_ON: {
		/* use rx work queue to send... */
		if (atomic_read(&sip->state) == SIP_PREPARE_BOOT ||
		    atomic_read(&sip->state) == SIP_BOOT) {
			if (sip_set_state(sip, SIP_SEND_INIT))
				sip_queue_work(sip, &sip->rx_process_work);
		} else {
			esp_dbg(ESP_DBG_ERROR,
				"%s boot during wrong state %d\n", __func__,
				atomic_read(&sip->state));
		}
		break;
	}
	case SIP_EVT_DEBUG:
		break;
	case SIP_EVT_TX_STATUS: {
		int count =
			esp_tx_report_count(buf + SIP_CTRL_HDR_LEN, payload);

		atomic64_inc(&sip->epub->stats[ESP_FW_TX_REPORTS]);
		if (count < 0) {
			atomic64_inc(&sip->epub->stats[ESP_FW_TX_REPORT_ERRORS]);
			return -EBADMSG;
		}
		atomic64_add(count, &sip->epub->stats[ESP_FW_TX_REPORTED_PKTS]);
		break;
	}

	case SIP_EVT_BOOTUP: {
		struct sip_evt_bootup2 *bootup_evt =
			(struct sip_evt_bootup2 *)(buf + SIP_CTRL_HDR_LEN);
		kfree(sip->rawbuf);
		sip->rawbuf = NULL;

		sip->epub->boot_error = sip_post_init(sip, bootup_evt);

		complete(&sip->epub->boot_completion);

		break;
	}
	case SIP_EVT_RESETTING: {
		sip->epub->wait_reset = 1;
		complete(&sip->epub->boot_completion);
		break;
	}
	case SIP_EVT_SLEEP: {
		break;
	}
	case SIP_EVT_TXIDLE: {
		break;
	}

	case SIP_EVT_SCAN_RESULT:
		break;

	case SIP_EVT_ROC: {
		struct sip_evt_roc *report =
			(struct sip_evt_roc *)(buf + SIP_CTRL_HDR_LEN);
		(void)report;
		break;
	}

	case SIP_EVT_CREDIT_RPT:
		break;

	case SIP_EVT_SNPRINTF_TO_HOST: {
		u16 len;

		if (le16_to_cpu(hdr->len) < SIP_CTRL_HDR_LEN + sizeof(__le16))
			return -EBADMSG;
		len = get_unaligned_le16(buf + SIP_CTRL_HDR_LEN);
		if (len >
		    le16_to_cpu(hdr->len) - SIP_CTRL_HDR_LEN - sizeof(__le16))
			return -EBADMSG;
		dev_dbg(sip->epub->dev, "firmware: %.*s\n", len,
			buf + SIP_CTRL_HDR_LEN + sizeof(__le16));
		break;
	}
	case SIP_EVT_TRC_AMPDU: {
		struct sip_evt_trc_ampdu *ep =
			(struct sip_evt_trc_ampdu *)(buf + SIP_CTRL_HDR_LEN);
		struct esp_node *node = NULL;
		int i = 0;

		if (atomic_read(&sip->epub->wl.off)) {
			esp_dbg(ESP_DBG_ERROR,
				"%s scan result while wlan off\n", __func__);
			return 0;
		}

		rcu_read_lock();
		node = esp_get_node_by_addr(sip->epub, ep->addr);
		if (!node) {
			rcu_read_unlock();
			break;
		}
		for (i = 0; i < 8; i++) {
			if (ep->tid & (1 << i))
				esp_tx_ba_session_op(sip, node, ep->state, i);
		}
		rcu_read_unlock();
		break;
	}

	case SIP_EVT_INIT_EP: {
		char *ep = (char *)(buf + SIP_CTRL_HDR_LEN);

		dev_dbg(sip->epub->dev, "PHY init: %.*s\n", (int)payload, ep);
		break;
	}

	case SIP_EVT_NOISEFLOOR: {
		struct sip_evt_noisefloor *ep =
			(struct sip_evt_noisefloor *)(buf + SIP_CTRL_HDR_LEN);
		atomic_set(&sip->noise_floor,
			   (s16)le16_to_cpu(ep->noise_floor));
		break;
	}
	default:
		break;
	}

	return 0;
}

void sip_send_chip_init(struct esp_sip *sip)
{
	atomic_sub(1, &sip->tx_credits);
	sip_send_cmd(sip, SIP_CMD_INIT, sizeof(sip->epub->init_data),
		     sip->epub->init_data);
}

int sip_send_config(struct esp_pub *epub, struct ieee80211_conf *conf)
{
	struct sk_buff *skb = NULL;
	struct sip_cmd_config *configcmd;

	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_config) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_CONFIG);
	if (!skb)
		return -EINVAL;
	esp_dbg(ESP_DBG_TRACE, "%s config center freq %d\n", __func__,
		conf->chandef.chan->center_freq);
	configcmd =
		(struct sip_cmd_config *)(skb->data + sizeof(struct sip_hdr));
	configcmd->center_freq = cpu_to_le16(conf->chandef.chan->center_freq);
	configcmd->duration = cpu_to_le16(0);
	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

int sip_send_bss_info_update(struct esp_pub *epub, struct esp_vif *evif,
			     u8 *bssid, int assoc)
{
	struct sk_buff *skb = NULL;
	struct sip_cmd_bss_info_update *bsscmd;

	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_bss_info_update) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_BSS_INFO_UPDATE);
	if (!skb)
		return -EINVAL;

	bsscmd = (struct sip_cmd_bss_info_update *)(skb->data +
						    sizeof(struct sip_hdr));
	if (assoc)
		set_bit(ESP_WL_FLAG_CONNECT, &epub->wl.flags);
	else
		clear_bit(ESP_WL_FLAG_CONNECT, &epub->wl.flags);
	bsscmd->bssid_no = evif->index;
	bsscmd->isassoc = cpu_to_le16(assoc);
	bsscmd->beacon_int = cpu_to_le32(evif->aid);
	memcpy(bsscmd->bssid, bssid, ETH_ALEN);
	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

int sip_send_wmm_params(struct esp_pub *epub, u8 aci,
			const struct ieee80211_tx_queue_params *params)
{
	struct sk_buff *skb = NULL;
	struct sip_cmd_set_wmm_params *bsscmd;

	if (aci >= IEEE80211_NUM_ACS || params->txop > U16_MAX / 32)
		return -EINVAL;
	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_set_wmm_params) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_SET_WMM_PARAM);
	if (!skb)
		return -EINVAL;

	bsscmd = (struct sip_cmd_set_wmm_params *)(skb->data +
						   sizeof(struct sip_hdr));
	bsscmd->aci = aci;
	bsscmd->aifs = params->aifs;
	bsscmd->txop_us = cpu_to_le16(params->txop * 32);

	bsscmd->ecw_min = fls(params->cw_min);
	bsscmd->ecw_max = fls(params->cw_max);

	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

int sip_send_ampdu_action(struct esp_pub *epub, u8 action_num, const u8 *addr,
			  u16 tid, u16 ssn, u8 buf_size)
{
	int index = 0, ret;
	struct sk_buff *skb = NULL;
	struct sip_cmd_ampdu_action *action;

	if (action_num == SIP_AMPDU_RX_STOP) {
		index = esp_find_rxampdu(epub, addr, tid);
		/* A firmware restart or station removal already discarded it. */
		if (index == -ENOENT)
			return 0;
		if (index < 0)
			return index;
	}
	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_ampdu_action) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_AMPDU_ACTION);
	if (!skb) {
		if (atomic_read(&epub->sip->state) == SIP_STOP)
			return -ESHUTDOWN;
		/* mac80211 stops its reorder session even if RX_STOP fails. */
		if (action_num == SIP_AMPDU_RX_STOP)
			esp_recovery_request(epub, -ENOMEM);
		return -ENOMEM;
	}
	if (action_num == SIP_AMPDU_RX_START) {
		index = esp_alloc_rxampdu(epub, addr, tid);
		if (index < 0) {
			kfree_skb(skb);
			return index;
		}
	}

	action = (struct sip_cmd_ampdu_action *)(skb->data +
						 sizeof(struct sip_hdr));
	action->action = action_num;
	action->index = ssn;

	switch (action_num) {
	case SIP_AMPDU_RX_START:
		action->ssn = cpu_to_le16(ssn);
		fallthrough;
	case SIP_AMPDU_RX_STOP:
		action->index = index;
		fallthrough;
	case SIP_AMPDU_TX_OPERATIONAL:
	case SIP_AMPDU_TX_STOP:
		action->win_size = buf_size;
		action->tid = tid;
		memcpy(action->addr, addr, ETH_ALEN);
		break;
	}

	ret = sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
	if ((action_num == SIP_AMPDU_RX_START && ret) ||
	    (action_num == SIP_AMPDU_RX_STOP && !ret))
		esp_release_rxampdu(epub, index);
	return ret;
}

int sip_send_setkey(struct esp_pub *epub, u8 bssid_no, u8 *peer_addr,
		    struct ieee80211_key_conf *key, u8 isvalid)
{
	struct sip_cmd_setkey *setkeycmd;
	struct sk_buff *skb = NULL;

	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_setkey) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_SETKEY);

	if (!skb)
		return atomic_read(&epub->sip->state) == SIP_STOP ?
			-ESHUTDOWN : -ENOMEM;

	setkeycmd =
		(struct sip_cmd_setkey *)(skb->data + sizeof(struct sip_hdr));

	if (peer_addr)
		memcpy(setkeycmd->addr, peer_addr, ETH_ALEN);
	else
		memset(setkeycmd->addr, 0, ETH_ALEN);

	setkeycmd->bssid_no = bssid_no;
	setkeycmd->hw_key_idx = key->hw_key_idx;

	if (isvalid) {
		setkeycmd->alg = esp_cipher2alg(key->cipher);
		setkeycmd->keyidx = key->keyidx;
		setkeycmd->keylen = key->keylen;
		if (key->cipher == WLAN_CIPHER_SUITE_TKIP) {
			memcpy(setkeycmd->key, key->key, 16);
			memcpy(setkeycmd->key + 16, key->key + 24, 8);
			memcpy(setkeycmd->key + 24, key->key + 16, 8);
		} else {
			memcpy(setkeycmd->key, key->key, key->keylen);
		}

		setkeycmd->flags = 1;
	} else {
		setkeycmd->flags = 0;
	}
	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

int sip_send_set_sta(struct esp_pub *epub, u8 ifidx, u8 set,
		     struct ieee80211_sta *sta, struct ieee80211_vif *vif,
		     u8 index)
{
	struct sk_buff *skb = NULL;
	struct sip_cmd_setsta *setstacmd;

	skb = sip_alloc_ctrl_skbuf(epub->sip,
				   sizeof(struct sip_cmd_setsta) +
					   sizeof(struct sip_hdr),
				   SIP_CMD_SETSTA);
	if (!skb)
		return -EINVAL;

	setstacmd =
		(struct sip_cmd_setsta *)(skb->data + sizeof(struct sip_hdr));
	setstacmd->ifidx = ifidx;
	setstacmd->index = index;
	setstacmd->set = set;
	if (sta->aid == 0)
		setstacmd->aid = cpu_to_le16(vif->cfg.aid);
	else
		setstacmd->aid = cpu_to_le16(sta->aid);
	memcpy(setstacmd->mac, sta->addr, ETH_ALEN);
	if (set) {
		if (sta->deflink.ht_cap.ht_supported) {
			if (sta->deflink.ht_cap.cap & IEEE80211_HT_CAP_SGI_20)
				setstacmd->phymode = ESP_IEEE80211_T_HT20_S;
			else
				setstacmd->phymode = ESP_IEEE80211_T_HT20_L;
			setstacmd->ampdu_factor =
				sta->deflink.ht_cap.ampdu_factor;
			setstacmd->ampdu_density =
				sta->deflink.ht_cap.ampdu_density;
		} else {
			if (sta->deflink.supp_rates[NL80211_BAND_2GHZ] &
			    (~(u32)CONF_HW_BIT_RATE_11B_MASK))
				setstacmd->phymode = ESP_IEEE80211_T_OFDM;
			else
				setstacmd->phymode = ESP_IEEE80211_T_CCK;
		}
	}
	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

int sip_send_recalc_credit(struct esp_pub *epub)
{
	struct sk_buff *skb = NULL;

	skb = sip_alloc_ctrl_skbuf(epub->sip, 0 + sizeof(struct sip_hdr),
				   SIP_CMD_RECALC_CREDIT);
	if (!skb)
		return -ENOMEM;

	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_HEAD);
}

int sip_cmd(struct esp_pub *epub, enum sip_cmd_id cmd_id, u8 *cmd_buf,
	    u8 cmd_len)
{
	struct sk_buff *skb = NULL;

	if (atomic_read(&epub->sip->state) == SIP_STOP)
		return -ESHUTDOWN;
	skb = sip_alloc_ctrl_skbuf(epub->sip, cmd_len + sizeof(struct sip_hdr),
				   cmd_id);
	if (!skb)
		return -ENOMEM;

	memcpy(skb->data + sizeof(struct sip_hdr), cmd_buf, cmd_len);

	return sip_cmd_enqueue(epub->sip, skb, ENQUEUE_PRIOR_TAIL);
}

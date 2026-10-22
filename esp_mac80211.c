// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2011-2014 Espressif System. */
#include <kunit/visibility.h>
#include <linux/etherdevice.h>
#include <linux/workqueue.h>
#include <linux/nl80211.h>
#include <linux/ieee80211.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <net/regulatory.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_ctrl.h"
#include "esp_sif.h"
#include "esp_debug.h"
#include "esp_wl.h"
#include "esp_utils.h"

#define ESP_IEEE80211_DBG esp_dbg

#define GET_NEXT_SEQ(seq) (((seq) + 1) & 0x0fff)

static void esp_op_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control, struct sk_buff *skb)
{
	struct esp_pub *epub = hw->priv;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_vif *vif = info->control.vif;
	struct ieee80211_key_conf *key = info->control.hw_key;
	struct ieee80211_hdr *hdr = (void *)skb->data;
	struct esp_tx_packet_info packet = {};
	struct esp_vif *evif;
	struct esp_node *node = control->sta ? (void *)control->sta->drv_priv :
					       NULL;
	int alg;

	if (atomic_read(&epub->wl.off) || !vif)
		goto drop;
	evif = (void *)vif->drv_priv;
	if (evif->epub != epub)
		goto drop;
	packet.vif_index = evif->index;
	packet.sta_index = node ? node->index : ESP_PUB_MAX_STA + 1;
	packet.key_index = evif->index;
	if (key) {
		alg = esp_cipher2alg(key->cipher);
		if (alg < 0)
			goto drop;
		packet.enc_flags = (alg + 1) << 1;
		packet.key_index = key->hw_key_idx;
	}
	if (node && ieee80211_is_data_qos(hdr->frame_control) &&
	    !(info->flags & IEEE80211_TX_CTL_AMPDU)) {
		u8 tid = ieee80211_get_tid(hdr);

		spin_lock_bh(&epub->tx_ampdu_lock);
		node->tid[tid].ssn =
			GET_NEXT_SEQ(le16_to_cpu(hdr->seq_ctrl) >> 4);
		spin_unlock_bh(&epub->tx_ampdu_lock);
	}
	*esp_tx_packet_info(skb) = packet;
	sip_tx_data_pkt_enqueue(epub, skb);
	sip_trigger_txq_process(epub->sip);
	return;
drop:
	ieee80211_free_txskb(hw, skb);
}

static int esp_op_start(struct ieee80211_hw *hw)
{
	struct esp_pub *epub = hw->priv;

	if (atomic_read(&epub->sip->state) != SIP_RUN) {
		esp_recovery_complete(epub);
		return -ESHUTDOWN;
	}

	atomic_set(&epub->wl.off, 0);
	atomic_set(&epub->txq_stopped, false);
	ieee80211_wake_queues(hw);
	return 0;
}

static void esp_op_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct esp_pub *epub;

	if (!hw) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s no hw!\n", __func__);
		return;
	}

	epub = (struct esp_pub *)hw->priv;

	if (!epub) {
		ESP_IEEE80211_DBG(ESP_DBG_ERROR, "%s no epub!\n", __func__);
		return;
	}

	atomic_set(&epub->wl.off, 1);
	/* The interface can close before the queued mac80211 restart runs. */
	esp_recovery_complete(epub);
}

static int esp_op_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct esp_pub *epub = hw->priv;
	struct esp_vif *evif = (void *)vif->drv_priv;
	struct sip_cmd_setvif cmd = {};
	int ret;

	if (vif->type != NL80211_IFTYPE_STATION || epub->vif)
		return -EOPNOTSUPP;
	ether_addr_copy(cmd.mac, vif->addr);
	cmd.set = 1;
	ret = sip_cmd(epub, SIP_CMD_SETVIF, (u8 *)&cmd, sizeof(cmd));
	if (ret)
		return ret;
	evif->index = 0;
	evif->epub = epub;
	epub->vif = vif;
	if (READ_ONCE(epub->recovery.reconfiguring) && vif->cfg.assoc)
		ieee80211_hw_restart_disconnect(vif);
	return 0;
}

static void esp_op_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	struct sip_cmd_setvif svif;

	ESP_IEEE80211_DBG(ESP_DBG_OP,
			  "%s enter, vif addr %pM, beacon enable %x\n",
			  __func__, vif->addr, vif->bss_conf.enable_beacon);

	memset(&svif, 0, sizeof(struct sip_cmd_setvif));
	svif.index = evif->index;
	epub->vif_slot &= ~(1 << svif.index);

	epub->vif = NULL;
	evif->epub = NULL;

	sip_cmd(epub, SIP_CMD_SETVIF, (u8 *)&svif,
		sizeof(struct sip_cmd_setvif));

	/* clean up tx/rx queue */
}

static int esp_config_power(struct esp_pub *epub)
{
	int dbm = min(epub->hw->conf.power_level, 20);

	/* mac80211 supplies per-interface limits through link_info_changed. */
	if (epub->vif && epub->vif->bss_conf.txpower != INT_MIN)
		dbm = min(dbm, epub->vif->bss_conf.txpower);
	return esp_power_set(epub->sip, dbm < 0 ? 83 : dbm * 4);
}

static int esp_op_get_txpower(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     unsigned int link_id, int *dbm)
{
	struct esp_pub *epub = hw->priv;

	return esp_power_get(epub->sip, dbm);
}

static int esp_op_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	int ret;

	ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s enter 0x%08x\n", __func__,
			  changed);
	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		/* Preserve a negative limit as an invalid request; the power RPC
		 * keeps data paused when firmware cannot enforce the requested cap.
		 */
		ret = esp_config_power(epub);
		if (ret)
			return ret;
	}

	if (changed &
	    (IEEE80211_CONF_CHANGE_CHANNEL | IEEE80211_CONF_CHANGE_IDLE))
		return sip_send_config(epub, &hw->conf);

	return 0;
}

VISIBLE_IF_KUNIT
void esp_op_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *info,
				    u64 changed)
{
	struct esp_pub *epub = hw->priv;
	struct esp_vif *evif = (void *)vif->drv_priv;

	if (changed & BSS_CHANGED_TXPOWER) {
		int ret = esp_config_power(epub);

		if (ret)
			dev_err(epub->dev, "Firmware power limit failed: %d; data paused\n", ret);
	}
	if (!(changed & (BSS_CHANGED_BSSID | BSS_CHANGED_ASSOC)))
		return;
	evif->aid = vif->cfg.assoc ? vif->cfg.aid : 0;
	/* mac80211 supplies the target before authentication, while assoc
	 * is still false. Keep it until an explicit BSSID update clears or
	 * replaces it; association changes alone only update AID/status.
	 */
	if (changed & BSS_CHANGED_BSSID) {
		if (info->bssid)
			ether_addr_copy(epub->wl.bssid, info->bssid);
		else
			eth_zero_addr(epub->wl.bssid);
	}
	sip_send_bss_info_update(epub, evif, epub->wl.bssid, vif->cfg.assoc);
}
EXPORT_SYMBOL_IF_KUNIT(esp_op_bss_info_changed);

static u64 esp_op_prepare_multicast(struct ieee80211_hw *hw,
				    struct netdev_hw_addr_list *mc_list)
{
	return 0;
}

static void esp_op_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags, u64 multicast)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;

	epub->rx_filter = 0;

	if (*total_flags & FIF_ALLMULTI)
		epub->rx_filter |= FIF_ALLMULTI;

	*total_flags = epub->rx_filter;
}

static void esp_op_vif_cfg_changed(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif, u64 changed)
{
	if (changed & BSS_CHANGED_ASSOC)
		esp_op_bss_info_changed(hw, vif, &vif->bss_conf, changed);
}

VISIBLE_IF_KUNIT
int esp_op_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			  struct ieee80211_key_conf *key)
{
	struct esp_pub *epub = hw->priv;
	struct esp_vif *evif = (void *)vif->drv_priv;
	struct esp_hw_idx_map *pool, *slot;
	const u8 *peer = sta ? sta->addr : epub->wl.bssid;
	bool valid = cmd == SET_KEY, pairwise, wep;
	unsigned int count, base, index;
	int ret;

	if (evif->index >= ESP_PUB_MAX_VIF || key->keyidx >= 4)
		return -EINVAL;
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		if (key->keylen != 5)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		if (key->keylen != 13)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		if (key->keylen != 32)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		if (key->keylen != 16)
			return -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}
	wep = key->cipher == WLAN_CIPHER_SUITE_WEP40 ||
	      key->cipher == WLAN_CIPHER_SUITE_WEP104;
	pairwise = key->flags & IEEE80211_KEY_FLAG_PAIRWISE || wep;
	pool = pairwise ? epub->hi_map : epub->low_map[evif->index];
	count = pairwise ? ARRAY_SIZE(epub->hi_map) :
			   ARRAY_SIZE(epub->low_map[0]);
	base = pairwise ? 6 : 2 + evif->index * 2;
	if (valid) {
		for (index = 0; index < count; index++)
			if (!pool[index].flag)
				break;
		if (index == count)
			return -ENOSPC;
		key->hw_key_idx = base + index;
	} else {
		if (key->hw_key_idx < base || key->hw_key_idx >= base + count)
			return -EINVAL;
		index = key->hw_key_idx - base;
		if (!pool[index].flag)
			return 0;
	}
	slot = &pool[index];
	if (atomic_read(&epub->sip->state) == SIP_STOP) {
		if (valid)
			return -ESHUTDOWN;
	} else {
		ret = sip_send_setkey(epub, evif->index, (u8 *)peer, key,
				      valid);
		if (ret) {
			if (valid)
				return ret;
			/* mac80211 will retire the key even if DISABLE_KEY fails.
			 * Stop RX/TX before releasing its host slot: firmware may
			 * still have the old key. Recovery resets that uncertain
			 * state and requires a fresh association.
			 */
			esp_recovery_request(epub, ret);
		}
	}
	slot->flag = valid;
	if (valid)
		ether_addr_copy(slot->mac, peer);
	else
		eth_zero_addr(slot->mac);
	if (pairwise || wep)
		atomic_add(valid ? 1 : -1, &epub->wl.ptk_cnt);
	if (!pairwise || wep)
		atomic_add(valid ? 1 : -1, &epub->wl.gtk_cnt);
	key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(esp_op_set_key);

static int esp_node_attach(struct ieee80211_hw *hw, u8 ifidx,
			   struct ieee80211_sta *sta)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_node *node;
	u8 tidno;
	struct esp_tx_tid *tid;
	int i;

	spin_lock_bh(&epub->tx_ampdu_lock);

	i = ffz(epub->enodes_map);
	if (hweight32(epub->enodes_maps[ifidx]) < ESP_PUB_MAX_STA &&
	    i < ESP_PUB_MAX_STA + 1) {
		epub->enodes_map |= (1 << i);
		epub->enodes_maps[ifidx] |= (1 << i);
		node = (struct esp_node *)sta->drv_priv;
		epub->enodes[i] = node;
		node->sta = sta;
		node->ifidx = ifidx;
		node->index = i;

		for (tidno = 0; tidno < WME_NUM_TID; tidno++) {
			tid = &node->tid[tidno];
			tid->ssn = 0;
			tid->cnt = 0;
			tid->state = ESP_TID_STATE_INIT;
		}
	} else {
		i = -1;
	}

	spin_unlock_bh(&epub->tx_ampdu_lock);
	return i;
}

VISIBLE_IF_KUNIT
int esp_node_detach(struct ieee80211_hw *hw, u8 ifidx,
		   struct ieee80211_sta *sta)
{
	struct esp_pub *epub = hw->priv;
	struct esp_node *node = (void *)sta->drv_priv;
	int index = -1;
	unsigned int i;

	lockdep_assert_wiphy(hw->wiphy);
	spin_lock_bh(&epub->tx_ampdu_lock);
	for (i = 0; i < ARRAY_SIZE(epub->enodes); i++) {
		if (epub->enodes[i] != node)
			continue;
		epub->enodes[i] = NULL;
		epub->enodes_map &= ~BIT(i);
		epub->enodes_maps[ifidx] &= ~BIT(i);
		index = i;
		break;
	}
	spin_unlock_bh(&epub->tx_ampdu_lock);
	/* Keep node->sta intact for readers in an RCU read-side section. */
	spin_lock_bh(&epub->rx_ampdu_lock);
	for (i = 0; i < ARRAY_SIZE(epub->rxampdu_node); i++) {
		if (epub->rxampdu_node[i] == node) {
			epub->rxampdu_map &= ~BIT(i);
			epub->rxampdu_node[i] = NULL;
		}
	}
	spin_unlock_bh(&epub->rx_ampdu_lock);
	/* mac80211 may free sta immediately after ->sta_remove returns; its
	 * grace period precedes that callback. Unpublish first, then drain our
	 * readers before returning or reusing the slot. This also covers the
	 * ->sta_add command-failure rollback. Never wait with a spinlock held.
	 */
	if (index >= 0)
		synchronize_net();
	return index;
}
EXPORT_SYMBOL_IF_KUNIT(esp_node_detach);

struct esp_node *esp_get_node_by_addr(struct esp_pub *epub, const u8 *addr)
{
	int i;
	u32 map;
	struct esp_node *node = NULL;

	if (!addr)
		return NULL;
	spin_lock_bh(&epub->tx_ampdu_lock);
	map = epub->enodes_map;
	while (map != 0) {
		i = ffs(map) - 1;
		if (i < 0) {
			spin_unlock_bh(&epub->tx_ampdu_lock);
			return NULL;
		}
		map &= ~(1 << i);
		if (memcmp(epub->enodes[i]->sta->addr, addr, ETH_ALEN) == 0) {
			node = epub->enodes[i];
			break;
		}
	}

	spin_unlock_bh(&epub->tx_ampdu_lock);
	return node;
}
EXPORT_SYMBOL_IF_KUNIT(esp_get_node_by_addr);

int esp_alloc_rxampdu(struct esp_pub *epub, const u8 *addr, u8 tid)
{
	struct esp_node *node;
	int index;

	lockdep_assert_wiphy(epub->hw->wiphy);
	rcu_read_lock();
	node = esp_get_node_by_addr(epub, addr);
	if (!node) {
		rcu_read_unlock();
		return -ENOENT;
	}
	spin_lock_bh(&epub->rx_ampdu_lock);
	index = ffz(epub->rxampdu_map);
	if (index >= ESP_PUB_MAX_RXAMPDU) {
		index = -ENOSPC;
	} else {
		epub->rxampdu_map |= BIT(index);
		epub->rxampdu_node[index] = node;
		epub->rxampdu_tid[index] = tid;
	}
	spin_unlock_bh(&epub->rx_ampdu_lock);
	rcu_read_unlock();
	return index;
}

int esp_find_rxampdu(struct esp_pub *epub, const u8 *addr, u8 tid)
{
	int index = -ENOENT;
	unsigned int i;

	lockdep_assert_wiphy(epub->hw->wiphy);
	spin_lock_bh(&epub->rx_ampdu_lock);
	for (i = 0; i < ESP_PUB_MAX_RXAMPDU; i++) {
		struct esp_node *node = epub->rxampdu_node[i];

		if (!(epub->rxampdu_map & BIT(i)) || !node)
			continue;
		if (epub->rxampdu_tid[i] == tid &&
		    ether_addr_equal(node->sta->addr, addr)) {
			index = i;
			break;
		}
	}
	spin_unlock_bh(&epub->rx_ampdu_lock);
	return index;
}

void esp_release_rxampdu(struct esp_pub *epub, unsigned int index)
{
	/* Configuration callbacks and recovery serialize slot transactions. */
	lockdep_assert_wiphy(epub->hw->wiphy);
	if (index >= ESP_PUB_MAX_RXAMPDU)
		return;
	spin_lock_bh(&epub->rx_ampdu_lock);
	epub->rxampdu_map &= ~BIT(index);
	epub->rxampdu_node[index] = NULL;
	epub->rxampdu_tid[index] = 0;
	spin_unlock_bh(&epub->rx_ampdu_lock);
}

static int esp_op_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	int index, ret;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, vif addr %pM, sta addr %pM\n",
			  __func__, vif->addr, sta->addr);
	index = esp_node_attach(hw, evif->index, sta);

	if (index < 0)
		return -ENOSPC;
	ret = sip_send_set_sta(epub, evif->index, 1, sta, vif, (u8)index);
	if (ret)
		esp_node_detach(hw, evif->index, sta);
	return ret;
}

static int esp_op_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_vif *evif = (struct esp_vif *)vif->drv_priv;
	int index;

	ESP_IEEE80211_DBG(ESP_DBG_OP, "%s enter, vif addr %pM, sta addr %pM\n",
			  __func__, vif->addr, sta->addr);
	index = esp_node_detach(hw, evif->index, sta);
	if (index < 0)
		return 0;
	sip_send_set_sta(epub, evif->index, 0, sta, vif, (u8)index);

	return 0;
}

static int esp_op_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  unsigned int link_id, u16 queue,
			  const struct ieee80211_tx_queue_params *params)
{
	struct esp_pub *epub = (struct esp_pub *)hw->priv;

	return sip_send_wmm_params(epub, queue, params);
}

static void esp_op_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 u32 queues, bool drop)
{
	struct esp_pub *epub = hw->priv;
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	struct sk_buff *skb;
	bool pending;

	if (atomic_read(&epub->sip->state) == SIP_STOP)
		return;
	if (drop) {
		/* The TX worker also holds tx_mtx while packing and writing. */
		mutex_lock(&epub->tx_mtx);
		while ((skb = skb_dequeue(&epub->txq))) {
			atomic_dec(&epub->sip->tx_data_pkt_queued);
			ieee80211_free_txskb(hw, skb);
		}
		mutex_unlock(&epub->tx_mtx);
	} else {
		sip_queue_work(epub->sip, &epub->tx_work);
	}
	/* The count includes frames owned by firmware until radio completion. */
	while (atomic_read(&epub->sip->tx_data_pkt_queued) &&
	       atomic_read(&epub->sip->state) != SIP_STOP &&
	       time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	/* Never leave old-key packets queued when a bounded flush expires. */
	mutex_lock(&epub->tx_mtx);
	while ((skb = skb_dequeue(&epub->txq))) {
		atomic_dec(&epub->sip->tx_data_pkt_queued);
		atomic64_inc(&epub->stats[ESP_TX_FLUSH_DROPS]);
		ieee80211_free_txskb(hw, skb);
	}
	pending = atomic_read(&epub->sip->tx_data_pkt_queued);
	mutex_unlock(&epub->tx_mtx);
	/* Do not let key removal/reuse overtake uncompleted firmware frames. */
	if (pending && atomic_read(&epub->sip->state) != SIP_STOP)
		esp_recovery_request(epub, -ETIMEDOUT);
}

VISIBLE_IF_KUNIT
int esp_op_ampdu_action(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct ieee80211_ampdu_params *params)
{
	int ret = -EOPNOTSUPP;
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct ieee80211_sta *sta = params->sta;
	u16 tid = params->tid;
	u16 *ssn = &params->ssn;
	u8 buf_size = params->buf_size;
	struct esp_pub *epub = (struct esp_pub *)hw->priv;
	struct esp_node *node = (struct esp_node *)sta->drv_priv;
	struct esp_tx_tid *tid_info = &node->tid[tid];

	if (atomic_read(&epub->sip->state) == SIP_STOP) {
		/* mac80211 still tears sessions down after transport shutdown. */
		switch (action) {
		case IEEE80211_AMPDU_TX_STOP_CONT:
			ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
			fallthrough;
		case IEEE80211_AMPDU_TX_STOP_FLUSH:
		case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		case IEEE80211_AMPDU_RX_STOP:
			spin_lock_bh(&epub->tx_ampdu_lock);
			tid_info->state = ESP_TID_STATE_STOP;
			spin_unlock_bh(&epub->tx_ampdu_lock);
			return 0;
		default:
			return -ESHUTDOWN;
		}
	}
	switch (action) {
	case IEEE80211_AMPDU_TX_START:
		if (cfg80211_get_chandef_type(&epub->hw->conf.chandef) ==
			    NL80211_CHAN_NO_HT ||
		    !sta->deflink.ht_cap.ht_supported)
			return ret;

		ESP_IEEE80211_DBG(ESP_DBG_TRACE,
				  "%s TX START, addr:%pM,tid:%u,state:%d\n",
				  __func__, sta->addr, tid, READ_ONCE(tid_info->state));
		spin_lock_bh(&epub->tx_ampdu_lock);
		ESSERT(tid_info->state == ESP_TID_STATE_TRIGGER);
		*ssn = tid_info->ssn;
		tid_info->state = ESP_TID_STATE_PROGRESS;

		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		spin_unlock_bh(&epub->tx_ampdu_lock);
		ret = 0;
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		ESP_IEEE80211_DBG(ESP_DBG_TRACE,
				  "%s TX STOP, addr:%pM,tid:%u,state:%d\n",
				  __func__, sta->addr, tid, READ_ONCE(tid_info->state));
		spin_lock_bh(&epub->tx_ampdu_lock);
		if (tid_info->state == ESP_TID_STATE_WAIT_STOP)
			tid_info->state = ESP_TID_STATE_STOP;
		else
			tid_info->state = ESP_TID_STATE_INIT;
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		spin_unlock_bh(&epub->tx_ampdu_lock);
		ret = sip_send_ampdu_action(epub, SIP_AMPDU_TX_STOP, sta->addr,
					    tid, node->ifidx, 0);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		spin_lock_bh(&epub->tx_ampdu_lock);
		if (tid_info->state == ESP_TID_STATE_WAIT_STOP)
			tid_info->state = ESP_TID_STATE_STOP;
		else
			tid_info->state = ESP_TID_STATE_INIT;
		spin_unlock_bh(&epub->tx_ampdu_lock);
		ret = sip_send_ampdu_action(epub, SIP_AMPDU_TX_STOP, sta->addr,
					    tid, node->ifidx, 0);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		ESP_IEEE80211_DBG(ESP_DBG_TRACE,
				  "%s TX OPERATION, addr:%pM,tid:%u,state:%d\n",
				  __func__, sta->addr, tid, READ_ONCE(tid_info->state));
		spin_lock_bh(&epub->tx_ampdu_lock);

		if (tid_info->state != ESP_TID_STATE_PROGRESS) {
			if (tid_info->state == ESP_TID_STATE_INIT) {
				dev_dbg(epub->dev,
					"Ignoring aggregation after reset\n");
				spin_unlock_bh(&epub->tx_ampdu_lock);
				return -ENETRESET;
			}
			ESSERT(0);
		}

		tid_info->state = ESP_TID_STATE_OPERATIONAL;
		spin_unlock_bh(&epub->tx_ampdu_lock);
		ret = sip_send_ampdu_action(epub, SIP_AMPDU_TX_OPERATIONAL,
					    sta->addr, tid, node->ifidx,
					    buf_size);
		break;
	case IEEE80211_AMPDU_RX_START:
		if (cfg80211_get_chandef_type(&epub->hw->conf.chandef) ==
			    NL80211_CHAN_NO_HT ||
		    !sta->deflink.ht_cap.ht_supported)
			return ret;

		ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s RX START %pM tid %u %u\n",
				  __func__, sta->addr, tid, *ssn);
		ret = sip_send_ampdu_action(epub, SIP_AMPDU_RX_START, sta->addr,
					    tid, *ssn, buf_size);
		break;
	case IEEE80211_AMPDU_RX_STOP:
		ESP_IEEE80211_DBG(ESP_DBG_TRACE, "%s RX STOP %pM tid %u\n",
				  __func__, sta->addr, tid);
		ret = sip_send_ampdu_action(epub, SIP_AMPDU_RX_STOP, sta->addr,
					    tid, 0, 0);
		break;
	default:
		break;
	}
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(esp_op_ampdu_action);

static void esp_tx_work(struct work_struct *work)
{
	struct esp_pub *epub = container_of(work, struct esp_pub, tx_work);

	mutex_lock(&epub->tx_mtx);
	sip_txq_process(epub);
	mutex_unlock(&epub->tx_mtx);
}

static void esp_op_reconfig_complete(struct ieee80211_hw *hw,
				     enum ieee80211_reconfig_type type)
{
	if (type == IEEE80211_RECONFIG_TYPE_RESTART)
		esp_recovery_complete(hw->priv);
}

static const char esp_stat_names[][ETH_GSTRING_LEN] = {
	"rx_interrupts",
	"rx_transfers",
	"rx_bytes",
	"rx_max_transfer",
	"rx_invalid_transfer_size",
	"rx_io_errors",
	"rx_bad_packets",
	"rx_frame_drops",
	"rx_aggregates",
	"tx_transfers",
	"tx_io_errors",
	"tx_flush_drops",
	"fw_tx_reports",
	"fw_tx_reported_packets",
	"fw_tx_report_errors",
	"tx_logical_bytes",
	"tx_packed_data",
	"tx_packed_ctrl",
	"tx_kick_no_room",
	"tx_credit_unit_bytes",
	"fw_tx_records",
	"fw_tx_duplicates",
	"fw_tx_malformed",
	"fw_tx_orphan",
	"fw_tx_ack",
	"fw_tx_noack",
	"fw_tx_fail",
	"fw_tx_retries",
	"fw_tx_lost",
	"fw_tx_attempts_known",
	"fw_tx_attempts_unknown",
	"fw_tx_attempts",
	"fw_tx_replay_requests",
	"fw_tx_coalesced_queries",
	"fw_tx_pending",
	"fw_tx_active",
	"fw_tx_failed",
	"fw_tx_submitted",
	"fw_tx_acked",
	"fw_tx_next",
	"fw_power_blocked",
	"fw_recovery_requests",
	"fw_recovery_boots",
	"fw_recovery_completed",
	"fw_recovery_failures",
	"fw_recovery_pending",
	"fw_recovery_disabled",
};

static int esp_get_et_sset_count(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif, int sset)
{
	return sset == ETH_SS_STATS ? ARRAY_SIZE(esp_stat_names) : 0;
}

static void esp_get_et_strings(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, esp_stat_names, sizeof(esp_stat_names));
}

static void esp_get_et_stats(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     struct ethtool_stats *stats, u64 *data)
{
	struct esp_pub *epub = hw->priv;
	struct esp_reports *r = &epub->sip->reports;
	struct esp_recovery *recovery = &epub->recovery;
	unsigned int i;

	/* Counters are individually atomic; this is not a coherent event log. */
	for (i = 0; i < ESP_STATS_COUNT; i++)
		*data++ = atomic64_read(&epub->stats[i]);
	*data++ = atomic64_read(&r->records);
	*data++ = atomic64_read(&r->duplicates);
	*data++ = atomic64_read(&r->malformed);
	*data++ = atomic64_read(&r->orphan);
	*data++ = atomic64_read(&r->ack);
	*data++ = atomic64_read(&r->noack);
	*data++ = atomic64_read(&r->fail);
	*data++ = atomic64_read(&r->retry_counter);
	*data++ = atomic64_read(&r->lost);
	*data++ = atomic64_read(&r->attempts_known);
	*data++ = atomic64_read(&r->attempts_unknown);
	*data++ = atomic64_read(&r->attempts_total);
	*data++ = atomic64_read(&r->replay_requests);
	*data++ = atomic64_read(&r->coalesced_queries);
	*data++ = skb_queue_len(&r->pending);
	*data++ = READ_ONCE(r->active);
	*data++ = READ_ONCE(r->failed);
	*data++ = READ_ONCE(r->submitted);
	*data++ = READ_ONCE(r->acked);
	*data++ = READ_ONCE(r->next);
	*data++ = READ_ONCE(epub->sip->power.blocked);
	*data++ = READ_ONCE(recovery->requests);
	*data++ = READ_ONCE(recovery->boots);
	*data++ = READ_ONCE(recovery->completed);
	*data++ = READ_ONCE(recovery->failures);
	*data++ = READ_ONCE(recovery->pending);
	*data++ = READ_ONCE(recovery->disabled);
}

static int esp_op_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx,
				    u32 value)
{
	/* Firmware controls RTS; the host can only request its default mode. */
	return value == U32_MAX ? 0 : -EOPNOTSUPP;
}

static void esp_op_sta_statistics(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_sta *sta,
				  struct station_info *sinfo)
{
	struct esp_pub *epub = hw->priv;
	struct esp_node *node = (void *)sta->drv_priv;

	/* The extended completion API preserves a final rate without inventing
	 * per-rate attempts. Export that same observed rate through nl80211.
	 */
	spin_lock_bh(&epub->tx_ampdu_lock);
	if (node->last_tx_rate_valid) {
		sinfo->txrate = node->last_tx_rate;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
	}
	/* Count only observed retries. An unknown completion contributes no
	 * guessed attempts; the coverage counter remains available through ethtool.
	 */
	sinfo->tx_retries = node->tx_retries;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_RETRIES);
	spin_unlock_bh(&epub->tx_ampdu_lock);
}

static const struct ieee80211_ops esp_mac80211_ops = {
	.get_txpower = esp_op_get_txpower,
	.sta_statistics = esp_op_sta_statistics,
	.get_et_sset_count = esp_get_et_sset_count,
	.get_et_strings = esp_get_et_strings,
	.get_et_stats = esp_get_et_stats,
	.tx = esp_op_tx,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	.start = esp_op_start,
	.stop = esp_op_stop,
	.reconfig_complete = esp_op_reconfig_complete,
	.add_interface = esp_op_add_interface,
	.remove_interface = esp_op_remove_interface,
	.config = esp_op_config,
	.set_rts_threshold = esp_op_set_rts_threshold,
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,

	.link_info_changed = esp_op_bss_info_changed,
	.vif_cfg_changed = esp_op_vif_cfg_changed,
	.prepare_multicast = esp_op_prepare_multicast,
	.configure_filter = esp_op_configure_filter,
	.set_key = esp_op_set_key,
	.conf_tx = esp_op_conf_tx,
	.ampdu_action = esp_op_ampdu_action,
	.sta_add = esp_op_sta_add,
	.sta_remove = esp_op_sta_remove,
	.flush = esp_op_flush,
};

struct esp_pub *esp_pub_alloc_mac80211(struct device *dev)
{
	struct ieee80211_hw *hw;
	struct esp_pub *epub;
	int ret = 0;

	hw = ieee80211_alloc_hw(sizeof(struct esp_pub), &esp_mac80211_ops);

	if (!hw) {
		esp_dbg(ESP_DBG_ERROR, "ieee80211 can't alloc hw!\n");
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}

	epub = hw->priv;
	memset(epub, 0, sizeof(*epub));
	epub->hw = hw;
	init_completion(&epub->boot_completion);
	SET_IEEE80211_DEV(hw, dev);
	epub->dev = dev;
	esp_recovery_init(epub);

	skb_queue_head_init(&epub->txq);
	skb_queue_head_init(&epub->ctrlq);
	skb_queue_head_init(&epub->txdoneq);
	skb_queue_head_init(&epub->rxq);

	spin_lock_init(&epub->tx_ampdu_lock);
	spin_lock_init(&epub->rx_ampdu_lock);
	spin_lock_init(&epub->tx_lock);
	mutex_init(&epub->tx_mtx);

	INIT_WORK(&epub->tx_work, esp_tx_work);
	epub->esp_wkq = alloc_ordered_workqueue("esp8089", WQ_MEM_RECLAIM);

	if (!epub->esp_wkq) {
		ieee80211_free_hw(hw);
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}

	return epub;
}

int esp_pub_dealloc_mac80211(struct esp_pub *epub)
{
	destroy_workqueue(epub->esp_wkq);
	mutex_destroy(&epub->tx_mtx);

	if (epub->hw)
		ieee80211_free_hw(epub->hw);

	return 0;
}

/* 2G band channels */
static struct ieee80211_channel esp_channels_2ghz[] = {
	{ .hw_value = 1, .center_freq = 2412, .max_power = 20 },
	{ .hw_value = 2, .center_freq = 2417, .max_power = 20 },
	{ .hw_value = 3, .center_freq = 2422, .max_power = 20 },
	{ .hw_value = 4, .center_freq = 2427, .max_power = 20 },
	{ .hw_value = 5, .center_freq = 2432, .max_power = 20 },
	{ .hw_value = 6, .center_freq = 2437, .max_power = 20 },
	{ .hw_value = 7, .center_freq = 2442, .max_power = 20 },
	{ .hw_value = 8, .center_freq = 2447, .max_power = 20 },
	{ .hw_value = 9, .center_freq = 2452, .max_power = 20 },
	{ .hw_value = 10, .center_freq = 2457, .max_power = 20 },
	{ .hw_value = 11, .center_freq = 2462, .max_power = 20 },
	{ .hw_value = 12, .center_freq = 2467, .max_power = 20 },
	{ .hw_value = 13, .center_freq = 2472, .max_power = 20 },
};

/* 11G rate */
static struct ieee80211_rate esp_rates_2ghz[] = {
	{
		.bitrate = 10,
		.hw_value = CONF_HW_BIT_RATE_1MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_1MBPS,
	},
	{ .bitrate = 20,
	  .hw_value = CONF_HW_BIT_RATE_2MBPS,
	  .hw_value_short = CONF_HW_BIT_RATE_2MBPS,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55,
	  .hw_value = CONF_HW_BIT_RATE_5_5MBPS,
	  .hw_value_short = CONF_HW_BIT_RATE_5_5MBPS,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110,
	  .hw_value = CONF_HW_BIT_RATE_11MBPS,
	  .hw_value_short = CONF_HW_BIT_RATE_11MBPS,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{
		.bitrate = 60,
		.hw_value = CONF_HW_BIT_RATE_6MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_6MBPS,
	},
	{
		.bitrate = 90,
		.hw_value = CONF_HW_BIT_RATE_9MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_9MBPS,
	},
	{
		.bitrate = 120,
		.hw_value = CONF_HW_BIT_RATE_12MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_12MBPS,
	},
	{
		.bitrate = 180,
		.hw_value = CONF_HW_BIT_RATE_18MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_18MBPS,
	},
	{
		.bitrate = 240,
		.hw_value = CONF_HW_BIT_RATE_24MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_24MBPS,
	},
	{
		.bitrate = 360,
		.hw_value = CONF_HW_BIT_RATE_36MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_36MBPS,
	},
	{
		.bitrate = 480,
		.hw_value = CONF_HW_BIT_RATE_48MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_48MBPS,
	},
	{
		.bitrate = 540,
		.hw_value = CONF_HW_BIT_RATE_54MBPS,
		.hw_value_short = CONF_HW_BIT_RATE_54MBPS,
	},
};

static void esp_pub_init_mac80211(struct esp_pub *epub)
{
	struct ieee80211_hw *hw = epub->hw;

	static const u32 cipher_suites[] = {
		WLAN_CIPHER_SUITE_WEP40,
		WLAN_CIPHER_SUITE_WEP104,
		WLAN_CIPHER_SUITE_TKIP,
		WLAN_CIPHER_SUITE_CCMP,
	};

	hw->max_listen_interval = 10;

	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, HAS_RATE_CONTROL);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	/* Firmware owns decryption; software-encrypted TX/RX is unsupported.
	 * A failed hardware key install must not silently enable that path.
	 */
	ieee80211_hw_set(hw, SW_CRYPTO_CONTROL);
	/* BA sequence windows, not the number of MPDUs in one radio burst. */
	hw->max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
	hw->max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;

	hw->wiphy->cipher_suites = cipher_suites;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);
	hw->wiphy->max_scan_ie_len = epub->sip->tx_blksz -
				     sizeof(struct sip_hdr) -
				     sizeof(struct sip_cmd_scan);

	/* ONLY station for now, support P2P soon... */
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	hw->wiphy->max_scan_ssids = 2;

	atomic_set(&epub->wl.off, 1);

	epub->wl.sbands[NL80211_BAND_2GHZ].band = NL80211_BAND_2GHZ;
	memcpy(epub->channels, esp_channels_2ghz, sizeof(epub->channels));
	epub->wl.sbands[NL80211_BAND_2GHZ].channels = epub->channels;
	epub->wl.sbands[NL80211_BAND_2GHZ].bitrates = esp_rates_2ghz;
	epub->wl.sbands[NL80211_BAND_2GHZ].n_channels =
		ARRAY_SIZE(esp_channels_2ghz);
	epub->wl.sbands[NL80211_BAND_2GHZ].n_bitrates =
		ARRAY_SIZE(esp_rates_2ghz);
	/* One spatial stream, HT20. Do not advertise 40 MHz-only features. */
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.ht_supported = true;
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.cap =
		IEEE80211_HT_CAP_SM_PS | IEEE80211_HT_CAP_SGI_20 |
		(1 << IEEE80211_HT_CAP_RX_STBC_SHIFT);
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.ampdu_factor =
		IEEE80211_HT_MAX_AMPDU_16K;
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.ampdu_density =
		IEEE80211_HT_MPDU_DENSITY_NONE;
	memset(&epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.mcs, 0,
	       sizeof(epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.mcs));
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.mcs.rx_mask[0] = 0xff;
	/* TX and RX both support MCS 0-7. mac80211 requires TX_DEFINED before
	 * intersecting our transmit MCS set with the peer's receive MCS set.
	 */
	epub->wl.sbands[NL80211_BAND_2GHZ].ht_cap.mcs.tx_params =
		IEEE80211_HT_MCS_TX_DEFINED;

	/* BAND_5GHZ TBD */

	hw->wiphy->bands[NL80211_BAND_2GHZ] =
		&epub->wl.sbands[NL80211_BAND_2GHZ];
	/* BAND_5GHZ TBD */

	/*no fragment */
	hw->wiphy->frag_threshold = IEEE80211_MAX_FRAG_THRESHOLD;

	/* handle AC queue in f/w */
	hw->queues = 4;
	hw->max_rates = 4;

	hw->vif_data_size = sizeof(struct esp_vif);
	hw->sta_data_size = sizeof(struct esp_node);
}

int esp_register_mac80211(struct esp_pub *epub)
{
	int ret;

	esp_pub_init_mac80211(epub);
	SET_IEEE80211_PERM_ADDR(epub->hw, epub->mac_addr);
	ret = ieee80211_register_hw(epub->hw);
	if (ret)
		return ret;
	set_bit(ESP_WL_FLAG_HW_REGISTERED, &epub->wl.flags);
	return 0;
}

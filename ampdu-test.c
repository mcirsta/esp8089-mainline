// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include "esp_pub.h"
#include "esp_sip.h"

struct esp_ampdu_fixture {
	struct ieee80211_hw hw;
	struct ieee80211_channel channel;
	struct esp_pub epub;
	struct esp_sip sip;
	struct ieee80211_sta *sta;
};

static const struct cfg80211_ops esp_test_cfg_ops;

static void esp_ampdu_work(struct work_struct *work)
{
	/* Leave commands queued for inspection; this fixture has no transport. */
}

static void esp_ampdu_cleanup(void *data)
{
	struct esp_ampdu_fixture *f = data;

	cancel_work_sync(&f->epub.tx_work);
	skb_queue_purge(&f->epub.ctrlq);
	wiphy_free(f->hw.wiphy);
}

static int esp_ampdu_init(struct kunit *test)
{
	struct esp_ampdu_fixture *f;
	struct esp_node *node;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->sta = kunit_kzalloc(test, sizeof(*f->sta) + sizeof(*node), GFP_KERNEL);
	if (!f->sta)
		return -ENOMEM;
	f->hw.wiphy = wiphy_new(&esp_test_cfg_ops, 0);
	if (!f->hw.wiphy)
		return -ENOMEM;
	INIT_WORK(&f->epub.tx_work, esp_ampdu_work);
	skb_queue_head_init(&f->epub.ctrlq);
	if (kunit_add_action_or_reset(test, esp_ampdu_cleanup, f))
		return -ENOMEM;
	f->hw.priv = &f->epub;
	f->epub.hw = &f->hw;
	f->epub.sip = &f->sip;
	f->epub.esp_wkq = system_dfl_wq;
	f->sip.epub = &f->epub;
	f->sip.tx_blksz = 256;
	atomic_set(&f->sip.state, SIP_RUN);
	spin_lock_init(&f->sip.state_lock);
	spin_lock_init(&f->epub.tx_ampdu_lock);
	spin_lock_init(&f->epub.rx_ampdu_lock);
	f->channel.band = NL80211_BAND_2GHZ;
	f->channel.center_freq = 2412;
	cfg80211_chandef_create(&f->hw.conf.chandef, &f->channel, NL80211_CHAN_HT20);
	f->sta->deflink.ht_cap.ht_supported = true;
	f->sta->addr[0] = 0x02;
	f->sta->addr[5] = 0x01;
	node = (void *)f->sta->drv_priv;
	node->sta = f->sta;
	f->epub.enodes[0] = node;
	f->epub.enodes_map = BIT(0);
	test->priv = f;
	return 0;
}

static int esp_ampdu_call(struct esp_ampdu_fixture *f,
			  struct ieee80211_ampdu_params *params)
{
	int ret;

	wiphy_lock(f->hw.wiphy);
	ret = esp_op_ampdu_action(&f->hw, NULL, params);
	wiphy_unlock(f->hw.wiphy);
	return ret;
}

static void esp_ampdu_rx_window_test(struct kunit *test)
{
	static const u16 windows[] = { 1, 8, 16, 64 };
	struct esp_ampdu_fixture *f = test->priv;
	struct ieee80211_ampdu_params params = {
		.sta = f->sta,
		.tid = 3,
		.ssn = 0xabc,
	};
	struct sip_cmd_ampdu_action *cmd;
	struct sk_buff *skb;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		params.action = IEEE80211_AMPDU_RX_START;
		params.buf_size = windows[i];
		KUNIT_ASSERT_EQ(test, esp_ampdu_call(f, &params), 0);
		KUNIT_ASSERT_EQ(test, skb_queue_len(&f->epub.ctrlq), 1);
		skb = skb_dequeue(&f->epub.ctrlq);
		cmd = (void *)(skb->data + sizeof(struct sip_hdr));
		KUNIT_EXPECT_EQ(test, cmd->action, SIP_AMPDU_RX_START);
		KUNIT_EXPECT_EQ(test, cmd->win_size, windows[i]);
		KUNIT_EXPECT_EQ(test, le16_to_cpu(cmd->ssn), params.ssn);
		KUNIT_EXPECT_EQ(test, cmd->tid, params.tid);
		KUNIT_EXPECT_EQ(test, cmd->index, 0);
		KUNIT_EXPECT_MEMEQ(test, cmd->addr, f->sta->addr, ETH_ALEN);
		KUNIT_EXPECT_EQ(test, f->epub.rxampdu_map, BIT(0));
		kfree_skb(skb);

		params.action = IEEE80211_AMPDU_RX_STOP;
		KUNIT_ASSERT_EQ(test, esp_ampdu_call(f, &params), 0);
		KUNIT_ASSERT_EQ(test, skb_queue_len(&f->epub.ctrlq), 1);
		skb = skb_dequeue(&f->epub.ctrlq);
		cmd = (void *)(skb->data + sizeof(struct sip_hdr));
		KUNIT_EXPECT_EQ(test, cmd->action, SIP_AMPDU_RX_STOP);
		KUNIT_EXPECT_EQ(test, cmd->index, 0);
		KUNIT_EXPECT_EQ(test, f->epub.rxampdu_map, 0);
		kfree_skb(skb);
	}
}

static void esp_ampdu_tx_flush_state_test(struct kunit *test)
{
	struct esp_ampdu_fixture *f = test->priv;
	struct esp_node *node = (void *)f->sta->drv_priv;
	struct ieee80211_ampdu_params params = {
		.sta = f->sta,
		.tid = 3,
		.action = IEEE80211_AMPDU_TX_STOP_FLUSH,
	};

	node->tid[3].state = ESP_TID_STATE_WAIT_STOP;
	KUNIT_ASSERT_EQ(test, esp_ampdu_call(f, &params), 0);
	KUNIT_EXPECT_EQ(test, node->tid[3].state, ESP_TID_STATE_STOP);
	node->tid[3].state = ESP_TID_STATE_OPERATIONAL;
	params.action = IEEE80211_AMPDU_TX_STOP_FLUSH_CONT;
	KUNIT_ASSERT_EQ(test, esp_ampdu_call(f, &params), 0);
	KUNIT_EXPECT_EQ(test, node->tid[3].state, ESP_TID_STATE_INIT);
	sip_stop(&f->sip);
	node->tid[3].state = ESP_TID_STATE_OPERATIONAL;
	KUNIT_ASSERT_EQ(test, esp_ampdu_call(f, &params), 0);
	KUNIT_EXPECT_EQ(test, node->tid[3].state, ESP_TID_STATE_STOP);
}

static struct kunit_case esp_ampdu_cases[] = {
	KUNIT_CASE(esp_ampdu_rx_window_test),
	KUNIT_CASE(esp_ampdu_tx_flush_state_test),
	{}
};

static struct kunit_suite esp_ampdu_suite = {
	.name = "esp8089-ampdu",
	.init = esp_ampdu_init,
	.test_cases = esp_ampdu_cases,
};

kunit_test_suite(esp_ampdu_suite);

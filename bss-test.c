// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/etherdevice.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_utils.h"

struct bss_fixture {
	struct ieee80211_hw hw;
	struct esp_pub epub;
	struct esp_sip sip;
	struct ieee80211_vif *vif;
};

static const u8 bss_target[ETH_ALEN] = { 0x02, 0x35, 0x00, 0x00, 0x00, 0x01 };
static const u8 bss_other[ETH_ALEN] = { 0x02, 0x35, 0x00, 0x00, 0x00, 0x02 };
static const u8 bss_zero[ETH_ALEN];

static void bss_tx_work(struct work_struct *work) {}

static void bss_cleanup(void *data)
{
	struct bss_fixture *f = data;

	cancel_work_sync(&f->epub.tx_work);
	skb_queue_purge(&f->epub.ctrlq);
}

static int bss_init(struct kunit *test)
{
	struct bss_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	f->vif = kunit_kzalloc(test, sizeof(*f->vif) + sizeof(struct esp_vif), GFP_KERNEL);
	if (!f->vif)
		return -ENOMEM;
	f->hw.priv = &f->epub;
	f->epub.hw = &f->hw;
	f->epub.sip = &f->sip;
	f->epub.esp_wkq = system_dfl_wq;
	f->sip.epub = &f->epub;
	f->sip.tx_blksz = 256;
	spin_lock_init(&f->sip.state_lock);
	atomic_set(&f->sip.state, SIP_RUN);
	skb_queue_head_init(&f->epub.ctrlq);
	INIT_WORK(&f->epub.tx_work, bss_tx_work);
	test->priv = f;
	return kunit_add_action_or_reset(test, bss_cleanup, f);
}

static void bss_notify(struct bss_fixture *f, const u8 *bssid, u64 changed)
{
	f->vif->bss_conf.bssid = bssid;
	esp_op_bss_info_changed(&f->hw, f->vif, &f->vif->bss_conf, changed);
}

static void bss_expect_command(struct kunit *test, const u8 *bssid,
			       bool associated, u16 aid)
{
	struct bss_fixture *f = test->priv;
	struct sip_cmd_bss_info_update *wire;
	struct sk_buff *skb = skb_dequeue(&f->epub.ctrlq);
	struct sip_hdr *hdr;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	hdr = (void *)skb->data;
	wire = (void *)(skb->data + sizeof(*hdr));
	KUNIT_EXPECT_EQ(test, skb->len, sizeof(*hdr) + sizeof(*wire));
	KUNIT_EXPECT_EQ(test, le32_to_cpu(hdr->c_cmdid), SIP_CMD_BSS_INFO_UPDATE);
	KUNIT_EXPECT_MEMEQ(test, wire->bssid, bssid, ETH_ALEN);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(wire->isassoc), associated);
	/* The legacy ABI names the AID field beacon_int. */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(wire->beacon_int), aid);
	kfree_skb(skb);
}

static void bss_preassociation_retains_target(struct kunit *test)
{
	struct bss_fixture *f = test->priv;
	struct esp_vif *evif = (void *)f->vif->drv_priv;

	f->vif->cfg.aid = 0x123;
	evif->aid = 0x456;
	bss_notify(f, bss_target, BSS_CHANGED_BSSID);
	KUNIT_EXPECT_MEMEQ(test, f->epub.wl.bssid, bss_target, ETH_ALEN);
	KUNIT_EXPECT_EQ(test, evif->aid, 0);
	KUNIT_EXPECT_FALSE(test, test_bit(ESP_WL_FLAG_CONNECT, &f->epub.wl.flags));
	bss_expect_command(test, bss_target, false, 0);
	/* A new authentication target replaces the first without association. */
	bss_notify(f, bss_other, BSS_CHANGED_BSSID);
	KUNIT_EXPECT_MEMEQ(test, f->epub.wl.bssid, bss_other, ETH_ALEN);
	bss_expect_command(test, bss_other, false, 0);
}

static void bss_preassociation_default_key_peer(struct kunit *test)
{
	struct bss_fixture *f = test->priv;
	struct ieee80211_key_conf *key;
	struct sip_cmd_setkey *wire;
	struct sk_buff *skb;

	bss_notify(f, bss_target, BSS_CHANGED_BSSID);
	bss_expect_command(test, bss_target, false, 0);
	key = kunit_kzalloc(test, sizeof(*key) + 5, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, key);
	key->cipher = WLAN_CIPHER_SUITE_WEP40;
	key->keylen = 5;
	memcpy(key->key, "abcde", 5);
	KUNIT_ASSERT_EQ(test, esp_op_set_key(&f->hw, SET_KEY, f->vif, NULL, key), 0);
	skb = skb_dequeue(&f->epub.ctrlq);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	wire = (void *)(skb->data + sizeof(struct sip_hdr));
	KUNIT_EXPECT_MEMEQ(test, wire->addr, bss_target, ETH_ALEN);
	KUNIT_EXPECT_MEMEQ(test, f->epub.hi_map[0].mac, bss_target, ETH_ALEN);
	kfree_skb(skb);
}

static void bss_assoc_only_preserves_target(struct kunit *test)
{
	struct bss_fixture *f = test->priv;
	struct esp_vif *evif = (void *)f->vif->drv_priv;

	ether_addr_copy(f->epub.wl.bssid, bss_target);
	f->vif->cfg.assoc = true;
	f->vif->cfg.aid = 0x321;
	/* BSSID is not updated by this notification. */
	bss_notify(f, NULL, BSS_CHANGED_ASSOC);
	KUNIT_EXPECT_EQ(test, evif->aid, 0x321);
	KUNIT_EXPECT_TRUE(test, test_bit(ESP_WL_FLAG_CONNECT, &f->epub.wl.flags));
	bss_expect_command(test, bss_target, true, 0x321);
	f->vif->cfg.assoc = false;
	bss_notify(f, NULL, BSS_CHANGED_ASSOC);
	KUNIT_EXPECT_MEMEQ(test, f->epub.wl.bssid, bss_target, ETH_ALEN);
	KUNIT_EXPECT_EQ(test, evif->aid, 0);
	KUNIT_EXPECT_FALSE(test, test_bit(ESP_WL_FLAG_CONNECT, &f->epub.wl.flags));
	bss_expect_command(test, bss_target, false, 0);
}

static void bss_explicit_zero_clears_target(struct kunit *test)
{
	struct bss_fixture *f = test->priv;

	ether_addr_copy(f->epub.wl.bssid, bss_target);
	bss_notify(f, bss_zero, BSS_CHANGED_BSSID | BSS_CHANGED_ASSOC);
	KUNIT_EXPECT_TRUE(test, is_zero_ether_addr(f->epub.wl.bssid));
	bss_expect_command(test, bss_zero, false, 0);
}

static void bss_explicit_null_clears_target(struct kunit *test)
{
	struct bss_fixture *f = test->priv;

	ether_addr_copy(f->epub.wl.bssid, bss_target);
	bss_notify(f, NULL, BSS_CHANGED_BSSID);
	KUNIT_EXPECT_TRUE(test, is_zero_ether_addr(f->epub.wl.bssid));
	bss_expect_command(test, bss_zero, false, 0);
}

static void bss_unrelated_change_is_ignored(struct kunit *test)
{
	struct bss_fixture *f = test->priv;

	ether_addr_copy(f->epub.wl.bssid, bss_target);
	bss_notify(f, bss_other, BSS_CHANGED_BEACON_INT);
	KUNIT_EXPECT_MEMEQ(test, f->epub.wl.bssid, bss_target, ETH_ALEN);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
}

static struct kunit_case bss_cases[] = {
	KUNIT_CASE(bss_preassociation_retains_target),
	KUNIT_CASE(bss_preassociation_default_key_peer),
	KUNIT_CASE(bss_assoc_only_preserves_target),
	KUNIT_CASE(bss_explicit_zero_clears_target),
	KUNIT_CASE(bss_explicit_null_clears_target),
	KUNIT_CASE(bss_unrelated_change_is_ignored),
	{}
};

static struct kunit_suite bss_suite = {
	.name = "esp8089-bss",
	.init = bss_init,
	.test_cases = bss_cases,
};
kunit_test_suite(bss_suite);

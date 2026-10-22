// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/static_stub.h>
#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_utils.h"

struct key_fixture {
	struct ieee80211_hw hw;
	struct esp_pub epub;
	struct esp_sip sip;
	struct ieee80211_vif *vif;
	struct ieee80211_key_conf *key;
};

static void key_work(struct work_struct *work) {}

static void key_cleanup(void *data)
{
	struct key_fixture *f = data;

	cancel_work_sync(&f->epub.tx_work);
	skb_queue_purge(&f->epub.ctrlq);
}

static int esp_key_test_init(struct kunit *test)
{
	struct key_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->vif = kunit_kzalloc(test, sizeof(*f->vif) + sizeof(struct esp_vif), GFP_KERNEL);
	f->key = kunit_kzalloc(test, sizeof(*f->key) + 32, GFP_KERNEL);
	if (!f->vif || !f->key)
		return -ENOMEM;
	f->hw.priv = &f->epub;
	f->epub.hw = &f->hw;
	f->epub.sip = &f->sip;
	f->epub.esp_wkq = system_dfl_wq;
	f->epub.wl.bssid[0] = 0x02;
	f->epub.wl.bssid[5] = 0x32;
	f->sip.epub = &f->epub;
	f->sip.tx_blksz = 256;
	spin_lock_init(&f->sip.state_lock);
	atomic_set(&f->sip.state, SIP_RUN);
	init_completion(&f->epub.boot_completion);
	init_completion(&f->sip.power.done);
	init_waitqueue_head(&f->sip.power.drain);
	skb_queue_head_init(&f->epub.ctrlq);
	INIT_WORK(&f->epub.tx_work, key_work);
	test->priv = f;
	return kunit_add_action_or_reset(test, key_cleanup, f);
}

static int key_call(struct key_fixture *f, enum set_key_cmd cmd)
{
	return esp_op_set_key(&f->hw, cmd, f->vif, NULL, f->key);
}

static void key_supported_wire_formats(struct kunit *test)
{
	static const struct {
		u32 cipher;
		u8 length, alg;
		bool pairwise;
	} vectors[] = {
		{ WLAN_CIPHER_SUITE_WEP40, 5, ALG_WEP, false },
		{ WLAN_CIPHER_SUITE_WEP104, 13, ALG_WEP, false },
		{ WLAN_CIPHER_SUITE_TKIP, 32, ALG_TKIP, true },
		{ WLAN_CIPHER_SUITE_CCMP, 16, ALG_CCMP, true },
		{ WLAN_CIPHER_SUITE_CCMP, 16, ALG_CCMP, false },
	};
	struct key_fixture *f = test->priv;
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(vectors); i++) {
		struct sip_cmd_setkey *wire;
		struct sk_buff *skb;
		u8 expected[32];

		f->key->cipher = vectors[i].cipher;
		f->key->keylen = vectors[i].length;
		f->key->flags = vectors[i].pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
		f->key->keyidx = vectors[i].pairwise ? 0 : 1;
		for (j = 0; j < 32; j++)
			f->key->key[j] = expected[j] = 0x80 + j;
		if (vectors[i].cipher == WLAN_CIPHER_SUITE_TKIP) {
			memcpy(expected + 16, f->key->key + 24, 8);
			memcpy(expected + 24, f->key->key + 16, 8);
		}
		KUNIT_ASSERT_EQ(test, key_call(f, SET_KEY), 0);
		skb = skb_dequeue(&f->epub.ctrlq);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		wire = (void *)(skb->data + sizeof(struct sip_hdr));
		KUNIT_EXPECT_EQ(test, wire->alg, vectors[i].alg);
		KUNIT_EXPECT_EQ(test, wire->keyidx, f->key->keyidx);
		KUNIT_EXPECT_EQ(test, wire->keylen, vectors[i].length);
		KUNIT_EXPECT_EQ(test, wire->hw_key_idx, f->key->hw_key_idx);
		KUNIT_EXPECT_EQ(test, wire->flags, 1);
		KUNIT_EXPECT_MEMEQ(test, wire->key, expected, vectors[i].length);
		KUNIT_EXPECT_MEMEQ(test, wire->addr, f->epub.wl.bssid, ETH_ALEN);
		KUNIT_EXPECT_TRUE(test, f->key->flags & IEEE80211_KEY_FLAG_GENERATE_IV);
		kfree_skb(skb);
		KUNIT_ASSERT_EQ(test, key_call(f, DISABLE_KEY), 0);
		skb = skb_dequeue(&f->epub.ctrlq);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		wire = (void *)(skb->data + sizeof(struct sip_hdr));
		KUNIT_EXPECT_EQ(test, wire->flags, 0);
		kfree_skb(skb);
		KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.ptk_cnt), 0);
		KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.gtk_cnt), 0);
		KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
	}
}

static void key_rejections_do_not_publish(struct kunit *test)
{
	struct key_fixture *f = test->priv;
	static const u32 unsupported[] = {
		WLAN_CIPHER_SUITE_AES_CMAC, WLAN_CIPHER_SUITE_GCMP,
		WLAN_CIPHER_SUITE_CCMP_256, WLAN_CIPHER_SUITE_GCMP_256,
	};
	unsigned int i;

	f->key->keylen = 16;
	for (i = 0; i < ARRAY_SIZE(unsupported); i++) {
		f->key->cipher = unsupported[i];
		KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -EOPNOTSUPP);
	}
	f->key->cipher = WLAN_CIPHER_SUITE_CCMP;
	f->key->keylen = 15;
	KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -EINVAL);
	f->key->keylen = 16;
	f->key->keyidx = 4;
	KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -EINVAL);
	f->key->keyidx = 0;
	f->key->flags = IEEE80211_KEY_FLAG_PAIRWISE;
	for (i = 0; i < ARRAY_SIZE(f->epub.hi_map); i++)
		f->epub.hi_map[i].flag = 1;
	KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -ENOSPC);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.ptk_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.gtk_cnt), 0);
}

static struct sk_buff *key_alloc_failure(struct esp_sip *sip, u16 len, u32 cid)
{
	return NULL;
}

static struct sk_buff *key_alloc_stop_race(struct esp_sip *sip, u16 len, u32 cid)
{
	struct sk_buff *skb = alloc_skb(len, GFP_KERNEL);
	struct sip_hdr *hdr;

	if (!skb)
		return NULL;
	hdr = skb_put_zero(skb, len);
	hdr->len = cpu_to_le16(len);
	hdr->c_cmdid = cpu_to_le32(cid);
	sip_stop(sip);
	return skb;
}

static void key_remove_failure(struct kunit *test, u32 cipher, bool pairwise,
			       bool stopped_during_enqueue)
{
	struct key_fixture *f = test->priv;
	struct esp_hw_idx_map *slot;
	struct sk_buff *skb;
	bool wep = cipher == WLAN_CIPHER_SUITE_WEP40;

	f->key->cipher = cipher;
	f->key->keylen = wep ? 5 : 16;
	f->key->flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
	KUNIT_ASSERT_EQ(test, key_call(f, SET_KEY), 0);
	skb = skb_dequeue(&f->epub.ctrlq);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	kfree_skb(skb);
	slot = pairwise || wep ? &f->epub.hi_map[0] : &f->epub.low_map[0][0];
	KUNIT_ASSERT_TRUE(test, slot->flag);
	if (stopped_during_enqueue)
		kunit_activate_static_stub(test, sip_alloc_ctrl_skbuf, key_alloc_stop_race);
	else
		kunit_activate_static_stub(test, sip_alloc_ctrl_skbuf, key_alloc_failure);
	KUNIT_EXPECT_EQ(test, key_call(f, DISABLE_KEY), 0);
	KUNIT_EXPECT_FALSE(test, slot->flag);
	KUNIT_EXPECT_TRUE(test, is_zero_ether_addr(slot->mac));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.ptk_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.gtk_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->sip.state), SIP_STOP);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.off), 1);
	KUNIT_EXPECT_TRUE(test, f->sip.reports.failed);
	KUNIT_EXPECT_TRUE(test, f->sip.power.blocked);
	/* This isolated fixture has no registered radio. The real recovery
	 * request must still stop traffic and wake the failed-boot waiter.
	 */
	KUNIT_EXPECT_TRUE(test, completion_done(&f->epub.boot_completion));
	KUNIT_EXPECT_EQ(test, f->epub.boot_error,
			stopped_during_enqueue ? -ESHUTDOWN : -ENOMEM);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
	KUNIT_EXPECT_EQ(test, key_call(f, DISABLE_KEY), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.ptk_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.gtk_cnt), 0);
	KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -ESHUTDOWN);
}

static void key_remove_pairwise_allocation_failure(struct kunit *test)
{
	key_remove_failure(test, WLAN_CIPHER_SUITE_CCMP, true, false);
}

static void key_remove_group_allocation_failure(struct kunit *test)
{
	key_remove_failure(test, WLAN_CIPHER_SUITE_CCMP, false, false);
}

static void key_remove_wep_allocation_failure(struct kunit *test)
{
	key_remove_failure(test, WLAN_CIPHER_SUITE_WEP40, false, false);
}

static void key_remove_enqueue_stop_race(struct kunit *test)
{
	key_remove_failure(test, WLAN_CIPHER_SUITE_CCMP, true, true);
}

static void key_set_allocation_failure(struct kunit *test)
{
	struct key_fixture *f = test->priv;

	f->key->cipher = WLAN_CIPHER_SUITE_CCMP;
	f->key->keylen = 16;
	f->key->flags = IEEE80211_KEY_FLAG_PAIRWISE;
	kunit_activate_static_stub(test, sip_alloc_ctrl_skbuf, key_alloc_failure);
	KUNIT_EXPECT_EQ(test, key_call(f, SET_KEY), -ENOMEM);
	KUNIT_EXPECT_FALSE(test, f->epub.hi_map[0].flag);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->epub.wl.ptk_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->sip.state), SIP_RUN);
	KUNIT_EXPECT_FALSE(test, completion_done(&f->epub.boot_completion));
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
}

static struct kunit_case key_cases[] = {
	KUNIT_CASE(key_remove_pairwise_allocation_failure),
	KUNIT_CASE(key_remove_group_allocation_failure),
	KUNIT_CASE(key_remove_wep_allocation_failure),
	KUNIT_CASE(key_remove_enqueue_stop_race),
	KUNIT_CASE(key_set_allocation_failure),
	KUNIT_CASE(key_supported_wire_formats),
	KUNIT_CASE(key_rejections_do_not_publish),
	{}
};

static struct kunit_suite key_suite = {
	.name = "esp8089-key",
	.init = esp_key_test_init,
	.test_cases = key_cases,
};
kunit_test_suite(key_suite);

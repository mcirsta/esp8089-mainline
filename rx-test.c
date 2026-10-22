// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <net/mac80211.h>

#include "esp_pub.h"
#include "esp_sip.h"
#include "esp_wmac.h"

struct esp_rx_fixture {
	struct esp_sip *sip;
	struct esp_mac_rx_ctrl ctrl;
	struct sk_buff *view;
	struct sk_buff_head frames;
	__le32 entries[2];
};

static void esp_rx_cleanup(void *data)
{
	struct esp_rx_fixture *f = data;

	kfree_skb(f->view);
	skb_queue_purge(&f->frames);
}

static int esp_rx_init(struct kunit *test)
{
	struct esp_rx_fixture *f;
	struct esp_pub *epub;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	epub = kunit_kzalloc(test, sizeof(*epub), GFP_KERNEL);
	if (!f || !epub)
		return -ENOMEM;
	f->sip = kunit_kzalloc(test, sizeof(*f->sip), GFP_KERNEL);
	if (!f->sip)
		return -ENOMEM;
	f->sip->epub = epub;
	atomic_set(&epub->wl.ptk_cnt, 1);
	f->ctrl.aggregation = true;
	f->ctrl.ampdu_cnt = ARRAY_SIZE(f->entries);
	f->ctrl.channel = 9;
	f->ctrl.sig_mode = 1;
	f->ctrl.mcs = 7;
	skb_queue_head_init(&f->frames);
	f->view = alloc_skb(80, GFP_KERNEL);
	if (!f->view)
		return -ENOMEM;
	if (kunit_add_action_or_reset(test, esp_rx_cleanup, f))
		return -ENOMEM;
	/* Two protected MPDUs: 24-byte header, 8-byte IV, 8-byte payload.
	 * Firmware strips the 8-byte MIC and 4-byte FCS from stored bytes,
	 * but includes both in the aggregate length table.
	 */
	for (i = 0; i < ARRAY_SIZE(f->entries); i++) {
		u8 *data = skb_put_zero(f->view, 40);

		put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_PROTECTED,
				   data);
		data[32] = i + 1;
		f->entries[i] = cpu_to_le32(52 << 8);
	}
	test->priv = f;
	return 0;
}

static void esp_rx_valid_aggregate_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	struct sk_buff *frame;
	unsigned int i = 0;

	sip_rx_ampdu(f->sip, f->view, &f->ctrl, f->entries, &f->frames);
	KUNIT_EXPECT_EQ(test, f->view->len, 0);
	KUNIT_EXPECT_EQ(test, skb_queue_len(&f->frames), 2);
	skb_queue_walk(&f->frames, frame) {
		KUNIT_EXPECT_EQ(test, frame->len, 40);
		KUNIT_EXPECT_TRUE(test, IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_MIC_STRIPPED);
		KUNIT_EXPECT_EQ(test, frame->data[32], ++i);
		KUNIT_EXPECT_EQ(test, IEEE80211_SKB_RXCB(frame)->enc_flags, 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->sip->epub->stats[ESP_RX_FRAME_DROPS]), 0);
}

static void esp_rx_mic_error_tail_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	struct sk_buff *frame;

	f->entries[0] |= cpu_to_le32(RX_CCMPMIC_ERR);
	sip_rx_ampdu(f->sip, f->view, &f->ctrl, f->entries, &f->frames);
	KUNIT_EXPECT_EQ(test, f->view->len, 0);
	KUNIT_ASSERT_EQ(test, skb_queue_len(&f->frames), 1);
	frame = skb_peek(&f->frames);
	KUNIT_EXPECT_EQ(test, frame->len, 40);
	KUNIT_EXPECT_EQ(test, frame->data[32], 2);
	KUNIT_EXPECT_FALSE(test, IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_MMIC_ERROR);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->sip->epub->stats[ESP_RX_FRAME_DROPS]), 1);
}

static void esp_rx_stbc_status_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	struct ieee80211_rx_status *status;
	struct sk_buff *frame;
	u8 wire[ESP_RX_CTRL_LEN] = {};

	/* HT MCS7, one STBC stream, short GI and two aggregate entries. */
	put_unaligned_le32(BIT(14), wire);
	put_unaligned_le32(0x98000007, wire + 4);
	put_unaligned_le32(0x00090200, wire + 8);
	esp_rx_ctrl_decode(wire, &f->ctrl);
	sip_rx_ampdu(f->sip, f->view, &f->ctrl, f->entries, &f->frames);
	KUNIT_ASSERT_EQ(test, skb_queue_len(&f->frames), 2);
	skb_queue_walk(&f->frames, frame) {
		status = IEEE80211_SKB_RXCB(frame);
		KUNIT_EXPECT_EQ(test, (u8)status->encoding, RX_ENC_HT);
		KUNIT_EXPECT_EQ(test, status->rate_idx, 7);
		KUNIT_EXPECT_EQ(test, status->enc_flags,
				RX_ENC_FLAG_SHORT_GI | (1 << RX_ENC_FLAG_STBC_SHIFT));
	}
}

static void esp_rx_bad_boundary_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;

	/* A length beyond the supplied buffer cannot safely skip a frame. */
	f->entries[0] = cpu_to_le32(2000 << 8);
	sip_rx_ampdu(f->sip, f->view, &f->ctrl, f->entries, &f->frames);
	KUNIT_EXPECT_EQ(test, f->view->len, 80);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->frames));
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->sip->epub->stats[ESP_RX_FRAME_DROPS]), 1);
}

static void esp_rx_single_security_status_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	static const u8 states[] = {
		0, RX_SECOV_ERR, RX_SECPROT_ERR0, RX_SECPROT_ERR1,
		RX_SECKEY_ERR, RX_SECCRLEN_ERR, RX_SECFIFO_TIMEOUT,
		RX_WEPICV_ERR, RX_TKIPICV_ERR, RX_TKIPMIC_ERR,
		RX_CCMPMIC_ERR, RX_WAPIMIC_ERR,
	};
	unsigned int i, dropped = 0;

	f->ctrl.aggregation = false;
	for (i = 0; i < ARRAY_SIZE(states); i++) {
		struct sk_buff *view, *frame;
		bool accepted = !states[i] || states[i] == RX_TKIPMIC_ERR;
		int pulled = 0;

		view = alloc_skb(48, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, view);
		put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_PROTECTED,
				   skb_put_zero(view, 48));
		f->ctrl.rxend_state = states[i];
		frame = sip_parse_data_rx_info(f->sip, view, 48, 48,
					       &f->ctrl, &pulled);
		KUNIT_EXPECT_EQ_MSG(test, !!frame, accepted, "RX state %02x", states[i]);
		KUNIT_EXPECT_EQ(test, pulled, 48);
		KUNIT_EXPECT_EQ(test, view->len, 0);
		if (frame) {
			KUNIT_EXPECT_EQ(test,
				!!(IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_MMIC_ERROR),
				states[i] == RX_TKIPMIC_ERR);
			kfree_skb(frame);
		} else {
			dropped++;
		}
		kfree_skb(view);
	}
	KUNIT_EXPECT_EQ(test, dropped, ARRAY_SIZE(states) - 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->sip->epub->stats[ESP_RX_FRAME_DROPS]),
			dropped);
}

/* Captured SIP contract: WEP stores no four-byte ICV, CCMP no eight-byte
 * MIC, and TKIP neither its eight-byte Michael MIC nor four-byte ICV.
 * Each still carries its IV and complete MSDU. Include all alignment
 * residues, with poisoned transfer padding that must never reach the stack.
 */
static void esp_rx_single_trailer_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	static const struct {
		unsigned int iv, removed;
	} ciphers[] = { {4, 4}, {8, 8}, {8, 12} };
	static const unsigned int payloads[] = { 63, 64, 65, 66, 512, 1400 };
	unsigned int cipher, sample, i;
	u32 flags = RX_FLAG_DECRYPTED | RX_FLAG_MMIC_STRIPPED |
		    RX_FLAG_MIC_STRIPPED | RX_FLAG_ICV_STRIPPED;

	f->ctrl.aggregation = false;
	for (cipher = 0; cipher < ARRAY_SIZE(ciphers); cipher++) {
		for (sample = 0; sample < ARRAY_SIZE(payloads); sample++) {
			unsigned int stored = 24 + ciphers[cipher].iv + 8 + payloads[sample];
			unsigned int encoded = stored + ciphers[cipher].removed;
			unsigned int transfer = ALIGN(stored, 4);
			struct sk_buff *view, *frame;
			u8 *wire;
			int pulled = 0;

			view = alloc_skb(transfer, GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, view);
			wire = skb_put(view, transfer);
			memset(wire, 0xa5, transfer);
			put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_PROTECTED, wire);
			wire[4] = 0x02; /* Unicast destination. */
			for (i = 24; i < stored; i++)
				wire[i] = i * 31 + sample;
			frame = sip_parse_data_rx_info(f->sip, view, encoded, transfer,
						       &f->ctrl, &pulled);
			KUNIT_EXPECT_EQ(test, pulled, transfer);
			KUNIT_EXPECT_EQ(test, view->len, 0);
			KUNIT_EXPECT_NOT_NULL(test, frame);
			if (frame) {
				KUNIT_EXPECT_EQ_MSG(test, frame->len, stored,
						   "cipher %u payload %u",
						   cipher, payloads[sample]);
				KUNIT_EXPECT_MEMEQ(test, frame->data, wire,
						   min_t(unsigned int, stored, frame->len));
				KUNIT_EXPECT_EQ(test, IEEE80211_SKB_RXCB(frame)->flag, flags);
				kfree_skb(frame);
			}
			kfree_skb(view);
		}
	}
}

static void esp_rx_tkip_michael_error_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	struct sk_buff *view, *frame;
	int pulled = 0;

	/* Same 64-byte TKIP payload geometry as the live trace: encoded 116,
	 * stored 104. A failed Michael check must still reach countermeasures.
	 */
	f->ctrl.aggregation = false;
	f->ctrl.rxend_state = RX_TKIPMIC_ERR;
	view = alloc_skb(104, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, view);
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_PROTECTED,
			   skb_put_zero(view, 104));
	frame = sip_parse_data_rx_info(f->sip, view, 116, 104, &f->ctrl, &pulled);
	KUNIT_EXPECT_EQ(test, pulled, 104);
	KUNIT_EXPECT_NOT_NULL(test, frame);
	if (frame) {
		KUNIT_EXPECT_EQ(test, frame->len, 104);
		KUNIT_EXPECT_TRUE(test, IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_MMIC_ERROR);
		KUNIT_EXPECT_TRUE(test, IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_MMIC_STRIPPED);
		KUNIT_EXPECT_FALSE(test, IEEE80211_SKB_RXCB(frame)->flag & RX_FLAG_IV_STRIPPED);
		kfree_skb(frame);
	}
	kfree_skb(view);
}

static void esp_rx_cleartext_length_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	struct sk_buff *view, *frame;
	int pulled = 0;

	f->ctrl.aggregation = false;
	view = alloc_skb(48, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, view);
	put_unaligned_le16(IEEE80211_FTYPE_DATA, skb_put_zero(view, 48));
	view->data[44] = 0x5a;
	frame = sip_parse_data_rx_info(f->sip, view, 45, 48, &f->ctrl, &pulled);
	KUNIT_EXPECT_EQ(test, pulled, 48);
	KUNIT_EXPECT_NOT_NULL(test, frame);
	if (frame) {
		KUNIT_EXPECT_EQ(test, frame->len, 45);
		KUNIT_EXPECT_EQ(test, frame->data[44], 0x5a);
		KUNIT_EXPECT_EQ(test, IEEE80211_SKB_RXCB(frame)->flag, 0);
		kfree_skb(frame);
	}
	kfree_skb(view);
}

static void esp_rx_plaintext_security_flags_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	static const u16 types[] = {
		IEEE80211_FTYPE_DATA,
		IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH,
		IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON,
		IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DEAUTH,
		IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION,
	};
	unsigned int i;

	f->ctrl.aggregation = false;
	atomic_set(&f->sip->epub->wl.gtk_cnt, 1);
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		struct sk_buff *view = alloc_skb(48, GFP_KERNEL), *frame;
		int pulled = 0;
		u8 *wire;

		KUNIT_ASSERT_NOT_NULL(test, view);
		wire = skb_put_zero(view, 48);
		put_unaligned_le16(types[i], wire);
		wire[4] = 0x02;
		/* Plaintext EAPOL is valid during authentication, but it must
		 * not be reported to the control port as hardware-decrypted.
		 */
		memcpy(wire + 24, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8);
		frame = sip_parse_data_rx_info(f->sip, view, 48, 48,
					       &f->ctrl, &pulled);
		KUNIT_EXPECT_NOT_NULL(test, frame);
		if (frame) {
			KUNIT_EXPECT_EQ_MSG(test, IEEE80211_SKB_RXCB(frame)->flag,
					   0, "frame control %#x", types[i]);
			KUNIT_EXPECT_MEMEQ(test, frame->data, wire, 48);
			kfree_skb(frame);
		}
		kfree_skb(view);
	}
}

static void esp_rx_protected_missing_key_retains_iv_test(struct kunit *test)
{
	struct esp_rx_fixture *f = test->priv;
	unsigned int group;
	u32 flags = RX_FLAG_DECRYPTED | RX_FLAG_MMIC_STRIPPED |
		    RX_FLAG_MIC_STRIPPED | RX_FLAG_ICV_STRIPPED;

	f->ctrl.aggregation = false;
	atomic_set(&f->sip->epub->wl.ptk_cnt, 0);
	atomic_set(&f->sip->epub->wl.gtk_cnt, 0);
	for (group = 0; group < 2; group++) {
		struct sk_buff *view = alloc_skb(40, GFP_KERNEL), *frame;
		int pulled = 0;
		u8 *wire;

		KUNIT_ASSERT_NOT_NULL(test, view);
		wire = skb_put_zero(view, 40);
		put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_PROTECTED, wire);
		wire[4] = group ? 0xff : 0x02;
		memset(wire + 24, 0xa5, 16);
		frame = sip_parse_data_rx_info(f->sip, view, 48, 40,
					       &f->ctrl, &pulled);
		KUNIT_EXPECT_NOT_NULL(test, frame);
		if (frame) {
			/* No IV_STRIPPED/PN_VALIDATED shortcut: mac80211 must
			 * perform key lookup and reject the missing key itself.
			 */
			KUNIT_EXPECT_EQ(test, IEEE80211_SKB_RXCB(frame)->flag, flags);
			KUNIT_EXPECT_MEMEQ(test, frame->data, wire, 40);
			kfree_skb(frame);
		}
		kfree_skb(view);
	}
}

static struct kunit_case esp_rx_cases[] = {
	KUNIT_CASE(esp_rx_plaintext_security_flags_test),
	KUNIT_CASE(esp_rx_protected_missing_key_retains_iv_test),
	KUNIT_CASE(esp_rx_valid_aggregate_test),
	KUNIT_CASE(esp_rx_stbc_status_test),
	KUNIT_CASE(esp_rx_mic_error_tail_test),
	KUNIT_CASE(esp_rx_bad_boundary_test),
	KUNIT_CASE(esp_rx_single_security_status_test),
	KUNIT_CASE(esp_rx_single_trailer_test),
	KUNIT_CASE(esp_rx_tkip_michael_error_test),
	KUNIT_CASE(esp_rx_cleartext_length_test),
	{}
};

static struct kunit_suite esp_rx_suite = {
	.name = "esp8089-rx",
	.init = esp_rx_init,
	.test_cases = esp_rx_cases,
};

kunit_test_suite(esp_rx_suite);

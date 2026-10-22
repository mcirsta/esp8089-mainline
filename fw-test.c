// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "fw.h"
#include "esp_wmac.h"
#include "sip2_common.h"
#include "esp_sif.h"
#include "txreport.h"
#include "tx-batch.h"
#include "esp_sip.h"

static void esp8089_power_readback_test(struct kunit *test)
{
	struct esp_sip *sip;
	int dbm = -1;

	sip = kunit_kzalloc(test, sizeof(*sip), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sip);
	mutex_init(&sip->power.lock);
	atomic_set(&sip->state, SIP_RUN);
	sip->power.effective = 56;
	KUNIT_EXPECT_EQ(test, esp_power_get(sip, &dbm), 0);
	KUNIT_EXPECT_EQ(test, dbm, 14);
	sip->power.effective = 79;
	KUNIT_EXPECT_EQ(test, esp_power_get(sip, &dbm), 0);
	KUNIT_EXPECT_EQ(test, dbm, 19);
	sip->power.blocked = true;
	KUNIT_EXPECT_EQ(test, esp_power_get(sip, &dbm), -EAGAIN);
	KUNIT_EXPECT_EQ(test, dbm, 19);
	sip->power.blocked = false;
	atomic_set(&sip->state, SIP_STOP);
	KUNIT_EXPECT_EQ(test, esp_power_get(sip, &dbm), -EAGAIN);
	KUNIT_EXPECT_EQ(test, dbm, 19);
}

static void esp8089_ack_credit_test(struct kunit *test)
{
	u8 storage[14] = {}, *wire = storage + 1;
	size_t i;

	/* Unaligned payload, every short length, and all non-ACK flags. */
	put_unaligned_le32(ESP_TXR_MAGIC, wire);
	put_unaligned_le32(U32_MAX, wire + 4);
	KUNIT_EXPECT_TRUE(test, esp_txr_ack_only(ESP_TXR_COMMAND, wire, 12));
	for (i = 0; i < 12; i++)
		KUNIT_EXPECT_FALSE(test, esp_txr_ack_only(ESP_TXR_COMMAND, wire, i));
	KUNIT_EXPECT_FALSE(test, esp_txr_ack_only(ESP_TXR_COMMAND, wire, 13));
	KUNIT_EXPECT_FALSE(test, esp_txr_ack_only(29, wire, 12));
	put_unaligned_le32(0, wire);
	KUNIT_EXPECT_FALSE(test, esp_txr_ack_only(ESP_TXR_COMMAND, wire, 12));
	put_unaligned_le32(ESP_TXR_MAGIC, wire);
	for (i = 1; i <= 8; i++) {
		put_unaligned_le32(i, wire + 8);
		KUNIT_EXPECT_FALSE(test, esp_txr_ack_only(ESP_TXR_COMMAND, wire, 12));
	}
}

static void esp8089_tx_credit_fit_test(struct kunit *test)
{
	u32 header = ALIGN(sizeof(struct sip_hdr), 4);
	u32 aggregate = ALIGN(sizeof(struct esp_tx_ampdu_entry), 4);

	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(100, false, 0, 100, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(0, false, 128, 100, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(100, false, 128, -1, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(100, false, 128, 4, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(U32_MAX, true, 128, 100, 5));
	KUNIT_EXPECT_TRUE(test, esp_tx_data_fits(128 - header, false, 128, 6, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(129 - header, false, 128, 6, 5));
	KUNIT_EXPECT_TRUE(test, esp_tx_data_fits(129 - header, false, 128, 7, 5));
	/* The monitor negotiates 260-byte, non-power-of-two credit units. */
	KUNIT_EXPECT_TRUE(test, esp_tx_data_fits(260 - header, false, 260, 6, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(261 - header, false, 260, 6, 5));
	KUNIT_EXPECT_TRUE(test, esp_tx_data_fits(261 - header, false, 260, 7, 5));
	KUNIT_EXPECT_TRUE(test, esp_tx_data_fits(128 - header - aggregate, true, 128, 6, 5));
	KUNIT_EXPECT_FALSE(test, esp_tx_data_fits(129 - header - aggregate, true, 128, 6, 5));
}

static void esp8089_txr8_protocol_test(struct kunit *test)
{
	u8 storage[53] = {}, *wire = storage + 1;
	struct esp_reports *window;
	size_t i;

	window = kunit_kzalloc(test, sizeof(*window), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, window);
	window->active = true;
	window->capacity = 16;
	put_unaligned_le32(ESP_TXR_MAGIC, wire);
	put_unaligned_le32(32, wire + 48);
	KUNIT_EXPECT_EQ(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(4, wire + 8);
	for (i = 0; i < 4; i++)
		put_unaligned_le32(0x803f0007, wire + 20 + i * 8);
	KUNIT_EXPECT_EQ(test, esp_txr_payload_valid(wire, 52), 4);
	for (i = 0; i < 52; i++)
		KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, i), 0);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 53), 0);
	put_unaligned_le32(0x37525854, wire); /* Old ABI must not negotiate. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(ESP_TXR_MAGIC, wire);
	put_unaligned_le32(0x823f0007, wire + 20); /* Reserved bit25. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(0x803f0000, wire + 20); /* Known zero attempts. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(0x003f0007, wire + 20); /* Count/PHY without known. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(0x807f0007, wire + 20); /* Abort cannot be known. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(0x00600000, wire + 20); /* Explicitly unknown abort. */
	KUNIT_EXPECT_EQ(test, esp_txr_payload_valid(wire, 52), 4);
	put_unaligned_le32(0x813fffff, wire + 20); /* NoACK, PHY31,65535attempts. */
	KUNIT_EXPECT_EQ(test, esp_txr_payload_valid(wire, 52), 4);
	put_unaligned_le32(0, wire + 20); /* No outcome. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(5 << 21, wire + 20); /* Invalid outcome. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(0x803f0007, wire + 20);
	put_unaligned_le32(0, wire + 48);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(3, wire + 48);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	/* A batch cannot contain more records than the negotiated ring. */
	put_unaligned_le32(1, wire + 48);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(2, wire + 48);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(128, wire + 48);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(32, wire + 48);
	put_unaligned_le32(5, wire + 8);
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	put_unaligned_le32(4, wire + 8);
	put_unaligned_le32(1, wire + 12); /* Overflow is fatal to ABI. */
	KUNIT_EXPECT_LT(test, esp_txr_payload_valid(wire, 52), 0);
	window->submitted = 8;
	window->acked = U32_MAX - 7;
	KUNIT_EXPECT_TRUE(test, esp_reports_full(window));
	window->acked++;
	KUNIT_EXPECT_FALSE(test, esp_reports_full(window));
	window->capacity = 32;
	window->acked = 0;
	window->submitted = 31;
	KUNIT_EXPECT_FALSE(test, esp_reports_full(window));
	window->submitted++;
	KUNIT_EXPECT_TRUE(test, esp_reports_full(window));
	window->acked++;
	KUNIT_EXPECT_FALSE(test, esp_reports_full(window));
}

static void esp8089_sdio_padding_test(struct kunit *test)
{
	u8 data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	u8 dma[20];
	unsigned int i;

	memset(dma, 0xa5, sizeof(dma));
	KUNIT_ASSERT_EQ(test,
			esp_sdio_prepare_tx(dma, sizeof(dma), data, 8, 16), 0);
	KUNIT_EXPECT_MEMEQ(test, dma, data, sizeof(data));
	for (i = 8; i < 16; i++)
		KUNIT_EXPECT_EQ(test, dma[i], 0);
	for (i = 16; i < sizeof(dma); i++)
		KUNIT_EXPECT_EQ(test, dma[i], 0xa5);
	KUNIT_EXPECT_LT(test, esp_sdio_prepare_tx(dma, sizeof(dma), data, 8, 4),
			0);
	KUNIT_EXPECT_LT(test,
			esp_sdio_prepare_tx(dma, sizeof(dma), data, 8, 24), 0);
}

static void esp8089_rx_transfer_size_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, esp_rx_transfer_size_valid(0));
	KUNIT_EXPECT_FALSE(test, esp_rx_transfer_size_valid(sizeof(struct sip_hdr) - 1));
	KUNIT_EXPECT_TRUE(test,
			  esp_rx_transfer_size_valid(sizeof(struct sip_hdr)));
	KUNIT_EXPECT_TRUE(test, esp_rx_transfer_size_valid(16384));
	KUNIT_EXPECT_TRUE(test, esp_rx_transfer_size_valid(17336));
	KUNIT_EXPECT_TRUE(test, esp_rx_transfer_size_valid(U16_MAX));
	KUNIT_EXPECT_FALSE(test, esp_rx_transfer_size_valid(U16_MAX + 1U));
}

static void esp8089_tx_report_bounds_test(struct kunit *test)
{
	u8 storage[1 + sizeof(__le32) + 2 * sizeof(struct sip_tx_status)] = {};
	u8 *wire = storage + 1;
	size_t length = sizeof(storage) - 1, i;

	put_unaligned_le32(2, wire);
	KUNIT_EXPECT_EQ(test, esp_tx_report_count(wire, length), 2);
	for (i = 0; i < length; i++)
		KUNIT_EXPECT_LT(test, esp_tx_report_count(wire, i), 0);
	put_unaligned_le32(U32_MAX, wire);
	KUNIT_EXPECT_LT(test, esp_tx_report_count(wire, length), 0);
	put_unaligned_le32(0, wire);
	KUNIT_EXPECT_EQ(test, esp_tx_report_count(wire, sizeof(__le32)), 0);
}

static void esp8089_fw_valid(u8 *data)
{
	memset(data, 0, 32);
	data[0] = 0xe9;
	data[1] = 1;
	put_unaligned_le32(0x40100000, data + 4);
	put_unaligned_le32(0x40100000, data + 8);
	put_unaligned_le32(3, data + 12);
	data[16] = 1;
	data[17] = 2;
	data[18] = 3;
	data[31] = 0xef ^ 1 ^ 2 ^ 3;
}

static void esp8089_fw_valid_test(struct kunit *test)
{
	struct esp8089_fw_image image;
	u8 data[32];

	esp8089_fw_valid(data);
	KUNIT_ASSERT_EQ(test, esp8089_fw_parse(data, sizeof(data), &image), 0);
	KUNIT_EXPECT_EQ(test, image.entry, 0x40100000U);
	KUNIT_EXPECT_EQ(test, image.nsegments, 1);
	KUNIT_EXPECT_EQ(test, image.segments[0].len, 3U);
	KUNIT_EXPECT_PTR_EQ(test, image.segments[0].data, data + 16);
}

static void esp8089_fw_truncation_test(struct kunit *test)
{
	struct esp8089_fw_image image;
	u8 data[32];
	size_t size;

	esp8089_fw_valid(data);
	for (size = 0; size < sizeof(data); size++)
		KUNIT_EXPECT_LT(test, esp8089_fw_parse(data, size, &image), 0);
}

static void esp8089_fw_corruption_test(struct kunit *test)
{
	struct esp8089_fw_image image;
	u8 data[32];
	unsigned int i;
	/* Every payload, header and padding byte is checked or bounded. */
	const unsigned int offsets[] = { 0, 1, 12, 16, 17, 18, 19, 30, 31 };

	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		esp8089_fw_valid(data);
		data[offsets[i]] ^= 0xff;
		KUNIT_EXPECT_LT(test, esp8089_fw_parse(data, sizeof(data), &image), 0);
	}
	esp8089_fw_valid(data);
	put_unaligned_le32(U32_MAX, data + 8);
	KUNIT_EXPECT_LT(test, esp8089_fw_parse(data, sizeof(data), &image), 0);
	esp8089_fw_valid(data);
	put_unaligned_le32(U32_MAX, data + 12);
	KUNIT_EXPECT_LT(test, esp8089_fw_parse(data, sizeof(data), &image), 0);
}

static void esp8089_rx_descriptor_test(struct kunit *test)
{
	/* Deliberately unaligned three-word descriptor with signed fields. */
	u8 storage[ESP_RX_CTRL_LEN + 1] = {};
	u8 *wire = storage + 1;
	struct esp_mac_rx_ctrl ctrl;

	put_unaligned_le32(0x06024cd6, wire);
	put_unaligned_le32(0x98080007, wire + 4);
	put_unaligned_le32(0xa0090800, wire + 8);
	esp_rx_ctrl_decode(wire, &ctrl);
	KUNIT_EXPECT_EQ(test, ctrl.rssi, -42);
	KUNIT_EXPECT_EQ(test, ctrl.noise_floor, -96);
	KUNIT_EXPECT_EQ(test, ctrl.rate, 12);
	KUNIT_EXPECT_EQ(test, ctrl.sig_mode, 1);
	KUNIT_EXPECT_EQ(test, ctrl.legacy_length, 1538);
	KUNIT_EXPECT_EQ(test, ctrl.mcs, 7);
	KUNIT_EXPECT_EQ(test, ctrl.ht_length, 2048);
	KUNIT_EXPECT_TRUE(test, ctrl.aggregation);
	KUNIT_EXPECT_TRUE(test, ctrl.sgi);
	KUNIT_EXPECT_EQ(test, ctrl.stbc, 1);
	KUNIT_EXPECT_EQ(test, ctrl.ampdu_cnt, 8);
	KUNIT_EXPECT_EQ(test, ctrl.channel, 9);
}

static struct kunit_case esp8089_fw_cases[] = {
	KUNIT_CASE(esp8089_power_readback_test),
	KUNIT_CASE(esp8089_ack_credit_test),
	KUNIT_CASE(esp8089_tx_credit_fit_test),
	KUNIT_CASE(esp8089_txr8_protocol_test),
	KUNIT_CASE(esp8089_sdio_padding_test),
	KUNIT_CASE(esp8089_rx_transfer_size_test),
	KUNIT_CASE(esp8089_tx_report_bounds_test),
	KUNIT_CASE(esp8089_rx_descriptor_test),
	KUNIT_CASE(esp8089_fw_valid_test),
	KUNIT_CASE(esp8089_fw_truncation_test),
	KUNIT_CASE(esp8089_fw_corruption_test),
	{}
};

static struct kunit_suite esp8089_fw_suite = {
	.name = "esp8089-firmware",
	.test_cases = esp8089_fw_cases,
};

kunit_test_suite(esp8089_fw_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("ESP8089 firmware and protocol KUnit tests");
MODULE_LICENSE("GPL");

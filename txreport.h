/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP_TXREPORT_H
#define ESP_TXREPORT_H
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/log2.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>
#define ESP_TXR_MAGIC 0x38525854U
#define ESP_TXR_MAX_WINDOW 64
#define ESP_TXR_EVENT 23
#define ESP_TXR_COMMAND 28
/* Only an exact cursor ACK can omit an urgent credit reply. Polls, replay,
 * capability and other control commands retain the original request path.
 */
static inline bool esp_txr_ack_only(u32 command, const u8 *buf, size_t len)
{
	return command == ESP_TXR_COMMAND && len == 12 &&
	       get_unaligned_le32(buf) == ESP_TXR_MAGIC &&
	       !get_unaligned_le32(buf + 8);
}

static inline int esp_txr_payload_valid(const u8 *buf, size_t len)
{
	u32 count, i, status, outcome, capacity;

	if (len != 52 || get_unaligned_le32(buf) != ESP_TXR_MAGIC)
		return -EBADMSG;
	count = get_unaligned_le32(buf + 8);
	capacity = get_unaligned_le32(buf + 48);
	if (count > 4 || get_unaligned_le32(buf + 12) ||
	    !is_power_of_2(capacity) || count > capacity ||
	    capacity > ESP_TXR_MAX_WINDOW)
		return -EBADMSG;
	for (i = 0; i < count; i++) {
		status = get_unaligned_le32(buf + 20 + i * 8);
		outcome = (status >> 21) & 7;
		if (status & 0x7e000000U || !outcome || outcome > 4 ||
		    ((status & BIT(31)) && (!(status & 0xffff) || outcome > 2)) ||
		    (!(status & BIT(31)) && (status & 0x1fffffU)))
			return -EBADMSG;
	}
	return count;
}

struct esp_sip;
struct esp_reports {
	struct completion ready;
	struct sk_buff_head pending;
	struct delayed_work poll;
	u32 submitted, acked, next, capacity;
	bool active, failed;
	unsigned long progress;
	unsigned long replay_at;
	u32 replay_delay_ms;
	atomic64_t replay_requests, coalesced_queries;
	atomic64_t records, duplicates, malformed, orphan, ack, noack, fail;
	atomic64_t retry_counter, lost;
	atomic64_t attempts_known, attempts_unknown, attempts_total;
};

void esp_reports_init(struct esp_sip *sip);
int esp_reports_negotiate(struct esp_sip *sip);
int esp_reports_receive(struct esp_sip *sip, const u8 *buf, size_t len);
bool esp_reports_is_ack_only(struct esp_sip *sip, struct sk_buff *skb);
void esp_reports_packed_control(struct esp_sip *sip, struct sk_buff *skb);
void esp_reports_submit(struct esp_sip *sip, struct sk_buff *skb, u32 seq);
void esp_reports_abort(struct esp_sip *sip);
static inline bool esp_reports_full(struct esp_reports *r)
{
	return READ_ONCE(r->failed) ||
	       (r->active && r->submitted - r->acked >= r->capacity);
}
#endif

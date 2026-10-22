// SPDX-License-Identifier: Apache-2.0
/* Modified ESP8089 v19: RAM-only, versioned radio completion protocol.
 * Newly written code; no ESP8266 SDK code is included.
 * State occupies a separately loaded application DRAM extension.
 * SIP callbacks can interrupt completion processing. Protect cursor changes.
 */
#include "fw-critical.h"
typedef unsigned int u32;
#define MAGIC 0x38525854U /* TXR8 */
#define WINDOW 32
#define BATCH 4

struct record { u32 seq, status; };
/* ROM sip_alloc_to_host_evt() pops fixed 64-byte buffers without checking
 * its length argument. Twelve bytes belong to the SIP header.
 */
_Static_assert(sizeof(struct record) == 8, "TXR8 record layout changed");
_Static_assert(16 + 4 * sizeof(struct record) + 4 == 64 - 12,
               "Completion event exceeds the ROM pool buffer");
struct state {
 u32 enabled, head, tail, sent, overflow;
 struct record ring[WINDOW];
};
__attribute__((section(".report_data")))
volatile struct state report_state = { 0 };

extern u32 original_tx_done(u32 *ecb);
extern void original_control(u32 *packet);
extern u32 *alloc_event(u32 length, u32 id);
extern void send_event(u32 *first, u32 *last);
extern void power_control(u32 *request);
extern u32 attempts_finish(u32 *ecb);

/* Retain every record until an explicit host acknowledgement. Allocation
 * failure merely postpones delivery. Queries retransmit from the host cursor.
 * The host must limit unacknowledged submissions to WINDOW and place its
 * acknowledgement command before new data in the same SIP stream.
 */
static void pump(u32 query)
{
 volatile struct state *s = &report_state;
 u32 count, i, ps, *event, *payload;

 /* Gate once, then drain all available events so four records become
  * one SIP reply. A poll/replay flushes a partial batch.
  */
 if (!query && s->head - s->sent < BATCH)
  return;
 do {
  count = s->head - s->sent;
  if (!count && query != 2)
   return;
  event = alloc_event(52, 23);
  if (!event)
   return;
  /* Allocation may run an interrupting SIP callback and replay the queue.
   * Recompute the range and reserve it while interrupts are masked.
   */
  ps = fw_irq_save();
  count = s->head - s->sent;
  if (count > 4)
   count = 4;
  payload = (u32 *)event[1] + 3;
  for (i = 0; i < 13; i++)
   payload[i] = 0;
  payload[0] = MAGIC;
  payload[1] = s->sent;
  payload[2] = count;
  payload[3] = s->overflow;
  payload[12] = WINDOW; /* Negotiated retained-completion capacity. */
  for (i = 0; i < count; i++) {
   volatile struct record *r = &s->ring[(s->sent + i) & (WINDOW - 1)];
   payload[4 + i * 2] = r->seq;
   payload[5 + i * 2] = r->status;
  }
  s->sent += count;
  fw_irq_restore(ps);
  send_event(event, event);
 } while (s->sent != s->head);
}

void report_control(u32 *packet)
{
 u32 *header = (u32 *)packet[1], ps, ack;
 volatile struct state *s = &report_state;

 /* Initialization storage is reused only after the capability handshake. */
 if (header[1] == 7)
  return;
 if (header[1] == 29) {
  if ((header[0] >> 16) >= 24)
   power_control(header + 3);
  return;
 }
#ifdef TXPOWER_CAP_QDB
 /* Experimental boot-time ceiling. Feed reduced targets into the original
  * full PHY calibration, preserving every lower factory per-rate target.
  * This is not a live set_tx_power API or a measured RF compliance claim.
  */
 if (header[1] == 7 && (header[0] >> 16) >= 140) {
  unsigned char *init = (unsigned char *)header + 12;
  u32 i;
  for (i = 34; i < 40; i++)
   if (init[i] > TXPOWER_CAP_QDB)
    init[i] = TXPOWER_CAP_QDB;
 }
#endif
 if (header[1] != 28) {
  original_control(packet);
  return;
 }
 if ((header[0] >> 16) < 24 || header[3] != MAGIC || header[5] > 7)
  return;
 ack = header[4];
 ps = fw_irq_save();
 if (ack - s->tail > s->head - s->tail) {
  fw_irq_restore(ps);
  return;
 }
 s->enabled = 1;
 s->tail = ack;
 /* A delayed ACK can overtake a failed replay allocation. Never read
  * slots the host has released and may already have reused.
  */
 if ((header[5] & 2) || (int)(ack - s->sent) > 0)
  s->sent = ack;
 fw_irq_restore(ps);
 pump((header[5] & 4) ? 2 : (header[5] & 3) ? 1 : 0);
}

u32 report_tx_done(u32 *ecb)
{
 volatile struct state *s = &report_state;
 u32 *desc = (u32 *)ecb[7];
 u32 *header = (u32 *)ecb[3], tracked = attempts_finish(ecb);
 u32 outcome = desc[3] >> 24;
 u32 ps = fw_irq_save();

 if (s->enabled) {
  if (s->head - s->tail < WINDOW) {
   volatile struct record *r = &s->ring[s->head & (WINDOW - 1)];
   r->seq = header[2];
   /* Preserve unknown attempt histories on abort/timeout. Invalid
    * outcomes encode zero and are rejected by the consumer, never folded
    * into a valid outcome or NoACK flag by a narrow mask.
    * bits0..15:attempts;16..20:observed final PHY;21..23:outcome;
    * bit24:NoACK;25..30:reserved zero;31:attempt history known.
    */
   if (outcome > 4) outcome = 0;
   r->status = ((outcome && outcome <= 2) ? tracked : 0) |
               (outcome << 21) | ((desc[0] & 128) << 17);
   s->head++;
  } else {
   s->overflow++;
  }
 }
 fw_irq_restore(ps);
 pump(0);
 return original_tx_done(ecb);
}

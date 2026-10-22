// SPDX-License-Identifier: Apache-2.0
/* Reserve a control buffer when allocating new TXR/PWR events.
 * The ROM free-list owns descriptors of three words and 64-byte buffers.
 */
#include "fw-critical.h"
typedef unsigned int u32;
__attribute__((section(".event_alloc_state")))
volatile u32 event_alloc_deferred;
#ifndef EVENT_FREE_HEAD
#define EVENT_FREE_HEAD (*(volatile u32 *)0x3fffdff8)
#endif

u32 *report_alloc_event(u32 length, u32 id)
{
 u32 ps, *event, *header;
 if (length > 52)
  return 0;
 ps = fw_irq_save();
 event = (u32 *)EVENT_FREE_HEAD;
 /* Check and detach atomically; the ROM allocator's unconditional IRQ
  * unlock cannot provide a nestable reservation around a separate peek.
  */
 if (!event || !event[2]) {
  event_alloc_deferred++;
  fw_irq_restore(ps);
  return 0;
 }
 EVENT_FREE_HEAD = event[2];
 event[2] = 0;
 fw_irq_restore(ps);
 header = (u32 *)event[1];
 header[0] = ((length + 12) << 16) | (id << 8);
 header[1] = header[2] = 0;
 event[0] = (event[0] & 0xff000fffU) | ((length + 12) << 12);
 return event;
}

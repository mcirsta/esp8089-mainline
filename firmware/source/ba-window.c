// SPDX-License-Identifier: Apache-2.0
/* The v19 aggregate builder and BAR calculation share peer byte 0x75.
 * Keep their existing eight-frame ceiling, but never exceed any TX BA
 * window accepted during this peer lifetime. The minimum is intentionally
 * sticky across TID stops: raising it could violate another active TID.
 * SETSTA initializes it to eight for a new HT peer. No new state or ABI.
 */
typedef unsigned int u32;
extern void original_ba_state(unsigned char *peer, u32 action, u32 tid);

void ba_window_start(unsigned char *peer, u32 action, u32 tid, u32 window)
{
 if (!peer || tid >= 8 || !window || window > 64)
  return;
 if (!peer[0x75] || peer[0x75] > 8)
  return;
 if (window < peer[0x75])
  peer[0x75] = window;
 original_ba_state(peer, action, tid);
}

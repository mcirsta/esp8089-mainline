/* SPDX-License-Identifier: Apache-2.0 */
.section .text.ba_window_entry,"ax",@progbits
.global ba_window_entry
.type ba_window_entry,@function
ba_window_entry:
.begin no-transform
 /* At the original TX_OPERATIONAL call, a12 points to the SIP payload.
  * Keep peer/action/TID in a2/a3/a4, add negotiated window in a5.
  * A tail jump preserves the dispatcher's original return address.
  */
 l8ui a5, a12, 3
 j ba_window_start
.end no-transform
.size ba_window_entry,.-ba_window_entry

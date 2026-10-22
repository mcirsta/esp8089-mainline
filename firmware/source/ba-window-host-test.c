// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ba-window.c"

static unsigned calls, observed_tid, observed_limit;
void original_ba_state(unsigned char *peer, u32 action, u32 tid)
{
 assert(action == 2);
 observed_tid = tid;
 observed_limit = peer[0x75];
 calls++;
}

int main(void)
{
 unsigned char peer[0x78], expected[sizeof(peer)];
 unsigned tests = 0;

 for (unsigned old = 1; old <= 8; old++) {
  for (unsigned window = 1; window <= 64; window++) {
   for (unsigned tid = 0; tid < 8; tid++) {
    memset(peer, 0xa5, sizeof(peer));
    peer[0x75] = old;
    memcpy(expected, peer, sizeof(peer));
    expected[0x75] = old < window ? old : window;
    calls = 0;
    ba_window_start(peer, 2, tid, window);
    assert(calls == 1 && observed_tid == tid);
    assert(observed_limit == expected[0x75]);
    assert(!memcmp(peer, expected, sizeof(peer)));
    tests++;
   }
  }
 }
 /* Other TIDs and later sessions cannot reopen a smaller active window. */
 peer[0x75] = 8;
 ba_window_start(peer, 2, 0, 4);
 ba_window_start(peer, 2, 3, 2);
 ba_window_start(peer, 2, 0, 64);
 assert(peer[0x75] == 2);
 for (unsigned window = 0; window <= 255; window++) {
  if (window && window <= 64)
   continue;
  peer[0x75] = 8;
  calls = 0;
  ba_window_start(peer, 2, 0, window);
  assert(!calls && peer[0x75] == 8);
 }
 calls = 0;
 ba_window_start(NULL, 2, 0, 1);
 ba_window_start(peer, 2, 8, 1);
 peer[0x75] = 0;
 ba_window_start(peer, 2, 0, 1);
 peer[0x75] = 9;
 ba_window_start(peer, 2, 0, 1);
 assert(!calls);
 printf("PASS: %u valid peer/window/TID combinations; neighboring fields, minimum-before-enable, sticky multi-TID bound and invalid inputs\n", tests);
}

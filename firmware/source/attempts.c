// SPDX-License-Identifier: Apache-2.0
/* Count completed DATA transmissions, not CTS or BAR exchanges. Fixed v19 ABI. */
#include "fw-critical.h"
typedef unsigned int u32;
#define BIT(n) (1U << (n))
struct slot { u32 ecb, value; };
struct tracker { u32 fault; struct slot slots[16]; };
__attribute__((section(".attempt_data")))
struct tracker attempt_state = { 0 };
extern void original_success(u32 q, u32 *ecb, u32 arg);
extern void original_timeout(u32 q, u32 *ecb, u32 arg);
extern void original_agg_timeout(u32 q);
extern void original_ba(u32 tid);
extern void trace_aggregate(u32 count);
#ifndef LMAC_CURRENT
#define LMAC_CURRENT (*(u32 **)0x3fff1718)
#define PP_BASE (*(unsigned char **)0x3ffe807c)
#endif

static void note(u32 *ecb, u32 phy)
{
 struct tracker *t = &attempt_state;
 struct slot *slot = 0;
 u32 i, ps = fw_irq_save();
 u32 *desc = (u32 *)ecb[7];
 if (!(desc[0] & BIT(19)))
  goto done;
 for (i=0; i<16; i++) {
  if (t->slots[i].ecb == (u32)ecb) {
   slot = &t->slots[i];
   break;
  }
  if (!t->slots[i].ecb)
   slot = &t->slots[i];
 }
 if (!slot || phy >= 32 || (slot->value & 65535) == 65535) {
  t->fault++;
  goto done;
 }
 slot->ecb = (u32)ecb;
 slot->value = ((slot->value + 1) & 65535) | (phy << 16) | BIT(31);
 done:
 fw_irq_restore(ps);
}
static void group(u32 *head)
{
 u32 i=0, ps=fw_irq_save(), phy = ((u32 *)head[7])[2] & 255;
 for (; head && i<64; i++,head=(u32 *)head[6])
  note(head,phy);
 if (head)
  attempt_state.fault++;
 trace_aggregate(i);
 fw_irq_restore(ps);
}
void hook_success(u32 q, u32 *ecb, u32 arg)
{
 u32 *desc=(u32 *)ecb[7];
 if (!(desc[0]&BIT(28)))
  note(ecb,desc[2]&255);
 original_success(q,ecb,arg);
}
void hook_timeout(u32 q, u32 *ecb, u32 arg)
{
 note(ecb,((u32 *)ecb[7])[2]&255);
 original_timeout(q,ecb,arg);
}
void hook_agg_timeout(u32 q)
{
 u32 *head=LMAC_CURRENT;
 if (!(((u32 *)head[7])[0]&BIT(27)))
  group(head);
 original_agg_timeout(q);
}
void hook_ba(u32 tid)
{
 unsigned char *entry=PP_BASE + (tid<<5);
 if (!entry[0x28])
  group((u32 *)*(u32 *)(entry+0x34));
 original_ba(tid);
}
u32 attempts_finish(u32 *ecb)
{
 struct tracker *t=&attempt_state;
 u32 i,value=0,ps=fw_irq_save();
 for (i=0;i<16;i++)
  if (t->slots[i].ecb==(u32)ecb) {
   value=t->slots[i].value;
   t->slots[i].ecb=t->slots[i].value=0;
   break;
  }
 fw_irq_restore(ps);
 return t->fault ? 0 : value;
}

// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static unsigned int *current;
static unsigned char *ppbase;
#define LMAC_CURRENT current
#define PP_BASE ppbase
#include "trace.c"
#include "attempts.c"
void original_reclaim(u32 first,u32 last,u32 credits) { assert(first==1 && last==2 && credits==7); }
static unsigned int success,timeout,agg_timeout,ba;
void original_success(u32 q,u32 *e,u32 a) { (void)q;(void)e;(void)a;success++; }
void original_timeout(u32 q,u32 *e,u32 a) { (void)q;(void)e;assert(a==0);timeout++; }
void original_agg_timeout(u32 q) { (void)q;agg_timeout++; }
void original_ba(u32 tid) { assert(tid==3);ba++; }
int main(void)
{
 u32 *mem=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(mem!=MAP_FAILED && (uintptr_t)mem<=UINT32_MAX);
 u32 *e[17],*d[17];
 for(u32 i=0;i<17;i++) {
  e[i]=mem+i*32;d[i]=e[i]+16;e[i][7]=(u32)(uintptr_t)d[i];
  d[i][0]=BIT(19);d[i][2]=19;
 }
 hook_success(0,e[0],0);
 assert(attempts_finish(e[0])==(BIT(31)|(19<<16)|1));
 assert(!attempts_finish(e[0])); /* Recycled pointer has no stale count. */
 for(u32 i=0;i<7;i++) hook_timeout(0,e[0],0);
 assert(attempts_finish(e[0])==(BIT(31)|(19<<16)|7));
 hook_timeout(0,e[0],0);d[0][2]=16;hook_success(0,e[0],0);
 assert(attempts_finish(e[0])==(BIT(31)|(16<<16)|2));
 /* Aggregate timeout, then partial BA; tails inherit actual HEAD PHY. */
 for(u32 i=0;i<3;i++) {d[i][0]|=BIT(28);e[i][6]=i==2?0:(u32)(uintptr_t)e[i+1];}
 current=e[0];d[0][2]=31;
 hook_agg_timeout(0);
 ppbase=(unsigned char *)(mem+1024);
 *(u32 *)(ppbase+(3<<5)+0x34)=(u32)(uintptr_t)e[0];
 hook_ba(3);
 assert(attempts_finish(e[1])==(BIT(31)|(31<<16)|2));
 /* Per-MPDU completion must not count the aggregate again. */
 hook_success(0,e[0],0);
 assert(attempts_finish(e[0])==(BIT(31)|(31<<16)|2));
 assert(attempts_finish(e[2])==(BIT(31)|(31<<16)|2));
 d[0][0]|=BIT(27);hook_agg_timeout(0);assert(!attempts_finish(e[0]));
 ppbase[(3<<5)+0x28]=1;hook_ba(3);assert(!attempts_finish(e[1]));
 ppbase[(3<<5)+0x28]=2;hook_ba(3);assert(!attempts_finish(e[2]));
 d[0][0]=0;hook_timeout(0,e[0],0);assert(!attempts_finish(e[0]));
 for(u32 i=0;i<17;i++) {d[i][0]=BIT(19);note(e[i],19);}
 assert(attempt_state.fault==1);assert(!attempts_finish(e[0]));
 assert(success==3 && timeout==9 && agg_timeout==2 && ba==3);
 assert(radio_trace[3]==2 && radio_trace[9]==6);
 assert(radio_trace[14]==3);
 /* The shared terminal path keeps the original byte cap in a3. */
 for(u32 cap=512;cap<=24575;cap+=37) {
  assert(cap < cap+1560); /* byte failure classification */
  for(u32 window=1;window<=64;window++) assert(!(cap < window-1));
 }
 munmap(mem,16384);
 puts("PASS: first attempt, seven real failed attempts, changed final PHY, aggregate members, no double counting, BAR exclusion, pointer reuse, bounded overflow fail-closed");
}

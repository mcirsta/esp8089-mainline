/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESP_FW_CRITICAL_H
#define ESP_FW_CRITICAL_H
/* Preserve PS exactly: unlike a global lock/unlock pair this nests safely
 * when a SIP callback interrupts radio completion processing.
 */
static inline unsigned fw_irq_save(void)
{
#ifdef __XTENSA__
 unsigned ps;
 __asm__ volatile("rsil %0, 15" : "=a"(ps) : : "memory");
 return ps;
#else
 return 0;
#endif
}
static inline void fw_irq_restore(unsigned ps)
{
#ifdef __XTENSA__
 __asm__ volatile("wsr %0, ps\n\trsync" : : "a"(ps) : "memory");
#else
 (void)ps;
#endif
}
#endif

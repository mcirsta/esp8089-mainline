/* SPDX-License-Identifier: Apache-2.0 */
.section .text.handoff,"ax",@progbits
.global copy_handoff
.type copy_handoff,@function
copy_handoff:
.begin no-transform
1:
 l32i.n a5, a3, 0
 s32i.n a5, a2, 0
 addi.n a3, a3, 4
 addi.n a2, a2, 4
 addi a4, a4, -4
 bnez a4, 1b
 memw
 isync
 wsr a6, ps
 rsync
 mov.n a2, a7
 jx a8
.end no-transform
.size copy_handoff,.-copy_handoff

/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(report_control)
PROVIDE(original_tx_done = 0x40109fb0);
PROVIDE(original_control = 0x40100e10);
PROVIDE(alloc_event = report_alloc_event);
PROVIDE(send_event = 0x40005808);
PROVIDE(tx_atten_set_interp = 0x40101c68);
PROVIDE(original_success = 0x401070a4);
PROVIDE(original_timeout = 0x40107754);
PROVIDE(original_agg_timeout = 0x401079dc);
PROVIDE(original_ba = 0x4010b060);
PROVIDE(original_ba_state = 0x4010cae4);
SECTIONS
{
 . = 0x3fffb348;
 .state_data : { report.o(.report_data) powerctl.o(.power_state) attempts.o(.attempt_data) event-alloc.o(.event_alloc_state) trace.o(.trace_data) }
 ASSERT(. <= 0x3fffc000, "State exceeds application DRAM")
 . = 0x40100358;
 .handoff : { handoff.o(.text.handoff) . = ALIGN(4); }
 ASSERT(. <= 0x40100378, "Copy handoff exceeds reserved 32 bytes")
 . = 0x40100378;
 .prepare : {
  powerctl.o(.literal.power_prepare) powerctl.o(.text.power_prepare)
  . = ALIGN(4);
 }
 ASSERT(. <= 0x40100426, "Prepare overlay exceeds retired INIT code")
 . = 0x4010045c;
 .boot_overlay : {
  debug-read.o(.literal*) debug-read.o(.text*)
  attempts.o(.literal.group) attempts.o(.text.group) . = ALIGN(4);
 }
 ASSERT(. <= 0x401004f3, "Debug overlay exceeds retired startup code")
 . = 0x40101414;
 .power : {
  powerctl.o(.literal.power_apply) powerctl.o(.text.power_apply) . = ALIGN(4);
 }
 ASSERT(. <= 0x40101548, "Power overlay exceeds retired debug handler")
 . = 0x4010d3e4;
 .reply : {
  powerctl.o(.literal.power_control) powerctl.o(.text.power_control) . = ALIGN(4);
 }
 ASSERT(. <= 0x4010d474, "Power reply exceeds retired debug dump")
 . = 0x4010051c;
 .wrappers : { attempts.o(.literal.hook_success) attempts.o(.text.hook_success)
  attempts.o(.literal.hook_timeout) attempts.o(.text.hook_timeout) . = ALIGN(4); }
 ASSERT(. <= 0x401005bc, "Normal wrappers overflow")
 . = 0x40100920;
 .aggwrap : { attempts.o(.literal.hook_agg_timeout) attempts.o(.text.hook_agg_timeout) . = ALIGN(4); }
 ASSERT(. <= 0x4010096c, "Aggregate wrapper overflow")
 . = 0x40100c5c;
 .tracker : { attempts.o(.literal.note) attempts.o(.text.note) . = ALIGN(4); }
 ASSERT(. <= 0x40100cd4, "Tracker overflow")
 . = 0x4010b520;
 .event_allocator : { ba-window-entry.o(.text*)
  ba-window.o(.literal*) ba-window.o(.text*)
  event-alloc.o(.literal*) event-alloc.o(.text*)
  trace.o(.literal.trace_aggregate) trace.o(.text.trace_aggregate) . = ALIGN(4); }
 ASSERT(. <= 0x4010b660, "Event allocator exceeds retired PP INIT code")
 . = 0x4010fcc8;
 .upper : {
  report.o(.literal*)
  ASSERT(. <= 0x4010fd00, "Too many upper literals");
  . = 0x38;
  report.o(.text.report_control)
  report.o(.text.pump*)
  report.o(.text.report_tx_done)
  attempts.o(.literal.attempts_finish) attempts.o(.text.attempts_finish)
  attempts.o(.literal.hook_ba) attempts.o(.text.hook_ba)
  . = ALIGN(4);
 }
 ASSERT(. <= 0x40110000, "Upper overlay exceeds IRAM")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.xtensa*) *(.xt.*) }
 /* Every loadable byte must be explicitly staged and installed. Compiler
  * helper sections must never become uncopied linker orphans.
  */
 .unexpected : { *(*) }
 ASSERT(SIZEOF(.unexpected) == 0, "Unmapped overlay input section")
}

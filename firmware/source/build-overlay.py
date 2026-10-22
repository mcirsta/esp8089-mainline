#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a fixed-input, capability-installed RAM overlay. Never accesses USB."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

P = Path(__file__).resolve().parent
ORIGINAL = P.parent / 'vendor/eagle_fw_first_init_v19.bin'
EXPECTED = '1b8b7f93f40d3c2dd7ab7f3e2d29cd118604249a5926a320163bf358254e4162'
TOOL = os.environ.get('CROSS_COMPILE', 'xtensa-lx106-elf-')


def run(*args):
    subprocess.run([str(a) for a in args], check=True, cwd=P)


def extract(elf, section, output):
    run(TOOL+'objcopy', '-O', 'binary', '--only-section='+section, elf, output)
    return (P/output).read_bytes()


def segments(image):
    result, at = [], 8
    for _ in range(image[1]):
        addr, length = struct.unpack_from('<II', image, at)
        at += 8
        result.append((addr, image[at:at+length]))
        at += length
    return result


def pack(original, parts):
    out = bytearray(original[:8]); out[1] = len(parts); checksum = 0xef
    for address, data in parts:
        out += struct.pack('<II', address, len(data)) + data
        for byte in data:
            checksum ^= byte
    out += bytes((15-len(out)%16)%16) + bytes([checksum])
    return out


def main():
    global ORIGINAL, TOOL
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original',type=Path,default=ORIGINAL)
    parser.add_argument('--toolchain-prefix',default=TOOL)
    args=parser.parse_args()
    ORIGINAL=args.original.expanduser().resolve()
    TOOL=args.toolchain_prefix
    original = ORIGINAL.read_bytes()
    assert hashlib.sha256(original).hexdigest() == EXPECTED
    source_parts = segments(original)

    def read(address, length):
        for base, data in source_parts:
            if base <= address and address+length <= base+len(data):
                return data[address-base:address-base+length]
        raise ValueError(hex(address))

    for source, obj in [('txreport.c','report.o'),('powerctl.c','powerctl.o'),
                        ('debug-read.c','debug-read.o'),('attempts.c','attempts.o'),('event-alloc.c','event-alloc.o'),('trace.c','trace.o'),('ba-window.c','ba-window.o')]:
        run(TOOL+'gcc', '-Os', '-mlongcalls', '-ffreestanding', '-fno-builtin',
            '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-nostdlib',
            '-ffunction-sections', '-Wall', '-Wextra', '-Werror', '-c', source, '-o', obj)
    run(TOOL+'as','handoff.S','-o','handoff.o')
    run(TOOL+'as','ba-window.S','-o','ba-window-entry.o')
    run(TOOL+'ld','-T','overlay.ld','-Map=overlay.map','report.o','powerctl.o',
        'debug-read.o','handoff.o','attempts.o','event-alloc.o','trace.o','ba-window.o','ba-window-entry.o','-o','overlay.elf')
    mapping=(P/'overlay.map').read_text()

    def symbol(name):
        match=re.search(r'0x([0-9a-f]+)\s+'+re.escape(name)+r'\s*$',mapping,re.M)
        assert match,name
        return int(match[1],16)

    assert symbol('report_control') == 0x4010fd00
    state=extract('overlay.elf','.state_data','state.bin')
    stage_address=(0x3fffb348+len(state)+3)&~3
    stage=bytearray(); plan=[]

    def stage_bytes(data):
        while len(stage)&3:
            stage.append(0)
        address=stage_address+len(stage)
        stage.extend(data)
        return address

    for name, address in [('event_allocator',0x4010b520),('handoff',0x40100358),('prepare',0x40100378),
                           ('boot_overlay',0x4010045c),('power',0x40101414),
                           ('reply',0x4010d3e4),('tracker',0x40100c5c),('wrappers',0x4010051c),('aggwrap',0x40100920)]:
        data=extract('overlay.elf','.'+name,name+'.bin')
        assert data and len(data)%4==0
        plan.append({'role':name,'destination':address,'source':stage_bytes(data),
                     'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})

    # Install unaligned three-byte CALL0 replacements as whole aligned words.
    for name, address, target in [('done',0x4010a1d6,symbol('report_tx_done')),
                                   ('read',0x401012ae,symbol('debug_read')),
        ('success1',0x4010742d,symbol('hook_success')),
        ('success2',0x401074b6,symbol('hook_success')),
        ('success3',0x401075aa,symbol('hook_success')),
        ('timeout',0x40107bc6,symbol('hook_timeout')),
        ('agg_timeout',0x40107bbe,symbol('hook_agg_timeout')),
        ('ba',0x401097bb,symbol('hook_ba')),
        ('ba_window',0x401013c7,symbol('ba_window_entry'))]:
        (P/(name+'.S')).write_text('.section .hook,"ax",@progbits\n'
            '.begin no-transform\ncall0 target\n.end no-transform\n')
        (P/(name+'.ld')).write_text(f'PROVIDE(target = {target:#x});\n'
            f'SECTIONS {{ . = {address:#x}; .hook : {{ *(.hook) }} }}\n')
        run(TOOL+'as',name+'.S','-o',name+'.o')
        run(TOOL+'ld','-T',name+'.ld',name+'.o','-o',name+'.elf')
        call=extract(name+'.elf','.hook',name+'.bin')
        assert len(call)==3
        base=address&~3
        block=bytearray(read(base,8));at=address-base;block[at:at+3]=call
        plan.append({'role':name+'_call','destination':base,'source':stage_bytes(block),
                     'bytes':8,'sha256':hashlib.sha256(block).hexdigest(),
                     'call_address':address,'target':target,
                     'original_sha256':hashlib.sha256(read(base,8)).hexdigest()})

    # Retire the three diagnostic pipeline-stop hooks. Their former code
    # and staging space now hold the TX BA enforcement hook; no new RAM pool.
    # Proven rate-dependent byte budget: rates21..23 and29..31 select
    # this sole-caller literal. Keep the station peer maximum clamp and its
    # eight-frame sequence window unchanged; only allow the eighth frame.
    assert read(0x4010c1e0,4)==struct.pack('<I',10920)
    block=struct.pack('<I',12480)
    plan.append({'role':'high_rate_aggregate_budget','destination':0x4010c1e0,
                 'source':stage_bytes(block),'bytes':4,
                 'sha256':hashlib.sha256(block).hexdigest(),
                 'old_bytes':10920,'new_bytes':12480,
                 'original_sha256':hashlib.sha256(read(0x4010c1e0,4)).hexdigest()})
    upper=extract('overlay.elf','.upper','upper.bin')
    assert len(upper)%4==0
    upper_source=stage_bytes(upper)
    plan_address=stage_bytes(b''.join(struct.pack('<III',e['destination'],e['source'],e['bytes']) for e in plan))
    assert stage_address+len(stage)<=0x3fffc000
    (P/'stage.bin').write_bytes(stage)
    # CALL0 ABI: RA/SP and callee-saved a12..a15 remain untouched. The last
    # copy runs outside the overwritten bootstrap and tail-jumps to its replacement.
    bootstrap=f'''/* SPDX-License-Identifier: Apache-2.0 */
.section .bootstrap_literals,"a",@progbits
.align 4
magic: .word 0x38525854
original_control_ptr: .word 0x40100e10
plan_ptr: .word {plan_address:#x}
upper_dest: .word 0x4010fcc8
upper_src: .word {upper_source:#x}
upper_size: .word {len(upper)}
handoff_ptr: .word 0x40100358
entry_ptr: .word 0x4010fd00
.section .bootstrap,"ax",@progbits
.global bootstrap_control
bootstrap_control:
.begin no-transform
 l32i.n a3, a2, 4
 l32i.n a4, a3, 4
 movi a5, 28
 bne a4, a5, forward
 l16ui a4, a3, 2
 movi a5, 24
 bltu a4, a5, done
 l32i.n a4, a3, 12
 l32r a5, magic
 bne a4, a5, done
 l32i.n a4, a3, 20
 bbci a4, 2, done
 mov.n a7, a2
 rsil a6, 15
 l32r a9, plan_ptr
 movi a10, {len(plan)}
next_copy:
 l32i.n a2, a9, 0
 l32i.n a3, a9, 4
 l32i.n a4, a9, 8
 addi.n a9, a9, 12
copy_words:
 l32i.n a5, a3, 0
 s32i.n a5, a2, 0
 addi.n a3, a3, 4
 addi.n a2, a2, 4
 addi a4, a4, -4
 bnez a4, copy_words
 addi.n a10, a10, -1
 bnez a10, next_copy
 l32r a2, upper_dest
 l32r a3, upper_src
 l32r a4, upper_size
 l32r a8, entry_ptr
 l32r a5, handoff_ptr
 memw
 isync
 jx a5
forward:
 l32r a3, original_control_ptr
 jx a3
done:
 ret.n
.end no-transform
'''
    (P/'bootstrap.S').write_text(bootstrap)
    (P/'bootstrap.ld').write_text('ENTRY(bootstrap_control)\nSECTIONS {\n'
        ' . = 0x4010fcc8; .bootstrap_literals : { *(.bootstrap_literals) }\n'
        ' ASSERT(. <= 0x4010fd00, "Bootstrap literals exceed prefix")\n'
        ' . = 0x4010fd00; .bootstrap : { *(.bootstrap) . = ALIGN(4); }\n'
        ' ASSERT(. <= 0x40110000, "Bootstrap exceeds IRAM")\n}\n')
    run(TOOL+'as','bootstrap.S','-o','bootstrap.o')
    run(TOOL+'ld','-T','bootstrap.ld','bootstrap.o','-o','bootstrap.elf')
    run(TOOL+'objcopy','-O','binary','bootstrap.elf','bootstrap.bin')
    bootstrap_blob=(P/'bootstrap.bin').read_bytes()
    assert len(bootstrap_blob)<=0x338
    initial=[]
    for base, data in source_parts:
        data=bytearray(data)
        if base<=0x40100904<base+len(data):
            at=0x40100904-base
            assert data[at:at+4]==struct.pack('<I',0x40100e10)
            data[at:at+4]=struct.pack('<I',0x4010fd00)
        assert base+len(data)<=0x4010fcc8 or base>=0x40110000
        initial.append((base,data))
    initial.extend([(0x4010fcc8,bootstrap_blob),(0x3fffb348,state),(stage_address,stage)])
    image=pack(original,initial)
    (P/'esp8089-txr8-v1.bin').write_bytes(image)
    manifest={'filename':'esp8089-txr8-v1.bin','bytes':len(image),
        'sha256':hashlib.sha256(image).hexdigest(),'original_sha256':EXPECTED,
        'state_address':0x3fffb348,'state_bytes':len(state),
        'state_symbols':{n:hex(symbol(n)) for n in ['report_state','power_state','attempt_state','event_alloc_deferred','radio_trace']},
        'source_sha256':{n:hashlib.sha256((P/n).read_bytes()).hexdigest() for n in ['txreport.c','attempts.c','powerctl.c','debug-read.c','fw-critical.h','event-alloc.c','trace.c','ba-window.c','ba-window.S','handoff.S','overlay.ld','build-overlay.py']},
        'staging_address':stage_address,'staging_bytes':len(stage),
        'plan_address':plan_address,'plan':plan,
        'upper':{'destination':0x4010fcc8,'source':upper_source,'bytes':len(upper),
                 'sha256':hashlib.sha256(upper).hexdigest()},
        'entry':0x4010fd00,'bootstrap_bytes':len(bootstrap_blob),
        'symbols':{n:hex(symbol(n)) for n in ['report_control','report_tx_done','debug_read','power_apply','power_control']},
        'event_capacity':64,'payload_bytes':52,'records_per_event':4,
        'protocol':'TXR8','retained_completion_capacity':32,
        'notification_batch_records':4,'explicit_partial_flush_flag':1,
        'active_attempt_tracking_slots':16,'event_buffer_bytes':64,'tx_ba_window_enforced':True,'tx_ba_policy':'minimum accepted TX window per peer lifetime, ceiling8','pipeline_stop_probes_removed':True,'radio_group_hist_bins':9,'radio_group_sum_word':9,'radio_group_max_word':14,'original_reclaim_unwrapped':True,
        'install_trigger':'first valid explicit TXR8 capability request, after runtime INIT',
        'init_after_install':'unsupported; host must reset and download fresh firmware',
        'license':'Apache-2.0','experimental':True,'mainline_ready':False}
    (P/'overlay-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))

if __name__=='__main__':
    main()

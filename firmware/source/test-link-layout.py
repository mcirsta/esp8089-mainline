#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject any compiler-generated code/data omitted from the RAM copy plan."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--toolchain-prefix',default=os.environ.get('CROSS_COMPILE','xtensa-lx106-elf-'))
a=p.parse_args();here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='txr7-link-',dir=here) as temp:
    d=Path(temp);source=d/'omitted.S';obj=d/'omitted.o'
    source.write_text('.section .text.omitted_helper,"ax",@progbits\n.byte 0,0,0,0\n')
    subprocess.run([a.toolchain_prefix+'as',str(source),'-o',str(obj)],check=True)
    r=subprocess.run([a.toolchain_prefix+'ld','-T','overlay.ld','report.o','powerctl.o','debug-read.o','handoff.o','attempts.o','event-alloc.o','trace.o','ba-window.o','ba-window-entry.o',str(obj),'-o',str(d/'rejected.elf')],cwd=here,capture_output=True,text=True)
    assert r.returncode and 'Unmapped overlay input section' in r.stderr,r.stderr
print('PASS: unexpected compiler section rejected by linker')

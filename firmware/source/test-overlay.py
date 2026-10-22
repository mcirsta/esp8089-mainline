#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check image checksum, copy bounds, staged bytes and untouched live instructions."""
import hashlib, importlib.util, json, struct
from pathlib import Path
P=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('builder',P/'build-overlay.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
import argparse
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--original',type=Path,default=b.ORIGINAL)
args=parser.parse_args();b.ORIGINAL=args.original
m=json.loads((P/'overlay-manifest.json').read_text())
image=(P/m['filename']).read_bytes()
assert hashlib.sha256(image).hexdigest()==m['sha256']
parts=b.segments(image);original=b.segments(b.ORIGINAL.read_bytes())
check=0xef
for _,data in parts:
 for value in data: check^=value
assert image[-1]==check and len(image)%16==0
assert len(parts)<=16
memory={}
for base,data in parts:
 for i,value in enumerate(data):
  assert base+i not in memory
  memory[base+i]=value

def read(base,n): return bytes(memory[base+i] for i in range(n))
# Before negotiation, INIT/startup/shared literal pools and TX paths are original.
for base,data in original:
 expected=bytearray(data)
 if base<=0x40100904<base+len(data):
  struct.pack_into('<I',expected,0x40100904-base,m['entry'])
 assert read(base,len(data))==expected
allowed=[(0x401013c4,0x401013cc),(0x4010ad6c,0x4010ad70),(0x4010ad78,0x4010ad7c),(0x4010adb8,0x4010adc0),(0x4010c1e0,0x4010c1e4),(0x40100984,0x40100988),(0x4010b520,0x4010b660),(0x40100358,0x40100426),(0x4010045c,0x401004f3),
         (0x40101414,0x40101548),(0x4010d3e4,0x4010d474),
         (0x4010a1d4,0x4010a1dc),(0x401012ac,0x401012b4),
         (0x4010fcc8,0x40110000), (0x4010051c,0x401005bc),
         (0x40100920,0x4010096c),(0x40100c5c,0x40100cd4),
         (0x4010742c,0x40107434),(0x401074b4,0x401074bc),
         (0x401075a8,0x401075b0),(0x40107bc4,0x40107bcc),
         (0x40107bbc,0x40107bc4),(0x401097b8,0x401097c0)]
changed=set()
for e in m['plan']+[m['upper']]:
 dst,src,n=(e[k] for k in ('destination','source','bytes'))
 assert n>0 and not (dst|src|n)&3
 assert m['staging_address']<=src<src+n<=m['staging_address']+m['staging_bytes']<=0x3fffc000
 assert any(lo<=dst<dst+n<=hi for lo,hi in allowed)
 data=read(src,n)
 assert hashlib.sha256(data).hexdigest()==e['sha256']
 if 'patch_address' in e:
  offset=e['patch_address']-dst;before=read(dst,n)
  assert before[:offset]==data[:offset] and before[offset+e['patch_bytes']:]==data[offset+e['patch_bytes']:]
 if 'call_address' in e:
  offset=e['call_address']-dst
  before=read(dst,n)
  assert before[:offset]==data[:offset] and before[offset+3:]==data[offset+3:]
 for i,value in enumerate(data):
  assert dst+i not in changed
  changed.add(dst+i);memory[dst+i]=value
# No shared pool or unrelated live code is overwritten by the installer.
for base,data in original:
 for i,value in enumerate(data):
  address=base+i
  if address not in changed and not 0x40100904<=address<0x40100908:
   assert memory[address]==value
assert read(0x40100984,4)==struct.pack('<I',0x400052c0)
assert read(0x4010fcc8,m['upper']['bytes'])==(P/'upper.bin').read_bytes()
assert read(0x40100358,32)==(P/'handoff.bin').read_bytes()
# Debug replies stay live after INIT. Do not reclaim this sole-caller function.
assert not any(0x40100c1c<=a<0x40100c5c for a in changed)
print('PASS: checksum, segment nonoverlap, original INIT preservation, aligned bounded copies, call neighbors, shared literals, final handoff')

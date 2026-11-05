#!/usr/bin/env python3
"""whowrote.py - find the instruction that stored an object handler / pointer value (original image).

  whowrote.py --script S.txt --frame F --value 6b83e
Step 1: break at the object dispatcher (jsr (A0) at 0x9de4) in frame F and find the object (A4)
        whose handler A0 == value.   (--addr ADDR skips this step and watches ADDR directly)
Step 2: watch that RAM longword up to frame F and print the writes that produced the value,
        followed by the disassembly around the writing instruction."""
import argparse
import os
import sys as _sys
_sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
KOF97 = os.path.join(ROOT, 'build/kof97')
NV = os.path.join(ROOT, 'build/shifttest/whowrote.nv')


def run(script, frames, extra):
    if os.path.exists(NV):
        os.remove(NV)
    cmd = [KOF97, '--interp', '--headless', '--frames', str(frames), '--script', script, '--rtc-fixed', '883612800',
           '--misses', '', '--nvram', NV, '--quiet'] + extra
    return subprocess.run(cmd, capture_output=True, text=True).stderr.splitlines()


ap = argparse.ArgumentParser()
ap.add_argument('--script', required=True)
ap.add_argument('--frame', type=int, required=True)
ap.add_argument('--value', required=True)
ap.add_argument('--addr')
ap.add_argument('--dispatch', default=game.CFG['objects']['dispatch'])
ap.add_argument('--spawn', default=game.CFG['objects']['spawn_store'])
args = ap.parse_args()
val = int(args.value, 16)
addr = int(args.addr, 16) if args.addr else None
if addr is None:
    for l in run(args.script, args.frame + 1, ['--break', args.dispatch]):
        t = l.split()
        if l.startswith('break frame %d ' % args.frame) and int(t[17], 16) == val:
            addr = int(t[21], 16)
            break
    if addr is None:
        sys.exit('no object with handler %x at frame %d' % (val, args.frame))
    print('object at %06x' % addr)
writes = [l for l in run(args.script, args.frame + 1, ['--watch', '%x-%x' % (addr, addr + 3)]) if l.startswith('watch')]
lo = '%04x' % (val & 0xffff)
hits = [l for l in writes if ('= ' + lo) in l]
for l in hits[-3:]:
    print(l)
if hits:
    pc = int(hits[-1].split()[4], 16)
    print(subprocess.run([sys.executable, os.path.join(ROOT, 'tools/recomp/disasm.py'), '%x' % max(0, pc - 16), '8'],
                         capture_output=True, text=True).stdout)
    # object spawn routine (move.l A0,(A1) at 0x30cc): show the caller that passed the handler in A0
    if pc == int(args.spawn, 16):
        fr = int(hits[-1].split()[2])
        for l in run(args.script, fr + 1, ['--break', args.spawn]):
            t = l.split()
            if l.startswith('break frame %d ' % fr) and int(t[17], 16) == val:
                ret = int(t[6], 16)
                print('spawned by the call returning to %06x:' % ret)
                print(subprocess.run([sys.executable, os.path.join(ROOT, 'tools/recomp/disasm.py'), '%x' % (ret - 24), '8'],
                                     capture_output=True, text=True).stdout)
                break

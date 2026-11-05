#!/usr/bin/env python3
"""pairrun.py - run the original and one shifted image side by side for the shift debug loop.

Both runs use --interp --headless --rtc-fixed and a fresh backup RAM; stderr (--watch / --break /
--trace-frame logs) goes to <out>/<side>.log, everything else into <out>/<side>.* .

  pairrun.py --insert 1e62 --script S.txt --frames 1300 -- --ram-log {out}/{side}.ram --ram-log-from 1000
  pairrun.py --insert 1e62 --script S.txt --frames 1300 -- --watch 108100-1082ff
Placeholders in the passthrough args: {out}, {side}; {pc:XXXX} becomes the address of original label
L_XXXX in that image (so --break {pc:9eb0} breaks at the same instruction on both sides).
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.dirname(__file__))
import shifttest  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--insert', required=True)
    ap.add_argument('--script')
    ap.add_argument('--autoplay', action='store_true')
    ap.add_argument('--frames', type=int, required=True)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/shifttest/pair'))
    ap.add_argument('rest', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    rest = args.rest[1:] if args.rest[:1] == ['--'] else args.rest
    os.makedirs(args.out, exist_ok=True)
    roms_s, _, _ = shifttest.build_shift(args.insert)
    syms = shifttest.elf_syms(os.path.join(shifttest.OUT, 'ins_' + args.insert, 'shift', 'p1.elf'))
    inp = ['--autoplay'] if args.autoplay else ['--script', os.path.abspath(args.script)]
    procs = []
    for side, roms in (('orig', os.path.join(ROOT, 'build/roms')), ('shift', roms_s)):
        nv = os.path.join(args.out, side + '.nv')
        if os.path.exists(nv):
            os.remove(nv)

        def sub(a):
            a = a.replace('{out}', args.out).replace('{side}', side)
            return re.sub(r'\{pc:([0-9a-fA-F]+)\}',
                          lambda m: '%x' % (int(m.group(1), 16) if side == 'orig' else syms[int(m.group(1), 16)]), a)
        cmd = [shifttest.KOF97, '--roms', roms, '--interp', '--headless', '--frames', str(args.frames),
               '--rtc-fixed', shifttest.RTC, '--misses', '', '--nvram', nv, '--quiet'] + inp + [sub(a) for a in rest]
        procs.append((side, subprocess.Popen(cmd, stderr=open(os.path.join(args.out, side + '.log'), 'w'))))
    for side, p in procs:
        if p.wait():
            sys.exit('%s run failed (see %s/%s.log)' % (side, args.out, side))
    print('logs in', args.out)


if __name__ == '__main__':
    main()

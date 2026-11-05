#!/usr/bin/env python3
"""regdiff.py - compare --break logs of the original and a shifted image (pairrun.py output).
A shifted value counts as equal if it equals the original raw or after mapping a moved P1 address
back (so correctly relocated pointers are not reported, and wrongly relocated constants are).
usage: regdiff.py ORIG.log SHIFT.log INSERT_HEX"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shifttest  # noqa: E402

o_log, s_log, ins = sys.argv[1:4]
syms = shifttest.elf_syms(os.path.join(shifttest.OUT, 'ins_' + ins, 'shift', 'p1.elf'))
sa = syms[int(ins, 16)]
NAMES = ['D%d' % i for i in range(8)] + ['A%d' % i for i in range(8)]


def back(v):
    return v - 2 if sa + 2 <= v < 0x100000 else v


o = [l.split() for l in open(o_log) if l.startswith('break')]
s = [l.split() for l in open(s_log) if l.startswith('break')]
print('breaks: original %d, shifted %d' % (len(o), len(s)))
for k, (x, y) in enumerate(zip(o, s)):
    a = [int(v, 16) for v in x[8:16] + x[17:25]]
    b = [int(v, 16) for v in y[8:16] + y[17:25]]
    d = [(NAMES[i], '%x' % p, '%x' % q) for i, (p, q) in enumerate(zip(a, b)) if p != q and p != back(q)]
    if d:
        print('first difference at break #%d, frame %s: %s' % (k, x[2], d))
        print('  orig ', ' '.join(x[8:25]))
        print('  shift', ' '.join(y[8:25]))
        break
else:
    print('no difference')

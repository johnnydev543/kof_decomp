#!/usr/bin/env python3
"""tracediff.py - compare --trace-frame logs of the original and a shifted image (pairrun.py output).
Shifted PCs are mapped back to original addresses; prints the first step where PC or remaining
cycles differ, with context (and before that, the first register difference not explained by a
moved P1 address; --pc-only skips registers).  usage: tracediff.py ORIG.log SHIFT.log INSERT_HEX [--pc-only]"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shifttest  # noqa: E402

pc_only = '--pc-only' in sys.argv
o_log, s_log, ins = [a for a in sys.argv[1:] if a != '--pc-only'][:3]
syms = shifttest.elf_syms(os.path.join(shifttest.OUT, 'ins_' + ins, 'shift', 'p1.elf'))
sa = syms[int(ins, 16)]


def back(a):
    return a if a >= 0x100000 or a < sa else (a - 2 if a >= sa + 2 else a)


o = [l.split() for l in open(o_log) if l.startswith('trace')]
s = [l.split() for l in open(s_log) if l.startswith('trace')]
print('steps: original %d, shifted %d' % (len(o), len(s)))
NAMES = ['D%d' % i for i in range(8)] + ['A%d' % i for i in range(8)]
for i, (x, y) in enumerate(zip(o, s)):
    # registers before this instruction: equal raw, or equal after mapping a moved P1 address back
    rd = [(NAMES[r], x[4 + r], y[4 + r]) for r in range(min(16, len(x) - 4, len(y) - 4))
          if x[4 + r] != y[4 + r] and int(x[4 + r], 16) != back(int(y[4 + r], 16))]
    if rd and not pc_only:
        print('first register difference before step %d (%s): %s' % (i, x[1], rd))
        for k in range(max(0, i - 6), i + 1):
            print('  %s | %s (=%06x)' % (o[k][1], s[k][1], back(int(s[k][1], 16))))
        break
    if int(x[1], 16) != back(int(y[1], 16)) or x[3] != y[3]:
        print('first difference at step %d' % i)
        for k in range(max(0, i - 10), min(i + 6, len(o), len(s))):
            print('  %s cyc %5s | %s (=%06x) cyc %5s' % (o[k][1], o[k][3], s[k][1], back(int(s[k][1], 16)), s[k][3]))
        break
else:
    print('no difference in the common prefix')

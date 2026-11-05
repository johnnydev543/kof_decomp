#!/usr/bin/env python3
"""
writediff.py - first differing RAM write between an original and a shifted P1 build.

Input: the stderr of two runs with the same inputs and `--watch A-B` (e.g. `2> orig.w`). Writing PCs
of the shifted build are mapped back to original addresses, and a 32-bit write (two word writes by
the same instruction) whose value is a relocated ROM pointer counts as equal. This finds the
instruction where the shifted build first behaves differently, which ramdiff.py (end-of-frame
snapshots) can only narrow down to a frame.

usage: writediff.py orig.w shift.w --elf build/disasm/shift/p1.elf [--exact]
"""
import argparse
import re

from ramdiff import load_reloc

W = re.compile(r'^watch frame (\d+) ppc ([0-9a-f]+) write ([0-9a-f]+) = ([0-9a-f]+) mask ([0-9a-f]+)')


def load(path):
    out = []
    for line in open(path):
        m = W.match(line)
        if m:
            out.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16),
                        int(m.group(5), 16)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('orig')
    ap.add_argument('shift')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--exact', action='store_true',
                    help='a value counts as a relocated pointer only if it is exactly a label address '
                         '(catches counters/coordinates that merely look like P1 addresses)')
    ap.add_argument('--max', type=int, default=1, help='differences to list (default 1)')
    args = ap.parse_args()
    reloc, pairs = load_reloc(args.elf)
    if args.exact:
        exact = {o for o, _ in pairs}
        loose = reloc
        reloc = lambda v: loose(v) if v in exact else None
    back = {s: o for o, s in pairs}
    shifted_keys = sorted(back)

    def unreloc_pc(pc):
        import bisect
        i = bisect.bisect_right(shifted_keys, pc) - 1
        if pc >= 0x100000 or i < 0:
            return pc
        s = shifted_keys[i]
        return back[s] + (pc - s)

    a, b = load(args.orig), load(args.shift)
    i = shown = 0
    while i < min(len(a), len(b)):
        fa, pa, aa, va, ma = a[i]
        fb, pb, ab, vb, mb = b[i]
        pb = unreloc_pc(pb)
        if (fa, pa, aa, ma) == (fb, pb, ab, mb) and va == vb:
            i += 1
            continue
        # long write: high word at addr, low word at addr+2 by the same instruction
        if (fa, pa, aa, ma) == (fb, pb, ab, mb) and i + 1 < min(len(a), len(b)):
            na, nb = a[i + 1], b[i + 1]
            if na[1] == pa and na[2] == aa + 2 and unreloc_pc(nb[1]) == pa and nb[2] == ab + 2:
                lo, ls = (va << 16) | na[3], (vb << 16) | nb[3]
                if lo < 0x100000 and reloc(lo) == ls:
                    i += 2
                    continue
        if i > 0 and (fa, pa, aa) == (fb, pb, ab) and a[i - 1][1] == pa and a[i - 1][2] == aa - 2:
            prev_a, prev_b = a[i - 1], b[i - 1]
            lo, ls = (prev_a[3] << 16) | va, (prev_b[3] << 16) | vb
            if lo < 0x100000 and reloc(lo) == ls:
                i += 1
                continue
        print('differing write #%d: frame %d pc %06x write %06x = %04x | shift: frame %d pc %06x write %06x = %04x'
              % (i, fa, pa, aa, va, fb, pb, ab, vb))
        shown += 1
        if shown >= args.max or (fa, pa, aa) != (fb, pb, ab):
            return                      # control flow differs: later writes no longer line up
        i += 1
        continue
    if not shown:
        print('no difference in %d / %d writes' % (len(a), len(b)))


if __name__ == '__main__':
    main()

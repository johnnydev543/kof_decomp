#!/usr/bin/env python3
"""
ramdiff.py - find the first frame where a modified (shifted) P1 build diverges from the original.

Both runs must log work RAM with the same inputs, e.g.
  kof97 --roms ORIG  --interp --headless --autoplay --frames 3000 --nvram a.nv --ram-log orig.ram
  kof97 --roms SHIFT --interp --headless --autoplay --frames 3000 --nvram b.nv --ram-log shift.ram

RAM legitimately differs where the game stores ROM pointers (state-machine addresses, table
pointers): with --elf (the shifted build's ELF, symbols L_xxxxxx / D_xxxxxx named after their
original address) a longword is treated as equal when shifted == relocate(original).

usage: ramdiff.py orig.ram shift.ram [--elf build/disasm/shift/p1.elf] [--from N] [--show 40]
                  [--ignore 10f000-10f2ff]   (e.g. the stack, where stale return addresses linger)
                  [--exact]                  (only exact label addresses count as pointers)
"""
import argparse
import bisect
import subprocess
import os as _os
import shutil as _shutil

NM = _os.environ.get('NM_M68K') or next((p + 'nm' for p in (_os.environ.get('M68K_PREFIX', ''), 'm68k-elf-', 'm68k-linux-gnu-') if p and _shutil.which(p + 'nm')), 'm68k-elf-nm')

RAM = 0x10000
RAM_BASE = 0x100000
P1_END = 0x100000


def load_reloc(elf):
    """original address -> shifted address, from the label names of the shifted build."""
    pairs = []
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        v, _, n = line.split()
        if n[:2] in ('L_', 'D_'):
            pairs.append((int(n[2:], 16), int(v, 16)))
    pairs.sort()
    keys = [o for o, _ in pairs]

    def reloc(a):
        i = bisect.bisect_right(keys, a) - 1
        if i < 0:
            return a
        o, s = pairs[i]
        return a + (s - o)
    return reloc, pairs


def be32(b, k):
    return int.from_bytes(b[k:k + 4], 'big')


def diff_frame(a, b, reloc, ignore=()):
    """differing byte offsets that are not explained by a relocated ROM pointer."""
    bad = []
    explained = set()
    i = 0
    while i < RAM:
        if a[i] != b[i] and i not in explained:
            ok = False
            if reloc:
                for k in range(max(0, (i - 3) & ~1), min(i, RAM - 4) + 1, 2):
                    va, vb = be32(a, k), be32(b, k)
                    if va != vb and va < P1_END and reloc(va) == vb:
                        explained.update(range(k, k + 4))
                        ok = True
                        break
            if not ok and not any(lo <= RAM_BASE + i <= hi for lo, hi in ignore):
                bad.append(i)
        i += 1
    return bad


def ranges(offs):
    out = []
    for o in offs:
        if out and o <= out[-1][1] + 2:
            out[-1][1] = o
        else:
            out.append([o, o])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('orig')
    ap.add_argument('shift')
    ap.add_argument('--elf', help='shifted build ELF (enables pointer relocation)')
    ap.add_argument('--from', dest='start', type=int, default=0, help='frame number of the first logged frame')
    ap.add_argument('--show', type=int, default=40, help='differing ranges to print')
    ap.add_argument('--ignore', action='append', default=[], help='RAM range A-B (hex) to skip; repeatable')
    ap.add_argument('--exact', action='store_true',
                    help='a value counts as a relocated pointer only if it is exactly a label address')
    args = ap.parse_args()
    reloc = load_reloc(args.elf)[0] if args.elf else None
    if reloc and args.exact:
        exact = {o for o, _ in load_reloc(args.elf)[1]}
        loose = reloc
        reloc = lambda v: loose(v) if v in exact else None
    ignore = [tuple(int(x, 16) for x in r.split('-')) for r in args.ignore]
    fa, fb = open(args.orig, 'rb'), open(args.shift, 'rb')
    f = args.start
    while True:
        a, b = fa.read(RAM), fb.read(RAM)
        if len(a) < RAM or len(b) < RAM:
            print('no unexplained difference in %d frames' % (f - args.start))
            return
        if a != b:
            bad = diff_frame(a, b, reloc, ignore)
            if bad:
                print('first divergence after frame %d: %d bytes differ (pointer-relocation explained the rest)'
                      % (f + 1, len(bad)))
                for lo, hi in ranges(bad)[:args.show]:
                    lo &= ~1
                    hi = min(hi | 1, RAM - 1)
                    print('  %06x-%06x  orig %s  shift %s' % (RAM_BASE + lo, RAM_BASE + hi,
                                                              a[lo:hi + 1][:16].hex(), b[lo:hi + 1][:16].hex()))
                lo = RAM_BASE + (bad[0] & ~1)
                print('next: rerun both with --interp --frames %d --watch %06x-%06x and compare the writing PCs'
                      % (f + 1, lo, lo + 3))
                return
        f += 1


if __name__ == '__main__':
    main()

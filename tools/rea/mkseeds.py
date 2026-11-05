#!/usr/bin/env python3
"""mkseeds.py - REA/Ghidra analysis seeds (REA_GHIDRA_SEED_FILE) for a Neo Geo P1 image and the BIOS.

P1 (mapped at 0x000000): the cartridge header jump entries (USER 0x122, PLAYER_START 0x128,
DEMO_END 0x12E, COIN_SOUND 0x134) and every game exception vector that points into P1.
BIOS (mapped at 0xC00000): its exception vectors and the BIOS call jump table at 0xC00402
(jmp abs.l entries) plus their targets.

usage: mkseeds.py [--p1 build/roms/p1.be] [--bios build/roms/bios.be] [--out tools/rea]
"""
import argparse
import os
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--p1', default=os.path.join(ROOT, 'build/roms/p1.be'))
    ap.add_argument('--bios', default=os.path.join(ROOT, 'build/roms/bios.be'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'tools/rea'))
    args = ap.parse_args()
    p1 = open(args.p1, 'rb').read()
    bios = open(args.bios, 'rb').read()

    def be(d, o):
        return struct.unpack('>I', d[o:o + 4])[0]

    seen = set()

    def add(lines, a, name):
        if a not in seen:
            seen.add(a)
            lines.append('0x%x\tfunction\t%s' % (a, name))

    lines = ['# Neo Geo P1 (68000, mapped at 0x000000) analysis seeds for REA_GHIDRA_SEED_FILE']
    for a, n in ((0x122, 'USER'), (0x128, 'PLAYER_START'), (0x12e, 'DEMO_END'), (0x134, 'COIN_SOUND')):
        add(lines, a, n)
    for i in range(2, 64):
        v = be(p1, i * 4)
        if 0x100 <= v < len(p1) and v % 2 == 0:
            add(lines, v, 'vec%02d_%x' % (i, v))
    open(os.path.join(args.out, 'seeds_p1.tsv'), 'w').write('\n'.join(lines) + '\n')

    seen.clear()
    blines = ['# Neo Geo MVS BIOS (68000, mapped at 0xC00000) analysis seeds']
    for i in range(1, 64):
        v = be(bios, i * 4)
        if 0xc00000 <= v < 0xc00000 + len(bios) and v % 2 == 0:
            add(blines, v, 'biosvec%02d_%x' % (i, v))
    a = 0xc00402
    while struct.unpack('>H', bios[a - 0xc00000:a - 0xc00000 + 2])[0] == 0x4ef9:
        add(blines, a, 'bios_jt_%x' % a)
        t = be(bios, a - 0xc00000 + 2)
        if 0xc00000 <= t < 0xc00000 + len(bios):
            add(blines, t, 'bios_fn_%x' % t)
        a += 6
    open(os.path.join(args.out, 'seeds_bios.tsv'), 'w').write('\n'.join(blines) + '\n')
    print('seeds: P1 %d, BIOS %d' % (len(lines) - 1, len(blines) - 1))


if __name__ == '__main__':
    main()

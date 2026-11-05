#!/usr/bin/env python3
"""romprep.py - verify the KOF97 + Neo Geo BIOS ROM sets and normalise them for the runtime.

Reads the game and BIOS zips named in games/<GAME>/game.json (MAME sets; default kof97) and writes build/roms/:
  p1.bin p2.bin bios.bin   program ROMs byte-swapped to big-endian (68000 order)
  p1.be  bios.be           same images, names used by the REA/Ghidra and recompiler tooling
  s1.bin sfix.bin          fix layer ROMs
  m1.bin sm1.bin           Z80 ROMs
  v.bin                    v1+v2+v3 (ADPCM-A/B)
  c.bin                    sprite ROMs interleaved like MAME's "sprites" region (c1/c2 byte pairs, ...)
  lo.bin                   L0 zoom ROM
"""
import argparse
import os
import sys
import zipfile
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game  # noqa: E402  (games/<GAME>/game.json: ROM names and CRCs)

KOF97 = game.rom_crcs()   # name: crc32 of every game ROM (kept under this name for older callers)
# BIOS files: accept both the old and the current MAME names.
BIOS_ALIASES = {
    'bios': ['sp-s2.sp1', 'neo-geo.rom'],
    'sfix': ['sfix.sfix', 'ng-sfix.rom'],
    'sm1': ['sm1.sm1', 'ng-sm1.rom'],
    'lo': ['000-lo.lo', 'ng-lo.rom'],
}


def swap16(b):
    a = bytearray(b)
    a[0::2], a[1::2] = a[1::2], a[0::2]
    return bytes(a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(ROOT, game.CFG['rom_zip']))
    ap.add_argument('--bios', default=os.path.join(ROOT, game.CFG['bios_zip']))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/roms'))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    g = zipfile.ZipFile(args.game)
    files = {}
    for name, crc in KOF97.items():
        data = g.read(name)
        got = zlib.crc32(data)
        if got != crc:
            sys.exit('%s: CRC %08x, expected %08x (wrong ROM set?)' % (name, got, crc))
        files[name] = data
    b = zipfile.ZipFile(args.bios)
    names = set(b.namelist())
    bios = {}
    for key, cands in BIOS_ALIASES.items():
        for c in cands:
            if c in names:
                bios[key] = b.read(c)
                break
        else:
            sys.exit('neogeo.zip: none of %s found' % cands)

    def out(name, data):
        with open(os.path.join(args.out, name), 'wb') as f:
            f.write(data)

    r = game.CFG['roms']
    p1 = swap16(files[r['p1'][0]])
    p2 = swap16(files[r['p2'][0]]) if r.get('p2') else b''
    bi = swap16(bios['bios'])
    out('p1.bin', p1); out('p1.be', p1)
    out('p2.bin', p2)
    out('bios.bin', bi); out('bios.be', bi)
    out('s1.bin', files[r['s1'][0]])
    out('sfix.bin', bios['sfix'])
    out('m1.bin', files[r['m1'][0]])
    out('sm1.bin', bios['sm1'])
    out('v.bin', b''.join(files[f] for f, _ in r['v']))
    lo = bios['lo']
    out('lo.bin', lo[:0x10000])
    # sprites: ROM_LOAD16_BYTE pairs (even/odd), one pair after the other (c1/c2 at 0, c3/c4 next, ...)
    c = bytearray()
    for (even, _), (odd, _) in r['c']:
        e, o = files[even], files[odd]
        pair = bytearray(len(e) * 2)
        pair[0::2] = e
        pair[1::2] = o
        c += pair
    out('c.bin', bytes(c))
    print('ROMs verified and written to', args.out)


if __name__ == '__main__':
    main()

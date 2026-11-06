#!/usr/bin/env python3
"""rendertest.py - the sprite replacement path must reproduce the original picture.

Every exported frame (build/assets/<GAME>/sprites, export_sprites.py) is used unchanged as its own
replacement image (a mods directory of symbolic links). Replacements are then drawn through the whole
replacement path - frame recognition at the native hook, box placement, flips, shrinking, flat-palette
fills, and the palette changes measured against the exported palettes - so the result must equal the run
without replacements, both when each frame is drawn at once (--frame-render) and line by line (the default,
which shows the game's mid-frame writes):
  * 1x: every screenshot identical, byte for byte
  * --hd 2 (tagged frames composed at 2x): the 1x picture doubled, exactly (art at the original resolution
    takes the box pixel the hardware chose for each screen pixel, shrunk objects included)
Runs use fresh backup RAM, a fixed calendar and the coverage scripts in games/<GAME>/scripts.

usage: rendertest.py [--scripts arcade_00_japan versus ...] [--every 20] [--jobs N] [--keep]
Exit status 0 only if every comparison passes.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402

ROOT = game.ROOT
OUT = os.path.join(ROOT, 'build/rendertest')
SPRITES = os.path.join(ROOT, 'build/assets', game.NAME, 'sprites')
DEFAULT_SCRIPTS = ['arcade_00_japan', 'arcade_01_fatalfury', 'arcade_02_aof', 'arcade_03_ikari', 'arcade_04_psycho',
                   'versus']


def identity_mods():
    """OUT/idmods/sprites/<key>.png -> the exported PNG of that frame"""
    index = json.load(open(os.path.join(SPRITES, 'index.json')))
    d = os.path.join(OUT, 'idmods', 'sprites')
    os.makedirs(d, exist_ok=True)
    for key, f in index['frames'].items():
        link = os.path.join(d, key + '.png')
        if not os.path.lexists(link):
            os.symlink(os.path.join(SPRITES, f['png']), link)
    return os.path.dirname(d)


def run(script, mode, mods, every):
    """screenshots of one run in OUT/<script>.<mode>/"""
    name = os.path.basename(script)[:-4]
    d = os.path.join(OUT, '%s.%s' % (name, mode))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    frames = 15000
    for line in open(script):
        if line.startswith('# frames '):
            frames = int(line.split()[2])
    args = [os.path.join(ROOT, 'build/kof97'), '--headless', '--frames', str(frames), '--rtc-fixed', '883612800',
            '--misses', '', '--quiet', '--nvram', os.path.join(d, 'nv'), '--script', script,
            '--dump', os.path.join(d, 's'), '--dump-every', str(every),
            '--mods', mods if not mode.endswith('base') else os.path.join(d, 'none')]
    if mode.startswith('f'):
        args += ['--frame-render']
    if mode.endswith('hd'):
        args += ['--hd', '2']
    subprocess.run(args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return d


def ppm(path):
    import numpy as np
    data = open(path, 'rb').read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(parts[4][-w * h * 3:], np.uint8).reshape(h, w, 3)


MODES = ('fbase', 'fid', 'fhd', 'lbase', 'lid', 'lhd')     # f: --frame-render, l: line by line


def compare(name, kind, tolerance):
    """(1x mismatches, hd mismatches, frames, worst hd share) for kind 'f' or 'l'"""
    import numpy as np
    base, ident, hd = (os.path.join(OUT, '%s.%s%s' % (name, kind, m)) for m in ('base', 'id', 'hd'))
    bad1 = badhd = n = 0
    worst = 0.0
    for f in sorted(glob.glob(os.path.join(base, 's_*.ppm'))):
        s = os.path.basename(f)
        n += 1
        a = open(f, 'rb').read()
        if open(os.path.join(ident, s), 'rb').read() != a:
            bad1 += 1
        big = ppm(f).repeat(2, 0).repeat(2, 1)
        share = float((np.abs(ppm(os.path.join(hd, s)).astype(int) - big).max(2) > 0).mean())
        worst = max(worst, share)
        if share > tolerance:
            badhd += 1
    return bad1, badhd, n, worst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scripts', nargs='*', default=DEFAULT_SCRIPTS)
    ap.add_argument('--every', type=int, default=20, help='screenshot interval in frames')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))   # never all cores
    ap.add_argument('--hd-tolerance', type=float, default=0.0, help='share of pixels allowed to differ at 2x')
    ap.add_argument('--keep', action='store_true', help='keep the screenshots')
    a = ap.parse_args()
    if not os.path.exists(os.path.join(SPRITES, 'index.json')):
        sys.exit('build/assets/%s/sprites missing: run `make assets` first' % game.NAME)
    mods = identity_mods()
    scripts = [os.path.join(game.DIR, 'scripts', s + '.txt') for s in a.scripts]
    jobs = [(s, m) for s in scripts for m in MODES]
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(lambda j: run(j[0], j[1], mods, a.every), jobs))
    ok = True
    for s in scripts:
        name = os.path.basename(s)[:-4]
        for kind, label in (('f', 'frame'), ('l', 'line')):
            bad1, badhd, n, worst = compare(name, kind, a.hd_tolerance)
            good = n > 0 and not bad1 and not badhd
            ok &= good
            print('%-4s %-22s %-5s %5d screenshots  1x differ %d  hd over tolerance %d (worst %.4f%%)'
                  % ('OK' if good else 'FAIL', name, label, n, bad1, badhd, worst * 100), flush=True)
        if not a.keep:
            for m in MODES:
                for f in glob.glob(os.path.join(OUT, '%s.%s' % (name, m), 's_*.ppm')):
                    os.remove(f)
    print('rendertest: %s' % ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

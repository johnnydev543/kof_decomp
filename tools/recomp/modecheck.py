#!/usr/bin/env python3
"""modecheck.py - whole-program differential test of the static recompiler.

Every coverage script (and autoplay) is run twice with identical input: once on the recompiled C
(default mode) and once purely on the Musashi interpreter (--interp, the reference). The recompiled
code is cycle-exact (it stops after the same instruction as Musashi when a slice runs out), so the
two runs must agree on every frame: 68000 PC and cycle carry at the end of each frame, a hash of the
64 KB work RAM, every screenshot, the complete audio stream and the final backup RAM.

usage: modecheck.py --all            (autoplay + games/<GAME>/scripts/*.txt)
       modecheck.py --script S.txt [--frames N]
Exit status 0 only if every case matches. Runs with --misses '' so the miss log is not touched.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)

OUT = os.path.join(ROOT, 'build/modecheck')
KOF97 = os.path.join(ROOT, 'build/kof97')


def run(tag, interp, inp, frames, every):
    d = os.path.join(OUT, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = [KOF97, '--headless', '--frames', str(frames), '--rtc-fixed', '883612800', '--misses', '',
           '--nvram', os.path.join(d, 'nv'), '--quiet', '--dump', os.path.join(d, 's'), '--dump-every', str(every),
           '--wav', os.path.join(d, 'a.wav'), '--frame-sig', os.path.join(d, 'sig.txt')] + inp
    if interp:
        cmd.append('--interp')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s failed: %s' % (tag, r.stderr[-800:]))
    h = lambda p: hashlib.sha1(open(p, 'rb').read()).hexdigest()
    return {'sig': open(os.path.join(d, 'sig.txt')).read().splitlines(),
            'shots': {os.path.basename(f): h(f) for f in glob.glob(os.path.join(d, 's_*.ppm'))},
            'audio': h(os.path.join(d, 'a.wav')), 'nv': h(os.path.join(d, 'nv'))}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--script', action='append', default=[])
    ap.add_argument('--autoplay', action='store_true')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--frames', type=int, default=15000)
    ap.add_argument('--dump-every', type=int, default=250)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))   # never all cores
    args = ap.parse_args()
    cases = []
    if args.autoplay or args.all:
        cases.append(('autoplay', ['--autoplay'], args.frames))
    scripts = args.script + (sorted(glob.glob(game.path('scripts', '*.txt'))) if args.all else [])
    for sc in scripts:
        frames = args.frames
        for line in open(sc):
            if line.startswith('# frames '):
                frames = int(line.split()[2])
        cases.append((os.path.splitext(os.path.basename(sc))[0], ['--script', os.path.abspath(sc)], frames))
    with ThreadPoolExecutor(args.jobs) as ex:
        jobs = {(n, m): ex.submit(run, '%s_%s' % (n, m), m == 'interp', inp, fr, args.dump_every)
                for n, inp, fr in cases for m in ('recomp', 'interp')}
        res = {k: f.result() for k, f in jobs.items()}
    bad = 0
    for n, _, fr in cases:
        a, b = res[(n, 'recomp')], res[(n, 'interp')]
        msgs = []
        for x, y in zip(a['sig'], b['sig']):
            if x != y:
                msgs.append('frame signature differs from frame %s (recomp "%s" / interp "%s")' % (x.split()[0], x, y))
                break
        if len(a['sig']) != len(b['sig']):
            msgs.append('frame count differs')
        if a['shots'] != b['shots']:
            msgs.append('screenshots differ')
        if a['audio'] != b['audio']:
            msgs.append('audio differs')
        if a['nv'] != b['nv']:
            msgs.append('backup RAM differs')
        bad += bool(msgs)
        print('%s %-28s %6d frames%s' % ('OK  ' if not msgs else 'FAIL', n, fr, '' if not msgs else ': ' + '; '.join(msgs)))
    print('%d/%d cases match' % (len(cases) - bad, len(cases)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

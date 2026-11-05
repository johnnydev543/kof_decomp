#!/usr/bin/env python3
"""endcheck.py - did every arcade coverage script reach the ending?

Ending code is found by observation in a reference run (arcade_00_japan): code executed between
frame --before (fighting Orochi) and --after (TOTAL SCORE after the ending), minus everything the
gameover and versus scripts execute (score / name entry / ranking without a cleared game). The
executed-address maps of the latest shift-test run (build/shifttest/runs/*_orig/exec.bin) are then
checked for that code: a script that cleared the game executes nearly all of it (team-specific
ending parts make up the rest).

usage: endcheck.py [--before 29500] [--after 31500]   (run make shifttest or make matching first)
"""
import argparse
import glob
import os
import sys
import subprocess

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)

OUT = os.path.join(ROOT, 'build/endcheck')
RUNS = os.path.join(ROOT, 'build/shifttest/runs')


def bits(path):
    m = open(path, 'rb').read()[:0x10000]
    return {i * 2 for i in range(len(m) * 8) if m[i >> 3] >> (i & 7) & 1}


def exec_until(frames):
    os.makedirs(OUT, exist_ok=True)
    ex, nv = os.path.join(OUT, 'e%d.bin' % frames), os.path.join(OUT, 'nv%d' % frames)
    for f in (ex, nv):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run([os.path.join(ROOT, 'build/kof97'), '--interp', '--headless', '--frames', str(frames), '--script',
                    game.path('scripts', 'arcade_00_japan.txt'), '--rtc-fixed', '883612800',
                    '--misses', '', '--nvram', nv, '--quiet', '--exec-log', ex], check=True)
    return bits(ex)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--before', type=int, default=29500)
    ap.add_argument('--after', type=int, default=31500)
    ap.add_argument('--min', type=float, default=80.0, help='percent of ending code that counts as cleared')
    args = ap.parse_args()
    late = exec_until(args.after) - exec_until(args.before)
    ending = late - bits(os.path.join(RUNS, 'gameover_orig/exec.bin')) - bits(os.path.join(RUNS, 'versus_orig/exec.bin'))
    print('ending code (reference run, frames %d-%d): %d instructions' % (args.before, args.after, len(ending)))
    ok = True
    for d in sorted(glob.glob(os.path.join(RUNS, 'arcade_*_orig'))):
        pct = 100.0 * len(ending & bits(os.path.join(d, 'exec.bin'))) / max(1, len(ending))
        cleared = pct >= args.min
        ok &= cleared
        print('  %-30s %5.1f%%  %s' % (os.path.basename(d)[:-5], pct, 'ending reached' if cleared else 'NO ENDING'))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

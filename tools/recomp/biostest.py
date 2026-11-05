#!/usr/bin/env python3
"""biostest.py - the native BIOS (src/bios_hle.c) must behave as the BIOS ROM.

Each script runs twice with a fresh backup RAM and a fixed calendar: with the BIOS ROM, and with
--bios-hle LEVEL (level 3 with --bios-hle-timing measured from the ROM run, so boot and eye-catcher take as long as there).
The raster line counter read (which the game mixes into its random numbers) is made independent of CPU timing
(--test-raster), the 68000 gets four times its cycles per line so the game never runs a frame late
(--test-cpu-scale 4), and sound uses the native mixer: the few cycles the native BIOS spends differently then
do not change the game. Compared: screenshots (whole frames, every --every frames), the sound commands with their frames, and the
BIOS's calls into the game (USER, PLAYER_START, DEMO_END, COIN_SOUND, SYSTEM_RETURN; --bios-log).
Level 3 draws the eye-catcher black, so screenshots before the first USER(2) and during eye-catchers are
not compared.

usage: biostest.py [--level 3] [--scripts arcade_00_japan versus ...] [--all] [--every 20] [--jobs 10]
Exit status 0 only if every script matches.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402

ROOT = game.ROOT
OUT = os.path.join(ROOT, 'build/biostest')
DEFAULT = ['versus', 'arcade_00_japan', 'arcade_05_women', 'gameover']   # testmode: the ROM's F2 menu


def rom_timing(d):
    """boot timing of the ROM run, for --bios-hle-timing: USER(0) frame, eye-catcher start - watchdog reset"""
    calls = bios_calls(d)
    user0 = next((c[0] for c in calls if c[1] == 'USER' and c[2] == '0'), 0)
    reset = next((c[0] for c in calls if c[1] == 'RESET'), 0)
    jingle = next((int(l.split()[0]) for l in open(os.path.join(d, 'sound.log')) if l.split()[2] == '02'), 0)
    return '%d,%d,465,%d' % (user0, jingle - 6, jingle - 6 - reset)


EXTRA = []                     # --args: more emulator options for both runs (e.g. --aes)


def run(script, mode, level, every, timing=None):
    name = os.path.basename(script)[:-4]
    d = os.path.join(OUT, '%s.%s' % (name, mode))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    frames = 15000
    for line in open(script):
        if line.startswith('# frames '):
            frames = int(line.split()[2])
    args = [os.path.join(ROOT, 'build/kof97'), '--headless', '--frames', str(frames), '--rtc-fixed', '883612800',
            '--misses', '', '--quiet', '--nvram', os.path.join(d, 'nv'), '--script', script, '--frame-render',
            '--dump', os.path.join(d, 's'), '--dump-every', str(every), '--sound-log', os.path.join(d, 'sound.log'),
            '--bios-log', '--test-raster', '--test-cpu-scale', '4', '--native-audio'] + EXTRA
    if mode == 'rom':
        args += ['--interp']          # every instruction passes the call log (recompiled code may chain calls)
    if mode == 'hle':
        args += ['--bios-hle', str(level)] + (['--bios-hle-timing', timing] if level >= 3 and timing else [])
    with open(os.path.join(d, 'bios.log'), 'w') as log:
        subprocess.run(args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=log)
    return d


def bios_calls(d):
    calls = []
    for line in open(os.path.join(d, 'bios.log')):
        m = re.match(r'bios: frame (\d+) (\S+) (\S+) (\S+)', line)
        if m:
            calls.append((int(m.group(1)), m.group(2), m.group(3)))
    return calls


def compare(name, level):
    rom, hle = (os.path.join(OUT, '%s.%s' % (name, m)) for m in ('rom', 'hle'))
    problems = []
    a, b = bios_calls(rom), bios_calls(hle)
    if level >= 3:          # SYSTEM_RETURN inside the ROM's own flows is seen at other addresses; compare game calls
        a = [c for c in a if c[1] != 'SYSTEM_RETURN']
        b = [c for c in b if c[1] != 'SYSTEM_RETURN']
    if a != b:
        diff = next(((x, y) for x, y in zip(a, b) if x != y), (a[len(b):len(b) + 1], b[len(a):len(a) + 1]))
        problems.append('BIOS calls differ: rom %s / hle %s' % diff)
    start = next((c[0] for c in a if c[1] == 'USER' and c[2] in ('2', '3')), 0) if level >= 3 else 0
    sa = [l for l in open(os.path.join(rom, 'sound.log')) if int(l.split()[0]) >= start]
    sb = [l for l in open(os.path.join(hle, 'sound.log')) if int(l.split()[0]) >= start]
    if sa != sb:
        diff = next(((x.strip(), y.strip()) for x, y in zip(sa, sb) if x != y), ('end', 'end'))
        problems.append('sound commands differ (first: rom %s / hle %s)' % diff)
    shots = sorted(glob.glob(os.path.join(rom, 's_*.ppm')))
    # the native BIOS draws its eye-catcher black: skip from its sound $03 (6 frames before the jingle $02)
    # to the USER call that follows it
    eye = []
    if level >= 3:
        users = [c[0] for c in bios_calls(rom) if c[1] == 'USER']
        for l in open(os.path.join(rom, 'sound.log')):
            f, _, cmd = l.split()[:3]
            if cmd == '02':
                e = int(f) - 6
                eye.append((e, next((u for u in users if u > e), 1 << 60)))

    def differs(f):
        g = f.replace('.rom/', '.hle/')
        n = int(os.path.basename(f)[2:8])
        return n > start and not any(e <= n <= u for e, u in eye) and \
            (not os.path.exists(g) or open(f, 'rb').read() != open(g, 'rb').read())
    differ = [differs(f) for f in shots]
    bad = [os.path.basename(f) for f, d in zip(shots, differ) if d]
    # a screenshot taken while the game's work for a frame straddles VBlank can show it one frame earlier or
    # later (the native BIOS spends slightly different cycles): isolated differences are timing; two in a row
    # or more than 1% are not
    runs = sum(1 for i in range(1, len(differ)) if differ[i] and differ[i - 1])
    note = ''
    if bad and (runs or len(bad) > len(shots) // 100):
        problems.append('%d of %d screenshots differ (first %s)' % (len(bad), len(shots), bad[0]))
    elif bad:
        note = '(%d isolated screenshot timing differences: %s)' % (len(bad), ' '.join(bad[:4]))
    return problems, len(shots), len(a), note


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--level', type=int, default=3)
    ap.add_argument('--scripts', nargs='*', default=DEFAULT)
    ap.add_argument('--all', action='store_true', help='every script in games/<GAME>/scripts')
    ap.add_argument('--every', type=int, default=20)
    ap.add_argument('--jobs', type=int, default=10)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--args', default='', help='more emulator options for both runs, e.g. "--aes"')
    a = ap.parse_args()
    EXTRA[:] = a.args.split()
    if a.all:
        a.scripts = sorted(os.path.basename(s)[:-4] for s in glob.glob(os.path.join(game.DIR, 'scripts', '*.txt')))
    scripts = [os.path.join(game.DIR, 'scripts', s + '.txt') for s in a.scripts]
    def both(s):
        d = run(s, 'rom', a.level, a.every)
        run(s, 'hle', a.level, a.every, rom_timing(d))
    with ThreadPoolExecutor(max(1, a.jobs // 2)) as ex:
        list(ex.map(both, scripts))
    ok = True
    for s in scripts:
        name = os.path.basename(s)[:-4]
        problems, n, calls, note = compare(name, a.level)
        ok &= not problems
        print('%-4s %-26s %5d screenshots %4d BIOS calls  %s' % ('OK' if not problems else 'FAIL', name, n, calls,
                                                                 '; '.join(problems) or note), flush=True)
        if not a.keep:
            for m in ('rom', 'hle'):
                for f in glob.glob(os.path.join(OUT, '%s.%s' % (name, m), 's_*.ppm')):
                    os.remove(f)
    print('biostest level %d: %s' % (a.level, 'PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

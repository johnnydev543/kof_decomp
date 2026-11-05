#!/usr/bin/env python3
"""
shifttest.py - stage-2 shift test for the symbolized P1 disassembly.

For every insertion point, `mkasm.py --symbolize --insert-nop X` builds a P1 image in which all code
and data after X moved by 2 bytes. The original and every shifted image are run with identical input
(--interp --rtc-fixed, fresh backup RAM) and compared on:
  * screenshots (every --dump-every frames)
  * the complete audio stream
  * the position of every VBlank interrupt (return address logged at the game's VBlank handler,
    mapped back to original addresses)
Any difference means a pointer was not symbolized (or something was symbolized that is not a pointer).

usage:
  shifttest.py --autoplay --frames 15000 --insert 1e62 60d36
  shifttest.py --script games/<GAME>/scripts/team1.txt --frames 9000 --insert 1e62 60d36
  shifttest.py --all                      # every script in games/<GAME>/scripts/ + autoplay
Exit status 0 only if every run matches.
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
OUT = os.path.join(ROOT, 'build/shifttest')
KOF97 = os.path.join(ROOT, 'build/kof97')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/game.json)
VBLANK_BREAK = game.hexval(game.CFG['disasm']['vblank_break'])   # instruction the game's VBlank handler runs
RTC = '883612800'
EXEC_MAP = os.path.join(ROOT, 'build/disasm/exec_p1.bin')
DEFAULT_INSERTS = list(game.CFG['disasm']['shift_inserts'])   # first code label after the header, early, middle


def elf_syms(elf):
    nm = None
    for pre in (os.environ.get('M68K_PREFIX', ''), 'm68k-elf-', 'm68k-linux-gnu-'):
        if pre and shutil.which(pre + 'nm'):
            nm = pre + 'nm'
            break
    syms = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        v, _, n = line.split()
        if n.startswith('L_') or n.startswith('D_'):
            syms[int(n[2:], 16)] = int(v, 16)
    return syms


def build_shift(ins):
    """Build the shifted image for insertion point ins (hex string); returns (roms_dir, map_back)."""
    out = os.path.join(OUT, 'ins_' + ins)
    img = os.path.join(out, 'shift', 'p1.bin')
    stamp_src = [os.path.join(ROOT, 'tools/disasm/mkasm.py'), os.path.join(ROOT, 'tools/recomp/m68k_recomp.py')]
    if os.path.exists(EXEC_MAP):
        stamp_src.append(EXEC_MAP)
    if not os.path.exists(img) or any(os.path.getmtime(s) > os.path.getmtime(img) for s in stamp_src):
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/disasm/mkasm.py'), '--symbolize',
                            '--insert-nop', ins, '--out', out], capture_output=True, text=True)
        if r.returncode or 'P1 image match: True' not in r.stdout:
            sys.exit('mkasm failed for %s:\n%s%s' % (ins, r.stdout[-2000:], r.stderr[-2000:]))
    roms = os.path.join(out, 'roms')
    os.makedirs(roms, exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, 'build/roms')):
        dst = os.path.join(roms, f)
        if f == 'p1.bin':
            shutil.copyfile(img, dst)
        elif not os.path.lexists(dst):
            os.symlink(os.path.join(ROOT, 'build/roms', f), dst)
    syms = elf_syms(os.path.join(out, 'shift', 'p1.elf'))
    ia = int(ins, 16)
    shifted_ins = syms[ia]

    def back(a):
        if a >= 0x100000 or a < shifted_ins:
            return a
        return a - 2 if a >= shifted_ins + 2 else a
    return roms, syms[VBLANK_BREAK], back


def run(tag, roms, input_args, frames, every, brk, exec_log=False):
    d = os.path.join(OUT, 'runs', tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = [KOF97, '--roms', roms, '--interp', '--headless', '--frames', str(frames), '--rtc-fixed', RTC,
           '--misses', '', '--nvram', os.path.join(d, 'nv'), '--quiet', '--dump', os.path.join(d, 's'),
           '--dump-every', str(every), '--wav', os.path.join(d, 'a.wav'), '--break', '%x' % brk] + input_args
    if exec_log:
        cmd += ['--exec-log', os.path.join(d, 'exec.bin')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('run %s failed: %s' % (tag, r.stderr[-1000:]))
    # exception frame at the handler entry: [SP] = SR, [SP+2] = interrupted PC ('sp+2' field)
    vbl = [(int(l.split()[2]), int(l.split()[-1], 16) & 0xffffff) for l in r.stderr.splitlines() if l.startswith('break frame')]
    shots = {}
    for f in sorted(glob.glob(os.path.join(d, 's_*.ppm'))):
        shots[int(f.rsplit('_', 1)[1][:-4])] = hashlib.sha1(open(f, 'rb').read()).hexdigest()
    # keep only a hash in memory (a 60000-frame WAV is ~225 MB); the file is read again only on a mismatch
    audio = hashlib.sha1(open(os.path.join(d, 'a.wav'), 'rb').read()).hexdigest()
    return {'dir': d, 'vbl': vbl, 'shots': shots, 'audio': audio}


def compare(o, s, back):
    msgs = []
    sv = [(f, back(a)) for f, a in s['vbl']]
    if sv != o['vbl']:
        for i, (x, y) in enumerate(zip(o['vbl'], sv)):
            if x != y:
                msgs.append('VBlank #%d differs: original frame %d at %06x, shifted frame %d at %06x' % (i, x[0], x[1], y[0], y[1]))
                break
        else:
            msgs.append('VBlank count differs: %d vs %d' % (len(o['vbl']), len(sv)))
    bad = [f for f in sorted(o['shots']) if s['shots'].get(f) != o['shots'][f]]
    if bad:
        msgs.append('screenshots differ from frame %d (%d of %d)' % (bad[0], len(bad), len(o['shots'])))
    if o['audio'] != s['audio']:
        a = open(os.path.join(o['dir'], 'a.wav'), 'rb').read()
        b = open(os.path.join(s['dir'], 'a.wav'), 'rb').read()
        i = next((k for k in range(44, min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        msgs.append('audio differs from %.2f s' % ((i - 44) / 4 / 55555))
    return msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--script', action='append', default=[])
    ap.add_argument('--autoplay', action='store_true')
    ap.add_argument('--all', action='store_true', help='autoplay + every games/<GAME>/scripts/*.txt')
    ap.add_argument('--frames', type=int, default=15000)
    ap.add_argument('--dump-every', type=int, default=250)
    ap.add_argument('--insert', nargs='+', default=DEFAULT_INSERTS)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))   # never all cores: the host runs other services
    ap.add_argument('--collect-only', action='store_true',
                    help='only run the original image and merge build/disasm/exec_p1.bin (used by make matching)')
    args = ap.parse_args()

    cases = []
    if args.autoplay or args.all:
        cases.append(('autoplay', ['--autoplay'], args.frames))
    scripts = list(args.script)
    if args.all:
        scripts += sorted(glob.glob(game.path('scripts', '*.txt')))
    for sc in scripts:
        frames = args.frames
        for line in open(sc):
            if line.startswith('# frames '):
                frames = int(line.split()[2])
        cases.append((os.path.splitext(os.path.basename(sc))[0], ['--script', os.path.abspath(sc)], frames))
    if not cases:
        sys.exit('nothing to test (use --autoplay, --script or --all)')

    results = {}
    with ThreadPoolExecutor(args.jobs) as ex:
        # 1. original runs; their executed-address maps tell mkasm.py what is code
        jobs = [(name, ex.submit(run, name + '_orig', os.path.join(ROOT, 'build/roms'), inp, frames,
                                 args.dump_every, VBLANK_BREAK, True)) for name, inp, frames in cases]
        for name, f in jobs:
            results[(name, None)] = f.result()
        merged = bytearray(open(EXEC_MAP, 'rb').read()) if os.path.exists(EXEC_MAP) else bytearray()
        merged += bytearray(0x40000 - len(merged))      # executed, 32-bit reads, 16/8-bit reads, code ptrs
        before = bytes(merged)
        for name, _ in jobs:
            em = open(os.path.join(results[(name, None)]['dir'], 'exec.bin'), 'rb').read()
            for i, b in enumerate(em):
                merged[i] |= b
        if bytes(merged) != before:
            os.makedirs(os.path.dirname(EXEC_MAP), exist_ok=True)
            open(EXEC_MAP, 'wb').write(merged)
        print('executed P1 instruction addresses known: %d' % sum(bin(b).count('1') for b in merged[:0x10000]))
        if args.collect_only:
            return 0
        # 2. shifted images (rebuilt when mkasm.py or the executed-address map changed)
        shifts = {ins: build_shift(ins) for ins in args.insert}
        # 3. shifted runs
        jobs = [(name, ins, ex.submit(run, '%s_%s' % (name, ins), roms, inp, frames, args.dump_every, brk))
                for name, inp, frames in cases for ins, (roms, brk, back) in shifts.items()]
        for name, ins, f in jobs:
            results[(name, ins)] = f.result()

    failed = 0
    for name, inp, frames in cases:
        o = results[(name, None)]
        for ins, (_, _, back) in shifts.items():
            msgs = compare(o, results[(name, ins)], back)
            status = 'OK  ' if not msgs else 'FAIL'
            print('%s %-20s insert %-6s %6d frames, %5d VBlanks, %3d shots%s' % (
                status, name, ins, frames, len(o['vbl']), len(o['shots']), '' if not msgs else ': ' + '; '.join(msgs)))
            failed += bool(msgs)
    print('%d/%d runs match' % (len(cases) * len(shifts) - failed, len(cases) * len(shifts)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

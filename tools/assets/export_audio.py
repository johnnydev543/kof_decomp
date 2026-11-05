#!/usr/bin/env python3
"""export_audio.py - render every sound of the game to a WAV file (for the native mixer and for remaking).

games/<GAME>/audio.json lists every sound id: its Z80 command byte(s) (from SOUND_CMD_TABLE), a category
(music, voice, sfx, control ...), the channel group used for mixing, and for music the loop points measured
from the driver (single loop, or stems whose channel groups loop with different periods). build/sound_render
runs the game's sound driver (Z80 + YM2610 emulation) offline for one command at a time.

Writes OUT/<cmd>.wav (cmd = the command bytes in hex, e.g. 30.wav, 1a42.wav; stems: 30_s0.wav, 30_s1.wav ...)
and OUT/index.json (sound id -> file(s), category, group, loop points in samples). 16-bit stereo at the
YM2610 rate (55555 Hz) so the loop points stay exact. A remade sound saved under the same name in
mods/<GAME>/audio/ replaces the original (same rate; loop points apply to the new file, or give new ones in
mods/<GAME>/audio/loops.json). The WAVs are derived from the ROM: keep them local, never publish them.

usage: export_audio.py [--out build/assets/<GAME>/audio] [--jobs 8] [--only music,voice_character,...]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402

RATE = 55555
RENDER = os.path.join(game.ROOT, 'build/sound_render')
ALL = ['FM1', 'FM2', 'FM5', 'FM6', 'PA0', 'PA1', 'PA2', 'PA3', 'PA4', 'PA5', 'PB']
SKIP = ('control', 'unused', 'unknown')


def render(seq, secs, mute=None):
    """stereo int16 samples (bytes) of the driver's output for a command sequence"""
    with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as t:
        path = t.name
    try:
        args = [RENDER, '--roms', os.path.join(game.ROOT, 'build/roms'), '--seq', seq, '--secs', '%.3f' % secs,
                '--wav', path]
        if mute:
            args += ['--mute', mute]
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        with wave.open(path) as w:
            return w.readframes(w.getnframes())
    finally:
        os.remove(path)


def save(path, frames):
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)


def trim(frames, loop_len=None):
    """drop the leading silence (driver latency) and the trailing silence of a one-shot"""
    import array
    a = array.array('h', frames)
    n = len(a) // 2
    nz = [i for i in range(n) if abs(a[2 * i]) > 0 or abs(a[2 * i + 1]) > 0]
    if not nz:
        return None
    s = nz[0]
    e = s + loop_len if loop_len else nz[-1] + 1
    return frames[4 * s:4 * min(e, n)]


def job(task):
    name, e, out = task
    z = e['z80']
    seq = '0:%02x' % z[0] if len(z) == 1 else '0:%02x,1:%02x' % tuple(z)
    files = []
    if e['cat'] == 'music' and e.get('stems'):
        for gi, st in enumerate(e['stems']):
            x = render(seq, (st['loop_end'] + RATE) / RATE, ','.join(c for c in ALL if c not in st['chans']))
            f = '%s_s%d.wav' % (name, gi)
            save(os.path.join(out, f), x[:4 * st['loop_end']])
            files.append(dict(file=f, loop_start=st['loop_start'], loop_end=st['loop_end'], chans=st['chans']))
    elif e['cat'] == 'music':
        n = e['loop_end'] if e['loop'] else e['length']
        save(os.path.join(out, name + '.wav'), render(seq, (n + RATE) / RATE)[:4 * n])
        files.append(dict(file=name + '.wav', loop_start=e.get('loop_start'), loop_end=e.get('loop_end')))
    else:
        loop = e.get('loop_len') if e.get('loop') else None
        secs = (loop / RATE + 0.1) if loop else (e.get('secs') or 3) + 0.3
        x = trim(render(seq, secs + 0.2), loop)
        if x is None:
            return name, []
        save(os.path.join(out, name + '.wav'), x)
        files.append(dict(file=name + '.wav', loop_start=0 if loop else None, loop_end=len(x) // 4 if loop else None))
    return name, files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(game.ROOT, 'build/assets', game.NAME, 'audio'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--only', help='comma list of categories')
    a = ap.parse_args()
    if not os.path.exists(RENDER):
        sys.exit('build/sound_render missing: make build/sound_render')
    sounds = json.load(open(game.path('audio.json')))
    os.makedirs(a.out, exist_ok=True)
    only = set(a.only.split(',')) if a.only else None
    tasks, seen = [], set()
    for sid, e in sounds.items():
        name = ''.join('%02x' % c for c in e['z80'])
        if e['cat'] in SKIP or (only and e['cat'] not in only) or name in seen:
            continue
        seen.add(name)
        tasks.append((name, e, a.out))
    files = {}
    with ProcessPoolExecutor(a.jobs) as ex:
        for i, (name, fs) in enumerate(ex.map(job, tasks, chunksize=4)):
            files[name] = fs
            if i % 100 == 0:
                print('%d/%d' % (i, len(tasks)), flush=True)
    index = {}
    for sid, e in sounds.items():
        name = ''.join('%02x' % c for c in e['z80'])
        rec = dict(z80=e['z80'], cat=e['cat'], group=e.get('group'))
        if name in files:
            rec['files'] = files[name]
        index[sid] = rec
    json.dump(dict(game=game.NAME, rate=RATE, replace_dir='mods/%s/audio' % game.NAME, sounds=index),
              open(os.path.join(a.out, 'index.json'), 'w'), indent=0)
    write_runtime_table(index, a.out)
    print('%d sound files -> %s' % (sum(len(f) for f in files.values()), a.out))


def write_runtime_table(index, out):
    """commands.txt for the native mixer (src/snd_native.c): one line per distinct Z80 command:
    <command hex> <group> <loop 0/1> <n files> then per file: <name> <loop_start> <loop_end> (-1 = none)"""
    lines, seen = [], set()
    for sid, r in index.items():
        name = ''.join('%02x' % c for c in r['z80'])
        if name in seen or not r.get('files'):
            continue
        seen.add(name)
        fs = r['files']
        loop = int(any(f.get('loop_end') is not None for f in fs))
        parts = ['%s %d %d' % (f['file'], f['loop_start'] if f.get('loop_start') is not None else -1,
                               f['loop_end'] if f.get('loop_end') is not None else -1) for f in fs]
        lines.append('%s %s %d %d %s' % (name, r.get('group') or 'sfx', loop, len(fs), ' '.join(parts)))
    open(os.path.join(out, 'commands.txt'), 'w').write('\n'.join(sorted(lines)) + '\n')


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""ogg_audio.py - compress the exported sounds (export_audio.py) to Ogg Vorbis for the web build.

The WAVs are ~820 MB; as Vorbis (quality 4, same 55555 Hz rate) they are ~50 MB. The native mixer
(src/snd_native.c) loads <name>.ogg wherever <name>.wav is missing, so OUT holds only commands.txt and the
.ogg files. Vorbis keeps the exact sample count, so the loop points in commands.txt still apply.
Files already up to date are skipped. Derived from the ROM: keep them local, never publish them.

usage: ogg_audio.py [--src build/assets/<GAME>/audio] [--out build/assets/<GAME>/audio_ogg] [--quality 4] [--jobs N]
"""
import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402


def encode(src, dst, quality):
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return 0
    tmp = dst + '.tmp.ogg'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-c:a', 'libvorbis', '-q:a', str(quality), tmp],
                   check=True)
    os.replace(tmp, dst)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=os.path.join(game.ROOT, 'build/assets', game.NAME, 'audio'))
    ap.add_argument('--out', default=os.path.join(game.ROOT, 'build/assets', game.NAME, 'audio_ogg'))
    ap.add_argument('--quality', type=int, default=4)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))   # never all cores
    a = ap.parse_args()
    if not os.path.exists(os.path.join(a.src, 'commands.txt')):
        sys.exit('%s/commands.txt missing: run `make assets` first' % a.src)
    os.makedirs(a.out, exist_ok=True)
    shutil.copy(os.path.join(a.src, 'commands.txt'), a.out)
    wavs = sorted(f for f in os.listdir(a.src) if f.endswith('.wav'))
    with ThreadPoolExecutor(a.jobs) as ex:
        done = sum(ex.map(lambda f: encode(os.path.join(a.src, f), os.path.join(a.out, f[:-4] + '.ogg'), a.quality),
                          wavs))
    size = sum(os.path.getsize(os.path.join(a.out, f)) for f in os.listdir(a.out))
    print('%d sounds (%d encoded) -> %s, %.1f MB' % (len(wavs), done, a.out, size / 1e6))


if __name__ == '__main__':
    main()

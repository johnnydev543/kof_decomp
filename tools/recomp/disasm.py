#!/usr/bin/env python3
"""Linear 68000 disassembly of the P1/BIOS images via build/m68kinfo (Musashi).
usage: disasm.py START [COUNT]     e.g. disasm.py c12cd8 40"""
import os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
info = os.path.join(ROOT, 'build/m68kinfo')
p1 = os.path.join(ROOT, 'build/roms/p1.be'); bios = os.path.join(ROOT, 'build/roms/bios.be')
a = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 32
proc = subprocess.Popen([info, 'disasm', p1, bios], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
for _ in range(n):
    proc.stdin.write('%x\n' % a); proc.stdin.flush()
    addr, ln, text = proc.stdout.readline().rstrip('\n').split('\t', 2)
    print('%06x  %s' % (a, text))
    a += int(ln)
proc.stdin.close()

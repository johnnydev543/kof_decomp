#!/usr/bin/env python3
"""codemap.py - code addresses of the assembled P1 image for the native build.

The assembly source (build/disasm/p1.s) is the source of truth: it is assembled to build/disasm/p1.bin and
that image, not the original ROM dump, is what tools/recomp/m68k_recomp.py translates. Every instruction
of p1.s carries a label L_xxxxxx (named after its original address); its address in the assembled image
comes from the ELF symbol table, so the map stays right when the source has been edited and code moved.
Output: one hex address per line (build/disasm/code_p1.txt), used by the recompiler as discovery seeds
so the native code covers everything the disassembler decoded (handlers, jump tables, unrolled loops).

usage: codemap.py [--elf build/disasm/p1.elf] [--out build/disasm/code_p1.txt]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=os.path.join(ROOT, 'build/disasm/p1.elf'))
    ap.add_argument('--src', default=os.path.join(ROOT, 'build/disasm/p1.s'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/disasm/code_p1.txt'))
    args = ap.parse_args()
    nm = next((p + 'nm' for p in (os.environ.get('M68K_PREFIX', ''), 'm68k-elf-', 'm68k-linux-gnu-')
               if p and shutil.which(p + 'nm')), None)
    if nm is None:
        sys.exit('no m68k nm found (binutils-m68k-linux-gnu)')
    # labels that start an instruction (not data): an L_ label followed by an instruction line in p1.s
    code_labels, pending = set(), []
    for line in open(args.src):
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):\s*$', line)
        if m:
            pending.append(m.group(1))
            continue
        if line.startswith('\t'):
            if not line.lstrip().startswith('.'):
                code_labels.update(n for n in pending if n.startswith('L_'))
            pending = []
    addrs = set()
    for line in subprocess.run([nm, args.elf], capture_output=True, text=True, check=True).stdout.splitlines():
        v, _, n = line.split()
        if n in code_labels:
            addrs.add(int(v, 16))
    open(args.out, 'w').write(''.join('%06x\n' % a for a in sorted(addrs)))
    print('%d code addresses -> %s' % (len(addrs), os.path.relpath(args.out, ROOT)))


if __name__ == '__main__':
    sys.exit(main())

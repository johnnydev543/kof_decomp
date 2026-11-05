#!/usr/bin/env python3
"""namecov.py - how much executed P1 code has a stage-3 name, and which unnamed functions are biggest.

Each executed instruction (build/disasm/exec_p1.bin) is attributed to the nearest preceding named label
(anything but L_/D_) in build/disasm/p1.s; a return (rts/rte/rtr/jmp) ends the attribution, except that code
after a return that a named function branches to (bra/bcc/dbcc, not calls) belongs to that function. Named code is
split into curated names (symbols.txt), plr_ (actionmap), chr_ by command (specmap) and chr_ owner only
(charmap). Unnamed code is grouped by the kinds of coverage run that execute it (build/shifttest/runs/
*_orig/exec.bin), and the biggest unnamed functions (bsr/jsr targets or entries after a return) are listed.

usage: namecov.py [--top 40] [--kind ALL|arcade|moves|versus|gameover|testmode|autoplay]
       (run make matching / make shifttest first)
"""
import argparse
import collections
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)
KINDS = ['autoplay', 'arcade', 'moves', 'versus', 'gameover', 'testmode']
RETURNS = ('rts', 'rte', 'rtr', 'jmp')
CMD = re.compile(r'_(qcf|qcb|dp|rdp|hcf|hcb|chg|r360|r720|w_|dm_)')


def bits(path):
    m = open(path, 'rb').read()[:0x10000]
    return {i * 2 for i in range(len(m) * 8) if m[i >> 3] >> (i & 7) & 1}


SOURCES = [('moves', 'official move names (movenames)'),
           ('derived', 'derived from owner (derivemap)'),
           ('specials', 'chr by command (specmap)'),
           ('actions', 'plr (actionmap)'),
           ('chars', 'chr owner only (charmap)')]


def name_sources():
    """{name: category} from the symbol files (symbols.txt names are curated)"""
    out = {}
    for f, cat in SOURCES:
        path = game.symbols(f)
        if os.path.exists(path):
            for line in open(path):
                m = re.match(r'^[0-9a-f]{6}\s+(\S+)', line)
                if m:
                    out.setdefault(m.group(1), cat)
    return out


_SRC = None


def category(name):
    global _SRC
    if _SRC is None:
        _SRC = name_sources()
    return _SRC.get(name, 'curated (symbols/curated.txt)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--kind', default='ALL', help='list unnamed functions executed by this kind of run (ALL = every kind)')
    args = ap.parse_args()

    execd = bits(os.path.join(ROOT, 'build/disasm/exec_p1.bin'))
    kinds = collections.defaultdict(set)
    for p in glob.glob(os.path.join(ROOT, 'build/shifttest/runs/*_orig/exec.bin')):
        n = os.path.basename(os.path.dirname(p))[:-len('_orig')]
        kinds['arcade' if n.startswith('arcade') else 'moves' if n.startswith('moves') else n] |= bits(p)
    want = set.intersection(*kinds.values()) if args.kind == 'ALL' else kinds[args.kind]

    lines = open(os.path.join(ROOT, 'build/disasm/p1.s')).read().split('\n')
    callers = collections.Counter()
    for line in lines:
        m = re.match(r'\t(bsr|jsr)(\.[sw])? \(?L_([0-9a-f]{6})', line)
        if m:
            callers[int(m.group(3), 16)] += 1

    # code after a return that is reached by branches (not calls) from a named function belongs to it:
    # attribute in passes, each one inheriting names through bra/bcc/dbcc targets found in the previous one
    inherit = {}
    for _ in range(4):
        cur, src_of = None, {}
        for line in lines:
            if line.startswith('L_') and line.endswith(':'):
                a = int(line[2:8], 16)
                if cur is None and a in inherit:
                    cur = inherit[a]
                continue
            m = re.match(r'^([A-Za-z_][A-Za-z0-9_.]*):\s*$', line)
            if m:
                if not m.group(1).startswith(('L_', 'D_')):
                    cur = m.group(1)
                continue
            if line.startswith('\t'):
                ins = line.split('|')[0].split()
                if cur and ins and re.match(r'^(bra|b[a-z]{2}|db[a-z]{1,2})(\.[sw])?$', ins[0]) and ins[0] != 'bsr':
                    t = re.search(r'\bL_([0-9a-f]{6})\b', ins[-1])
                    if t:
                        src_of.setdefault(int(t.group(1), 16), cur)
                if ins and ins[0] in RETURNS and not (ins[0] == 'jmp' and ',%pc,' in line.split('|')[0]):
                    cur = None
        if all(inherit.get(k) == v for k, v in src_of.items()) and len(src_of) == len(inherit):
            break
        inherit = src_of

    per_cat = collections.Counter()
    unnamed = set()
    funcs = []                       # [entry, executed+unnamed in `want`, size, named]
    name = None
    func = None
    addr = None
    for line in lines:
        if line.startswith('L_') and line.endswith(':'):
            addr = int(line[2:8], 16)
            if name is None and addr in inherit:
                name = inherit[addr]
                if func:
                    func[3] = True
            if addr in callers or func is None:
                func = [addr, 0, 0, False]
                funcs.append(func)
            continue
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_.]*):\s*$', line)
        if m:
            if not m.group(1).startswith(('L_', 'D_')):
                name = m.group(1)
                if func:
                    func[3] = True
            continue
        if line.startswith('\t') and addr is not None:
            if addr in execd:
                if name is None:
                    per_cat['(unnamed)'] += 1
                    unnamed.add(addr)
                else:
                    per_cat[category(name)] += 1
            if func is not None:
                func[2] += 1
                if name is None and addr in want:
                    func[1] += 1
            addr = None
            ins = line.split('|')[0].split()
            # jmp (base,%pc,Xn) enters an unrolled loop or branch table of the same function
            if ins and ins[0] in RETURNS and not (ins[0] == 'jmp' and ',%pc,' in line.split('|')[0]):
                name = None
                func = None

    tot = sum(per_cat.values())
    print('executed instructions: %d' % tot)
    for k, v in per_cat.most_common():
        print('  %-26s %6d  %5.1f%%' % (k, v, 100.0 * v / tot))

    groups = collections.Counter()
    for a in unnamed:
        ks = [k for k in KINDS if a in kinds[k]]
        groups['ALL' if len(ks) == len(KINDS) else '3+ kinds' if len(ks) >= 3 else '+'.join(ks) or '(none)'] += 1
    print('\nunnamed executed instructions by the kinds of run that execute them:')
    for k, v in groups.most_common(12):
        print('  %-26s %6d' % (k, v))

    res = sorted((f for f in funcs if not f[3] and f[1] > 0), key=lambda f: -f[1])
    print('\nbiggest unnamed functions executed by %s (%d functions, %d instructions):'
          % (args.kind, len(res), sum(f[1] for f in res)))
    for f in res[:args.top]:
        print('  %06x  executed %4d  size %4d  callers %d' % (f[0], f[1], f[2], callers[f[0]]))


if __name__ == '__main__':
    main()

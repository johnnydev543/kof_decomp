#!/usr/bin/env python3
"""derivemap.py - derive names for unnamed code from the one named function that uses it (stage 3).

Most of the remaining unnamed code is small: object states, spawned helper objects and subroutines of a
function that already has a name. An unnamed entry whose every reference comes from code attributed to the
same named function R gets a name from R and the kind of reference:
  R_state_ADDR  stored with move.l #entry into an object field (next handler / state machine step)
  R_obj_ADDR    loaded into an address register with lea (handler of a spawned object)
  R_sub_ADDR    called with bsr/jsr
Code is attributed like namecov.py (named label up to a return; code after a return reached by branches
of a named function belongs to it). Derived names count as R for further derivation, so a chain of states
all carry the root name R. Entries referenced from data tables or from more than one function are left
alone. Output: games/<GAME>/symbols/derived.txt (loaded after the other generated files, before
symbols.txt, so curated names win).

usage: derivemap.py   (after make matching; then make matching again to use the names)
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)

OUT = game.symbols('derived')
RETURNS = ('rts', 'rte', 'rtr', 'jmp')
DERIVED = re.compile(r'_(state|obj|sub)_[0-9a-f]{6}$')


def parse(path):
    """[(addr, [names], code text)] for every instruction / data line of p1.s"""
    out, names, addr = [], [], None
    for line in open(path).read().split('\n'):
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_.]*):$', line)
        if m:
            n = m.group(1)
            if re.match(r'^[LD]_[0-9a-f]{6}$', n):
                addr = int(n[2:], 16)
            if not DERIVED.search(n):   # our own earlier output: derived again from scratch
                names.append(n)
            continue
        if line.startswith('\t'):
            code = line.split('|')[0].strip()
            # operands name derived labels: refer to them by address
            code = re.sub(r'\b[A-Za-z_][A-Za-z0-9_]*_(?:state|obj|sub)_([0-9a-f]{6})\b', r'L_\1', code)
            out.append((addr, names, code))
            names, addr = [], None
    return out


def attribute(lines, extra):
    """per line: the root name it belongs to (or None); extra = {addr: derived root} for unnamed entries"""
    def own(names, addr):
        for n in names:
            if not re.match(r'^[LD]_[0-9a-f]{6}$', n):
                return n
        return extra.get(addr)
    inherit = {}
    for _ in range(4):
        cur, src, res = None, {}, []
        for addr, names, code in lines:
            o = own(names, addr)
            if o:
                cur = o
            elif cur is None and addr in inherit:
                cur = inherit[addr]
            res.append(cur)
            ins = code.split()
            if not ins:
                continue
            if cur and re.match(r'^(bra|b[a-z]{2}|db[a-z]{1,2})(\.[sw])?$', ins[0]) and ins[0] != 'bsr':
                t = re.search(r'\bL_([0-9a-f]{6})\b', code)
                if t:
                    src.setdefault(int(t.group(1), 16), cur)
            if ins[0] in RETURNS and ',%pc,' not in code or ins[0].startswith('.'):
                cur = None
        if src == inherit:
            return res
        inherit = src
    return res


def root(name):
    return DERIVED.sub('', name)


def main():
    lines = parse(os.path.join(ROOT, 'build/disasm/p1.s'))
    labelled = {a for a, names, _ in lines if a is not None and
                any(not re.match(r'^[LD]_[0-9a-f]{6}$', n) for n in names)}
    code_entries = {a for a, names, code in lines if a is not None and code and not code.startswith('.')}
    derived = {}                    # addr -> (root, kind)
    for _ in range(12):
        owner = attribute(lines, {a: r for a, (r, _) in derived.items()})
        refs = collections.defaultdict(set)     # target -> {(root, kind)}
        for (addr, names, code), o in zip(lines, owner):
            if code.startswith('.'):
                for t in re.findall(r'\bL_([0-9a-f]{6})\b', code):
                    refs[int(t, 16)].add((None, 'data'))
                continue
            ins = code.split()
            if not ins:
                continue
            kind = None
            if ins[0] == 'move.l' and code.startswith('move.l #L_'):
                kind = 'state'
            elif ins[0] == 'lea':
                kind = 'obj'
            elif re.match(r'^(bsr|jsr|jbsr)(\.[sw])?$', ins[0]):
                kind = 'sub'
            elif re.match(r'^(bra|b[a-z]{2}|db[a-z]{1,2}|jmp)(\.[sw])?$', ins[0]):
                kind = 'branch'
            for t in re.findall(r'\bL_([0-9a-f]{6})\b', code):
                refs[int(t, 16)].add((root(o) if o else None, kind or 'other'))
        new = {}
        for t, rs in refs.items():
            if t in labelled or t in derived or t not in code_entries:
                continue
            roots = {r for r, _ in rs}
            kinds = {k for _, k in rs}
            if len(roots) != 1 or None in roots or 'other' in kinds or 'data' in kinds:
                continue
            kinds.discard('branch')
            if not kinds:
                continue            # only branched to: part of the owner (namecov inherits it)
            kind = 'state' if 'state' in kinds else 'obj' if 'obj' in kinds else 'sub'
            new[t] = (roots.pop(), kind)
        if not new:
            break
        derived.update(new)
    out = ['# Generated by tools/disasm/derivemap.py - do not edit (curated names go to symbols.txt).',
           '# Unnamed code referenced only from one named function R: R_state_ (stored as a handler),',
           '# R_obj_ (spawned object handler), R_sub_ (called). Loaded after symbols/specials.txt.']
    kinds = collections.Counter()
    for a in sorted(derived):
        r, k = derived[a]
        kinds[k] += 1
        out.append('%06x  %s_%s_%06x  | %s of %s' % (a, r, k, a, {'state': 'state', 'obj': 'spawned object',
                                                                    'sub': 'subroutine'}[k], r))
    open(OUT, 'w').write('\n'.join(out) + '\n')
    print('derived %d names (%s) -> %s' % (len(derived), ', '.join('%s %d' % kv for kv in sorted(kinds.items())),
                                           os.path.relpath(OUT, ROOT)))


if __name__ == '__main__':
    sys.exit(main())

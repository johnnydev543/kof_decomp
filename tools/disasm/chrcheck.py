#!/usr/bin/env python3
"""chrcheck.py - drop generated chr_<name>_ labels that other runs contradict.

charmap.py / specmap.py attribute code to a character because only that character's runs executed it.
A mirror match also has its own stage, screens and opponents, so stage or screen code that happens to
run only there is attributed to the character too (chr_king_072a3e was the NEXT STAGE photo,
chr_leona_0290fe a branch of the HUD update). The moves_<team> coverage runs know exactly who is on
screen (P1 = the team, P2 = Benimaru x3): a chr_X label whose code runs in a moves run without X is
wrong. The generators' entry set also includes addresses in the middle of a function (dynamic entries:
an executed address not reached by fall-through in that run); a label there that nothing references and
that does not follow a return/jump/bra is dropped too. Both generators filter their output through
filter_lines(); --prune applies the same filter to the existing files (no new mirror-match runs needed).

usage: chrcheck.py [--prune]   (needs build/shifttest/runs/moves*_orig/exec.bin: make shifttest)
"""
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)

FILES = [game.symbols('chars'), game.symbols('specials')]
CHARS = game.CFG['characters']['names']
BASE = {int(k): v for k, v in game.CFG['characters']['base'].items()}
MOVES_P2 = set(game.CFG['characters']['moves_p2'])   # who P2 is in the moves_* scripts
LABEL = re.compile(r'^([0-9a-f]{6})\s+chr_([a-z]+(?:_iori|_leona)?)_')
# generic code that one character's move happened to trigger in the generator runs; every kind of coverage
# run executes it (arcade, attract demo, game over, test mode, versus), but the moves runs cannot
# contradict it because only that character's multi-hit move triggers it there
NOT_CHARACTER = [(int(lo, 16), int(hi, 16), why) for lo, hi, why in game.CFG['naming']['not_character']]


def _bits(path):
    m = open(path, 'rb').read()[:0x10000]
    return {i * 2 for i in range(len(m) * 8) if m[i >> 3] >> (i & 7) & 1}


def moves_runs():
    """[(run name, character ids on screen, executed addresses)] of the moves_* coverage runs"""
    sys.path.insert(0, game.path('scripts'))
    import gen_scripts
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'build/shifttest/runs/moves*_orig/exec.bin'))):
        run = os.path.basename(os.path.dirname(p))[:-len('_orig')]
        team = run.split('_', 1)[1]
        if team not in gen_scripts.TEAMS:
            continue
        ids = set(gen_scripts.TEAMS[team]) | MOVES_P2
        ids |= {BASE[i] for i in ids if i in BASE}
        out.append((run, ids, _bits(p)))
    return out


def contradicted(addr, name, runs):
    """name of a run that executes addr without character `name` on screen, else None"""
    if name not in CHARS:
        return None
    cid = CHARS.index(name)
    for run, ids, ex in runs:
        if addr in ex and cid not in ids:
            return run
    return None


def interior_labels():
    """addresses in build/disasm/p1.s that are the middle of a function (neither referenced nor after
    rts/rte/rtr/jmp/bra; the generators' dynamic entries include such addresses), or that lie inside a
    curated non-character function up to its return (chr_shingo_hcf_k_ labels were branch targets of the
    generic fix-layer print code)"""
    path = os.path.join(ROOT, 'build/disasm/p1.s')
    if not os.path.exists(path):
        return set()
    lines = open(path).read().split('\n')
    refs = set()
    for line in lines:
        if line.startswith('\t'):
            refs.update(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\b', line.split('|')[0]))
    curated = set()
    for line in open(game.symbols('curated')):
        m = re.match(r'^([0-9a-f]{6})\s+([A-Za-z_]\w*)', line)
        if m and not m.group(2).startswith('chr_') and int(m.group(1), 16) < 0x100000:
            curated.add(m.group(2))
    out = set()
    inside = None                   # curated (not character) function the current instruction belongs to
    prev = None                     # mnemonic of the previous instruction
    names = []                      # labels of the current instruction
    for line in lines:
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):$', line)
        if m:
            names.append(m.group(1))
            continue
        if line.startswith('\t'):
            mn = line.split()[0]
            if mn.startswith('.'):
                prev, names = None, []
                continue
            addr = next((int(n[2:], 16) for n in names if re.match(r'^L_[0-9a-f]{6}$', n)), None)
            if curated & set(names):
                inside = True
            elif any(n.startswith('chr_') for n in names) and prev in ('rts', 'rte', 'rtr', 'jmp', 'bra', 'bra.s',
                                                                       'bra.w', None):
                inside = None
            if addr is not None and (inside or (prev not in (None, 'rts', 'rte', 'rtr', 'jmp', 'bra', 'bra.s', 'bra.w')
                                                and not refs & set(names))):
                out.add(addr)
            if mn in ('rts', 'rte', 'rtr'):
                inside = None
            prev, names = mn, []
    return out


def filter_lines(lines, runs=None, interior=None):
    """drop label lines contradicted by the moves runs or in the middle of a function;
    returns (kept lines, number dropped)"""
    runs = moves_runs() if runs is None else runs
    interior = interior_labels() if interior is None else interior
    kept, dropped, inner = [], 0, 0
    for line in lines:
        m = LABEL.match(line)
        if m and contradicted(int(m.group(1), 16), m.group(2), runs):
            dropped += 1
            continue
        if m and (int(m.group(1), 16) in interior or any(lo <= int(m.group(1), 16) < hi for lo, hi, _ in NOT_CHARACTER)):
            inner += 1
            continue
        kept.append(line)
    i = next((i for i, l in enumerate(kept) if not l.startswith('#')), len(kept))
    kept[i:i] = ['# Filtered by chrcheck.py: no entry runs in a moves run without that character, sits inside a '
                 'function or in NOT_CHARACTER.']
    return kept, dropped + inner


def main():
    runs = moves_runs()
    if not runs:
        sys.exit('no build/shifttest/runs/moves*_orig/exec.bin: run make shifttest first')
    interior = interior_labels()
    total = 0
    for path in FILES:
        lines = [l for l in open(path).read().split('\n') if l and 'chrcheck.py' not in l]
        kept, n = filter_lines(lines, runs, interior)
        total += n
        print('%s: %d to drop (contradicted or inside a function)' % (os.path.relpath(path, ROOT), n))
        if '--prune' in sys.argv and n:
            open(path, 'w').write('\n'.join(kept) + '\n')
    return 0 if '--prune' in sys.argv or not total else 1


if __name__ == '__main__':
    sys.exit(main())

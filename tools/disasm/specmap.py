#!/usr/bin/env python3
"""specmap.py - name character special-move code by the command that triggers it (stage 3).

Same setup as actionmap.py (2P versus, P2 idle, identical prefix, P1 team poked to X,X,X). From the
first controllable frame P1 enters one command motion + button; the run is compared with the idle
baseline. Character-specific entries (chr_<name>_<addr> from symbols/chars.txt) that only one command
causes are renamed chr_<name>_<motion>_<p|k>_<addr> (both buttons: chr_<name>_<motion>_<addr>).
Motions are named from P1's side (facing right):
  qcf  down, down-forward, forward       qcb  down, down-back, back
  dp   forward, down, down-forward       rdp  back, down, down-back
  hcf  back, down-back, down, down-forward, forward
  hcb  forward, down-forward, down, down-back, back
  chg_bf  hold back 50 frames, forward   chg_du  hold down 50 frames, up
  r360  walk in for 90 frames, then a full circle (command throws)
  w_hcb / w_hcf  walk in for 90 frames first (throws only connect up close); {hcb, w_hcb} = hcb
Desperation moves (supers) are tested the same way with P1 in EXTRA mode and low health (where DMs
are free): qcf2, qcb2, qcf_hcb, qcb_hcf, r720 (walk in, two circles), chg_bfbf, chg_dbdf
-> chr_<name>_dm_<motion>_<p|k>_<addr>.
Output: games/<GAME>/symbols/specials.txt (loaded after symbols/actions.txt, before symbols.txt).

usage: specmap.py [--jobs N]
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import actionmap  # noqa: E402  (prefix / timing of the 2P versus setup)
import charmap    # noqa: E402  (character names)
game = actionmap.game

OUT = os.path.join(ROOT, 'build/specmap')
FIGHT = actionmap.FIGHT
END = FIGHT + 420
_CIRCLE = [(4, 'right'), (4, 'down,right'), (4, 'down'), (4, 'down,left'), (4, 'left'), (4, 'up,left'), (4, 'up'), (4, 'up,right')]
# motion -> [(frames, keys)]; the button is added to the last step
MOTIONS = {
    'qcf': [(4, 'down'), (4, 'down,right'), (4, 'right')],
    'qcb': [(4, 'down'), (4, 'down,left'), (4, 'left')],
    'dp': [(4, 'right'), (4, 'down'), (4, 'down,right')],
    'rdp': [(4, 'left'), (4, 'down'), (4, 'down,left')],
    'hcf': [(4, 'left'), (4, 'down,left'), (4, 'down'), (4, 'down,right'), (4, 'right')],
    'hcb': [(4, 'right'), (4, 'down,right'), (4, 'down'), (4, 'down,left'), (4, 'left')],
    'chg_bf': [(50, 'left'), (4, 'right')],      # charge back, forward
    'chg_du': [(50, 'down'), (4, 'up')],         # charge down, up
    'r360': [(90, 'right')] + _CIRCLE,            # walk in, full circle (command throws)
    # walk in for 90 frames first: command throws only connect up close (they whiff from the start position)
    'w_hcb': [(90, 'right'), (4, 'right'), (4, 'down,right'), (4, 'down'), (4, 'down,left'), (4, 'left')],
    'w_hcf': [(90, 'right'), (4, 'left'), (4, 'down,left'), (4, 'down'), (4, 'down,right'), (4, 'right')],
}
BUTTONS = {'p': 'a', 'k': 'b'}
# desperation moves (DM): P1 in EXTRA mode (0x10a849 = 1) with low health may use them freely; the
# 'dm' baseline uses the same setup. Named chr_<name>_dm_<motion>_<p|k>.
SUPERS = {
    'qcf2': [(4, 'down'), (4, 'down,right'), (4, 'right'), (4, 'down'), (4, 'down,right'), (4, 'right')],
    'qcb2': [(4, 'down'), (4, 'down,left'), (4, 'left'), (4, 'down'), (4, 'down,left'), (4, 'left')],
    'qcf_hcb': [(4, 'down'), (4, 'down,right'), (4, 'right'), (4, 'down,right'), (4, 'down'), (4, 'down,left'), (4, 'left')],
    'qcb_hcf': [(4, 'down'), (4, 'down,left'), (4, 'left'), (4, 'down,left'), (4, 'down'), (4, 'down,right'), (4, 'right')],
    'r720': [(90, 'right')] + _CIRCLE + _CIRCLE,                           # walk in, two circles (throw DMs)
    'w_hcb2': [(90, 'right')] + [(4, k) for k in ('right', 'down,right', 'down', 'down,left', 'left')] * 2,
    'chg_bfbf': [(50, 'left'), (4, 'right'), (4, 'left'), (4, 'right')],    # charge back, forward back forward
    'chg_dbdf': [(50, 'down,left'), (4, 'down,right'), (4, 'down,left'), (4, 'up,right')],  # charge down-back
}
DM_POKES = ['poke:%x=01' % actionmap.game.ram('p1_mode')]                      # P1 mode = EXTRA (P1 select block + 2)
DM_LOW_HEALTH = 'poke:%x=0010' % actionmap.game.ram('p1_health')                 # P1 health, set shortly before the fight


def run(cid, cmd):
    d = os.path.join(OUT, '%s_%s' % (charmap.CHARS[cid], cmd))
    os.makedirs(d, exist_ok=True)
    dm = cmd.startswith('dm')
    pk = ','.join(['poke:%x=%02x' % (a, cid) for a in range(actionmap.P1_TEAM, actionmap.P1_TEAM + 3)] +
                  ['poke:%x=%02x' % (a, actionmap.OPPONENT) for a in range(actionmap.P2_TEAM, actionmap.P2_TEAM + 3)] +
                  (DM_POKES if dm else []))
    lines = ['# frames %d' % END] + actionmap.prefix() + ['1950 650 ' + pk]   # END is part of the cache key
    if dm:
        lines.append('%d 2 %s' % (FIGHT - 60, DM_LOW_HEALTH))
    if cmd not in ('idle', 'dm'):
        motion, btn = cmd.rsplit('_', 1)
        steps = SUPERS[motion[3:]] if dm else MOTIONS[motion]
        t = FIGHT
        for i, (n, keys) in enumerate(steps):
            last = i == len(steps) - 1
            lines.append('%d %d %s' % (t, n, keys + (',' + BUTTONS[btn] if last else '')))
            t += n
    sc = os.path.join(d, 'script.txt')
    text = '\n'.join(lines) + '\n'
    ex = os.path.join(d, 'exec.bin')
    if os.path.exists(ex) and os.path.exists(sc) and open(sc).read() == text and os.path.getmtime(ex) > os.path.getmtime(
            os.path.join(ROOT, 'build/kof97')):
        em = open(ex, 'rb').read()[:0x10000]      # cached: same script, same binary
        return {i * 2 for i in range(len(em) * 8) if em[i >> 3] >> (i & 7) & 1}
    open(sc, 'w').write(text)
    for f in ('exec.bin', 'nv'):
        if os.path.exists(os.path.join(d, f)):
            os.remove(os.path.join(d, f))
    subprocess.run([os.path.join(ROOT, 'build/kof97'), '--interp', '--headless', '--frames', str(END), '--script', sc,
                    '--rtc-fixed', '883612800', '--misses', '', '--nvram', os.path.join(d, 'nv'), '--quiet',
                    '--exec-log', os.path.join(d, 'exec.bin')], check=True, capture_output=True)
    em = open(os.path.join(d, 'exec.bin'), 'rb').read()[:0x10000]
    return {i * 2 for i in range(len(em) * 8) if em[i >> 3] >> (i & 7) & 1}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))   # never all cores: the host runs other services
    args = ap.parse_args()
    cmds = ['%s_%s' % (m, b) for m in MOTIONS for b in BUTTONS]
    dms = ['dm_%s_%s' % (m, b) for m in SUPERS for b in BUTTONS]
    keys = [(c, cmd) for c in range(len(charmap.CHARS)) for cmd in ['idle'] + cmds + ['dm'] + dms]
    with ThreadPoolExecutor(args.jobs) as ex:
        sets = dict(zip(keys, ex.map(lambda k: run(*k), keys)))

    import mkasm
    chars = mkasm.load_symbols(game.symbols('chars'))
    manual = mkasm.load_symbols(game.symbols('curated'))
    owner_name = {a: v[0].split('_')[1] for a, v in chars.items() if v[0].startswith('chr_')}
    # entries executed in only one character's specmap runs (any command) are character-specific too,
    # even if the charmap mirror matches never executed them (desperation moves, rare specials)
    p1 = open(os.path.join(ROOT, 'build/roms/p1.be'), 'rb').read()
    bios = open(os.path.join(ROOT, 'build/roms/bios.be'), 'rb').read()
    insns = mkasm.collect_insns(p1, bios)
    calls = {t for d in insns.values() for t in d.targets if d.flow in ('call', 'jump')}
    entries = (calls | mkasm.ENTRIES | mkasm.DYN_ENTRIES) & set(insns)
    by_base = {}
    for c in range(len(charmap.CHARS)):
        b = charmap.BASE.get(c, c)
        by_base.setdefault(b, set()).update(*(sets[(c, cmd)] for cmd in ['idle'] + cmds + ['dm'] + dms))
    for b, u in by_base.items():
        others = set().union(*(v for k, v in by_base.items() if k != b))
        for a in (u - others) & entries:
            owner_name.setdefault(a, charmap.CHARS[b])
    lines = ['# Generated by tools/disasm/specmap.py - do not edit (curated names go to symbols.txt).',
             '# Character-specific code caused by one command motion (P1 facing right) in a 2P versus match.']
    per_char = {}
    for a in sorted(owner_name):
        if a in manual:
            continue
        name = owner_name[a]
        cids = [c for c, n in enumerate(charmap.CHARS) if n == name or charmap.BASE.get(c) is not None
                and charmap.CHARS[charmap.BASE[c]] == name]
        hits = sorted({cmd for c in cids for cmd in cmds if a in sets[(c, cmd)] - sets[(c, 'idle')]})
        motions = {h.rsplit('_', 1)[0] for h in hits}
        # a half circle ends with the quarter circle (hcf ... down, down-forward, forward): a qcf move also
        # comes out on an hcf input, so {qcf, hcf} means the qcf move
        for small, big in (('qcf', 'hcf'), ('qcb', 'hcb'), ('hcb', 'w_hcb'), ('hcf', 'w_hcf'), ('dm_hcb2', 'dm_w_hcb2')):
            if motions == {small, big}:
                motions = {small}
                hits = [h for h in hits if h.startswith(small + '_')]
        motions = sorted(motions)
        if len(motions) != 1:
            continue
        btns = sorted({h.rsplit('_', 1)[1] for h in hits})
        tag = motions[0] + ('_' + btns[0] if len(btns) == 1 else '')
        lines.append('%06x  chr_%s_%s_%06x  | %s-specific code, caused by %s' % (a, name, tag, a, name, ' '.join(hits)))
        per_char[name] = per_char.get(name, 0) + 1
    # desperation moves: caused by exactly one DM command (vs the DM baseline) and by no special command
    named = {int(l.split()[0], 16) for l in lines if not l.startswith('#')}
    for a in sorted(owner_name):
        if a in manual or a in named:
            continue
        name = owner_name[a]
        cids = [c for c, n in enumerate(charmap.CHARS) if n == name or charmap.BASE.get(c) is not None
                and charmap.CHARS[charmap.BASE[c]] == name]
        if any(a in sets[(c, cmd)] - sets[(c, 'idle')] for c in cids for cmd in cmds):
            continue
        hits = sorted({cmd for c in cids for cmd in dms if a in sets[(c, cmd)] - sets[(c, 'dm')]})
        motions = sorted({h.rsplit('_', 1)[0] for h in hits})
        if len(motions) != 1:
            continue
        btns = sorted({h.rsplit('_', 1)[1] for h in hits})
        tag = motions[0] + ('_' + btns[0] if len(btns) == 1 else '')
        lines.append('%06x  chr_%s_%s_%06x  | %s-specific code, caused by desperation move %s' % (a, name, tag, a, name, ' '.join(hits)))
        per_char[name] = per_char.get(name, 0) + 1
    import chrcheck
    import movenames
    lines, _ = chrcheck.filter_lines(lines)      # contradicted by the moves runs / inside a function
    lines, _ = movenames.filter_lines(lines)     # official move names for the command labels
    path = game.symbols('specials')
    open(path, 'w').write('\n'.join(lines) + '\n')
    for c, n in enumerate(charmap.CHARS):
        caused = sum(1 for cmd in cmds if sets[(c, cmd)] - sets[(c, 'idle')])
        print('  %-13s commands with an effect %2d/%d, entries named %3d' % (n, caused, len(cmds), per_char.get(n, 0)))
    print('named %d entries -> %s' % (sum(per_char.values()), path))


if __name__ == '__main__':
    main()

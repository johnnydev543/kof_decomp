#!/usr/bin/env python3
"""actionmap.py - name player-action code by observation (stage-3 naming).

A 2P versus match is set up (teams poked during order select; P2 never touches the controls). From
the first fight frame, P1 performs exactly one action (walk forward, jump, light punch, ...) while
the baseline run stays idle. Everything before is identical, so
    A(char, action) = executed(char, action) - executed(char, idle)
is the code the action caused. Code in A(char, action) for every test character is shared player
code (plr_<action>_<addr>); code only one character executes refines its chr_ name
(chr_<name>_<action>_<addr>). Output: games/<GAME>/symbols/actions.txt (loaded by mkasm.py between
symbols/chars.txt and the curated symbols.txt).

usage: actionmap.py [--jobs N]
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/game.json: RAM addresses)
P1_TEAM, P2_TEAM = game.ram('p1_team'), game.ram('p2_team')
OUT = os.path.join(ROOT, 'build/actionmap')
FIGHT = 3800            # first frame P1 has control (after the round intro) in this setup
END = FIGHT + 420
TEST_CHARS = {0: 'kyo', 3: 'terry', 10: 'ralf', 17: 'king'}
OPPONENT = 1            # benimaru, idle

# action -> list of (offset from FIGHT, duration, keys)
ACTIONS = {
    'walk_fwd': [(0, 300, 'right')],
    'walk_back': [(0, 300, 'left')],
    'crouch': [(0, 300, 'down')],
    'jump': [(0, 8, 'up')],
    'jump_fwd': [(0, 8, 'up,right')],
    'super_jump': [(0, 4, 'down'), (5, 8, 'up')],
    'run': [(0, 4, 'right'), (8, 200, 'right')],
    'backstep': [(0, 4, 'left'), (8, 6, 'left')],
    'punch_light': [(0, 4, 'a')],
    'kick_light': [(0, 4, 'b')],
    'punch_heavy': [(0, 4, 'c')],
    'kick_heavy': [(0, 4, 'd')],
    'crouch_punch': [(0, 30, 'down'), (10, 4, 'down,a')],
    'crouch_kick': [(0, 30, 'down'), (10, 4, 'down,d')],
    'jump_kick': [(0, 8, 'up'), (16, 4, 'd')],
    'roll': [(0, 4, 'a,b')],
    'pow_charge': [(0, 240, 'a,b,c')],
    'taunt': [(0, 4, 'start1')],
}
_ATK = {'punch_light', 'kick_light', 'punch_heavy', 'kick_heavy'}
_MOVE = {'walk_fwd', 'walk_back', 'run', 'backstep'}
# smallest group containing all actions that caused an entry gives its name (checked in this order)
GROUPS = [('walk', {'walk_fwd', 'walk_back'}),
          ('move', _MOVE),
          ('crouch', {'crouch', 'crouch_punch', 'crouch_kick'}),
          ('jump', {'jump', 'jump_fwd', 'super_jump', 'jump_kick'}),
          ('attack_stand', _ATK),
          ('attack', _ATK | {'crouch_punch', 'crouch_kick', 'jump_kick'}),
          ('ground_move', _MOVE | {'crouch'}),
          ('button', _ATK | {'crouch_punch', 'crouch_kick', 'jump_kick', 'roll', 'pow_charge', 'taunt'}),
          ('any_move', _MOVE | {'crouch', 'jump', 'jump_fwd', 'super_jump'})]


def prefix():
    lines = ['900 8 coin1', '960 8 coin2', '1020 6 start1']
    lines += ['%d 4 a' % f for f in range(1080, 1400, 30)]
    lines += ['1400 6 start2', '1420 8 coin2', '1440 6 start2']
    lines += ['%d 4 p2a' % f for f in range(1500, 2400, 25)]
    return lines


def run(c, action):
    d = os.path.join(OUT, '%s_%s' % (TEST_CHARS[c], action))
    os.makedirs(d, exist_ok=True)
    pk = ','.join(['poke:%x=%02x' % (a, c) for a in range(P1_TEAM, P1_TEAM + 3)] +
                  ['poke:%x=%02x' % (a, OPPONENT) for a in range(P2_TEAM, P2_TEAM + 3)])
    lines = prefix() + ['1950 650 ' + pk]
    for off, dur, keys in ACTIONS.get(action, []):
        lines.append('%d %d %s' % (FIGHT + off, dur, keys))
    sc = os.path.join(d, 'script.txt')
    open(sc, 'w').write('\n'.join(lines) + '\n')
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
    keys = [(c, a) for c in TEST_CHARS for a in ['idle'] + list(ACTIONS)]
    with ThreadPoolExecutor(args.jobs) as ex:
        ex_sets = dict(zip(keys, ex.map(lambda k: run(*k), keys)))
    caused = {(c, a): ex_sets[(c, a)] - ex_sets[(c, 'idle')] for c in TEST_CHARS for a in ACTIONS}

    import mkasm
    p1 = open(os.path.join(ROOT, 'build/roms/p1.be'), 'rb').read()
    bios = open(os.path.join(ROOT, 'build/roms/bios.be'), 'rb').read()
    insns = mkasm.collect_insns(p1, bios)
    calls = {t for d in insns.values() for t in d.targets if d.flow in ('call', 'jump')}
    entries = (calls | mkasm.ENTRIES | mkasm.DYN_ENTRIES) & set(insns)
    manual = mkasm.load_symbols(game.symbols('curated'))
    chars = mkasm.load_symbols(game.symbols('chars'))

    shared = {}     # entry -> set of actions in which every test character executed it
    for a in ACTIONS:
        common = set.intersection(*(caused[(c, a)] for c in TEST_CHARS))
        for e in common & entries:
            shared.setdefault(e, set()).add(a)
    lines = ['# Generated by tools/disasm/actionmap.py - do not edit (curated names go to symbols.txt).',
             '# Code caused by one P1 action in a 2P versus match (idle P2), compared with an idle baseline.']
    named = 0
    for e in sorted(shared):
        if e in manual:
            continue
        acts = shared[e]
        label = None
        if len(acts) == 1:
            label = next(iter(acts))
        else:
            for g, members in GROUPS:
                if acts <= members:
                    label = g
                    break
        if label is None:
            label = 'action'
        lines.append('%06x  plr_%s_%06x  | shared player code, caused by: %s' % (e, label, e, ' '.join(sorted(acts))))
        named += 1
    refined = 0
    for e in sorted(entries):
        if e in manual or e in shared or e not in chars:
            continue
        cname = chars[e][0]                      # chr_<name>_<addr>
        who = cname.split('_')[1] if cname.startswith('chr_') else None
        cid = next((c for c, n in TEST_CHARS.items() if n == who), None)
        if cid is None:
            continue
        acts = sorted(a for a in ACTIONS if e in caused[(cid, a)])
        if 1 <= len(acts) <= 2:
            lines.append('%06x  chr_%s_%s_%06x  | %s-specific code, caused by: %s' % (e, who, '_'.join(acts), e, who, ' '.join(acts)))
            refined += 1
    path = game.symbols('actions')
    open(path, 'w').write('\n'.join(lines) + '\n')
    for a in ACTIONS:
        n = sum(1 for e in shared if a in shared[e])
        sizes = ' '.join('%s:%d' % (TEST_CHARS[c][:4], len(caused[(c, a)])) for c in TEST_CHARS)
        print('  %-13s shared entries %3d   caused instructions %s' % (a, n, sizes))
    print('shared player entries named: %d, character entries refined: %d -> %s' % (named, refined, path))


if __name__ == '__main__':
    main()

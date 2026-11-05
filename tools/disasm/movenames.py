#!/usr/bin/env python3
"""movenames.py - official move names for the command-tagged character labels (stage 3).

specmap.py names character code by the command that caused it: chr_<name>_<motion>_<p|k>_<addr>
(qcf_p, dp_k, dm_qcf2_p, w_hcf_k ...). games/<GAME>/moves.tsv holds the game's move list (from MAME
command.dat, numpad notation). When exactly one special / desperation move of that character matches
the motion and button group, the label becomes chr_<name>_<move>_<addr> and the comment gets the
official name and command; when several match (A and C versions with different names), the label keeps
the command and the comment lists the candidates. filter_lines() is applied by specmap.py; --apply
rewrites games/<GAME>/symbols/specials.txt in place (idempotent: the command is kept in the comment).

usage: movenames.py [--apply]
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)

TSV = game.path('moves.tsv')
SPECIALS = game.symbols('specials')
CHAR = game.CFG['moves']['char_map']   # moves.tsv headings -> label names
# specmap motion -> (numpad sequence, close only)
MOTION = {'qcf': ('236', False), 'qcb': ('214', False), 'dp': ('623', False), 'rdp': ('421', False),
          'hcf': ('41236', False), 'hcb': ('63214', False), 'w_hcf': ('41236', True), 'w_hcb': ('63214', True),
          'chg_bf': ('[4]6', False), 'chg_du': ('[2]8', False), 'dm_qcf2': ('236236', False),
          'dm_qcb2': ('214214', False), 'dm_qcf_hcb': ('2363214', False), 'dm_qcb_hcf': ('2141236', False),
          'dm_w_hcb2': ('6321463214', True), 'dm_w_hcf2': ('4123641236', True)}
LABEL = re.compile(r'^([0-9a-f]{6})\s+chr_([a-z]+)_((?:dm_)?(?:w_)?(?:qcf2|qcb2|qcf_hcb|qcb_hcf|hcb2|hcf2|qcf|qcb|dp|rdp|'
                   r'hcf|hcb|chg_bf|chg_du))_([pk])_([0-9a-f]{6})(\s+\|\s*(.*))?$')


def load():
    """{char: [(kind, name, seq, close, buttons)]} (follow-ups and air moves left out)"""
    moves = {}
    for line in open(TSV):
        if line.startswith('#') or not line.strip():
            continue
        char, kind, name, cmd = line.rstrip('\n').split('\t')
        if kind not in ('special', 'dm') or name.startswith('^!') or '_^' in cmd or char not in CHAR:
            continue
        close = cmd.startswith('close ')
        c = cmd[6:] if close else cmd
        m = re.match(r'^(\^)?((?:_?[1-9])+)_\+((?:_[A-DPK])+)', c.replace(' ', ''))
        if not m:
            continue
        digits = re.sub(r'[^1-9]', '', m.group(2))
        seq = '[%s]%s' % (digits[0], digits[1:]) if m.group(1) else digits
        moves.setdefault(CHAR[char], []).append((kind, name, seq, close, set(re.findall(r'[A-DPK]', m.group(3))), cmd))
    return moves


def slug(name):
    s = re.sub(r'\b\d+\s*shiki\s*:', '', name, flags=re.I)     # '100 Shiki: Oniyaki' -> oniyaki, '88 Shiki' stays
    s = re.sub(r'[^a-z0-9]+', '_', s.lower()).strip('_')
    return s[:32].rstrip('_')


def lookup(moves, char, motion, btn):
    seq, close = MOTION[motion]
    group = {'p': {'P', 'A', 'C'}, 'k': {'K', 'B', 'D'}}[btn]
    dm = motion.startswith('dm_')
    out = []
    for kind, name, s, c, buttons, cmd in moves.get(char, []):
        if s == seq and (c == close or (not close and not c) or close) and buttons & group and (kind == 'dm') == dm:
            if close and not c and any(x[3] and x[2] == seq and x[4] & group for x in moves.get(char, [])):
                continue        # a close (throw) version exists: the walk-in command means that one
            out.append((name, cmd))
    return out


def filter_lines(lines, moves=None):
    moves = load() if moves is None else moves
    out, renamed = [], 0
    for line in lines:
        m = LABEL.match(line)
        if not m:
            m2 = re.match(r'^([0-9a-f]{6})\s+(chr_[a-z]+)_([a-z0-9_]+)_([0-9a-f]{6})\s+\|.*\[cmd (\S+)_([pk])\]', line)
            out.append(line)
            continue
        addr, char, motion, btn, _, _, comment = m.groups()
        comment = comment or ''
        cands = lookup(moves, char, motion, btn)
        names = sorted({n for n, _ in cands})
        if len(names) == 1:
            label = 'chr_%s_%s_%s' % (char, slug(names[0]), addr)
            renamed += 1
            out.append('%s  %s  | %s; %s (%s) [cmd %s_%s]' % (addr, label, comment, names[0], cands[0][1], motion, btn))
        elif names:
            out.append('%s | candidates: %s' % (line.rstrip(), ' / '.join(names)))
        else:
            out.append(line)
    return out, renamed


COMMANDS = game.path('commands.tsv')
MOVES_OUT = game.symbols('moves')
CMD_SEQ = {'dp': '623', 'rdp': '421', 'qcf': '236', 'qcb': '214', 'hcf': '41236', 'hcb': '63214', 'qcf2': '236236',
           'qcb2': '214214', 'qcf_hcb': '2363214', 'qcb_hcf': '2141236', 'hcb_f': '632146', 'hcb2': '6321463214',
           'hcf2': '4123641236', 'qcb_dbf': '21416', 'db_f': '16', 'f_hcf': '641236', 'b_hcb': '463214',
           'chg_bf': '[4]6', 'chg_du': '[2]8'}
TSV_CHAR = game.CFG['moves']['tsv_char']
OROCHI_ALT = game.CFG['moves']['orochi_alt']
THROW_TABLE = game.hexval(game.CFG['naming']['throw_table'])


def load_all():
    """{char: [(kind, name, seq, close, air, buttons, cmd)]} including throws and command moves"""
    out = {}
    for line in open(TSV):
        if line.startswith('#') or not line.strip():
            continue
        char, kind, name, cmd = line.rstrip('\n').split('\t')
        if name.startswith('^!') or char not in CHAR:
            continue
        c = cmd.replace(' ', '')
        close = c.startswith('close')
        c = c[5:] if close else c
        air = '_^' in c or '(_^)' in c
        c = c.replace('(_^)', '').replace('_^', '')
        if kind == 'throw':
            m = re.match(r'^_4/_6_\+((?:_[A-D])+)', c)
            if m:
                out.setdefault(CHAR[char], []).append((kind, name, 'throw', True, air, set(re.findall(r'[A-D]', m.group(1))), cmd))
            continue
        m = re.match(r'^(\^)?((?:_?[1-9])+)_\+((?:_[A-DPK])+)', c)
        if m:
            digits = re.sub(r'[^1-9]', '', m.group(2))
            seq = '[%s]%s' % (digits[0], digits[1:]) if m.group(1) else digits
            out.setdefault(CHAR[char], []).append((kind, name, seq, close, air, set(re.findall(r'[A-DPK]', m.group(3))), cmd))
        elif re.match(r'^\^\*_[PAC]', c):
            out.setdefault(CHAR[char], []).append((kind, name, 'mash', close, air, {'P', 'A', 'C'}, cmd))
    return out


def command_names(p1):
    """[(addr, label, comment)] from kof97_commands.tsv x kof97_moves.tsv"""
    moves = load_all()
    res = {}
    for line in open(COMMANDS):
        if line.startswith('#') or line.startswith('char_id'):
            continue
        f = line.rstrip('\n').split('\t')
        if len(f) < 9:
            continue
        cid, char, routine, slot, motion, buttons, target, result, cond = f[:9]
        conds = cond.split()
        if 'cpu' in conds:
            continue
        char = TSV_CHAR.get(char, char)
        if 'alt1d6' in conds:
            if char not in OROCHI_ALT:
                continue
            char = OROCHI_ALT[char]
        air = 'air' in conds
        throw = re.search(r'idx([0-9a-f]+)', target)
        if throw:
            i = int(throw.group(1), 16)
            addr = int.from_bytes(p1[THROW_TABLE + 4 * i:THROW_TABLE + 4 * i + 4], 'big')
        elif re.match(r'^[0-9a-f]{6}$', target):
            addr = int(target, 16)
        else:
            continue
        if motion.startswith('throw'):
            seq, kinds = 'throw', ('throw',)
        elif motion == 'mash_AC':
            seq, kinds = 'mash', ('special',)
        elif motion in CMD_SEQ:
            seq, kinds = CMD_SEQ[motion], (('special', 'dm') if result == '1' else ('command',))
        else:
            m = re.match(r'^(u|d|f|b|df|db|uf|ub)\+([A-D])$', motion)
            if not m:
                continue
            seq = {'u': '8', 'd': '2', 'f': '6', 'b': '4', 'df': '3', 'db': '1', 'uf': '9', 'ub': '7'}[m.group(1)]
            buttons, kinds = m.group(2), ('command',)
        group = set(buttons.replace('-', '')) or {'A', 'B', 'C', 'D'}
        if group & {'A', 'C'}:
            group |= {'P'}
        if group & {'B', 'D'}:
            group |= {'K'}
        cands = [mv for mv in moves.get(char, []) if mv[0] in kinds and mv[2] == seq and mv[4] == air and mv[5] & group]
        if len(cands) > 1:      # prefer the exact button
            exact = [mv for mv in cands if mv[5] & set(buttons)]
            cands = exact or cands
        names = sorted({mv[1] for mv in cands})
        if len(names) != 1:
            continue
        mv = [c for c in cands if c[1] == names[0]][0]
        res.setdefault(addr, set()).add((char, names[0], mv[6], motion + ('+' + buttons if buttons != '-' else '')))
    out, used = [], {}
    for addr in sorted(res):
        entries = sorted(res[addr])
        if len({(c, n) for c, n, _, _ in entries}) != 1:
            continue                # one routine reached by two different moves: leave it
        char, name, cmd, motion = entries[0]
        label = 'chr_%s_%s' % (char, slug(name))
        used.setdefault(label, []).append(addr)
        out.append([addr, label, '%s (%s) [%s]' % (name, cmd, ', '.join(sorted({e[3] for e in entries})))])
    for row in out:
        if len(used[row[1]]) > 1:
            row[1] = '%s_%06x' % (row[1], row[0])
    return out


def write_moves(p1):
    rows = command_names(p1)
    lines = ['# Generated by tools/disasm/movenames.py - do not edit. Official move names (kof97_moves.tsv, MAME',
             '# command.dat) for the next_state / throw handler of each decoded command (kof97_commands.tsv).',
             '# mkasm.py applies them over chr_ names (curated or generated); other curated names win.']
    lines += ['%06x  %s  | %s' % (a, n, c) for a, n, c in rows]
    open(MOVES_OUT, 'w').write('\n'.join(lines) + '\n')
    return len(rows)


def main():
    p1 = open(os.path.join(ROOT, 'build/roms/p1.be'), 'rb').read()
    print('symbols/moves.txt: %d official move names' % write_moves(p1))
    moves = load()
    lines = open(SPECIALS).read().split('\n')
    out, n = filter_lines(lines, moves)
    print('%d of %d command labels get an official move name' % (
        n, sum(1 for l in lines if LABEL.match(l))))
    if '--apply' in sys.argv:
        open(SPECIALS, 'w').write('\n'.join(out))


if __name__ == '__main__':
    sys.exit(main())

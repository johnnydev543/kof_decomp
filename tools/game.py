#!/usr/bin/env python3
"""game.py - per-game data for the tools: games/<GAME>/ (GAME environment variable, default kof97).

games/<GAME>/game.json holds everything that differs between Neo Geo games: ROM names and CRCs, the
VBlank breakpoint and insertion points of the shift test, the A5 base for RAM hints, code addresses
the disassembler rules need (spawn functions, handler fields), the object dispatcher used by
whowrote.py, the RAM addresses the coverage scripts poke and the character list. The same directory
holds the names (symbols/), the coverage scripts (scripts/), the move list and decoded commands, and
the REA seeds. See docs/NEW_ROM_GUIDE.md.
"""
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
NAME = os.environ.get('GAME', 'kof97')
DIR = os.path.join(ROOT, 'games', NAME)
PATH = os.path.join(DIR, 'game.json')
CFG = json.load(open(PATH))


def hexval(v):
    """'9eb0' -> 0x9eb0, None -> None"""
    return None if v is None else int(v, 16)


def rom_crcs():
    """{filename: crc32} for every game ROM file"""
    r = CFG['roms']
    files = [r['p1'], r['s1'], r['m1']] + ([r['p2']] if r.get('p2') else []) + list(r['v'])
    for pair in r['c']:
        files += pair
    return {f: int(c, 16) for f, c in files}


def ram(key):
    return hexval(CFG['ram'][key])


def path(*parts):
    """a file of this game: path('symbols', 'curated.txt') -> games/<GAME>/symbols/curated.txt"""
    return os.path.join(DIR, *parts)


def symbols(kind='curated'):
    """games/<GAME>/symbols/<kind>.txt (curated, chars, actions, specials, derived, moves)"""
    return path('symbols', kind + '.txt')


def addrs(key, section='disasm'):
    """list of hex strings in game.json -> ints"""
    return [hexval(v) for v in CFG.get(section, {}).get(key, [])]

#!/usr/bin/env python3
"""
mkasm.py - pilot: P1 program ROM -> GNU as (m68k-elf-as) source that reassembles byte-identically.

Code map = the static recompiler's discovery (same seeds: vectors, header, REA index, extra seeds,
runtime misses, pointer scan). Instruction text = Musashi disassembly, rewritten to GAS syntax.
Every decoded instruction gets a label L_xxxxxx; branch / PC-relative / absolute-long targets that
land on a label are emitted symbolically. Everything else is data (.byte).

Instructions that GAS would encode differently (it optimises: addq for add #imm, moveq for move.l #,
drops zero displacements, ...) are found by assembling and comparing, and are then emitted as raw
.word with the Musashi text as a comment. The loop repeats until the image matches; the report lists
how many instructions needed the raw fallback and why (= the rules a real disassembler still needs).

usage: mkasm.py [--out build/disasm]
"""
import argparse
import os
import re
import subprocess
import sys
from collections import Counter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'recomp'))
import m68k_recomp as rc  # noqa: E402

def _binutils_prefix():
    """m68k binutils prefix: $M68K_PREFIX, else m68k-elf- (Homebrew) or m68k-linux-gnu- (Debian/Ubuntu)."""
    import shutil
    if os.environ.get('M68K_PREFIX'):
        return os.environ['M68K_PREFIX']
    for pre in ('m68k-elf-', 'm68k-linux-gnu-'):
        if shutil.which(pre + 'as'):
            return pre
    return 'm68k-elf-'


_PRE = _binutils_prefix()
AS = os.environ.get('AS_M68K', _PRE + 'as')
LD = os.environ.get('LD_M68K', _PRE + 'ld')
OBJCOPY = os.environ.get('OBJCOPY_M68K', _PRE + 'objcopy')
NM = os.environ.get('NM_M68K', _PRE + 'nm')


EXEC_MAP = os.environ.get('MKASM_EXEC_MAP', os.path.join(ROOT, 'build/disasm/exec_p1.bin'))
VECTOR_END = 0x100
VECTOR_NAMES = (['initial SSP', 'reset PC', 'bus error', 'address error', 'illegal instruction', 'zero divide',
                 'CHK', 'TRAPV', 'privilege violation', 'trace', 'line A', 'line F'] +
                ['reserved'] * 3 + ['uninitialized interrupt'] + ['reserved'] * 8 + ['spurious interrupt'] +
                ['level %d autovector%s' % (i, {1: ' (VBlank)', 2: ' (LSPC timer)', 3: ' (cold boot)'}.get(i, ''))
                 for i in range(1, 8)] +
                ['TRAP #%d' % i for i in range(16)] + ['reserved'] * 16)

EXECUTED = set()     # decoded instructions that were executed (--exec-log)
DATA_READ = set()    # P1 halfwords read as data, instruction fetches excluded (--exec-log)
PTR_SRC = set()      # P1 longwords whose value was the target of an indirect jsr/jmp (--exec-log)
RD_LONG = set()      # P1 addresses where a 32-bit read started (--exec-log)
RD_WORD = set()      # P1 halfwords touched by 16/8-bit reads (--exec-log)
DYN_ENTRIES = set()  # executed addresses entered by a jump (from the --exec-log map)
ENTRIES = set()   # vectors, header, REA functions, extra seeds, runtime misses
TRUSTED = set()   # instructions reachable from ENTRIES


OBJ_FIELDS = {}     # offset -> name, from '@field' lines (annotates (d16,%a4) operands)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/game.json)
A5_BASE = game.hexval(game.CFG['disasm'].get('a5_base'))   # game convention for (d16,A5) RAM hints
SYMBOLS_FILE = os.environ.get('MKASM_SYMBOLS', game.symbols('curated'))


def load_symbols(path=SYMBOLS_FILE):
    """games/<GAME>/symbols/curated.txt: 'ADDR NAME [| comment]'; an indented '| text' line continues the
    previous comment. Returns {addr: (name, [comment lines])}."""
    syms, last = {}, None
    if not os.path.exists(path):
        return syms
    names = set()
    for ln in open(path):
        if ln.startswith('#') or not ln.strip():
            continue
        if ln.startswith('@field'):
            f = ln.split('|')[0].split()
            OBJ_FIELDS[int(f[1], 16)] = f[2]
            continue
        if ln[0].isspace() and ln.strip().startswith('|') and last is not None:
            syms[last][1].append(ln.strip()[1:].strip())
            continue
        body, _, comment = ln.partition('|')
        f = body.split()
        a, name = int(f[0], 16), f[1]
        if not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', name) or name in names:
            sys.exit('symbols.txt: bad or duplicate name %r' % name)
        names.add(name)
        syms[a] = (name, [comment.strip()] if comment.strip() else [])
        last = a
    return syms


# curated names win over generated ones (tools/disasm/charmap.py -> symbols/chars.txt)
SYMS = load_symbols(game.symbols('chars'))
SYMS.update(load_symbols(game.symbols('actions')))   # actionmap.py refinements
SYMS.update(load_symbols(game.symbols('specials')))  # specmap.py refinements
SYMS.update(load_symbols(game.symbols('derived')))   # derivemap.py (owner-derived)
SYMS.update(load_symbols())
# official move names (movenames.py) replace chr_ names, curated or generated; other curated names win
for _a, (_n, _c) in load_symbols(game.symbols('moves')).items():
    if _a not in SYMS or SYMS[_a][0].startswith('chr_') or re.search(r'_(state|obj|sub)_[0-9a-f]{6}$', SYMS[_a][0]):
        SYMS[_a] = (_n, _c + (SYMS[_a][1] if _a in SYMS else []))


def apply_symbols(lines, p1_size):
    """Stage 3: name labels (original L_/D_ label kept as an alias), absolute operands and A5 variables.
    Purely textual: the assembled bytes do not change."""
    rom = {a: v for a, v in SYMS.items() if a < p1_size}
    equ = {a: v for a, v in SYMS.items() if a >= p1_size}
    lab = re.compile(r'\b([LD])_([0-9a-f]{6})\b')

    def ren(m):
        a = int(m.group(2), 16)
        return rom[a][0] if a in rom else m.group(0)

    def absl(m):
        a = int(m.group(1), 16)
        return '(%s).l' % equ[a][0] if a in equ else m.group(0)

    def a5hint(text):
        hints = []
        if A5_BASE is None:
            return ''
        for neg, d in re.findall(r'\((-?)\$([0-9a-f]+),A5\)', text):
            a = A5_BASE + (-int(d, 16) if neg else int(d, 16))
            if a in equ:
                hints.append(equ[a][0])
        return '  [%s]' % ' '.join(hints) if hints else ''

    out = ['| %s P1 program ROM - generated by tools/disasm/mkasm.py (names: games/%s/symbols/)' % (game.CFG.get('title', game.NAME), game.NAME)]
    for a in sorted(equ):
        name, com = equ[a]
        out.append('\t.equ %s, 0x%x%s' % (name, a, ('  | ' + com[0]) if com else ''))
    for ln in lines:
        m = re.match(r'^([LD])_([0-9a-f]{6}):$', ln)
        if m and int(m.group(2), 16) in rom:
            name, com = rom[int(m.group(2), 16)]
            out.append('')
            out.extend('| ' + c for c in com)
            out.append(ln)
            out.append(name + ':')
            continue
        code, sep, comment = ln.partition('  | ')
        if code.startswith('\t'):
            code = lab.sub(ren, code)
            code = re.sub(r'\(0x([0-9a-f]+)\)\.l', absl, code)
            if sep:
                hints = [('obj.' + OBJ_FIELDS[int(d, 16)]) for neg, d in re.findall(r'\((-?)\$([0-9a-f]+),A4\)', comment)
                         if not neg and int(d, 16) in OBJ_FIELDS]
                comment = comment + a5hint(comment) + ('  [%s]' % ' '.join(hints) if hints else '')
        out.append(code + sep + comment)
    return out


_rc_decode = rc.decode


def _decode_addr_error_stops(img, a, valid):
    """A word/long access to an odd absolute address (move.w $1.w,$1.w) raises an address error on the
    68000 and never falls through, so the bytes after it are not code."""
    d = _rc_decode(img, a, valid)
    if d is not None and d.size in (16, 32):
        eas = [d.p[k] for k in ('src', 'dst', 'ea') if k in d.p]
        if any(e.m in ('AW', 'AL') and e.val & 1 for e in eas):
            d.flow = 'exit'
    return d


rc.decode = _decode_addr_error_stops


def unrolled_bases(p1, executed):
    """Bases of unrolled loops entered by a computed jump: jmp (d8,PC,Xn) to base + (N - count) * size.
    The entries before the first executed one only run for counts larger than any test run used, so
    nothing marks them as code and their bytes can look like pointers (0xa512: move.w D0,(2,A1) =
    3340 0002 read as .long L_023340, which breaks when code moves). The base is code when the bytes
    from it to the first executed instruction repeat that instruction."""
    bases = set()
    for a in executed:
        if a + 4 > len(p1) or p1[a:a + 2] != b'\x4e\xfb' or a in bases:
            continue
        d8 = p1[a + 3] - 256 if p1[a + 3] >= 128 else p1[a + 3]
        t = a + 2 + d8
        e = next((x for x in range(t, t + 0x400, 2) if x in executed), None)
        if e is None or e == t:
            continue
        for size in (2, 4, 6):
            unit = p1[e:e + size]
            if (e - t) % size == 0 and p1[t:e] == unit * ((e - t) // size) and p1[e + size:e + 2 * size] == unit:
                bases.add(t)
                break
    return bases


WREL = {}          # P1 offset -> (table base, target): word-offset jump table entries (.word L_t-D_base)


def word_tables(insns, p1):
    """Word-offset jump tables: lea table,An, then a word read (An,Dx) that becomes the jump address
    (adda.w (An,Dx),An / move.w (An,Dx),Dm + adda Dm,An / add.l An,Dm + movea.l Dm,Ak / jmp (An,Dm)), then
    jsr/jmp. Each entry is table + word = handler (the CPU AI opcode table 0x2e3e2 has 256). Emitted raw,
    the words keep their old distance when code moves between the table and its handlers, and the
    handlers that no test run reaches stay .byte. Returns {entry offset: (table, target)}."""
    out = {}
    for a in sorted(insns):
        d = insns[a]
        if d.kind != 'lea' or d.p['ea'].m not in ('PCDI', 'AL', 'AW'):
            continue
        an, tab = d.p['an'], d.p['ea'].val & 0xffffff
        if tab in out.values() or tab & 1 or not VECTOR_END <= tab < len(p1):
            continue
        b, dm, ok = d.next, None, False
        for _ in range(24):
            e = insns.get(b)
            if e is None:
                break
            src = e.p.get('src') or e.p.get('ea')
            if dm is None and isinstance(src, rc.EA) and src.m == 'IX' and src.r == an and e.size == 16:
                if e.kind in ('adda', 'adda_w', 'add_a') or (e.kind.startswith('adda') and e.p.get('an') == an):
                    dm = 'A'
                else:
                    dst = e.p.get('dst') or e.p.get('dn')
                    dm = ('D', dst.r) if isinstance(dst, rc.EA) and dst.m == 'D' else ('D', e.p.get('dn'))
            elif dm and e.kind in ('jsr', 'jmp') and e.p['ea'].m in ('AI', 'IX'):
                ok = True
                if e.p['ea'].m == 'IX':
                    dm = 'A'       # jmp (An,Dm.w): base + offset
                break
            elif dm and dm != 'A' and (e.kind.startswith('add') or (e.kind == 'lea' and e.p['ea'].m == 'IX')):
                dm = 'A'           # the offset is combined with the base before the jump
            if e.kind == 'bra' and e.targets:
                b = e.targets[0]   # the table is loaded by the caller, the dispatch is shared (0x2d2ae)
                continue
            if e.flow not in ('seq', 'cjump'):
                break
            b = e.next
        if not ok or dm != 'A':
            continue
        ents, lo = {}, None
        for k in range(256):
            pos = tab + 2 * k
            if lo is not None and pos >= lo:
                break
            w = int.from_bytes(p1[pos:pos + 2], 'big')
            t = tab + (w - 0x10000 if w & 0x8000 else w)
            if t & 1 or not VECTOR_END <= t < len(p1) or t == tab:
                break
            w0 = int.from_bytes(p1[t:t + 2], 'big')
            if w0 >> 12 in (0xa, 0xf) or w0 == 0:
                break
            ents[pos] = (tab, t)
            if t > tab:
                lo = t if lo is None else min(lo, t)
        if len(ents) >= 2:
            out.update(ents)
    return out


SPAWN_FUNCS = tuple(game.addrs('spawn_funcs'))   # obj_spawn, obj_spawn_child, obj_init_static: A0 = handler
# (address register, displacement) of fields that hold code addresses: (A4) object handler, SCENE_HANDLER
# (0x500,A5), player next_state +0x198, callbacks +0x19c/+0x1a0 (also set on another object via A0/A1),
# +0x134, +0x8c, +0x36, +0x1e8 (saved/next handlers). Measured on p1.s: 99% of the labels stored there are code.
HANDLER_FIELDS = {(r, int(d, 16)) for r, d in game.CFG['disasm']['handler_fields']}


def _writes_a0(d):
    if d.kind in ('lea', 'movea') and d.p.get('an') == 0:
        return True
    return any(isinstance(e, rc.EA) and e.r == 0 and (e.m in ('PI', 'PD') or (k == 'dst' and e.m == 'A'))
               for k, e in d.p.items())


def _a0_reaches(insns, b, funcs, steps=24):
    """follow the fall-through path from b while A0 is unchanged: 'spawn' when it calls one of funcs,
    'jump' at jsr/jmp (A0), 'table' at movea.l (A0,Xn),A0 followed by jsr/jmp (A0), else None"""
    table = False
    for _ in range(steps):
        e = insns.get(b)
        if e is None:
            return None
        if e.kind in ('bsr', 'jsr') and e.targets and e.targets[0] in funcs and not table:
            return 'spawn'
        if e.kind in ('jsr', 'jmp') and e.p['ea'].m == 'AI' and e.p['ea'].r == 0:
            return 'table' if table else 'jump'
        if not table and e.kind == 'movea' and e.p['an'] == 0 and e.size == 32 and e.p['src'].m == 'IX' \
                and e.p['src'].r == 0:
            table = True
        elif e.flow not in ('seq', 'cjump') or _writes_a0(e):
            return None
        b = e.next
    return None


def _table_entries(p1, tab):
    out = set()
    for k in range(512):
        v = int.from_bytes(p1[tab + 4 * k:tab + 4 * k + 4], 'big')
        if v & 1 or not VECTOR_END <= v < len(p1):
            break
        w = int.from_bytes(p1[v:v + 2], 'big')
        if w >> 12 in (0xa, 0xf) or w == 0:   # the longword after the table (0x2d8f0 + 11: 0x00000404 -> ffff)
            break
        out.add(v)
    return out


def spawn_handler_seeds(insns, p1=None):
    """Code addresses the game is certain to execute as code even when no test run did:
    - move.l #imm into a handler field (HANDLER_FIELDS)
    - A0 loaded with lea abs/(d,PC),A0 or movea.l #imm,A0 and then, A0 unchanged, passed to a spawn
      function (obj_spawn / obj_spawn_child / obj_init_static, and every function that passes its A0 on
      to one of them unchanged, found here) or called with jsr/jmp (A0)
    - jump tables: movea.l (table,PC,Xn),A0 or lea table,A0 + movea.l (A0,Xn),A0, then jsr/jmp (A0)
    Left as data, their absolute operands (move.l #next_state,(A4), jsr abs.l) and branches are not
    relocated (0x6ea68, a win-screen portrait handler, held move.l #$6eb12,(A4) as raw bytes)."""
    funcs = set(SPAWN_FUNCS)
    calls = sorted({t for d in insns.values() if d.flow == 'call' for t in d.targets if t in insns})
    while True:                    # wrappers that pass A0 on to a spawn function
        more = {f for f in calls if f not in funcs and _a0_reaches(insns, f, funcs, 16) == 'spawn'}
        if not more:
            break
        funcs |= more
    found = set()
    for a in sorted(insns):
        d = insns[a]
        t = None
        if d.kind == 'lea' and d.p['an'] == 0 and d.p['ea'].m in ('AL', 'AW', 'PCDI'):
            t = d.p['ea'].val & 0xffffff
        elif d.kind == 'movea' and d.p['an'] == 0 and d.p['src'].m == 'IMM' and d.size == 32:
            t = d.p['src'].val & 0xffffff
        elif d.kind == 'movea' and d.p['an'] == 0 and d.p['src'].m == 'PCIX' and d.size == 32 and p1 is not None:
            if _a0_reaches(insns, d.next, ()) == 'jump':
                found |= _table_entries(p1, d.p['src'].val & 0xffffff)
            continue
        else:
            st = d.p.get('dst')
            if d.kind == 'move' and d.size == 32 and d.p['src'].m == 'IMM' and st is not None and \
                    (st.r, 0 if st.m == 'AI' else st.disp if st.m == 'DI' else None) in HANDLER_FIELDS and \
                    d.p['src'].val & 0xff:   # round values (#$20000 / #$e000 into +0x36 / +0x8c of the NEXT STAGE globe) are parameters
                found.add(d.p['src'].val & 0xffffff)
            continue
        how = _a0_reaches(insns, d.next, funcs)
        if how in ('spawn', 'jump'):
            found.add(t)
        elif how == 'table' and p1 is not None:
            found |= _table_entries(p1, t)
    return found


def collect_insns(p1, bios):
    img = rc.Image(p1, bios)
    valid = [int(l.split()[0]) for l in open(os.path.join(ROOT, 'build/m68k_table.txt'))]
    seeds = set()
    for i in range(1, 64):
        for data in (p1, bios):
            v = int.from_bytes(data[i * 4:i * 4 + 4], 'big') & 0xffffff
            if img.in_code(v) and not v & 1 and v >= 0x100:
                seeds.add(v)
    seeds.update(rc.P1_NAMES)
    a = 0xC00402
    while img.w16(a) == 0x4EF9:
        seeds.add(a)
        a += 6
    for path, ok in ((os.path.join(ROOT, 'build/rea/decomp_p1/decompile-index.json'), lambda x: x < len(p1)),
                     (os.path.join(ROOT, 'build/rea/decomp_bios/decompile-index.json'), lambda x: x >= rc.BIOS_BASE)):
        s, r, _ = rc.load_rea_index(path, ok)
        seeds.update(s)
        seeds.update(lo for (_, lo, _) in r)
    for path in (os.path.join(ROOT, 'tools/recomp/extra_seeds.txt'), os.path.join(ROOT, 'build/recomp/runtime_misses.txt')):
        if os.path.exists(path):
            for line in open(path):
                line = line.split('#')[0].strip()
                if line:
                    seeds.add(int(line.split()[0], 16))
    # addresses executed in interpreter runs (kof97 --interp --exec-log, merged by shifttest.py):
    # ground truth that they are instruction starts, e.g. handlers reached through lea (d,PC) or
    # computed addresses that static analysis treats as data
    # (they are instruction starts, not function entries: they join the trusted discovery seeds only)
    seeds = {x for x in seeds if img.in_code(x) and not x & 1}
    ENTRIES.update(seeds)          # known entry points, before the (data-driven) pointer scan
    executed = set()
    if os.path.exists(EXEC_MAP):
        em = open(EXEC_MAP, 'rb').read()

        def bits(m):
            return {i * 2 for i in range(len(m) * 8) if m[i >> 3] >> (i & 7) & 1}
        executed = bits(em[:0x10000])
        RD_LONG.update(bits(em[0x10000:0x20000]))
        RD_WORD.update(bits(em[0x20000:0x30000]))
        PTR_SRC.update(bits(em[0x30000:0x40000]))
    seeds |= {x for x in executed if img.in_code(x)}
    seeds |= unrolled_bases(p1, executed)
    base, _ = rc.discover(img, valid, sorted(seeds))
    while True:                    # object handlers spawned by trusted code (they may spawn more)
        found = {t for t in spawn_handler_seeds(base, p1) if img.in_code(t) and not t & 1}
        WREL.update(word_tables(base, p1))
        found |= {t for _, t in WREL.values()}
        ENTRIES.update(found)      # code entries: move.l #imm into a handler field is symbolized
        more = found - seeds
        if not more:
            break
        seeds |= more
        base, _ = rc.discover(img, valid, sorted(seeds))
    TRUSTED.update(base)           # reached from known entries without the pointer-scan heuristic
    seeds |= rc.scan_pointer_seeds(img, valid, set(base))
    insns, _ = rc.discover(img, valid, sorted(seeds))
    while True:                    # the same for code found through pointer tables
        found = {t for t in spawn_handler_seeds(insns, p1) if img.in_code(t) and not t & 1}
        ENTRIES.update(found)
        more = found - seeds
        if not more:
            break
        seeds |= more
        insns, _ = rc.discover(img, valid, sorted(seeds))
    # word-offset jump tables are data: drop instructions decoded over them
    for k in WREL:
        for a in (k - 4, k - 2, k):
            if a in insns and a + insns[a].length > k and a not in executed:
                del insns[a]
    # executed addresses that no executed instruction falls through to were entered by a jump:
    # indirect-jump / pointer-table targets (state machine steps, move handlers) = code entries
    EXECUTED.update(a for a in executed if a in insns)
    # data reads = 16/32-bit reads minus instruction fetches (bytes of executed instructions)
    fetched = set()
    for a in EXECUTED:
        fetched.update(range(a, insns[a].next, 2))
    DATA_READ.update((RD_WORD | RD_LONG | {k + 2 for k in RD_LONG}) - fetched)
    falls_into = {d.next for a, d in insns.items() if a in executed and d.flow in ('seq', 'cjump', 'call')}
    DYN_ENTRIES.update(v for v in executed if v in insns and v not in falls_into)
    TRUSTED.difference_update(range(0, VECTOR_END))
    # 0x000-0x0ff is the 68000 vector table (REA reports a function "entry" at 0): data, not code
    return {a: d for a, d in insns.items() if VECTOR_END <= a < len(p1)}


def musashi_text(addrs):
    out = subprocess.run([os.path.join(ROOT, 'build/m68kinfo'), 'disasm', os.path.join(ROOT, 'build/roms/p1.be'),
                          os.path.join(ROOT, 'build/roms/bios.be')],
                         input=''.join('%x\n' % a for a in addrs), capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        a, ln, text = line.split('\t', 2)
        res[int(a, 16)] = (int(ln), text.strip())
    return res


REG = re.compile(r'\b([DA][0-7]|PC|SR|CCR|USP)\b')
BRANCH = re.compile(r'^(b(?:ra|sr|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le))\s+\$([0-9a-f]+)$')
IMM_EXT_OPS = {'ori', 'andi', 'subi', 'addi', 'eori', 'cmpi', 'move', 'or', 'and', 'sub', 'add', 'cmp', 'eor'}
DBCC = re.compile(r'^(db(?:ra|t|f|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le))\s+(D[0-7]),\s*\$([0-9a-f]+)$')


HEADER_PTRS = (0x116, 0x11A, 0x11E, 0x182)
TABLE_RULE = os.environ.get('MKASM_TABLES', '1') == '1'


def op_size_l(mn):
    return mn.endswith('.l')


def pointer_refs(p1, order, texts, labels):
    """Addresses in P1 that the code refers to as pointers (abs.l/abs.w operands, PC-relative
    operands, long immediates), plus data longwords whose value is a code label (pointer tables)."""
    refs = set()
    end = set()
    for a in order:
        ln, text = texts[a]
        t = text.split(';')[0]
        mn = t.split(None, 1)[0]
        for v in re.findall(r'(?<![#\w(])\$([0-9a-f]+)\.[wl]\b', t):
            v = int(v, 16)
            if 0x100 <= v < len(p1):
                refs.add(v)
        for neg, d in re.findall(r'\((-?)\$([0-9a-f]+),PC', t):
            v = a + 2 - int(d, 16) if neg else a + 2 + int(d, 16)
            if 0 <= v < len(p1):
                refs.add(v)
        if op_size_l(mn):
            for v in re.findall(r'#\$([0-9a-f]+)\b', t):
                v = int(v, 16)
                if 0x200 <= v < len(p1) and v & 0xff:
                    refs.add(v)
        end.add(a + ln)
    return refs


class Conv:
    def __init__(self, labels, p1, dlabels=(), symbolize=False, entries=(), sentries=()):
        self.labels = labels
        self.entries = entries
        self.sentries = sentries     # static entries only (no --exec-log dynamic entries)
        self.dlabels = dlabels
        self.p1 = p1
        self.symbolize = symbolize

    def lab(self, t):
        return 'L_%06x' % t if t in self.labels else None

    def anylab(self, t):
        if t in self.labels:
            return 'L_%06x' % t
        if self.symbolize and t in self.dlabels:
            return 'D_%06x' % t
        return None

    def operand_syms(self, s, addr, op, mn=''):
        s_mn = [mn]
        # PC-relative "(d,PC)" / "(d,PC,Xn.s)": target = ext-word address + d
        def pcrel(m):
            neg, d, rest = m.group(1), int(m.group(2), 16), m.group(3) or ''
            t = (addr + 2 - d if neg else addr + 2 + d) & 0xffffff
            # the extension word is at addr+2 only when the PC-relative operand is the first one
            l = self.anylab(t)
            return '(%s,PC%s)' % (l, rest) if l else m.group(0)
        s = re.sub(r'\((-?)\$([0-9a-f]+),PC((?:,[DA][0-7]\.[wl])?)\)', lambda m: pcrel(m), s)
        # absolute long to a labelled P1 address
        def absl(m):
            l = self.anylab(int(m.group(1), 16))
            return '(%s).l' % l if l else '(0x%s).l' % m.group(1)
        s = re.sub(r'(?<![#\w(])\$([0-9a-f]+)\.l\b', absl, s)

        def absw(m):
            l = self.anylab(int(m.group(1), 16)) if self.symbolize else None
            return '(%s).w' % l if l else '(0x%s).w' % m.group(1)
        s = re.sub(r'(?<![#\w(])\$([0-9a-f]+)\.w\b', absw, s)
        if self.symbolize and op_size_l(s_mn[0]):
            def imm(m):
                # a constant that equals some instruction address (#$fffe) is not a pointer unless
                # that address is a function entry or nearby code (state machines store the address
                # of their next step: move.l #next, (A4))
                v = int(m.group(1), 16)
                ok = v in self.entries or v not in self.labels or abs(v - addr) < 0x1000
                # an instruction label right next to the instruction (move.l #next_step,(A4) with
                # next_step = the following instruction, e.g. #$5800 at 0x57fa) is a code address even
                # when it looks round
                near = v in self.labels and abs(v - addr) < 0x200
                # observed: the immediate (fetched as a longword at addr+2) was later jumped to
                proven = (addr + 2) in PTR_SRC and v in self.labels
                if ((v & 0xff) == 0 and v not in self.sentries or (v & 0xffff) == 0) and not near and not proven:
                    ok = False      # round constant (#$a0000 = fixed-point velocity), not an address
                ok = ok or proven
                l = self.anylab(v) if ok else None
                return '#' + l if l else m.group(0)
            s = re.sub(r'#\$([0-9a-f]+)\b', imm, s)
        return s

    def convert(self, addr, op, text):
        text = text.split(';')[0].strip()
        m = BRANCH.match(text)
        if m:
            t = int(m.group(2), 16)
            l = self.lab(t)
            if not l:
                return None, 'branch target not a label'
            size = '.w' if (op & 0xff) == 0 else '.s'
            return '%s%s %s' % (m.group(1), size, l), None
        m = DBCC.match(text)
        if m:
            l = self.lab(int(m.group(3), 16))
            if not l:
                return None, 'branch target not a label'
            mn = 'dbf' if m.group(1) == 'dbra' else m.group(1)
            return '%s %s,%s' % (mn, m.group(2), l), None
        if text.startswith('dc.w'):
            return None, 'dc.w (line-A/F)'
        if ',PC' in text and ',PC)' in text and text.count('(') > 1:
            pass
        parts = text.split(None, 1)
        mn, ops = parts[0], (parts[1] if len(parts) > 1 else '')
        if mn.endswith('.b') and ops.startswith('#') and mn[:-2] in IMM_EXT_OPS:
            ext = self.p1[addr + 2] << 8 | self.p1[addr + 3]
            if ext >> 8:
                return None, 'byte immediate with non-zero high byte (data?)'
            ops = re.sub(r'^#-?\$[0-9a-f]+', '#$%x' % (ext & 0xff), ops)
        if mn in ('btst', 'bset', 'bclr', 'bchg') and ops.startswith('#'):
            ext = self.p1[addr + 2] << 8 | self.p1[addr + 3]
            if ext >> 8:
                return None, 'bit number with garbage high bits (data?)'
        if '(' in ops and ',PC' in ops:
            # a PC-relative operand that is not the first extension word cannot be resolved by the
            # simple addr+2 rule; leave it to the raw fallback via mismatch check
            pass
        ops = self.operand_syms(ops, addr, op, mn)
        ops = re.sub(r'(-?)\$([0-9a-f]+)', lambda m: '%s0x%s' % (m.group(1), m.group(2)), ops)
        ops = REG.sub(lambda m: '%' + m.group(1).lower(), ops)
        ops = re.sub(r'%([da][0-7])\.([wl])', r'%\1.\2', ops)
        ops = re.sub(r',\s+', ',', ops)
        if mn == 'move' and '%sr' in ops or mn == 'move' and '%ccr' in ops or mn == 'move' and '%usp' in ops:
            mn = 'move.l' if '%usp' in ops else 'move.w'
        return ('%s %s' % (mn, ops)).strip(), None


def non_overlapping(insns, texts):
    """Drop instructions that overlap an earlier one. Rank: reachable from known entries (2) >
    branch/call target (1) > found only via the pointer scan (0); the higher rank wins."""
    targets = {t for d in insns.values() for t in d.targets}

    def rank(a):
        return 2 if a in TRUSTED else 1 if a in targets else 0
    keep = []
    for a in sorted(insns):
        r = rank(a)
        while keep and a < keep[-1] + texts[keep[-1]][0] and rank(keep[-1]) < r:
            keep.pop()
        if keep and a < keep[-1] + texts[keep[-1]][0]:
            continue
        keep.append(a)
    return keep


DATA_EVIDENCE = ('(data?)', 'branch target not a label', 'dc.w (line-A/F)')


def drop_data_runs(p1, insns, texts, order):
    """Untrusted (pointer-scan only) instruction runs that contain something real code never has
    (byte immediate with a non-zero high byte, a branch into the middle of nothing / an odd address,
    a line-A/F word) are data: drop the whole fall-through run. Repeated until nothing changes, because
    dropping a run turns branches into it into new evidence (word tables such as 2c48 6070 ...)."""
    keep = set(order)
    while True:
        cur = [a for a in order if a in keep]
        cv = Conv(set(cur), p1)
        prev = {x + texts[x][0]: x for x in cur}
        dropped = set()
        for a in cur:
            if a in dropped or a in EXECUTED:
                continue
            # observed: never executed, but its bytes are read as data (word tables such as 0068 0067
            # decoded as ori.w #$67,($66,A0)) -> data even if static analysis reached it
            read_as_data = any(h in DATA_READ for h in range(a, a + texts[a][0], 2))
            if a in TRUSTED and not read_as_data:
                continue
            _, why = cv.convert(a, insns[a].op, texts[a][1])
            if not (read_as_data or (why and why.endswith(DATA_EVIDENCE))):
                continue
            # walk back and forward over the untrusted fall-through chain; a data run read by the game
            # also swallows trusted instructions that are read as data, but stops at trusted code that
            # is not (a throw handler right after the motion table it uses: 0x35daa / 0x35dba)
            def stop(x):
                return x in EXECUTED or (x in TRUSTED and not (read_as_data and
                                                              any(h in DATA_READ for h in range(x, x + texts[x][0], 2))))
            run = [a]
            b = a
            while b in prev and not stop(prev[b]) and insns[prev[b]].flow not in ('jump', 'ret', 'exit'):
                b = prev[b]
                run.append(b)
            b = a
            while insns[b].flow not in ('jump', 'ret', 'exit') and b + texts[b][0] in keep and not stop(b + texts[b][0]):
                b = b + texts[b][0]
                run.append(b)
            dropped |= set(run)
        if not dropped:
            return cur
        keep -= dropped


def emit(p1, insns, texts, raw_force, path, symbolize=False, insert=None):
    order = drop_data_runs(p1, insns, texts, non_overlapping(insns, texts))
    labels = set(order)
    covered = bytearray(len(p1))
    for a in order:
        covered[a:a + texts[a][0]] = b'\1' * texts[a][0]
    dlabels, dptrs, entries, sentries = set(), {}, set(), set()
    if symbolize:
        dlabels = {v for v in pointer_refs(p1, order, texts, labels) if v not in labels and not covered[v]}
        # cartridge header pointers read by the BIOS: soft-DIP tables (JP/US/EU) and security code
        hdr = {k: int.from_bytes(p1[k:k + 4], 'big') for k in HEADER_PTRS}
        dlabels |= {v for v in hdr.values() if v not in labels}
        dlabels |= {tab for tab, _ in WREL.values() if tab not in labels}
        # a lone data longword is a code pointer only if it names a function entry (call/jump target,
        # REA function, vector/seed): any instruction start is too weak, word pairs such as
        # 0001 0002 or 0000 2000 often equal some instruction address by chance
        calls = {t for a in order for t in insns[a].targets if insns[a].flow in ('call', 'jump')}
        entries = (calls | ENTRIES | DYN_ENTRIES) & labels
        # round values (low byte 0: 0x8000, 0x30000) equal executed addresses by chance far too often
        # (fixed-point velocities); for them only static entries count as evidence
        sentries = (calls | ENTRIES) & labels
        def looks(k, strict=True):
            """data longword at k looks like a P1 pointer"""
            v = int.from_bytes(p1[k:k + 4], 'big')
            # odd targets only inside tables (pointers to byte strings)
            return (not any(covered[k:k + 4]) and 0x1000 <= v < len(p1) and (not strict or not v & 1)
                    # round values (0003 0000, 0000 2000) are usually data words; in strict mode
                    # they need to be a function entry
                    and (not strict or v & 0xff or v in sentries)
                    # word sequences (0007 0008, 0009 000a) and repeats (0004 0004) are index or
                    # parameter tables, not pointers
                    and ((v & 0xffff) - (v >> 16) not in (0, 1) or v in sentries)
                    and (v in labels or not covered[v]))
        for k in range(0, len(p1) - 3, 2):
            if not any(covered[k:k + 4]):
                v = int.from_bytes(p1[k:k + 4], 'big')
                if k in PTR_SRC and (v & 0xffffff) in labels:
                    dptrs[k] = v    # observed: read as a longword, then jumped to (jsr/jmp (An))
                    continue
                if (v & 0xff) == 0 and any(int.from_bytes(p1[j:j + 4], 'big') & 0xff == 0
                                           for j in (k - 4, k + 4) if 0 <= j <= len(p1) - 4):
                    continue    # inside a table of round constants (fixed-point values)
                if (v & 0xff) == 0:
                    continue    # round value on its own (0000 fa00 in a parameter block, 000a 0000):
                                # a constant far more often than a pointer
                # lone data longwords: static entries only (word pairs such as 0001 fffe in signed word
                # tables equal some executed address by chance)
                lo16 = v & 0xffff
                small_word = lo16 <= 0xff or lo16 >= 0xff00      # 0001 fffe = two small words
                if ((v in sentries and v >= 0x100) or (v in dlabels and looks(k))
                        or (v in DYN_ENTRIES and v >= 0x100 and not small_word and k in RD_LONG)):
                    dptrs[k] = v
        # flagged code pointers: top byte 0x80 + 24-bit code address (the 68000 ignores A24-A31), used
        # in the game's command streams (c01e 8007 9fc6 c01e 0000 ...): emitted as L_xxxxxx+0x80000000
        for k in range(0, len(p1) - 3, 2):
            v = int.from_bytes(p1[k:k + 4], 'big')
            v24 = v & 0xffffff
            if (v >> 24) == 0x80 and not any(covered[k:k + 4]) and v24 in labels and not v24 & 1 and v24 & 0xff and (
                    v24 in sentries or (v24 in DYN_ENTRIES and k in RD_LONG
                                        and not (v & 0xffff <= 0xff or v & 0xffff >= 0xff00))):
                dptrs[k] = v
        dptrs.update(hdr)
        if TABLE_RULE:
            # pointer tables: runs of >= 3 consecutive pointer-like longwords, at least two of them
            # strict (so a round value inside a real table is kept, a table of round values is not)
            weak = {k for k in range(0, len(p1) - 3, 2) if looks(k, False)}
            done = set()
            for k in sorted(weak):
                if k in done or k - 4 in weak:
                    continue
                run = [k]
                while run[-1] + 4 in weak:
                    run.append(run[-1] + 4)
                done.update(run)
                vals = [int.from_bytes(p1[j:j + 4], 'big') for j in run]
                # tables with odd entries (byte-string pointers) must point close to themselves, or
                # into one cluster of strings (monotonic values spanning < 4 KB)
                mono = vals == sorted(vals) or vals == sorted(vals, reverse=True)
                near = (not any(v & 1 for v in vals) or all(abs(v - k) < 0x10000 for v in vals)
                        or (mono and max(vals) - min(vals) < 0x1000))
                # round values never count as strict evidence here, even when they equal a function
                # entry (fixed-point tables full of 0x00004200)
                strong = sum(looks(j) and v & 0xff != 0 for j, v in zip(run, vals))
                if len(run) >= 3 and strong >= 2 and near:
                    for j in run:
                        v = int.from_bytes(p1[j:j + 4], 'big')
                        dptrs[j] = v
                        if v not in labels:
                            dlabels.add(v)
            # short tables (1-2 entries) that start at a label the code refers to and point to nearby
            # data (typically the records right after the table) or to function entries
            for k in sorted(dlabels):
                j = k
                while j % 2 == 0 and j + 4 <= len(p1) and looks(j, strict=False):
                    v = int.from_bytes(p1[j:j + 4], 'big')
                    if not (v in sentries or (not covered[v] and abs(v - k) < 0x10000)):
                        break
                    if (v & 0xff) == 0 and v not in sentries:
                        break       # round value: a constant (0x000a4200 velocity), not a pointer
                    dptrs[j] = v
                    if v not in labels:
                        dlabels.add(v)
                    j += 4
            # overlapping candidates (k and k+2 both chosen): keep the first
            for k in sorted(dptrs):
                if k in dptrs and k + 2 in dptrs:
                    del dptrs[k + 2]
        for k in list(dptrs):
            if {k + 1, k + 2, k + 3} & dlabels:
                del dptrs[k]
            # observed access pattern: read only with 16/8-bit reads, never as a longword -> word data
            elif k not in hdr and k not in RD_LONG and (k in RD_WORD or k + 2 in RD_WORD):
                del dptrs[k]
    cv = Conv(labels, p1, dlabels, symbolize, entries if symbolize else (), sentries if symbolize else ())
    raw_now = {}
    lines = ['\t.text', '\t.globl _start', '_start:', 'vectors:']
    for k in range(0, VECTOR_END, 4):
        v = int.from_bytes(p1[k:k + 4], 'big')
        lines.append('\t.long %s  | %02d %s' % ('L_%06x' % v if v in labels else '0x%08x' % v, k // 4, VECTOR_NAMES[k // 4]))
    pos, data = VECTOR_END, []

    def flush():
        for i in range(0, len(data), 16):
            lines.append('\t.byte ' + ','.join('0x%02x' % b for b in data[i:i + 16]))
        data.clear()

    named_data = {a for a in SYMS if VECTOR_END <= a < len(p1) and a not in labels and not covered[a]}

    def fill(upto):
        nonlocal pos
        while pos < upto:
            if pos in dlabels or pos in named_data:
                flush()
                lines.append('D_%06x:' % pos)
            if symbolize and pos in WREL and pos + 2 <= upto and WREL[pos][1] in labels:
                flush()
                tab, t = WREL[pos]
                lines.append('\t.word L_%06x-D_%06x' % (t, tab))
                pos += 2
                continue
            if pos in dptrs and pos + 4 <= upto:
                flush()
                v = dptrs[pos]
                flag, v = v & 0xff000000, v & 0xffffff
                lines.append('\t.long %s%s' % ('L_%06x' % v if v in labels else 'D_%06x' % v,
                                                '+0x%08x' % flag if flag else ''))
                pos += 4
                continue
            data.append(p1[pos]); pos += 1
        flush()

    for a in order:
        ln, text = texts[a]
        fill(a)
        if a == insert:
            lines.append('\tnop  | inserted by --insert-nop (shift test)')
        asm, why = (None, 'forced raw') if a in raw_force else cv.convert(a, insns[a].op, text)
        lines.append('L_%06x:' % a)
        if asm is None:
            raw_now[a] = raw_force.get(a, why)
            words = [p1[a + i] << 8 | p1[a + i + 1] for i in range(0, ln, 2)]
            lines.append('\t.word %s  | %s' % (','.join('0x%04x' % w for w in words), text))
        else:
            lines.append('\t%s  | %s' % (asm, text))
        pos = a + ln
    fill(len(p1))
    if insert is not None:
        # keep the image size: drop the last two bytes (end-of-ROM padding)
        assert p1[-2:] == b'\xff\xff', 'no padding at end of P1'
        lines[-1] = re.sub(r',0xff,0xff$', '', lines[-1])
    lines = apply_symbols(lines, len(p1))
    open(path, 'w').write('\n'.join(lines) + '\n')
    emit.stats = (len(dlabels), len(dptrs))
    return raw_now


def build(src, out):
    os.makedirs(out, exist_ok=True)
    obj, elf, binp = out + '/p1.o', out + '/p1.elf', out + '/p1.bin'
    r = subprocess.run([AS, '-m68000', '--register-prefix-optional', '-o', obj, src], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    r = subprocess.run([LD, '-Ttext=0', '-e', '_start', '-o', elf, obj], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    subprocess.run([OBJCOPY, '-O', 'binary', elf, binp], check=True)
    syms = {}
    for line in subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        v, _, n = line.split()
        if n.startswith('L_'):
            syms[int(n[2:], 16)] = int(v, 16)
    return (open(binp, 'rb').read(), syms), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/disasm'))
    ap.add_argument('--max-iter', type=int, default=40)
    ap.add_argument('--symbolize', action='store_true', help='stage 2: data labels, pointer tables, long immediates')
    ap.add_argument('--insert-nop', type=lambda x: int(x, 16), help='shift test: insert a nop before this label')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    p1 = open(os.path.join(ROOT, 'build/roms/p1.be'), 'rb').read()
    bios = open(os.path.join(ROOT, 'build/roms/bios.be'), 'rb').read()
    insns = collect_insns(p1, bios)
    texts = musashi_text(sorted(insns))
    src = os.path.join(args.out, 'p1.s')
    raw_force = {}
    asm_err = Counter()
    for it in range(args.max_iter):
        raw_now = emit(p1, insns, texts, raw_force, src, args.symbolize)
        res, err = build(src, args.out)
        if res is None:
            # assembler errors: force the offending source lines' instructions to raw
            bad = 0
            srcl = open(src).read().splitlines()
            for m in re.finditer(r':(\d+): Error: (.*)', err):
                ln = int(m.group(1)) - 1
                for k in range(ln, -1, -1):
                    if srcl[k].startswith('L_'):
                        a = int(srcl[k][2:-1], 16)
                        if a not in raw_force:
                            msg = re.sub(r"`.*'|-- statement.*", '', m.group(2)).strip()[:50]
                            raw_force[a] = 'as error: ' + msg
                            asm_err[msg] += 1
                            bad += 1
                        break
            print('iter %d: %d assembler errors -> raw' % (it, bad))
            if not bad:
                sys.exit(err[:2000])
            continue
        img, syms = res
        # first label whose address moved, or instruction bytes that differ
        fixed = 0
        order = sorted(syms)
        for a, nxt in zip(order, order[1:]):
            ln = texts[a][0]
            if syms[nxt] - syms[a] - (nxt - a - ln) != ln and a not in raw_force:
                raw_force[a] = 'size differs (GAS optimised)'
                fixed += 1
        if not fixed:
            for a in order:
                ln = texts[a][0]
                if img[a:a + ln] != p1[a:a + ln] and a not in raw_force:
                    raw_force[a] = 'encoding differs'
                    fixed += 1
        if not fixed:
            break
        print('iter %d: %d mismatching instructions -> raw' % (it, fixed))
    img = res[0] if res else b''
    ok = img == p1
    order = drop_data_runs(p1, insns, texts, non_overlapping(insns, texts))
    print('P1 image match: %s (%d bytes)' % (ok, len(img)))
    print('instructions decoded: %d, non-overlapping: %d, symbolic: %d, raw fallback: %d'
          % (len(insns), len(order), len(order) - len(raw_now), len(raw_now)))
    for k, v in Counter(raw_now.values()).most_common():
        print('  %6d  %s' % (v, k))
    if args.symbolize:
        print('symbolized: %d data labels, %d pointer longwords in data' % emit.stats)
    if args.insert_nop is not None:
        emit(p1, insns, texts, raw_force, os.path.join(args.out, 'p1_shift.s'), True, args.insert_nop)
        res2, err = build(os.path.join(args.out, 'p1_shift.s'), args.out + '/shift')
        if res2 is None:
            sys.exit(err[:2000])
        print('shifted image: %d bytes, %d bytes differ from original' % (len(res2[0]), sum(x != y for x, y in zip(res2[0], p1))))

if __name__ == '__main__':
    main()

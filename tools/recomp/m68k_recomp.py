#!/usr/bin/env python3
"""
m68k_recomp.py - static recompiler: Neo Geo 68000 program ROM (+ BIOS) -> C.

Inputs
  * byte-swapped (big-endian) P1 image mapped at 0x000000 and BIOS mapped at 0xC00000
  * REA `decompile-all` indexes (function entries + Ghidra body ranges) used as discovery seeds
  * vector tables, the cartridge header entry points and the BIOS jump table
  * optional extra seeds (tools/recomp/extra_seeds.txt) and runtime misses
    (build/recomp/runtime_misses.txt, written by the runtime with --log-misses)
  * build/m68k_table.txt / m68kinfo (Musashi-backed opcode validity, cycle table, disassembly)

Output (build/recomp/)
  * rc_NN.c        generated C, one C function per code chunk, one label per 68000 instruction
  * rc_table.c     entry address -> function tables used by the runtime dispatcher
  * rc_tests.c     per-instruction differential tests against Musashi (natively translated ops)
  * recomp_report.json  statistics

Execution model (see src/recomp_rt.h)
  * CPU state is Musashi's m68ki_cpu, so recompiled code and the Musashi interpreter can hand
    over at any instruction boundary.
  * Every decoded instruction address is an entry point (case label in the chunk's switch).
  * Branches inside a chunk are gotos (with a cycle-budget check), anything else stores REG_PC
    and returns to the dispatcher.
  * Rare/complex instructions call the corresponding Musashi opcode handler ("helper").
"""
import argparse
import json
import os
import struct
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402  (games/<GAME>/: per-game data)


BIOS_BASE = 0xC00000

BIOS_NAMES = {
    0xC00402: 'BIOS_RESET', 0xC00438: 'SYSTEM_INT1', 0xC0043E: 'SYSTEM_INT2',
    0xC00444: 'SYSTEM_RETURN', 0xC0044A: 'SYSTEM_IO', 0xC00450: 'CREDIT_CHECK',
    0xC00456: 'CREDIT_DOWN', 0xC0045C: 'READ_CALENDAR', 0xC00462: 'SETUP_CALENDAR',
    0xC00468: 'CARD', 0xC0046E: 'CARD_ERROR', 0xC00474: 'HOW_TO_PLAY',
    0xC004C2: 'FIX_CLEAR', 0xC004C8: 'LSP_1ST', 0xC004CE: 'MESS_OUT', 0xC004D4: 'CONTROLLER_SETUP',
}
P1_NAMES = {0x122: 'USER', 0x128: 'PLAYER_START', 0x12E: 'DEMO_END', 0x134: 'COIN_SOUND'}

COND_NAMES = ['T', 'F', 'HI', 'LS', 'CC', 'CS', 'NE', 'EQ', 'VC', 'VS', 'PL', 'MI', 'GE', 'LT', 'GT', 'LE']


def cond_expr(c):
    if c == 0:
        return '1'
    if c == 1:
        return '0'
    return 'COND_%s()' % COND_NAMES[c]


def s8(v):
    v &= 0xff
    return v - 0x100 if v & 0x80 else v


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def hx(v):
    return '0x%x' % (v & 0xffffffff)


class Image:
    def __init__(self, p1, bios):
        self.p1 = p1
        self.bios = bios

    def in_code(self, a):
        return (a < len(self.p1)) or (BIOS_BASE <= a < BIOS_BASE + len(self.bios))

    def b(self, a):
        if a < len(self.p1):
            return self.p1[a]
        if BIOS_BASE <= a < BIOS_BASE + len(self.bios):
            return self.bios[a - BIOS_BASE]
        raise IndexError(hex(a))

    def w16(self, a):
        return (self.b(a) << 8) | self.b(a + 1)

    def w32(self, a):
        return (self.w16(a) << 16) | self.w16(a + 2)


class EA:
    __slots__ = ('m', 'r', 'val', 'disp', 'xa', 'xr', 'xl', 'ext')

    def __init__(self, m, r=0, val=0, disp=0, xa=0, xr=0, xl=0, ext=0):
        self.m, self.r, self.val, self.disp, self.xa, self.xr, self.xl, self.ext = m, r, val, disp, xa, xr, xl, ext


def parse_ea(img, mode, reg, size, pos):
    if mode == 0:
        return EA('D', reg), 0
    if mode == 1:
        return EA('A', reg), 0
    if mode == 2:
        return EA('AI', reg), 0
    if mode == 3:
        return EA('PI', reg), 0
    if mode == 4:
        return EA('PD', reg), 0
    if mode == 5:
        return EA('DI', reg, disp=s16(img.w16(pos))), 1
    if mode == 6:
        ext = img.w16(pos)
        if ext & 0x0700:
            raise ValueError('68020 extension word: not 68000 code')
        return EA('IX', reg, disp=s8(ext), xa=(ext >> 15) & 1, xr=(ext >> 12) & 7, xl=(ext >> 11) & 1), 1
    if reg == 0:
        return EA('AW', val=s16(img.w16(pos)) & 0xffffffff), 1
    if reg == 1:
        return EA('AL', val=img.w32(pos)), 2
    if reg == 2:
        return EA('PCDI', val=(pos + s16(img.w16(pos))) & 0xffffffff), 1
    if reg == 3:
        ext = img.w16(pos)
        if ext & 0x0700:
            raise ValueError('68020 extension word: not 68000 code')
        return EA('PCIX', val=(pos + s8(ext)) & 0xffffffff, xa=(ext >> 15) & 1, xr=(ext >> 12) & 7, xl=(ext >> 11) & 1), 1
    if reg == 4:
        if size == 8:
            return EA('IMM', val=img.w16(pos) & 0xff), 1
        if size == 16:
            return EA('IMM', val=img.w16(pos)), 1
        if size == 32:
            return EA('IMM', val=img.w32(pos)), 2
    raise ValueError('bad ea %d/%d' % (mode, reg))


class Insn:
    __slots__ = ('addr', 'op', 'length', 'kind', 'size', 'p', 'flow', 'targets', 'helper', 'text')

    def __init__(self, addr, op):
        self.addr = addr
        self.op = op
        self.length = 2
        self.kind = None
        self.size = 0
        self.p = {}
        self.flow = 'seq'      # seq | jump | cjump | call | ret | exit
        self.targets = []
        self.helper = False
        self.text = ''

    @property
    def next(self):
        return self.addr + self.length


SZ2 = {0: 8, 1: 16, 2: 32}
IMM_OPS = {0: 'or', 1: 'and', 2: 'sub', 3: 'add', 5: 'eor', 6: 'cmp'}
SHIFT_NAMES = ['as', 'ls', 'rox', 'ro']


def decode(img, a, valid):
    op = img.w16(a)
    if not valid[op]:
        return None
    d = Insn(a, op)
    pos = [a + 2]

    def ea(mode, reg, size):
        e, n = parse_ea(img, mode, reg, size, pos[0])
        e.ext = pos[0]
        pos[0] += 2 * n
        return e

    def word():
        v = img.w16(pos[0])
        pos[0] += 2
        return v

    def helper(kind, flow='seq'):
        d.kind = kind
        d.helper = True
        d.flow = flow

    mode, reg = (op >> 3) & 7, op & 7
    hi = op >> 12
    if hi == 0:
        if op in (0x003C, 0x023C, 0x0A3C):            # ori/andi/eori to CCR
            word(); helper('ccr_imm')
        elif op in (0x007C, 0x027C, 0x0A7C):          # ori/andi/eori to SR
            word(); helper('sr_imm')
        elif (op & 0x0138) == 0x0108:                   # movep
            word(); helper('movep')
        elif op & 0x0100:                               # dynamic bit ops
            d.kind = 'bitdyn'
            d.p['bop'] = (op >> 6) & 3
            d.p['src'] = (op >> 9) & 7
            d.p['ea'] = ea(mode, reg, 32 if mode == 0 else 8)
        elif (op & 0x0F00) == 0x0800:                   # static bit ops
            d.kind = 'bitimm'
            d.p['bop'] = (op >> 6) & 3
            d.p['bit'] = word() & 0xff
            d.p['ea'] = ea(mode, reg, 32 if mode == 0 else 8)
        else:
            t = (op >> 9) & 7
            d.size = SZ2[(op >> 6) & 3]
            d.kind = 'imm_' + IMM_OPS[t]
            if d.size == 32:
                d.p['imm'] = img.w32(pos[0]); pos[0] += 4
            else:
                d.p['imm'] = word() & (0xff if d.size == 8 else 0xffff)
            d.p['ea'] = ea(mode, reg, d.size)
    elif hi in (1, 2, 3):
        d.size = {1: 8, 3: 16, 2: 32}[hi]
        d.p['src'] = ea(mode, reg, d.size)
        dmode, dreg = (op >> 6) & 7, (op >> 9) & 7
        if dmode == 1:
            d.kind = 'movea'
            d.p['an'] = dreg
        else:
            d.kind = 'move'
            d.p['dst'] = ea(dmode, dreg, d.size)
    elif hi == 4:
        if (op & 0xFFC0) == 0x40C0:
            ea(mode, reg, 16); helper('move_from_sr')
        elif (op & 0xFF00) == 0x4000:
            ea(mode, reg, SZ2[(op >> 6) & 3]); helper('negx')
        elif (op & 0xFF00) == 0x4200:
            d.kind = 'clr'; d.size = SZ2[(op >> 6) & 3]; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0xFFC0) == 0x44C0:
            ea(mode, reg, 16); helper('move_to_ccr')
        elif (op & 0xFF00) == 0x4400:
            d.kind = 'neg'; d.size = SZ2[(op >> 6) & 3]; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0xFFC0) == 0x46C0:
            ea(mode, reg, 16); helper('move_to_sr')
        elif (op & 0xFF00) == 0x4600:
            d.kind = 'not'; d.size = SZ2[(op >> 6) & 3]; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0xFFC0) == 0x4800:
            ea(mode, reg, 8); helper('nbcd')
        elif (op & 0xFFF8) == 0x4840:
            d.kind = 'swap'; d.p['dn'] = reg
        elif (op & 0xFFC0) == 0x4840:
            d.kind = 'pea'; d.p['ea'] = ea(mode, reg, 32)
        elif (op & 0xFFF8) == 0x4880:
            d.kind = 'extw'; d.p['dn'] = reg
        elif (op & 0xFFF8) == 0x48C0:
            d.kind = 'extl'; d.p['dn'] = reg
        elif (op & 0xFB80) == 0x4880:
            word(); ea(mode, reg, 32 if op & 0x40 else 16); helper('movem')
        elif op == 0x4AFC:
            helper('illegal', 'exit')
        elif (op & 0xFFC0) == 0x4AC0:
            ea(mode, reg, 8); helper('tas')
        elif (op & 0xFF00) == 0x4A00:
            d.kind = 'tst'; d.size = SZ2[(op >> 6) & 3]; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0xFFF0) == 0x4E40:
            helper('trap', 'exit')
        elif (op & 0xFFF8) == 0x4E50:
            word(); helper('link')
        elif (op & 0xFFF8) == 0x4E58:
            helper('unlk')
        elif (op & 0xFFF0) == 0x4E60:
            helper('move_usp')
        elif op == 0x4E70:
            helper('reset')
        elif op == 0x4E71:
            d.kind = 'nop'
        elif op == 0x4E72:
            word(); helper('stop', 'exit')
        elif op == 0x4E73:
            helper('rte', 'exit')
        elif op == 0x4E75:
            d.kind = 'rts'; d.flow = 'ret'
        elif op == 0x4E76:
            helper('trapv')
        elif op == 0x4E77:
            helper('rtr', 'exit')
        elif (op & 0xFFC0) == 0x4E80:
            d.kind = 'jsr'; d.flow = 'call'; d.p['ea'] = ea(mode, reg, 32)
        elif (op & 0xFFC0) == 0x4EC0:
            d.kind = 'jmp'; d.flow = 'jump'; d.p['ea'] = ea(mode, reg, 32)
        elif (op & 0xF1C0) == 0x4180:
            ea(mode, reg, 16); helper('chk')
        elif (op & 0xF1C0) == 0x41C0:
            d.kind = 'lea'; d.p['an'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, 32)
        else:
            return None
    elif hi == 5:
        if (op & 0xF0F8) == 0x50C8:
            d.kind = 'dbcc'; d.p['cc'] = (op >> 8) & 15; d.p['dn'] = reg
            disp = s16(word())
            d.p['target'] = (a + 2 + disp) & 0xffffffff
            d.flow = 'cjump'
        elif (op & 0xC0) == 0xC0:
            d.kind = 'scc'; d.p['cc'] = (op >> 8) & 15; d.p['ea'] = ea(mode, reg, 8)
        else:
            d.size = SZ2[(op >> 6) & 3]
            d.kind = 'subq' if op & 0x100 else 'addq'
            q = (op >> 9) & 7
            d.p['q'] = q if q else 8
            d.p['ea'] = ea(mode, reg, d.size)
    elif hi == 6:
        cc = (op >> 8) & 15
        disp = s8(op)
        wide = (op & 0xff) == 0
        if wide:
            disp = s16(word())
        d.p['target'] = (a + 2 + disp) & 0xffffffff
        d.p['wide'] = wide
        if cc == 0:
            d.kind = 'bra'; d.flow = 'jump'
        elif cc == 1:
            d.kind = 'bsr'; d.flow = 'call'
        else:
            d.kind = 'bcc'; d.p['cc'] = cc; d.flow = 'cjump'
    elif hi == 7:
        d.kind = 'moveq'; d.p['dn'] = (op >> 9) & 7; d.p['imm'] = s8(op) & 0xffffffff
    elif hi in (8, 0xC):
        name = 'or' if hi == 8 else 'and'
        if (op & 0x01F0) == 0x0100:
            helper('sbcd' if hi == 8 else 'abcd')
        elif hi == 0xC and (op & 0x01F8) in (0x0140, 0x0148, 0x0188):
            d.kind = 'exg'; d.p['mode'] = op & 0x01F8; d.p['rx'] = (op >> 9) & 7; d.p['ry'] = reg
        elif (op & 0x01C0) == 0x00C0:
            if hi == 8:
                ea(mode, reg, 16); helper('divu')
            else:
                d.kind = 'mulu'; d.p['dn'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, 16)
        elif (op & 0x01C0) == 0x01C0:
            if hi == 8:
                ea(mode, reg, 16); helper('divs')
            else:
                d.kind = 'muls'; d.p['dn'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, 16)
        else:
            d.size = SZ2[(op >> 6) & 3]
            d.kind = name + ('_re' if op & 0x100 else '_er')
            d.p['dn'] = (op >> 9) & 7
            d.p['ea'] = ea(mode, reg, d.size)
    elif hi in (9, 0xD):
        name = 'sub' if hi == 9 else 'add'
        if (op & 0xC0) == 0xC0:
            d.size = 32 if op & 0x100 else 16
            d.kind = name + 'a'; d.p['an'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0x0130) == 0x0100:
            helper(name + 'x')
        else:
            d.size = SZ2[(op >> 6) & 3]
            d.kind = name + ('_re' if op & 0x100 else '_er')
            d.p['dn'] = (op >> 9) & 7
            d.p['ea'] = ea(mode, reg, d.size)
    elif hi == 0xB:
        if (op & 0xC0) == 0xC0:
            d.size = 32 if op & 0x100 else 16
            d.kind = 'cmpa'; d.p['an'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0x100) == 0:
            d.size = SZ2[(op >> 6) & 3]
            d.kind = 'cmp_er'; d.p['dn'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, d.size)
        elif (op & 0x38) == 0x08:
            helper('cmpm')
        else:
            d.size = SZ2[(op >> 6) & 3]
            d.kind = 'eor_re'; d.p['dn'] = (op >> 9) & 7; d.p['ea'] = ea(mode, reg, d.size)
    elif hi == 0xE:
        if (op & 0xC0) == 0xC0:
            ea(mode, reg, 16); helper('shift_mem')
        else:
            t = (op >> 3) & 3
            d.size = SZ2[(op >> 6) & 3]
            if (op & 0x20) == 0:
                d.kind = 'shift_imm'
                cnt = (op >> 9) & 7
                d.p['cnt'] = cnt if cnt else 8
                d.p['type'] = SHIFT_NAMES[t]
                d.p['left'] = (op >> 8) & 1
                d.p['dn'] = reg
                if t == 2:          # roxl/roxr: leave to Musashi
                    helper('roxd')
            else:
                helper('shift_reg')
    elif hi in (0xA, 0xF):
        helper('line' + ('a' if hi == 0xA else 'f'), 'exit')
    else:
        return None
    d.length = pos[0] - a
    if d.kind in ('bra', 'bsr', 'bcc', 'dbcc'):
        d.targets = [d.p['target']]
    elif d.kind in ('jmp', 'jsr'):
        e = d.p['ea']
        if e.m in ('AW', 'AL', 'PCDI'):
            d.targets = [e.val]
            d.p['static'] = e.val
    return d


# --------------------------------------------------------------------------------------
# C generation
# --------------------------------------------------------------------------------------

MASK = {8: 'MASK_OUT_ABOVE_8', 16: 'MASK_OUT_ABOVE_16', 32: 'MASK_OUT_ABOVE_32'}
MBELOW = {8: 'MASK_OUT_BELOW_8', 16: 'MASK_OUT_BELOW_16'}
NFLAG = {8: 'NFLAG_8', 16: 'NFLAG_16', 32: 'NFLAG_32'}
CONSTMASK = {8: 0xff, 16: 0xffff, 32: 0xffffffff}


def idx_expr(e):
    reg = 'REG_A[%d]' % e.xr if e.xa else 'REG_D[%d]' % e.xr
    if e.xl:
        return reg
    return '(uint)MAKE_INT_16(%s)' % reg


def ea_addr(e, size, code):
    """Return a C expression for the effective address; side effects are emitted into code."""
    m = e.m
    if m == 'AI':
        return 'REG_A[%d]' % e.r
    if m == 'PI':
        inc = 2 if (size == 8 and e.r == 7) else size // 8
        code.append('ea = REG_A[%d]; REG_A[%d] += %d;' % (e.r, e.r, inc))
        return 'ea'
    if m == 'PD':
        dec = 2 if (size == 8 and e.r == 7) else size // 8
        code.append('REG_A[%d] -= %d; ea = REG_A[%d];' % (e.r, dec, e.r))
        return 'ea'
    if m == 'DI':
        return '(REG_A[%d] + %s)' % (e.r, hx(e.disp))
    if m == 'IX':
        return '(REG_A[%d] + %s + %s)' % (e.r, idx_expr(e), hx(e.disp))
    if m in ('AW', 'AL', 'PCDI'):
        return hx(e.val)
    if m == 'PCIX':
        return '(%s + %s)' % (hx(e.val), idx_expr(e))
    raise ValueError('no address for mode ' + m)


def ea_read(e, size, var, code):
    m = e.m
    if m == 'D':
        code.append('%s = %s;' % (var, 'REG_D[%d]' % e.r if size == 32 else '%s(REG_D[%d])' % (MASK[size], e.r)))
    elif m == 'A':
        code.append('%s = %s;' % (var, 'REG_A[%d]' % e.r if size == 32 else '%s(REG_A[%d])' % (MASK[size], e.r)))
    elif m == 'IMM':
        code.append('%s = %s;' % (var, hx(e.val & CONSTMASK[size])))
    else:
        a = ea_addr(e, size, code)
        code.append('%s = m68ki_read_%d(%s);' % (var, size, a))


def write_dn(n, size, val):
    if size == 32:
        return 'REG_D[%d] = %s;' % (n, val)
    return 'REG_D[%d] = %s(REG_D[%d]) | %s;' % (n, MBELOW[size], n, val)


def ea_write(e, size, val, code):
    if e.m == 'D':
        code.append(write_dn(e.r, size, val))
    elif e.m == 'A':
        raise ValueError('write to An via ea_write')
    else:
        a = ea_addr(e, size, code)
        code.append('m68ki_write_%d(%s, %s);' % (size, a, val))


def rmw_begin(e, size, code):
    """For read-modify-write: returns (read_var_stmt_done, writer function)."""
    if e.m == 'D':
        ea_read(e, size, 'dst', code)
        return lambda val: code.append(write_dn(e.r, size, val))
    a = ea_addr(e, size, code)
    if a != 'ea':
        code.append('ea = %s;' % a)
    code.append('dst = m68ki_read_%d(ea);' % size)
    return lambda val: code.append('m68ki_write_%d(ea, %s);' % (size, val))


def flags_logic(size, code, res='res'):
    code.append('FLAG_N = %s(%s); FLAG_Z = %s; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;' % (NFLAG[size], res, res))


def flags_add(size, code):
    code.append('FLAG_N = %s(res);' % NFLAG[size])
    code.append('FLAG_V = VFLAG_ADD_%d(src, dst, res);' % size)
    code.append('FLAG_X = FLAG_C = %s;' % ('CFLAG_ADD_32(src, dst, res)' if size == 32 else 'CFLAG_%d(res)' % size))
    code.append('FLAG_Z = %s(res);' % MASK[size])


def flags_sub(size, code, x=True):
    code.append('FLAG_N = %s(res);' % NFLAG[size])
    c = 'CFLAG_SUB_32(src, dst, res)' if size == 32 else 'CFLAG_%d(res)' % size
    code.append(('FLAG_X = FLAG_C = %s;' if x else 'FLAG_C = %s;') % c)
    code.append('FLAG_V = VFLAG_SUB_%d(src, dst, res);' % size)
    code.append('FLAG_Z = %s(res);' % MASK[size])


class Ctx:
    def __init__(self, local_set, consts):
        self.local = local_set
        self.consts = consts


def goto(ctx, target, code):
    if target in ctx.local:
        code.append('REG_PC = %s; if (m68ki_remaining_cycles <= 0) return; goto L_%x;' % (hx(target), target))
    else:
        code.append('REG_PC = %s; return;' % hx(target))


def gen_native(d, ctx, code):
    """Emit C for a natively translated instruction. Returns True if handled."""
    k, s, p = d.kind, d.size, d.p
    if k == 'nop':
        return True
    if k == 'move':
        ea_read(p['src'], s, 'res', code)
        if s == 32 and p['dst'].m == 'PD':
            # Musashi (like the real 68000) writes the low word first for move.l to -(An)
            a = ea_addr(p['dst'], s, code)
            code.append('m68ki_write_16(%s + 2, res & 0xffff); m68ki_write_16(%s, res >> 16);' % (a, a))
        else:
            ea_write(p['dst'], s, 'res', code)
        flags_logic(s, code)
        return True
    if k == 'movea':
        ea_read(p['src'], s, 'res', code)
        code.append('REG_A[%d] = %s;' % (p['an'], 'res' if s == 32 else '(uint)MAKE_INT_16(res)'))
        return True
    if k == 'moveq':
        code.append('res = %s; REG_D[%d] = res;' % (hx(p['imm']), p['dn']))
        flags_logic(32, code)
        return True
    if k == 'lea':
        a = ea_addr(p['ea'], 32, code)
        code.append('REG_A[%d] = %s;' % (p['an'], a))
        return True
    if k == 'pea':
        a = ea_addr(p['ea'], 32, code)
        code.append('ea = %s; m68ki_push_32(ea);' % a)
        return True
    if k == 'clr':
        ea_write(p['ea'], s, '0', code)
        code.append('FLAG_N = NFLAG_CLEAR; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR; FLAG_Z = ZFLAG_SET;')
        return True
    if k == 'tst':
        ea_read(p['ea'], s, 'res', code)
        flags_logic(s, code)
        return True
    if k == 'neg':
        w = rmw_begin(p['ea'], s, code)
        code.append('res = 0 - dst;')
        code.append('FLAG_N = %s(res);' % NFLAG[s])
        if s == 32:
            code.append('FLAG_C = FLAG_X = CFLAG_SUB_32(dst, 0, res);')
            code.append('FLAG_V = (dst & res) >> 24;')
        else:
            code.append('FLAG_C = FLAG_X = CFLAG_%d(res);' % s)
            code.append('FLAG_V = %s;' % ('dst & res' if s == 8 else '(dst & res) >> 8'))
        code.append('FLAG_Z = %s(res);' % MASK[s])
        w('FLAG_Z')
        return True
    if k == 'not':
        w = rmw_begin(p['ea'], s, code)
        code.append('res = %s(~dst);' % MASK[s])
        w('res')
        flags_logic(s, code)
        return True
    if k == 'swap':
        n = p['dn']
        code.append('FLAG_Z = MASK_OUT_ABOVE_32(REG_D[%d] << 16); REG_D[%d] = (REG_D[%d] >> 16) | FLAG_Z;' % (n, n, n))
        code.append('FLAG_Z = REG_D[%d]; FLAG_N = NFLAG_32(REG_D[%d]); FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;' % (n, n))
        return True
    if k == 'extw':
        n = p['dn']
        code.append('REG_D[%d] = MASK_OUT_BELOW_16(REG_D[%d]) | MASK_OUT_ABOVE_8(REG_D[%d]) | (GET_MSB_8(REG_D[%d]) ? 0xff00 : 0);' % (n, n, n, n))
        code.append('FLAG_N = NFLAG_16(REG_D[%d]); FLAG_Z = MASK_OUT_ABOVE_16(REG_D[%d]); FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;' % (n, n))
        return True
    if k == 'extl':
        n = p['dn']
        code.append('REG_D[%d] = MASK_OUT_ABOVE_16(REG_D[%d]) | (GET_MSB_16(REG_D[%d]) ? 0xffff0000 : 0);' % (n, n, n))
        code.append('FLAG_N = NFLAG_32(REG_D[%d]); FLAG_Z = REG_D[%d]; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;' % (n, n))
        return True
    if k == 'exg':
        rx, ry, md = p['rx'], p['ry'], p['mode']
        if md == 0x140:
            a, b = 'REG_D[%d]' % rx, 'REG_D[%d]' % ry
        elif md == 0x148:
            a, b = 'REG_A[%d]' % rx, 'REG_A[%d]' % ry
        else:
            a, b = 'REG_D[%d]' % rx, 'REG_A[%d]' % ry
        code.append('res = %s; %s = %s; %s = res;' % (a, a, b, b))
        return True
    if k.startswith('imm_'):
        o = k[4:]
        e = p['ea']
        imm = hx(p['imm'])
        if o == 'cmp':
            code.append('src = %s;' % imm)
            ea_read(e, s, 'dst', code)
            code.append('res = dst - src;')
            flags_sub(s, code, x=False)
            return True
        w = rmw_begin(e, s, code)
        code.append('src = %s;' % imm)
        if o in ('or', 'and', 'eor'):
            opc = {'or': '|', 'and': '&', 'eor': '^'}[o]
            code.append('res = %s(dst %s src);' % (MASK[s], opc))
            w('res')
            flags_logic(s, code)
        elif o == 'add':
            code.append('res = src + dst;')
            flags_add(s, code)
            w('FLAG_Z')
        elif o == 'sub':
            code.append('res = dst - src;')
            flags_sub(s, code)
            w('FLAG_Z')
        return True
    if k in ('add_er', 'sub_er', 'cmp_er', 'or_er', 'and_er'):
        o = k[:-3]
        n = p['dn']
        ea_read(p['ea'], s, 'src', code)
        code.append('dst = %s;' % ('REG_D[%d]' % n if s == 32 else '%s(REG_D[%d])' % (MASK[s], n)))
        if o == 'add':
            code.append('res = src + dst;')
            flags_add(s, code)
            code.append(write_dn(n, s, 'FLAG_Z'))
        elif o == 'sub':
            code.append('res = dst - src;')
            flags_sub(s, code)
            code.append(write_dn(n, s, 'FLAG_Z'))
        elif o == 'cmp':
            code.append('res = dst - src;')
            flags_sub(s, code, x=False)
        else:
            opc = '|' if o == 'or' else '&'
            code.append('res = %s(dst %s src);' % (MASK[s], opc))
            code.append(write_dn(n, s, 'res'))
            flags_logic(s, code)
        return True
    if k in ('add_re', 'sub_re', 'or_re', 'and_re', 'eor_re'):
        o = k[:-3]
        n = p['dn']
        code.append('src = %s;' % ('REG_D[%d]' % n if s == 32 else '%s(REG_D[%d])' % (MASK[s], n)))
        w = rmw_begin(p['ea'], s, code)
        if o == 'add':
            code.append('res = src + dst;')
            flags_add(s, code)
            w('FLAG_Z')
        elif o == 'sub':
            code.append('res = dst - src;')
            flags_sub(s, code)
            w('FLAG_Z')
        else:
            opc = {'or': '|', 'and': '&', 'eor': '^'}[o]
            code.append('res = %s(dst %s src);' % (MASK[s], opc))
            w('res')
            flags_logic(s, code)
        return True
    if k in ('adda', 'suba', 'cmpa'):
        an = p['an']
        ea_read(p['ea'], s, 'src', code)
        if s == 16:
            code.append('src = (uint)MAKE_INT_16(src);')
        if k == 'adda':
            code.append('REG_A[%d] = MASK_OUT_ABOVE_32(REG_A[%d] + src);' % (an, an))
        elif k == 'suba':
            code.append('REG_A[%d] = MASK_OUT_ABOVE_32(REG_A[%d] - src);' % (an, an))
        else:
            code.append('dst = REG_A[%d]; res = dst - src;' % an)
            code.append('FLAG_N = NFLAG_32(res); FLAG_Z = MASK_OUT_ABOVE_32(res);')
            code.append('FLAG_V = VFLAG_SUB_32(src, dst, res); FLAG_C = CFLAG_SUB_32(src, dst, res);')
        return True
    if k in ('addq', 'subq'):
        e = p['ea']
        q = p['q']
        if e.m == 'A':
            code.append('REG_A[%d] = MASK_OUT_ABOVE_32(REG_A[%d] %s %d);' % (e.r, e.r, '+' if k == 'addq' else '-', q))
            return True
        w = rmw_begin(e, s, code)
        code.append('src = %d;' % q)
        if k == 'addq':
            code.append('res = src + dst;')
            flags_add(s, code)
        else:
            code.append('res = dst - src;')
            flags_sub(s, code)
        w('FLAG_Z')
        return True
    if k in ('mulu', 'muls'):
        n = p['dn']
        ea_read(p['ea'], 16, 'src', code)
        code.append('m68ki_remaining_cycles -= %s(src);' % ('rc_mulu_cycles' if k == 'mulu' else 'rc_muls_cycles'))
        if k == 'mulu':
            code.append('res = MASK_OUT_ABOVE_16(src) * MASK_OUT_ABOVE_16(REG_D[%d]);' % n)
        else:
            code.append('res = MASK_OUT_ABOVE_32((uint)((sint)MAKE_INT_16(src) * (sint)MAKE_INT_16(MASK_OUT_ABOVE_16(REG_D[%d]))));' % n)
        code.append('REG_D[%d] = res; FLAG_Z = res; FLAG_N = NFLAG_32(res); FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;' % n)
        return True
    if k in ('bitdyn', 'bitimm'):
        e = p['ea']
        bop = p['bop']
        if e.m == 'D':
            bit = '(REG_D[%d] & 0x1f)' % p['src'] if k == 'bitdyn' else str(p['bit'] & 0x1f)
            code.append('src = 1u << %s;' % bit)
            code.append('FLAG_Z = REG_D[%d] & src;' % e.r)
            if bop == 1:
                code.append('REG_D[%d] ^= src;' % e.r)
            elif bop == 2:
                code.append('REG_D[%d] &= ~src;' % e.r)
            elif bop == 3:
                code.append('REG_D[%d] |= src;' % e.r)
            return True
        bit = '(REG_D[%d] & 7)' % p['src'] if k == 'bitdyn' else str(p['bit'] & 7)
        if bop == 0:
            ea_read(e, 8, 'dst', code)
            code.append('FLAG_Z = dst & (1u << %s);' % bit)
            return True
        a = ea_addr(e, 8, code)
        if a != 'ea':
            code.append('ea = %s;' % a)
        code.append('dst = m68ki_read_8(ea); src = 1u << %s; FLAG_Z = dst & src;' % bit)
        newv = {1: 'dst ^ src', 2: 'dst & ~src', 3: 'dst | src'}[bop]
        code.append('m68ki_write_8(ea, MASK_OUT_ABOVE_8(%s));' % newv)
        return True
    if k == 'shift_imm':
        t, left, cnt, n = p['type'], p['left'], p['cnt'], p['dn']
        if t not in ('ls', 'as', 'ro'):
            return False
        gen_shift_imm(code, t, left, cnt, n, s)
        code.append('m68ki_remaining_cycles -= %d;' % (cnt << ctx.consts['shift']))
        return True
    if k == 'scc':
        e = p['ea']
        c = cond_expr(p['cc'])
        if e.m == 'D':
            if p['cc'] == 0:
                code.append('REG_D[%d] |= 0xff;' % e.r)
            elif p['cc'] == 1:
                code.append('REG_D[%d] &= 0xffffff00;' % e.r)
            else:
                code.append('if (%s) { REG_D[%d] |= 0xff; m68ki_remaining_cycles -= %d; } else REG_D[%d] &= 0xffffff00;'
                            % (c, e.r, ctx.consts['scc_r_true'], e.r))
        else:
            a = ea_addr(e, 8, code)
            code.append('m68ki_write_8(%s, %s ? 0xff : 0);' % (a, c))
        return True
    return False


def gen_shift_imm(code, t, left, cnt, n, s):
    """Immediate-count shifts/rotates on Dn, mirroring Musashi's handlers."""
    m = MASK[s]
    hi_bit = {8: 0x80, 16: 0x8000, 32: 0x80000000}[s]
    code.append('src = %s;' % ('REG_D[%d]' % n if s == 32 else '%s(REG_D[%d])' % (m, n)))
    if t == 'ls':
        if left:
            code.append('res = %s(src << %d);' % (m, cnt))
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = %s(res); FLAG_Z = res;' % NFLAG[s])
            if s == 32:
                code.append('FLAG_X = FLAG_C = src >> (%d - %d);' % (24, cnt))
            elif s == 16:
                code.append('FLAG_X = FLAG_C = src >> (8 - %d);' % cnt)
            else:
                code.append('FLAG_X = FLAG_C = src << %d;' % cnt)
            code.append('FLAG_V = VFLAG_CLEAR;')
        else:
            code.append('res = src >> %d;' % cnt)
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = NFLAG_CLEAR; FLAG_Z = res;')
            if s == 32:
                code.append('FLAG_X = FLAG_C = src << (9 - %d);' % cnt)
            else:
                code.append('FLAG_X = FLAG_C = src << (9 - %d);' % cnt)
            code.append('FLAG_V = VFLAG_CLEAR;')
    elif t == 'as':
        if left:
            code.append('res = %s(src << %d);' % (m, cnt))
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = %s(res); FLAG_Z = res;' % NFLAG[s])
            if s == 32:
                code.append('FLAG_X = FLAG_C = src >> (24 - %d);' % cnt)
                code.append('src &= m68ki_shift_32_table[%d + 1];' % cnt)
                code.append('FLAG_V = (!(src == 0 || src == m68ki_shift_32_table[%d + 1])) << 7;' % cnt)
            elif s == 16:
                code.append('FLAG_X = FLAG_C = src >> (8 - %d);' % cnt)
                code.append('src &= m68ki_shift_16_table[%d + 1];' % cnt)
                code.append('FLAG_V = (!(src == 0 || src == m68ki_shift_16_table[%d + 1])) << 7;' % cnt)
            else:
                code.append('FLAG_X = FLAG_C = src << %d;' % cnt)
                code.append('src &= m68ki_shift_8_table[%d + 1];' % cnt)
                code.append('FLAG_V = (!(src == 0 || (src == m68ki_shift_8_table[%d + 1] && %d < 8))) << 7;' % (cnt, cnt))
        else:
            code.append('res = src >> %d;' % cnt)
            code.append('if (GET_MSB_%d(src)) res |= m68ki_shift_%d_table[%d];' % (s, s, cnt))
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = %s(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR;' % NFLAG[s])
            code.append('FLAG_X = FLAG_C = src << (9 - %d);' % cnt)
    elif t == 'ro':
        bits = s
        if left:
            code.append('res = ROL_%d(src, %d);' % (s, cnt & 7 if s == 8 else cnt))
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = %s(res); FLAG_Z = res;' % NFLAG[s])
            if s == 32:
                code.append('FLAG_C = src >> (24 - %d);' % cnt)
            elif s == 16:
                code.append('FLAG_C = src >> (8 - %d);' % cnt)
            else:
                code.append('FLAG_C = src << %d;' % cnt)
            code.append('FLAG_V = VFLAG_CLEAR;')
        else:
            code.append('res = ROR_%d(src, %d);' % (s, cnt & 7 if s == 8 else cnt))
            code.append(write_dn(n, s, 'res'))
            code.append('FLAG_N = %s(res); FLAG_Z = res;' % NFLAG[s])
            code.append('FLAG_C = src << (9 - %d);' % cnt)
            code.append('FLAG_V = VFLAG_CLEAR;')


def gen_insn(d, ctx, cycles):
    code = []
    code.append('m68ki_remaining_cycles -= %d;' % cycles[d.op])
    k, p = d.kind, d.p
    nxt = d.next
    if d.helper:
        code.append('REG_PPC = %s; REG_PC = %s; REG_IR = 0x%04x; m68ki_instruction_jump_table[0x%04x]();'
                    % (hx(d.addr), hx(d.addr + 2), d.op, d.op))
        if d.flow == 'exit':
            code.append('return;')
        else:
            code.append('if (REG_PC != %s) return;' % hx(nxt))
        return code
    if k == 'bra':
        t = p['target']
        if t == d.addr:
            code.append('REG_PC = %s; m68ki_remaining_cycles = 0; return;' % hx(t))
        else:
            goto(ctx, t, code)
        return code
    if k == 'bsr':
        code.append('m68ki_push_32(%s);' % hx(nxt))
        code.append('REG_PC = %s; return;' % hx(p['target']))
        return code
    if k == 'bcc':
        sub = []
        goto(ctx, p['target'], sub)
        code.append('if (%s) { %s }' % (cond_expr(p['cc']), ' '.join(sub)))
        code.append('m68ki_remaining_cycles -= %d;' % (ctx.consts['bcc_notake_w'] if p['wide'] else ctx.consts['bcc_notake_b']))
        return code
    if k == 'dbcc':
        n = p['dn']
        sub = []
        goto(ctx, p['target'], sub)
        body = ('res = MASK_OUT_ABOVE_16(REG_D[%d] - 1); REG_D[%d] = MASK_OUT_BELOW_16(REG_D[%d]) | res; '
                'if (res != 0xffff) { m68ki_remaining_cycles -= %d; %s } m68ki_remaining_cycles -= %d;'
                % (n, n, n, ctx.consts['dbcc_f_noexp'], ' '.join(sub), ctx.consts['dbcc_f_exp']))
        if p['cc'] == 1:
            code.append(body)
        else:
            code.append('if (!(%s)) { %s }' % (cond_expr(p['cc']), body))
        return code
    if k == 'jmp':
        if 'static' in p:
            t = p['static']
            if t == d.addr:
                code.append('REG_PC = %s; m68ki_remaining_cycles = 0; return;' % hx(t))
            else:
                goto(ctx, t, code)
        else:
            a = ea_addr(p['ea'], 32, code)
            code.append('REG_PC = %s; return;' % a)
        return code
    if k == 'jsr':
        a = ea_addr(p['ea'], 32, code)
        code.append('ea = %s; m68ki_push_32(%s); REG_PC = ea; return;' % (a, hx(nxt)))
        return code
    if k == 'rts':
        code.append('REG_PC = m68ki_pull_32(); return;')
        return code
    if not gen_native(d, ctx, code):
        raise ValueError('no generator for %s at %x' % (k, d.addr))
    return code


# --------------------------------------------------------------------------------------
# Differential tests (native translations vs Musashi)
# --------------------------------------------------------------------------------------

def gen_test(d, ctx, cycles, idx):
    """A test executes the translated instruction body against random state; the harness runs the
    Musashi handler on the same state and compares registers, SR and memory writes."""
    body = gen_insn(d, Ctx(set(), ctx.consts), cycles)
    # Turn non-local control flow into a plain return; REG_PC will be compared.
    return 'static void t_%d(void) {\n  uint ea, src, dst, res; (void)ea; (void)src; (void)dst; (void)res;\n  %s\n}\n' % (
        idx, '\n  '.join(body))


# --------------------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------------------

def load_rea_index(path, base_ok):
    seeds, ranges, names = [], [], {}
    if not os.path.exists(path):
        print('warning: REA index %s not found' % path, file=sys.stderr)
        return seeds, ranges, names
    idx = json.load(open(path))
    for f in idx['functions']:
        a = int(f['address'], 16)
        if not base_ok(a):
            continue
        seeds.append(a)
        names[a] = f.get('name') or ''
        body = f.get('body') or {}
        for r in body.get('ranges') or []:
            lo, hi = int(r['start'], 16), int(r['end'], 16)
            ranges.append((a, lo, hi))
    return seeds, ranges, names


def discover(img, valid, seeds):
    insns = {}
    bad = set()
    work = list(seeds)
    while work:
        a = work.pop()
        while True:
            if a in insns or a in bad:
                break
            if a & 1 or not img.in_code(a):
                bad.add(a)
                break
            try:
                d = decode(img, a, valid)
            except (IndexError, ValueError):
                d = None
            if d is None or not img.in_code(d.next - 1):
                bad.add(a)
                break
            insns[a] = d
            for t in d.targets:
                if t not in insns:
                    work.append(t)
            if d.flow in ('jump', 'ret', 'exit'):
                break
            a = d.next
    return insns, bad


def scan_pointer_seeds(img, valid, known):
    """Heuristic: longwords in P1 that point to an even P1 address where a run of valid instructions
    ends in a control transfer (rts/rte/jmp/bra) are treated as extra entry points (jump/pointer
    tables used by the game's state machines). A false positive only produces unreachable C."""
    p1 = img.p1
    found = set()
    cache = {}

    def plausible(a):
        if a in cache:
            return cache[a]
        x, ok = a, False
        for _ in range(48):
            if x in known:
                ok = True
                break
            try:
                d = decode(img, x, valid)
            except (IndexError, ValueError):
                d = None
            if d is None or d.kind in ('illegal', 'linea', 'linef', 'reset', 'stop'):
                break
            if d.flow in ('jump', 'ret') or d.kind in ('rte', 'rtr'):
                ok = True
                break
            x = d.next
        cache[a] = ok
        return ok

    for k in range(0, len(p1) - 3, 2):
        v = struct.unpack('>I', p1[k:k + 4])[0]
        if 0x200 <= v < len(p1) and not v & 1 and v not in known and plausible(v):
            found.add(v)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    # the assembly source is the source of truth: translate the image assembled from build/disasm/p1.s
    # (make matching) when it exists, else the original dump
    asm_p1 = os.path.join(ROOT, 'build/disasm/p1.bin')
    ap.add_argument('--p1', default=asm_p1 if os.path.exists(asm_p1) else os.path.join(ROOT, 'build/roms/p1.be'))
    ap.add_argument('--code-map', default=os.path.join(ROOT, 'build/disasm/code_p1.txt'),
                    help='instruction addresses of the assembled image (tools/disasm/codemap.py): discovery seeds')
    ap.add_argument('--bios', default=os.path.join(ROOT, 'build/roms/bios.be'))
    ap.add_argument('--rea-p1', default=os.path.join(ROOT, 'build/rea/decomp_p1/decompile-index.json'))
    ap.add_argument('--rea-bios', default=os.path.join(ROOT, 'build/rea/decomp_bios/decompile-index.json'))
    ap.add_argument('--table', default=os.path.join(ROOT, 'build/m68k_table.txt'))
    ap.add_argument('--m68kinfo', default=os.path.join(ROOT, 'build/m68kinfo'))
    ap.add_argument('--extra-seeds', default=os.path.join(ROOT, 'tools/recomp/extra_seeds.txt'))
    ap.add_argument('--misses', default=os.path.join(ROOT, 'build/recomp/runtime_misses.txt'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/recomp'))
    ap.add_argument('--files', type=int, default=48, help='number of generated .c files')
    ap.add_argument('--no-scan', action='store_true', help='disable the pointer-table seed heuristic')
    ap.add_argument('--max-chunk', type=int, default=1500, help='max instructions per C function')
    args = ap.parse_args()

    p1 = open(args.p1, 'rb').read()
    bios = open(args.bios, 'rb').read()
    img = Image(p1, bios)

    valid, cycles = [], []
    for line in open(args.table):
        v, c = line.split()
        valid.append(int(v))
        cycles.append(int(c))
    consts = {}
    for line in subprocess.run([args.m68kinfo, 'consts'], capture_output=True, text=True, check=True).stdout.splitlines():
        k, v = line.split()
        consts[k] = int(v)

    # ---------------- seeds ----------------
    seeds = set()
    names = {}
    for i in range(1, 64):
        for base, data in ((0, p1), (BIOS_BASE, bios)):
            v = struct.unpack('>I', data[i * 4:i * 4 + 4])[0] & 0xffffff
            if img.in_code(v) and not v & 1 and v >= 0x100:
                seeds.add(v)
    for a, n in P1_NAMES.items():
        seeds.add(a)
        names[a] = n
    a = 0xC00402
    while img.w16(a) == 0x4EF9:
        seeds.add(a)
        a += 6
    names.update(BIOS_NAMES)
    # stage-3 names shared with the disassembly (games/<GAME>/symbols/curated.txt)
    sym_path = game.symbols('curated')
    if os.path.exists(sym_path):
        for line in open(sym_path):
            f = line.split('|')[0].split()
            if len(f) >= 2 and not line[0].isspace() and not line.startswith(('#', '@')):
                a = int(f[0], 16)
                if img.in_code(a):
                    names[a] = f[1]
    rea_ranges = []
    for path, ok in ((args.rea_p1, lambda x: x < len(p1)), (args.rea_bios, lambda x: x >= BIOS_BASE)):
        s, r, n = load_rea_index(path, ok)
        if path == args.rea_bios:
            # The BIOS was analysed as a raw image mapped at 0xC00000 already.
            pass
        seeds.update(s)
        rea_ranges.extend(r)
        for k, v in n.items():
            names.setdefault(k, v)
        for (_, lo, _) in r:
            seeds.add(lo)
    n_rea = len(seeds)
    print('P1 image: %s' % os.path.relpath(args.p1, ROOT))
    for path in (args.extra_seeds, args.misses, args.code_map):
        if os.path.exists(path):
            for line in open(path):
                line = line.split('#')[0].strip()
                if line:
                    seeds.add(int(line.split()[0], 16))
    seeds = {x for x in seeds if img.in_code(x) and not x & 1}
    if not args.no_scan:
        base_insns, _ = discover(img, valid, sorted(seeds))
        extra = scan_pointer_seeds(img, valid, set(base_insns))
        print('pointer scan: %d extra seeds' % len(extra))
        seeds |= extra

    insns, bad = discover(img, valid, sorted(seeds))
    print('decoded %d instructions from %d seeds (%d bad starts)' % (len(insns), len(seeds), len(bad)))

    # ---------------- cross-check lengths with Musashi + disassembly ----------------
    addrs = sorted(insns)
    proc = subprocess.run([args.m68kinfo, 'disasm', args.p1, args.bios],
                          input=''.join('%x\n' % x for x in addrs), capture_output=True, text=True, check=True)
    mism = 0
    for line in proc.stdout.splitlines():
        a_s, ln, text = line.split('\t', 2)
        a = int(a_s, 16)
        d = insns[a]
        d.text = text.strip()
        if int(ln) != d.length:
            mism += 1
            if mism < 20:
                print('length mismatch at %x: musashi %s ours %d (%s)' % (a, ln, d.length, text), file=sys.stderr)
    if mism:
        sys.exit('ERROR: %d length mismatches' % mism)

    # ---------------- partition into chunks ----------------
    owner = {}
    rea_ranges.sort(key=lambda t: (t[0], t[1]))
    func_of_range = {}
    for (fa, lo, hi) in rea_ranges:
        for a in range(lo & ~1, hi + 1, 2):
            if a in insns and a not in owner:
                owner[a] = fa
    # Unowned instructions: group contiguous runs.
    cur = None
    prev_end = None
    for a in addrs:
        if a in owner:
            cur = None
            continue
        if cur is None or prev_end != a:
            cur = a
        owner[a] = cur
        prev_end = insns[a].next
    groups = defaultdict(list)
    for a in addrs:
        groups[owner[a]].append(a)
    chunks = []
    for fa in sorted(groups):
        lst = groups[fa]
        for i in range(0, len(lst), args.max_chunk):
            chunks.append((fa, lst[i:i + args.max_chunk]))

    os.makedirs(args.out, exist_ok=True)
    # ---------------- emit C ----------------
    native = defaultdict(int)
    helpers = defaultdict(int)
    fnames = []
    per_file = [[] for _ in range(args.files)]
    for ci, (fa, lst) in enumerate(chunks):
        region = 'bios' if fa >= BIOS_BASE else 'p1'
        nm = names.get(fa, '')
        fname = 'rc_%s_%06x%s' % (region, lst[0], ('_' + nm) if nm and lst[0] == fa and not nm.startswith('FUN_') else '')
        fname = ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in fname)
        fnames.append(fname)
        local = set(lst)
        ctx = Ctx(local, consts)
        out = []
        out.append('/* chunk %d: REA/Ghidra function %s (%s) */' % (ci, hx(fa), nm or 'unnamed run'))
        out.append('void %s(void)\n{' % fname)
        out.append('    uint ea, src, dst, res; (void)ea; (void)src; (void)dst; (void)res;')
        out.append('    switch (REG_PC) {')
        for a in lst:
            out.append('    case %s: goto L_%x;' % (hx(a), a))
        out.append('    default: return;\n    }')
        for i, a in enumerate(lst):
            d = insns[a]
            out.append('L_%x: /* %06x: %s */' % (a, a, d.text.replace('*/', '* /')))
            body = gen_insn(d, ctx, cycles)
            if d.helper:
                helpers[d.kind] += 1
            else:
                native[d.kind] += 1
            out.append('    ' + '\n    '.join(body))
            nxt = d.next
            falls = d.flow in ('seq', 'cjump')
            if falls and (i + 1 >= len(lst) or lst[i + 1] != nxt):
                out.append('    REG_PC = %s; return;' % hx(nxt))
            elif falls:
                # stop exactly where Musashi would (after the instruction that used up the slice), so
                # interrupts are taken at the same instruction as in the interpreter: cycle-exact
                out.append('    if (m68ki_remaining_cycles <= 0) { REG_PC = %s; return; }' % hx(nxt))
        out.append('}\n')
        per_file[ci % args.files].append('\n'.join(out))

    hdr = '/* Generated by tools/recomp/m68k_recomp.py from the KOF97 P1 ROM and Neo Geo BIOS. Do not edit. */\n#include "recomp_rt.h"\n\n'
    for i, parts in enumerate(per_file):
        with open(os.path.join(args.out, 'rc_%02d.c' % i), 'w') as f:
            f.write(hdr)
            f.write('\n'.join(parts))

    with open(os.path.join(args.out, 'rc_table.c'), 'w') as f:
        f.write(hdr)
        for fn in fnames:
            f.write('void %s(void);\n' % fn)
        f.write('\nconst rc_fn rc_functions[%d] = {\n' % len(fnames))
        for fn in fnames:
            f.write('    %s,\n' % fn)
        f.write('};\nconst unsigned rc_function_count = %d;\n\n' % len(fnames))
        ent = []
        for ci, (fa, lst) in enumerate(chunks):
            for a in lst:
                ent.append((a, ci))
        ent.sort()
        f.write('const uint32_t rc_entry_addr[%d] = {\n' % len(ent))
        for i in range(0, len(ent), 12):
            f.write('    ' + ', '.join(hx(a) for a, _ in ent[i:i + 12]) + ',\n')
        f.write('};\nconst uint16_t rc_entry_fn[%d] = {\n' % len(ent))
        for i in range(0, len(ent), 24):
            f.write('    ' + ', '.join(str(c) for _, c in ent[i:i + 24]) + ',\n')
        f.write('};\nconst unsigned rc_entry_count = %d;\n' % len(ent))

    # ---------------- differential tests ----------------
    seen = {}
    for a in addrs:
        d = insns[a]
        if d.helper or d.flow in ('call', 'ret', 'jump', 'cjump'):
            continue
        key = img.p1[a:a + d.length] if a < len(p1) else bios[a - BIOS_BASE:a - BIOS_BASE + d.length]
        if key not in seen:
            seen[key] = d
    tests = list(seen.values())
    with open(os.path.join(args.out, 'rc_tests.c'), 'w') as f:
        f.write(hdr)
        for i, d in enumerate(tests):
            f.write('/* %06x: %s */\n' % (d.addr, d.text.replace('*/', '* /')))
            f.write(gen_test(d, Ctx(set(), consts), cycles, i))
        f.write('\nconst rc_test rc_tests[%d] = {\n' % len(tests))
        for i, d in enumerate(tests):
            f.write('    { t_%d, 0x%x, %d },\n' % (i, d.addr, d.length))
        f.write('};\nconst unsigned rc_test_count = %d;\n' % len(tests))

    report = {
        'instructions': len(insns),
        'chunks': len(chunks),
        'seeds_total': len(seeds),
        'seeds_from_rea_and_vectors': n_rea,
        'bad_starts': len(bad),
        'native_by_kind': dict(sorted(native.items(), key=lambda kv: -kv[1])),
        'helper_by_kind': dict(sorted(helpers.items(), key=lambda kv: -kv[1])),
        'p1_instructions': sum(1 for a in insns if a < len(p1)),
        'bios_instructions': sum(1 for a in insns if a >= BIOS_BASE),
        'tests': len(tests),
    }
    json.dump(report, open(os.path.join(args.out, 'recomp_report.json'), 'w'), indent=1)
    print(json.dumps({k: v for k, v in report.items() if not isinstance(v, dict)}))
    print('native:', sum(native.values()), 'helper:', sum(helpers.values()))


if __name__ == '__main__':
    main()

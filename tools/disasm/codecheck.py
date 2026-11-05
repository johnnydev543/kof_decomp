#!/usr/bin/env python3
"""codecheck.py - find code that build/disasm/p1.s still emits as data (shift hazards the shift test cannot see).

Code that no test run executes and that static discovery misses is emitted as .byte/.long. Its pointers
to labelled code are relocated (.long L_x), but its absolute operands, branches and PC-relative
operands are raw bytes: in a shifted build they point to the old addresses. The shift test only sees
this when the code runs, so the rules in mkasm.py (unrolled_bases, spawn_handler_seeds) must catch it.

errors (exit 1): a D_ label used where the game needs code
  - stored with move.l #D_x into a handler field (mkasm.HANDLER_FIELDS), or a raw #address stored there
    (not symbolized: a shifted build keeps the old address)
  - loaded into A0 for obj_spawn / obj_spawn_child / obj_init_static or jsr/jmp (A0)
  - target of jsr/jmp/bsr/bra/bcc/dbcc
suspects (reported, not an error): data regions holding jsr/jmp abs.l (4eb9/4ef9) to a code label,
  with how the region is referenced - each needs a look (a table of code pointers, code after a data
  table, a list passed to a routine ...).

usage: codecheck.py [-v]   (after make matching)
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.dirname(__file__))

SPAWN = r'(obj_spawn|obj_spawn_child|obj_init_static)'
DATA_LABEL = re.compile(r'^(D_[0-9a-f]{6}):$')
CODE_LABEL = re.compile(r'^L_[0-9a-f]{6}:$')


def handler_fields():
    import mkasm
    return {('a%d' % r, d) for r, d in mkasm.HANDLER_FIELDS}


def main():
    verbose = '-v' in sys.argv
    lines = open(os.path.join(ROOT, 'build/disasm/p1.s')).read().split('\n')
    fields = handler_fields()
    errors = []
    for i, line in enumerate(lines):
        code = line.split('|')[0]
        m = re.match(r'\tmove\.l #(D_[0-9a-f]{6}),\((?:(0x[0-9a-f]+),)?%(a[0-7])\)', code)
        if m and (m.group(3), int(m.group(2) or '0', 16)) in fields:
            errors.append((m.group(1), 'stored into handler field (%s,%s)' % (m.group(2) or '0', m.group(3)), i))
        m = re.match(r'\tmove\.l #(0x[0-9a-f]+),\((?:(0x[0-9a-f]+),)?%(a[0-7])\)', code)
        if m and (m.group(3), int(m.group(2) or '0', 16)) in fields and 0x200 <= int(m.group(1), 16) < 0x100000 \
                and int(m.group(1), 16) & 0xff:     # round values are fixed-point parameters
            errors.append((m.group(1), 'code address not symbolized, stored into handler field (%s,%s)'
                           % (m.group(2) or '0', m.group(3)), i))
        m = re.match(r'\t(?:lea|movea?\.l) \(?#?(D_[0-9a-f]{6})(?:,%pc\)|\)\.l)?,%a0\s*$', code)
        if m:
            for j in range(i + 1, min(i + 13, len(lines))):
                c = lines[j].split('|')[0]
                if re.match(r'\t(jsr|bsr)(\.[sw])? \(?%s\b' % SPAWN, c) or re.match(r'\tj(sr|mp) \(%a0\)', c):
                    errors.append((m.group(1), 'A0 for ' + c.strip(), i))
                    break
                if re.search(r',%a0\s*$', c) or re.match(r'\t(rts|rte|jmp|bra)', c):
                    break
        m = re.match(r'\t(?:jsr|jmp|bsr|bra|b[a-z]{2}|db[a-z]{1,2})(?:\.[sw])? (?:[%a-z0-9]+,)?\(?(D_[0-9a-f]{6})\b', code)
        if m and ',%pc,' not in code:
            errors.append((m.group(1), 'branch/call target: ' + code.strip(), i))

    # suspects: jsr/jmp abs.l to a code label inside a data region
    region = None
    suspects = collections.OrderedDict()
    for i, line in enumerate(lines):
        m = DATA_LABEL.match(line)
        if m:
            region = m.group(1)
            continue
        if CODE_LABEL.match(line):
            region = None
            continue
        if region and line.startswith('\t.byte') and re.search(r'0x4e,0x(b9|f9)$', line.strip()) and \
                i + 1 < len(lines) and re.match(r'\t\.long (?!D_)[A-Za-z_]', lines[i + 1]):
            suspects.setdefault(region, 0)
            suspects[region] += 1
    refs = collections.defaultdict(list)
    names = set(suspects)
    for line in lines:
        for d in set(re.findall(r'\b(D_[0-9a-f]{6})\b', line)) & names:
            if not line.startswith(d + ':'):
                refs[d].append(line.split('|')[0].strip())

    print('errors (D_ labels used as code): %d' % len(errors))
    for d, why, i in errors:
        print('  %s  %s  (p1.s:%d)' % (d, why, i + 1))
    print('suspect data regions (jsr/jmp abs.l to code inside): %d' % len(suspects))
    if verbose:
        for d, n in suspects.items():
            print('  %s  %d jsr/jmp  refs: %s' % (d, n, '; '.join(refs[d][:3]) or '(none)'))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())

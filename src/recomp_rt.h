/* Runtime interface shared by the generated 68000->C code (build/recomp/rc_*.c) and src/cpu.c.
 * Generated code operates directly on Musashi's CPU state (m68ki_cpu) so recompiled code and the
 * Musashi interpreter can hand over at any instruction boundary. */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H
#include <stdint.h>
#include "m68kcpu.h"
#include "m68kops.h"

typedef void (*rc_fn)(void);
typedef struct { void (*fn)(void); uint32_t addr; uint32_t len; } rc_test;

extern const rc_fn rc_functions[];
extern const unsigned rc_function_count;
extern const uint32_t rc_entry_addr[];
extern const uint16_t rc_entry_fn[];
extern const unsigned rc_entry_count;

/* 68000 MULU/MULS data-dependent timing, as in Musashi */
static inline int rc_mulu_cycles(uint src)
{
    int c = 0;
    for (uint y = src & 0xffff; y; y >>= 1) if (y & 1) c += 2;
    return c;
}
static inline int rc_muls_cycles(uint src)
{
    int c = 0;
    uint f = 0;
    for (uint y = (uint)MAKE_INT_16(src); y; y >>= 1) if ((y & 1) != f) { c += 2; f = 1 - f; }
    return c;
}

extern const rc_test rc_tests[];
extern const unsigned rc_test_count;
#endif

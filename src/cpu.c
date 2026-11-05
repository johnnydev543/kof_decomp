/* Dispatcher: runs statically recompiled 68000 code (build/recomp) and falls back to the Musashi
 * interpreter for any address that was not discovered at recompile time (RAM code, pointer-table
 * targets the static analysis missed, ...). Misses are logged so the next recompile can include them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "recomp_rt.h"

#define P1_SPAN   0x100000u
#define BIOS_BASE 0xC00000u
#define BIOS_SPAN 0x20000u

static uint16_t lut_p1[P1_SPAN / 2];     /* fn index + 1, 0 = not recompiled */
static uint16_t lut_bios[BIOS_SPAN / 2];
static int cpu_mode;
static cpu_stats_t stats;
static int carry_cycles;

static const char *miss_path;
static uint8_t *miss_seen;               /* bitmap over (p1 + bios) halfwords */
static uint32_t *miss_list; static size_t miss_n, miss_cap;

void cpu_init(int mode, const char *miss_log_path)
{
    cpu_mode = mode;
    memset(lut_p1, 0, sizeof lut_p1);
    memset(lut_bios, 0, sizeof lut_bios);
    for (unsigned i = 0; i < rc_entry_count; i++) {
        uint32_t a = rc_entry_addr[i];
        if (a < P1_SPAN) lut_p1[a >> 1] = rc_entry_fn[i] + 1;
        else if (a >= BIOS_BASE && a < BIOS_BASE + BIOS_SPAN) lut_bios[(a - BIOS_BASE) >> 1] = rc_entry_fn[i] + 1;
    }
    miss_path = miss_log_path;
    if (miss_path) miss_seen = calloc((P1_SPAN + BIOS_SPAN) / 16, 1);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

void cpu_reset(void)
{
    m68k_pulse_reset();
    carry_cycles = 0;
}

static inline rc_fn lookup(uint32_t pc)
{
    uint16_t f;
    if (pc < P1_SPAN) f = lut_p1[pc >> 1];
    else if (pc - BIOS_BASE < BIOS_SPAN) f = lut_bios[(pc - BIOS_BASE) >> 1];
    else return NULL;
    return f ? rc_functions[f - 1] : NULL;
}

static void note_miss(uint32_t pc)
{
    uint32_t idx;
    if (!miss_seen) return;
    if (pc < P1_SPAN) idx = pc >> 1;
    else if (pc - BIOS_BASE < BIOS_SPAN) idx = (P1_SPAN + pc - BIOS_BASE) >> 1;
    else return;                                  /* RAM etc: never recompiled */
    if (miss_seen[idx >> 3] & (1 << (idx & 7))) return;
    miss_seen[idx >> 3] |= 1 << (idx & 7);
    if (miss_n == miss_cap) { miss_cap = miss_cap ? miss_cap * 2 : 1024; miss_list = realloc(miss_list, miss_cap * 4); }
    miss_list[miss_n++] = pc;
}

void (*cpu_debug_hook)(uint32_t pc);

/* --exec-log: bitmap of executed P1 instruction addresses (one bit per halfword), interpreter only */
static uint8_t *exec_map;
static const char *exec_path;

extern uint8_t *ng_rd_long, *ng_rd_word;   /* neogeo.c read maps */
extern uint32_t *ng_long_src;
static uint8_t *ptr_src;                     /* P1 longwords proven to be code pointers */

void cpu_set_exec_log(const char *path)
{
    exec_path = path;
    exec_map = calloc(P1_SPAN / 16, 1);
    ng_rd_long = calloc(P1_SPAN / 16, 1);
    ng_rd_word = calloc(P1_SPAN / 16, 1);
    ng_long_src = calloc(P1_SPAN / 2, sizeof *ng_long_src);
    ptr_src = calloc(P1_SPAN / 16, 1);
}

static inline void interp_one(void)
{
    if (cpu_debug_hook) cpu_debug_hook(REG_PC);
    if (exec_map) {
        uint32_t a = REG_PC & 0xffffff;      /* 24-bit bus: code also runs via flagged PCs (0x80079202) */
        if (a < P1_SPAN) exec_map[a >> 4] |= (uint8_t)(1 << ((a >> 1) & 7));
    }
    REG_PPC = REG_PC;
    REG_IR = m68ki_read_imm_16();
    m68ki_instruction_jump_table[REG_IR]();
    USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
    stats.interp_insns++;
    if (ptr_src && (REG_IR & 0xff80) == 0x4e80) {
        /* jsr/jmp through (An), (d16,An), (d8,An,Xn): if the target was read from P1 as a longword,
         * that longword is a code pointer */
        unsigned mode = (REG_IR >> 3) & 7;
        uint32_t t = REG_PC & 0xffffff;
        if ((mode == 2 || mode == 5 || mode == 6) && t < P1_SPAN && ng_long_src[t >> 1]) {
            uint32_t k = ng_long_src[t >> 1] - 1;
            ptr_src[k >> 4] |= (uint8_t)(1 << ((k >> 1) & 7));
        }
    }
}

int cpu_execute(int cycles)
{
    int budget = cycles + carry_cycles;
    SET_CYCLES(budget);
    for (;;) {
        m68ki_check_interrupts();
        if (CPU_STOPPED) { SET_CYCLES(0); break; }
        if (GET_CYCLES() <= 0) break;
        if (cpu_mode == CPU_MODE_RECOMP) {
            rc_fn fn = lookup(REG_PC);
            if (fn) { stats.rc_calls++; fn(); continue; }
            stats.interp_entries++;
            note_miss(REG_PC);
            /* interpret until we reach recompiled code again */
            do { interp_one(); } while (GET_CYCLES() > 0 && !lookup(REG_PC) && !CPU_STOPPED);
        } else {
            interp_one();
        }
    }
    carry_cycles = GET_CYCLES();     /* <= 0: overshoot is repaid next slice */
    if (carry_cycles > 0) carry_cycles = 0;
    return budget - GET_CYCLES();
}

void cpu_set_irq(int level) { m68k_set_irq(level); }
uint32_t cpu_pc(void) { return REG_PC; }
int cpu_cycles_carry(void) { return carry_cycles; }
const cpu_stats_t *cpu_stats(void) { return &stats; }

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* file = 4 bitmaps of P1_SPAN/16 bytes: executed instruction starts, 32-bit read starts, 16/8-bit reads,
 * longwords whose value was the target of an indirect jsr/jmp */
static void write_exec_log(void)
{
    if (!exec_map) return;
    uint8_t *maps[4] = { exec_map, ng_rd_long, ng_rd_word, ptr_src };
    FILE *f = fopen(exec_path, "rb");        /* merge with an existing log */
    if (f) {
        static uint8_t old[P1_SPAN / 16];
        for (int m = 0; m < 4; m++)
            if (fread(old, 1, sizeof old, f) == sizeof old)
                for (size_t i = 0; i < sizeof old; i++) maps[m][i] |= old[i];
        fclose(f);
    }
    f = fopen(exec_path, "wb");
    if (!f) { perror(exec_path); return; }
    for (int m = 0; m < 4; m++) fwrite(maps[m], 1, P1_SPAN / 16, f);
    fclose(f);
}

void cpu_shutdown(void)
{
    write_exec_log();
    if (!miss_path || !miss_n) return;
    /* merge with existing file */
    FILE *f = fopen(miss_path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            uint32_t a = (uint32_t)strtoul(line, NULL, 16);
            if (line[0] == '#' || !a) continue;
            if (miss_n == miss_cap) { miss_cap = miss_cap ? miss_cap * 2 : 1024; miss_list = realloc(miss_list, miss_cap * 4); }
            miss_list[miss_n++] = a;
        }
        fclose(f);
    }
    qsort(miss_list, miss_n, 4, cmp_u32);
    f = fopen(miss_path, "w");
    if (!f) { perror(miss_path); return; }
    fprintf(f, "# 68000 entry points reached at runtime but not recompiled (input for tools/recomp/m68k_recomp.py)\n");
    for (size_t i = 0; i < miss_n; i++)
        if (i == 0 || miss_list[i] != miss_list[i - 1]) fprintf(f, "%06x\n", miss_list[i]);
    fclose(f);
}

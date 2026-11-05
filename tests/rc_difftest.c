/* Differential test: every natively translated instruction from build/recomp/rc_tests.c is executed
 * on random CPU states and a synthetic memory, once by Musashi and once by the generated C code.
 * Registers, SR, cycle counts and the sequence of memory writes must match exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_rt.h"

static uint8_t *p1, *bios; static long p1n, biosn;
static uint32_t ins_addr, ins_len, seed;
typedef struct { uint32_t a; uint8_t v; } wr_t;
static wr_t wlog[256]; static int wn;

static uint8_t rom_byte(uint32_t a)
{
    if (a < (uint32_t)p1n) return p1[a];
    if (a >= 0xc00000 && a < 0xc00000u + (uint32_t)biosn) return bios[a - 0xc00000];
    return 0;
}
static uint8_t mem_byte(uint32_t a)
{
    a &= 0xffffff;
    if (a - ins_addr < ins_len) return rom_byte(a);
    for (int i = wn - 1; i >= 0; i--) if (wlog[i].a == a) return wlog[i].v;
    uint32_t h = (a * 2654435761u) ^ seed;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return (uint8_t)h;
}
static void wb(uint32_t a, uint8_t v) { if (wn < 256) { wlog[wn].a = a & 0xffffff; wlog[wn].v = v; wn++; } }
unsigned int m68k_read_memory_8(unsigned int a) { return mem_byte(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (mem_byte(a) << 8) | mem_byte(a + 1); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { wb(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { wb(a, (uint8_t)(v >> 8)); wb(a + 1, (uint8_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v & 0xffff); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return (rom_byte(a) << 8) | rom_byte(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return (m68k_read_disassembler_16(a) << 16) | m68k_read_disassembler_16(a + 2); }

static unsigned char *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)*n);
    if (fread(d, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f); return d;
}

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 64;
    p1 = load("build/roms/p1.be", &p1n);
    bios = load("build/roms/bios.be", &biosn);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68ki_cpu_core base = m68ki_cpu, s0, ra;
    wr_t wa[256]; int na;
    unsigned fails = 0, failed_tests = 0;
    for (unsigned t = 0; t < rc_test_count; t++) {
        const rc_test *T = &rc_tests[t];
        int tfail = 0;
        for (int it = 0; it < iters && !tfail; it++) {
            m68ki_cpu = base;
            uint32_t sr = 0x2000 | (rnd() & 0x071f);           /* supervisor, random mask and CCR */
            m68ki_set_sr(sr);
            for (int r = 0; r < 16; r++) {
                uint32_t v = rnd();
                if (it & 1) v &= 0xff00ffff;                       /* also exercise small values */
                if (it & 2) v = (v & 3) ? v : 0;
                REG_DA[r] = v;
            }
            seed = rnd();
            ins_addr = T->addr; ins_len = T->len;
            s0 = m68ki_cpu;
            /* Musashi */
            wn = 0;
            m68ki_remaining_cycles = 1000;
            REG_PC = T->addr; REG_PPC = T->addr;
            REG_IR = m68ki_read_imm_16();
            m68ki_instruction_jump_table[REG_IR]();
            USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
            ra = m68ki_cpu; int ca = m68ki_remaining_cycles; unsigned sra = m68ki_get_sr();
            na = wn; memcpy(wa, wlog, sizeof wlog);
            /* recompiled */
            m68ki_cpu = s0;
            wn = 0;
            m68ki_remaining_cycles = 1000;
            REG_PC = T->addr;
            T->fn();
            int cb = m68ki_remaining_cycles; unsigned srb = m68ki_get_sr();
            int bad = 0;
            for (int r = 0; r < 16; r++) if (ra.dar[r] != m68ki_cpu.dar[r]) bad = 1;
            if (sra != srb || ca != cb || na != wn) bad = 1;
            for (int i = 0; !bad && i < wn; i++) if (wa[i].a != wlog[i].a || wa[i].v != wlog[i].v) bad = 1;
            if (bad) {
                char dis[128];
                m68k_disassemble(dis, T->addr, M68K_CPU_TYPE_68000);
                if (failed_tests < 40) {
                    printf("FAIL %06x %-32s sr %04x/%04x cyc %d/%d writes %d/%d\n", T->addr, dis, sra, srb, ca, cb, na, wn);
                    for (int r = 0; r < 16; r++)
                        if (ra.dar[r] != m68ki_cpu.dar[r])
                            printf("     %c%d musashi %08x recomp %08x (before %08x)\n", r < 8 ? 'D' : 'A', r & 7, ra.dar[r], m68ki_cpu.dar[r], s0.dar[r]);
                }
                fails++; tfail = 1;
            }
        }
        failed_tests += tfail;
    }
    printf("%u instruction tests x %d random states: %u failed\n", rc_test_count, iters, failed_tests);
    return failed_tests != 0;
}

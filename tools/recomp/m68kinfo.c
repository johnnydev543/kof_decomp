/* m68kinfo: Musashi-backed helper for the static recompiler.
 *   m68kinfo table                      -> 65536 lines: "<valid> <cycles>" for MC68000
 *   m68kinfo consts                     -> 68000 timing constants used by generated code
 *   m68kinfo disasm <p1.be> <bios.be>   -> reads hex addresses on stdin, prints "addr<TAB>len<TAB>text"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "m68kcpu.h"

static unsigned char *p1, *bios; static long p1n, biosn;
static unsigned char rb(unsigned a) {
    a &= 0xffffff;
    if (a < (unsigned)p1n) return p1[a];
    if (a >= 0xc00000 && a < 0xc00000u + biosn) return bios[a - 0xc00000];
    return 0;
}
unsigned int m68k_read_memory_8(unsigned int a) { return rb(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (rb(a) << 8) | rb(a + 1); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }

static unsigned char *load(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(*n); if (fread(d, 1, *n, f) != (size_t)*n) exit(1); fclose(f); return d;
}

int main(int argc, char **argv) {
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    if (argc >= 2 && !strcmp(argv[1], "table")) {
        for (unsigned op = 0; op < 0x10000; op++)
            printf("%u %u\n", m68k_is_valid_instruction(op, M68K_CPU_TYPE_68000), (unsigned)m68ki_cpu.cyc_instruction[op]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "consts")) {
        printf("bcc_notake_b %d\nbcc_notake_w %d\ndbcc_f_noexp %d\ndbcc_f_exp %d\nscc_r_true %d\nshift %d\n",
               (int)m68ki_cpu.cyc_bcc_notake_b, (int)m68ki_cpu.cyc_bcc_notake_w, (int)m68ki_cpu.cyc_dbcc_f_noexp,
               (int)m68ki_cpu.cyc_dbcc_f_exp, (int)m68ki_cpu.cyc_scc_r_true, (int)m68ki_cpu.cyc_shift);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "disasm")) {
        p1 = load(argv[2], &p1n); bios = load(argv[3], &biosn);
        char line[64], buf[256];
        while (fgets(line, sizeof line, stdin)) {
            unsigned a = (unsigned)strtoul(line, NULL, 16);
            unsigned len = m68k_disassemble(buf, a, M68K_CPU_TYPE_68000);
            printf("%x\t%u\t%s\n", a, len, buf);
            fflush(stdout);
        }
        return 0;
    }
    fprintf(stderr, "usage: m68kinfo table|consts|disasm p1 bios\n");
    return 1;
}

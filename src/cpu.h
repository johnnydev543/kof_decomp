/* 68000 execution: dispatcher over statically recompiled code with Musashi interpreter fallback. */
#ifndef CPU_H
#define CPU_H
#include <stdint.h>

typedef struct {
    uint64_t rc_calls;        /* recompiled chunk invocations */
    uint64_t interp_insns;    /* instructions executed by the Musashi fallback */
    uint64_t interp_entries;  /* times the dispatcher fell back to the interpreter */
} cpu_stats_t;

enum { CPU_MODE_RECOMP = 0, CPU_MODE_INTERP = 1 };

void cpu_init(int mode, const char *miss_log_path);
void cpu_reset(void);
int  cpu_execute(int cycles);          /* returns cycles actually used */
void cpu_set_irq(int level);           /* highest pending autovector level, 0 = none */
void cpu_set_exec_log(const char *path); /* --exec-log: executed P1 addresses (interpreter) */
void cpu_shutdown(void);               /* flush miss log */
const cpu_stats_t *cpu_stats(void);
uint32_t cpu_pc(void);
int cpu_cycles_carry(void);           /* cycles overrun in the last slice (<= 0) */
extern void (*cpu_debug_hook)(uint32_t pc);  /* called before each interpreted instruction (--break) */
#endif

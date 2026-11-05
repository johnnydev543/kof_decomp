/* Neo Geo MVS system emulation around the recompiled 68000 program. */
#ifndef NEOGEO_H
#define NEOGEO_H
#include <stdint.h>
#include <stddef.h>

#define NG_SCREEN_W 320
#define NG_SCREEN_H 224
#define NG_LINES 264
#define NG_VBSTART 240          /* first vblank line (VBlank IRQ) */
#define NG_VBEND 16             /* first visible line */
extern int ng_frame_render;
extern void (*ng_sound_log)(int frame, int dir, uint8_t val);   /* dir 0: 68k -> Z80 command, 1: Z80 reply */
#define NG_CYCLES_PER_LINE 768  /* 12 MHz 68000, 384 pixel clocks per line at 6 MHz */
#define NG_Z80_CYCLES_PER_LINE 256
#define NG_WATCHDOG_CYCLES (3244030 / 2)   /* 24 MHz master clocks -> 68000 cycles */
#define NG_FPS (6000000.0 / (384.0 * 264.0))   /* 59.1856 Hz */

/* Input bits (active high here; the hardware registers are active low). */
enum {
    NG_UP = 0x01, NG_DOWN = 0x02, NG_LEFT = 0x04, NG_RIGHT = 0x08,
    NG_A = 0x10, NG_B = 0x20, NG_C = 0x40, NG_D = 0x80,
};
enum {
    NG_SYS_START1 = 0x01, NG_SYS_SELECT1 = 0x02, NG_SYS_START2 = 0x04, NG_SYS_SELECT2 = 0x08,
    NG_SYS_COIN1 = 0x10, NG_SYS_COIN2 = 0x20, NG_SYS_SERVICE = 0x40, NG_SYS_TEST = 0x80,
};

typedef struct {
    uint8_t *data;
    size_t size;
} ng_blob;

typedef struct {
    ng_blob p1, p2;      /* program ROMs, byte-swapped to big-endian */
    ng_blob bios;        /* MVS BIOS, big-endian */
    ng_blob s1, sfix;    /* fix layer ROMs */
    ng_blob m1, sm1;     /* Z80 ROMs */
    ng_blob vrom;        /* ADPCM v1..v3 concatenated */
    ng_blob crom;        /* sprite ROMs, MAME "sprites" region layout (c1/c2 byte interleaved, ...) */
    ng_blob lo;          /* L0 zoom table ROM */
} ng_roms;

typedef struct {
    /* memory */
    uint8_t ram[0x10000];
    uint8_t backup[0x10000];
    uint16_t vram[0x10000];
    uint16_t pal[2][0x1000];
    uint32_t pens[2][0x1000];       /* ARGB8888 of pal[] */
    /* system latches */
    int use_cart_vectors, use_cart_fix, sram_unlocked, pal_bank, shadow;
    uint32_t p2_bank_off;
    /* LSPC */
    uint16_t vram_addr, vram_mod, vram_read_buf;
    int auto_anim_speed, auto_anim_disabled, auto_anim_frame_counter;
    unsigned auto_anim_counter;
    int irq2_ctrl;
    uint32_t timer_reload;          /* REG_TIMERHIGH/LOW */
    int64_t timer_left;             /* pixel clocks until IRQ2, <0 = stopped */
    int irq_vblank, irq_timer, irq_reset;
    int line;                       /* current scanline 0..263 */
    int line_cycle;                 /* 68000 cycles elapsed in current line */
    /* input */
    uint8_t in_p1, in_p2, in_sys;
    uint8_t dipsw;
    /* RTC (uPD4990A) */
    struct { int clk, stb, din; uint8_t cmd_shift, mode; uint64_t shift; int tp_div; } rtc;
    uint64_t frame;
    uint64_t total_cycles;
    int watchdog;
    int watchdog_resets;
} ng_state;

extern ng_state ng;
extern ng_roms ngr;
extern uint32_t ng_watch_lo, ng_watch_hi;     /* work RAM write watch (debug, --watch) */
extern uint32_t ng_vwatch_lo, ng_vwatch_hi;   /* VRAM write watch (word addresses, --watch-vram) */
extern long long ng_rtc_base;                 /* --rtc-fixed: deterministic calendar epoch, -1 = host clock */

int  ng_load_roms(const char *dir);           /* expects files produced by tools/romprep.py */
void ng_init(int cpu_mode, const char *miss_log);
void ng_reset(void);
void ng_run_frame(uint32_t *fb);              /* fb: NG_SCREEN_W*NG_SCREEN_H ARGB8888 */
void ng_set_input(uint8_t p1, uint8_t p2, uint8_t sys);
int  ng_load_nvram(const char *path);
int  ng_save_nvram(const char *path);
void ng_shutdown(void);

/* video.c */
void video_init(void);
void video_render_line(int line, uint32_t *fb);
void video_update_pen(int bank, int idx);

#endif

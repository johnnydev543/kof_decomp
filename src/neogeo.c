/* Neo Geo MVS hardware around the recompiled 68000 code: memory map, I/O, LSPC timer/IRQs,
 * uPD4990A calendar, backup RAM and frame timing. Hardware behaviour follows MAME's neogeo driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "neogeo.h"
#include "cpu.h"
#include "snd_native.h"
#include "audio.h"
#include "m68k.h"

ng_state ng;
ng_roms ngr;
uint32_t ng_watch_lo = 1, ng_watch_hi = 0;   /* empty range = watch off */
uint32_t ng_vwatch_lo = 1, ng_vwatch_hi = 0;
long long ng_rtc_base = -1;                   /* >= 0: calendar = this epoch + emulated time (deterministic) */

/* ------------------------------------------------------------------------------------------ */
/* ROM loading (files normalised by tools/romprep.py)                                          */
/* ------------------------------------------------------------------------------------------ */

static int load_blob(const char *dir, const char *name, ng_blob *b, int required)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (required) fprintf(stderr, "missing ROM file %s (run tools/romprep.py)\n", path);
        return !required;
    }
    fseek(f, 0, SEEK_END);
    b->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b->data = malloc(b->size);
    if (!b->data || fread(b->data, 1, b->size, f) != b->size) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

int ng_load_roms(const char *dir)
{
    return load_blob(dir, "p1.bin", &ngr.p1, 1) && load_blob(dir, "p2.bin", &ngr.p2, 0) &&
           load_blob(dir, "bios.bin", &ngr.bios, 1) && load_blob(dir, "s1.bin", &ngr.s1, 1) &&
           load_blob(dir, "sfix.bin", &ngr.sfix, 1) && load_blob(dir, "m1.bin", &ngr.m1, 1) &&
           load_blob(dir, "sm1.bin", &ngr.sm1, 1) && load_blob(dir, "v.bin", &ngr.vrom, 1) &&
           load_blob(dir, "c.bin", &ngr.crom, 1) && load_blob(dir, "lo.bin", &ngr.lo, 1);
}

/* ------------------------------------------------------------------------------------------ */
/* REG_SOUND: command bytes to the sound CPU (optionally logged with --sound-log) */
void (*ng_sound_log)(int frame, int dir, uint8_t val);
static void sound_command(uint8_t cmd)
{
    if (ng_sound_log) ng_sound_log((int)ng.frame, 0, cmd);
    if (snd_native_enabled()) snd_native_command(cmd);
    else audio_write_command(cmd);
}

/* Interrupts and LSPC timer                                                                  */
/* ------------------------------------------------------------------------------------------ */

static int slice_len;                 /* cycles requested for the running cpu_execute slice */
extern int m68ki_remaining_cycles;
static int slice_consumed(void) { int c = slice_len - m68ki_remaining_cycles; return c < 0 ? 0 : c; }

static void update_irq(void)
{
    int level = ng.irq_reset ? 3 : ng.irq_timer ? 2 : ng.irq_vblank ? 1 : 0;
    cpu_set_irq(level);
}

/* timer_left is kept in 68000 cycles measured from the start of the current slice */
static void timer_reload_now(void)
{
    if (ng.timer_reload + 1 == 0) return;
    ng.timer_left = ((int64_t)ng.timer_reload + 1) * 2 + slice_consumed();
}

static void timer_fire(void)
{
    if (ng.irq2_ctrl & 0x10) { ng.irq_timer = 1; update_irq(); }
    if (ng.irq2_ctrl & 0x80) {
        int64_t over = ng.timer_left;   /* <= 0 */
        ng.timer_left = ((int64_t)ng.timer_reload + 1) * 2 + over;
        if (ng.timer_left <= 0) ng.timer_left = 1;
    } else {
        ng.timer_left = -1;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* uPD4990A real time clock (MVS calendar)                                                     */
/* ------------------------------------------------------------------------------------------ */

static uint64_t emu_cycles_now(void) { return ng.total_cycles + (uint64_t)slice_consumed(); }

static int rtc_tp(void)
{
    /* TP output: commands 4-7 select 64/256/2048/4096 Hz, 8-B select the 1/10/30/60 s interval timer.
     * The MVS BIOS selects the 1 s interval and expects ~60 VBlanks between TP rising edges. */
    static const uint64_t period[8] = { 12000000ull / 64, 12000000ull / 256, 12000000ull / 2048, 12000000ull / 4096,
                                        12000000ull, 120000000ull, 360000000ull, 720000000ull };
    uint64_t p = period[ng.rtc.tp_div & 7];
    return (int)((emu_cycles_now() % p) < p / 2);
}

static int rtc_one_hz(void) { return (int)((emu_cycles_now() / 6000000ull) & 1); }

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_load_time(void)
{
    time_t t;
    struct tm tm;
    if (ng_rtc_base >= 0) {
        t = (time_t)(ng_rtc_base + (long long)((double)ng.frame / NG_FPS));
        gmtime_r(&t, &tm);
    } else {
        t = time(NULL);
        localtime_r(&t, &tm);
    }
    uint64_t s = 0;
    s |= (uint64_t)bcd(tm.tm_sec);
    s |= (uint64_t)bcd(tm.tm_min) << 8;
    s |= (uint64_t)bcd(tm.tm_hour) << 16;
    s |= (uint64_t)bcd(tm.tm_mday) << 24;
    s |= (uint64_t)(tm.tm_wday & 0xf) << 32;
    s |= (uint64_t)((tm.tm_mon + 1) & 0xf) << 36;
    s |= (uint64_t)bcd(tm.tm_year % 100) << 40;
    ng.rtc.shift = s;
}

static int rtc_data_out(void) { return ng.rtc.mode == 1 ? (int)(ng.rtc.shift & 1) : rtc_one_hz(); }

static void rtc_write(int din, int clk, int stb)
{
    if (clk && !ng.rtc.clk) {
        if (ng.rtc.mode == 1)
            ng.rtc.shift = (ng.rtc.shift >> 1) | ((uint64_t)(din & 1) << 51);
        ng.rtc.cmd_shift = (uint8_t)((ng.rtc.cmd_shift >> 1) | ((din & 1) << 3));
    }
    if (stb && !ng.rtc.stb) {
        int c = ng.rtc.cmd_shift & 0xf;
        switch (c) {
        case 0: ng.rtc.mode = 0; break;                       /* register hold */
        case 1: ng.rtc.mode = 1; break;                       /* register shift */
        case 2: ng.rtc.mode = 0; break;                       /* time set (ignored, host clock) */
        case 3: ng.rtc.mode = 0; rtc_load_time(); break;      /* time read */
        case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: ng.rtc.tp_div = c - 4; break;
        default: break;                                       /* interval timer modes */
        }
    }
    ng.rtc.din = din; ng.rtc.clk = clk; ng.rtc.stb = stb;
}

/* ------------------------------------------------------------------------------------------ */
/* I/O registers                                                                              */
/* ------------------------------------------------------------------------------------------ */

static uint8_t reg_p1(void) { return (uint8_t)~ng.in_p1; }
static uint8_t reg_p2(void) { return (uint8_t)~ng.in_p2; }
static uint8_t reg_status_a(void)
{
    uint8_t v = 0x38;                                           /* coin3/4, 4-slot detect inactive */
    if (!(ng.in_sys & NG_SYS_COIN1)) v |= 0x01;
    if (!(ng.in_sys & NG_SYS_COIN2)) v |= 0x02;
    if (!(ng.in_sys & NG_SYS_SERVICE)) v |= 0x04;
    if (rtc_tp()) v |= 0x40;
    if (rtc_data_out()) v |= 0x80;
    return v;
}
static uint8_t reg_status_b(void)
{
    uint8_t v = 0xf0;                                           /* no memory card, MVS */
    v |= (uint8_t)(~ng.in_sys & 0x0f);
    return v;
}
static uint8_t reg_systype(void) { return (uint8_t)(0x40 | ((ng.in_sys & NG_SYS_TEST) ? 0 : 0x80)); }

static uint16_t lspc_read(uint32_t a)
{
    switch ((a >> 1) & 3) {
    case 0: case 1: return ng.vram_read_buf;
    case 2: return ng.vram_mod;
    default: {
        int v = ng.line + 0x100;
        if (v >= 0x200) v -= NG_LINES;
        return (uint16_t)((v << 7) | (ng.auto_anim_counter & 7));
    }
    }
}

static void vram_set_addr(uint16_t data)
{
    ng.vram_addr = (data & 0x8000) ? (data & 0x87ff) : data;
    ng.vram_read_buf = ng.vram[ng.vram_addr];
}

static void lspc_write(uint32_t a, uint16_t data)
{
    switch ((a >> 1) & 7) {
    case 0: vram_set_addr(data); break;
    case 1:
        if (ng.vram_addr >= ng_vwatch_lo && ng.vram_addr <= ng_vwatch_hi)
            fprintf(stderr, "vwatch frame %llu ppc %06x vram %04x = %04x\n",
                    (unsigned long long)ng.frame, m68k_get_reg(NULL, M68K_REG_PPC), ng.vram_addr, data);
        ng.vram[ng.vram_addr] = data;
        vram_set_addr((uint16_t)((ng.vram_addr & 0x8000) | ((ng.vram_addr + ng.vram_mod) & 0x7fff)));
        break;
    case 2: ng.vram_mod = data; break;
    case 3:
        ng.auto_anim_speed = data >> 8;
        ng.auto_anim_disabled = data & 0x0008;
        ng.irq2_ctrl = data & 0x00f0;
        break;
    case 4: ng.timer_reload = (ng.timer_reload & 0x0000ffff) | ((uint32_t)data << 16); break;
    case 5:
        ng.timer_reload = (ng.timer_reload & 0xffff0000) | data;
        if (ng.irq2_ctrl & 0x20) timer_reload_now();
        break;
    case 6:
        if (data & 1) ng.irq_reset = 0;
        if (data & 2) ng.irq_timer = 0;
        if (data & 4) ng.irq_vblank = 0;
        update_irq();
        break;
    default: break;                                              /* timer stop (PAL) */
    }
}

static void system_control_w(uint32_t a)
{
    int offset = (a & 0x1f) >> 1;
    int bit = (offset >> 3) & 1;
    switch (offset & 7) {
    case 0: ng.shadow = bit; break;
    case 1: ng.use_cart_vectors = bit; break;
    case 5: ng.use_cart_fix = bit; audio_set_cart_rom(bit); break;
    case 6: ng.sram_unlocked = bit; break;
    case 7: ng.pal_bank = bit ? 0 : 1; break;                  /* 0x3A000F = PALBANK1, 0x3A001F = PALBANK0 */
    default: break;
    }
}

static void io_control_w(uint32_t a, uint8_t data)
{
    switch ((a & 0x7f) >> 1) {
    case 0x28: rtc_write(data & 1, (data >> 1) & 1, (data >> 2) & 1); break;
    default: break;                                              /* controller select, LEDs, coin counters */
    }
}

static uint8_t io_read8(uint32_t a)
{
    switch ((a >> 16) & 0xe) {
    case 0x0: if (a & 0x80) return (a & 1) ? reg_systype() : 0xff;
              return (a & 1) ? ng.dipsw : reg_p1();
    case 0x2: return (a & 1) ? reg_status_a() : snd_native_enabled() ? snd_native_reply() : audio_read_reply();
    case 0x4: return (a & 1) ? 0xff : reg_p2();
    case 0x8: return (a & 1) ? 0xff : reg_status_b();
    case 0xc: { uint16_t w = lspc_read(a); return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8); }
    default: return 0xff;
    }
}

static uint16_t io_read16(uint32_t a)
{
    if (((a >> 16) & 0xe) == 0xc) return lspc_read(a);
    return (uint16_t)((io_read8(a & ~1u) << 8) | io_read8(a | 1));
}

static void io_write(uint32_t a, uint16_t data, int is_byte)
{
    switch ((a >> 16) & 0xe) {
    case 0x0: if (!is_byte || (a & 1)) ng.watchdog = 0; break;
    case 0x2: if (!is_byte) sound_command((uint8_t)(data >> 8)); else if (!(a & 1)) sound_command((uint8_t)data); break;
    case 0x8: if (!is_byte || (a & 1)) io_control_w(a | 1, (uint8_t)data); break;
    case 0xa: if (!is_byte || (a & 1)) system_control_w(a | 1); break;
    case 0xc:
        if (is_byte) data = (uint16_t)((data & 0xff) | (data << 8));
        lspc_write(a, data);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* 68000 memory map (Musashi callbacks, also used by the recompiled code)                     */
/* ------------------------------------------------------------------------------------------ */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void pal_write(uint32_t a, uint16_t data, uint16_t mask)
{
    int idx = (a >> 1) & 0xfff;
    uint16_t *p = &ng.pal[ng.pal_bank][idx];
    *p = (uint16_t)((*p & ~mask) | (data & mask));
    video_update_pen(ng.pal_bank, idx);
}

/* --exec-log read maps (allocated by cpu_set_exec_log): one bit per P1 halfword, set when a 32-bit read
 * starts there (ng_rd_long) or a 16/8-bit read touches it (ng_rd_word); used by mkasm.py to tell
 * pointers (read with move.l/movea.l) from word data */
uint8_t *ng_rd_long, *ng_rd_word;
uint32_t *ng_long_src;     /* [value24 >> 1] = P1 address + 1 of the last 32-bit read that returned it */
static int in_long_read;
#define RD_MARK(map, a) ((map)[(a) >> 4] |= (uint8_t)(1 << (((a) >> 1) & 7)))

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= 0xfffffe;
    if (ng_rd_word && !in_long_read && a < 0x100000) RD_MARK(ng_rd_word, a);
    switch (a >> 20) {
    case 0x0:
        if (a < 0x80 && !ng.use_cart_vectors) return be16(ngr.bios.data + a);
        return a < ngr.p1.size ? be16(ngr.p1.data + a) : 0xffff;
    case 0x1: return be16(ng.ram + (a & 0xffff));
    case 0x2: {
        uint32_t o = ng.p2_bank_off + (a & 0xfffff);
        return o < ngr.p2.size ? be16(ngr.p2.data + o) : 0xffff;
    }
    case 0x3: return io_read16(a);
    case 0x4: case 0x5: case 0x6: case 0x7: return ng.pal[ng.pal_bank][(a >> 1) & 0xfff];
    case 0xc: return be16(ngr.bios.data + (a & (ngr.bios.size - 1)));
    case 0xd: return be16(ng.backup + (a & 0xffff));
    default: return 0xffff;
    }
}

unsigned int m68k_read_memory_8(unsigned int a)
{
    a &= 0xffffff;
    if (ng_rd_word && a < 0x100000) RD_MARK(ng_rd_word, a);
    switch (a >> 20) {
    case 0x0:
        if (a < 0x80 && !ng.use_cart_vectors) return ngr.bios.data[a];
        return a < ngr.p1.size ? ngr.p1.data[a] : 0xff;
    case 0x1: return ng.ram[a & 0xffff];
    case 0x3: return io_read8(a);
    case 0xc: return ngr.bios.data[a & (ngr.bios.size - 1)];
    case 0xd: return ng.backup[a & 0xffff];
    default: {
        uint16_t w = (uint16_t)m68k_read_memory_16(a & ~1u);
        return (a & 1) ? (w & 0xff) : (w >> 8);
    }
    }
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    if (ng_rd_long && (a & 0xffffff) < 0x100000) RD_MARK(ng_rd_long, a & 0xfffffe);
    in_long_read = 1;
    unsigned int v = (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
    in_long_read = 0;
    if (ng_long_src && (a & 0xffffff) < 0x100000 && (v & 0xffffff) < 0x100000)
        ng_long_src[(v & 0xffffff) >> 1] = (a & 0xfffffe) + 1;
    return v;
}

static void write16(uint32_t a, uint16_t data, uint16_t mask, int is_byte, uint8_t bdata)
{
    switch (a >> 20) {
    case 0x1: {
        uint8_t *p = ng.ram + (a & 0xfffe);
        if (a + 1 >= ng_watch_lo && a <= ng_watch_hi)
            fprintf(stderr, "watch frame %llu ppc %06x write %06x = %04x mask %04x (was %02x%02x)\n",
                    (unsigned long long)ng.frame, m68k_get_reg(NULL, M68K_REG_PPC), a & 0xfffffe, data, mask, p[0], p[1]);
        if (mask & 0xff00) p[0] = (uint8_t)(data >> 8);
        if (mask & 0x00ff) p[1] = (uint8_t)data;
        break;
    }
    case 0x2:
        if ((a & 0xfffff) >= 0xffff0) {
            uint32_t bank = (is_byte ? bdata : data) & 7;
            uint32_t off = bank * 0x100000;
            ng.p2_bank_off = off < ngr.p2.size ? off : 0;
        }
        break;
    case 0x3: io_write(a | (is_byte && (mask == 0x00ff)), is_byte ? bdata : data, is_byte); break;
    case 0x4: case 0x5: case 0x6: case 0x7: pal_write(a, data, mask); break;
    case 0xd:
        if (ng.sram_unlocked) {
            uint8_t *p = ng.backup + (a & 0xfffe);
            if (mask & 0xff00) p[0] = (uint8_t)(data >> 8);
            if (mask & 0x00ff) p[1] = (uint8_t)data;
        }
        break;
    default: break;
    }
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    a &= 0xffffff;
    v &= 0xff;
    if (a & 1) write16(a & ~1u, (uint16_t)v, 0x00ff, 1, (uint8_t)v);
    else write16(a, (uint16_t)(v << 8), 0xff00, 1, (uint8_t)v);
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    a &= 0xfffffe;
    write16(a, (uint16_t)v, 0xffff, 0, 0);
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xffff);
}

unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ------------------------------------------------------------------------------------------ */
/* System                                                                                     */
/* ------------------------------------------------------------------------------------------ */

void ng_init(int cpu_mode, const char *miss_log)
{
    memset(&ng, 0, sizeof ng);
    memset(ng.backup, 0, sizeof ng.backup);
    ng.dipsw = 0xff;
    cpu_init(cpu_mode, miss_log);
    video_init();
    audio_init(ngr.m1.data, ngr.m1.size, ngr.sm1.data, ngr.sm1.size, ngr.vrom.data, ngr.vrom.size);
}

void ng_reset(void)
{
    ng.watchdog = 0;
    ng.use_cart_vectors = 0;
    ng.use_cart_fix = 0;
    ng.sram_unlocked = 0;
    ng.pal_bank = 0;
    ng.p2_bank_off = 0;
    ng.irq_vblank = ng.irq_timer = 0;
    ng.irq_reset = 1;                       /* cold boot interrupt */
    ng.timer_left = -1;
    ng.irq2_ctrl = 0;
    ng.rtc.tp_div = 0;
    audio_reset();
    audio_set_cart_rom(0);
    cpu_reset();
    update_irq();
}

void ng_set_input(uint8_t p1, uint8_t p2, uint8_t sys) { ng.in_p1 = p1; ng.in_p2 = p2; ng.in_sys = sys; }

static void run_cycles(int cycles)
{
    while (cycles > 0) {
        int slice = cycles;
        if (ng.timer_left >= 0 && ng.timer_left < slice) slice = ng.timer_left > 0 ? (int)ng.timer_left : 1;
        slice_len = slice;
        cpu_execute(slice);
        ng.total_cycles += (uint64_t)slice;
        /* MVS watchdog: resets the system if 0x300001 is not written for 3244030 master clocks
         * (KOF97 relies on this: its backup-RAM registration loop must be cut short by it). */
        ng.watchdog += slice;
        if (ng.watchdog > NG_WATCHDOG_CYCLES) {
            ng.watchdog = 0;
            ng.watchdog_resets++;
            ng_reset();
        }
        cycles -= slice;
        if (ng.timer_left >= 0) {
            ng.timer_left -= slice;
            if (ng.timer_left <= 0) timer_fire();
        }
        slice_len = 0;
    }
}

uint32_t *ng_hd_fb;
int ng_hd_scale = 1;
int ng_frame_render;        /* 1: render the whole frame at the start of VBlank (--frame-render) */
int ng_tagged;

void ng_run_frame(uint32_t *fb)
{
    for (int line = 0; line < NG_LINES; line++) {
        ng.line = line;
        if (line == 0) {
            if (ng.auto_anim_frame_counter == 0) {
                ng.auto_anim_frame_counter = ng.auto_anim_speed;
                ng.auto_anim_counter++;
            } else {
                ng.auto_anim_frame_counter--;
            }
        }
        if (line == NG_VBSTART && fb && ng_frame_render) {
            /* whole-frame rendering: the frame's final state, after the game logic and before the VBlank
               handler (the native renderer's reference; the per-line renderer also shows mid-frame writes) */
            if (ng_hd_fb || ng_tagged) {
                video_render_frame(fb);                    /* tagged: replacements sampled at full resolution */
                if (ng_hd_fb) video_compose(fb, ng_hd_fb, ng_hd_scale);
                if (!ng_tagged) video_compose(fb, fb, 1);  /* plain colours for 1x screenshots */
            } else
                for (int l = NG_VBEND; l < NG_VBSTART; l++) video_render_line(l, fb + (l - NG_VBEND) * NG_SCREEN_W);
        }
        if (line == NG_VBSTART) {
            if (ng.irq2_ctrl & 0x40) { slice_len = 0; timer_reload_now(); }
            ng.irq_vblank = 1;
            update_irq();
        }
        if (!ng_frame_render && line >= NG_VBEND && line < NG_VBSTART && fb)
            video_render_line(line, fb + (line - NG_VBEND) * NG_SCREEN_W);
        run_cycles(NG_CYCLES_PER_LINE);
        if (!snd_native_enabled()) audio_run(NG_Z80_CYCLES_PER_LINE);
    }
    ng.frame++;
}

int ng_load_nvram(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(ng.backup, 1, sizeof ng.backup, f);
    fclose(f);
    return n == sizeof ng.backup;
}

int ng_save_nvram(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(ng.backup, 1, sizeof ng.backup, f);
    fclose(f);
    return n == sizeof ng.backup;
}

void ng_shutdown(void) { cpu_shutdown(); }

/* KOF97 recompiled runtime - SDL2 frontend (window, keyboard/gamepad, audio) and headless test mode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "neogeo.h"
#include "cpu.h"
#include "audio.h"
#include "m68k.h"

typedef struct { int frame, len; uint8_t p1, p2, sys; } script_ev;

static struct {
    const char *roms, *nvram, *misses, *wav, *script, *dump_prefix, *ram_log;
    int headless, frames, interp, scale, fullscreen, dump_every, autoplay, quiet, trace_pc, ram_log_from, ram_log_every;
} opt = { "build/roms", "kof97.nvram", "build/recomp/runtime_misses.txt", NULL, NULL, NULL, NULL, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0 };

static script_ev *events; static int n_events;
static const char *exec_log;
static FILE *frame_sig;

/* --break: log every execution of these PCs (interpreter mode) with registers and return address */
static uint32_t break_pc[16]; static int n_break;
static long long trace_frame = -1;
static void break_hook(uint32_t pc)
{
    if ((long long)ng.frame == trace_frame) {
        fprintf(stderr, "trace %06x cyc %d", pc, m68k_cycles_remaining());
        for (int r = 0; r < 16; r++) fprintf(stderr, " %08x", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r)));
        fputc('\n', stderr);
    }
    for (int i = 0; i < n_break; i++)
        if (pc == break_pc[i]) {
            unsigned a7 = m68k_get_reg(NULL, M68K_REG_A7);
            fprintf(stderr, "break frame %llu pc %06x ret %06x d", (unsigned long long)ng.frame, pc, m68k_read_memory_32(a7));
            for (int r = 0; r < 8; r++) fprintf(stderr, " %08x", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r)));
            fprintf(stderr, " a");
            for (int r = 0; r < 8; r++) fprintf(stderr, " %08x", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + r)));
            fprintf(stderr, " sp+2 %08x\n", m68k_read_memory_32(a7 + 2));
        }
}

static void usage(void)
{
    puts("usage: kof97 [options]\n"
         "  --roms DIR          normalised ROM dir from tools/romprep.py (default build/roms)\n"
         "  --interp            run the 68000 purely on the Musashi interpreter (reference mode)\n"
         "  --headless          no window/audio device; use with --frames\n"
         "  --frames N          stop after N frames\n"
         "  --dump PREFIX       headless: write PREFIX_<frame>.ppm screenshots\n"
         "  --dump-every N      screenshot interval in frames (default 60)\n"
         "  --wav FILE          record audio to a WAV file\n"
         "  --script FILE       scripted input: lines 'frame duration key[,key..]' (keys may include\n"
         "                      poke:ADDR=VAL work-RAM writes) and 'mash FROM TO SEED p1|p2|both'\n"
         "  --autoplay          built-in script: insert coin, start, and mash buttons\n"
         "  --misses FILE       runtime miss log for the recompiler ('' disables)\n"
         "  --nvram FILE        MVS backup RAM file (default kof97.nvram)\n"
         "  --scale N  --fullscreen\n"
         "  --trace-pc N        headless: print 68000/Z80 PC every N frames\n"
         "  --ram-log FILE      headless: append the 64 KB work RAM after every frame (tools/disasm/ramdiff.py)\n"
         "  --ram-log-from N    first frame to log (default 0)\n"
         "  --ram-log-every N   log every Nth frame only\n"
         "  --watch A[-B]       log writes to work RAM A..B (hex, e.g. 10f000-10f003) with the writing PC;\n"
         "                      use with --interp for exact PCs\n"
         "  --break A[,B..]     log each execution of these PCs (hex) with D0-D7/A0-A7 and the return\n"
         "                      address on the stack; needs --interp\n"
         "  --watch-vram A[-B]  log VRAM writes to word addresses A..B (hex, e.g. 7000-74ff = fix layer)\n"
         "  --trace-frame N     log every interpreted PC (and remaining cycles) during frame N; needs --interp\n"
         "  --exec-log FILE     write/merge a bitmap of executed P1 instruction addresses (needs --interp;\n"
         "                      read by tools/disasm/mkasm.py)\n"
         "  --frame-sig FILE    headless: per-frame PC, cycle carry and work-RAM hash (tools/recomp/modecheck.py)\n"
         "  --rtc-fixed EPOCH   calendar starts at this Unix time and follows emulated time (reproducible runs)\n"
         "keys: arrows + Z X C V (P1), R F D G + A S Q W (P2), 1/2 start, 5/6 coin, 9 service,\n"
         "      F2 test, F3 reset, P pause, F12 screenshot, Esc quit; gamepads supported");
}

static int parse_keys(const char *s, uint8_t *p1, uint8_t *p2, uint8_t *sys)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        struct { const char *n; uint8_t *dst; uint8_t bit; } map[] = {
            { "up", p1, NG_UP }, { "down", p1, NG_DOWN }, { "left", p1, NG_LEFT }, { "right", p1, NG_RIGHT },
            { "a", p1, NG_A }, { "b", p1, NG_B }, { "c", p1, NG_C }, { "d", p1, NG_D },
            { "p2up", p2, NG_UP }, { "p2down", p2, NG_DOWN }, { "p2left", p2, NG_LEFT }, { "p2right", p2, NG_RIGHT },
            { "p2a", p2, NG_A }, { "p2b", p2, NG_B }, { "p2c", p2, NG_C }, { "p2d", p2, NG_D },
            { "start1", sys, NG_SYS_START1 }, { "start2", sys, NG_SYS_START2 },
            { "coin1", sys, NG_SYS_COIN1 }, { "coin2", sys, NG_SYS_COIN2 },
            { "service", sys, NG_SYS_SERVICE }, { "test", sys, NG_SYS_TEST },
        };
        int ok = 0;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(t, map[i].n)) { *map[i].dst |= map[i].bit; ok = 1; }
        if (!ok && strcmp(t, "none")) { fprintf(stderr, "unknown key '%s'\n", t); return 0; }
    }
    return 1;
}

/* poke:ADDR=VAL tokens: write work RAM (0x100000-0x10ffff) every frame while the event is active.
 * Identical in original and shifted runs (work RAM does not move), used to reach later game states. */
typedef struct { int frame, len; uint32_t addr; uint16_t val; int word; } script_poke;
static script_poke *pokes; static int n_pokes;

static void add_event(int frame, int len, const char *keys_in)
{
    script_ev e = { frame, len, 0, 0, 0 };
    char keys[256], rest[256] = "";
    snprintf(keys, sizeof keys, "%s", keys_in);
    for (char *t = strtok(keys, ","); t; t = strtok(NULL, ",")) {
        if (!strncmp(t, "poke:", 5)) {
            char *eq = strchr(t, '=');
            if (!eq) { fprintf(stderr, "bad poke '%s'\n", t); exit(1); }
            script_poke pk = { frame, len, (uint32_t)strtoul(t + 5, NULL, 16), (uint16_t)strtoul(eq + 1, NULL, 16), strlen(eq + 1) > 2 };
            if ((pk.addr & 0xff0000) != 0x100000) { fprintf(stderr, "poke outside work RAM '%s'\n", t); exit(1); }
            pokes = realloc(pokes, sizeof *pokes * (size_t)(n_pokes + 1));
            pokes[n_pokes++] = pk;
        } else {
            if (*rest) strcat(rest, ",");
            strncat(rest, t, sizeof rest - strlen(rest) - 2);
        }
    }
    if (!*rest) strcpy(rest, "none");
    if (!parse_keys(rest, &e.p1, &e.p2, &e.sys)) exit(1);
    events = realloc(events, sizeof *events * (size_t)(n_events + 1));
    events[n_events++] = e;
}

static void add_mash(int from, int to, unsigned seed, const char *who)
{
    /* deterministic pseudo-random fighting input for one or both players */
    static const char *p1m[] = { "right", "left", "down", "up", "a", "b", "c", "d", "down,right,a", "down,left,c",
                                 "right,b", "left,d", "a,b,c", "down,c", "down,right", "left,a", "right,c,d", "up,right" };
    static const char *p2m[] = { "p2right", "p2left", "p2down", "p2up", "p2a", "p2b", "p2c", "p2d", "p2down,p2left,p2a",
                                 "p2down,p2right,p2c", "p2left,p2b", "p2right,p2d", "p2a,p2b,p2c", "p2down,p2c" };
    int both = !strcmp(who, "both"), two = both || !strcmp(who, "p2"), one = both || !strcmp(who, "p1");
    unsigned s = seed ? seed : 1;
    for (int f = from; f < to; f += 7) {
        s = s * 1103515245u + 12345u;
        if (one) add_event(f, 3 + (int)((s >> 16) % 10), p1m[(s >> 8) % (sizeof p1m / sizeof p1m[0])]);
        s = s * 1103515245u + 12345u;
        if (two) add_event(f + 3, 3 + (int)((s >> 16) % 10), p2m[(s >> 8) % (sizeof p2m / sizeof p2m[0])]);
    }
}

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int fr, len; char keys[256]; unsigned seed;
        if (line[0] == '#') continue;
        if (sscanf(line, "mash %d %d %u %255s", &fr, &len, &seed, keys) == 4) { add_mash(fr, len, seed, keys); continue; }
        if (sscanf(line, "%d %d %255s", &fr, &len, keys) != 3) continue;
        add_event(fr, len, keys);
    }
    fclose(f);
}

static void script_pokes(int frame)
{
    for (int i = 0; i < n_pokes; i++)
        if (frame >= pokes[i].frame && frame < pokes[i].frame + pokes[i].len) {
            uint32_t a = pokes[i].addr & 0xffff;
            if (pokes[i].word) { ng.ram[a & 0xfffe] = (uint8_t)(pokes[i].val >> 8); ng.ram[(a & 0xfffe) + 1] = (uint8_t)pokes[i].val; }
            else ng.ram[a] = (uint8_t)pokes[i].val;
        }
}

static void build_autoplay(void)
{
    /* boot (~10 s incl. BIOS checks), coin, start, then deterministic pseudo-random play */
    add_event(900, 8, "coin1");
    add_event(960, 8, "coin1");
    add_event(1020, 6, "start1");
    for (int f = 1080; f < 1400; f += 30) add_event(f, 4, "a");      /* team/character select */
    add_event(1400, 6, "start2");
    add_event(1420, 8, "coin2");
    add_event(1440, 6, "start2");
    unsigned s = 12345;
    static const char *moves[] = { "right", "left", "down", "up", "a", "b", "c", "d", "down,right,a", "down,left,c",
                                   "right,b", "left,d", "p2left", "p2right", "p2a", "p2b", "p2c,p2down", "start1",
                                   "a,b,c", "down,c" };
    for (int f = 1500; f < 400000; f += 7) {
        s = s * 1103515245u + 12345u;
        add_event(f, 3 + (int)((s >> 16) % 10), moves[(s >> 8) % (sizeof moves / sizeof moves[0])]);
        if (f % 3000 == 0) { add_event(f, 6, "coin1"); add_event(f + 20, 6, "start1"); }
    }
}

static void script_input(int frame, uint8_t *p1, uint8_t *p2, uint8_t *sys)
{
    for (int i = 0; i < n_events; i++)
        if (frame >= events[i].frame && frame < events[i].frame + events[i].len) {
            *p1 |= events[i].p1; *p2 |= events[i].p2; *sys |= events[i].sys;
        }
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", NG_SCREEN_W, NG_SCREEN_H);
    for (int i = 0; i < NG_SCREEN_W * NG_SCREEN_H; i++) {
        uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* ---- WAV writer ---- */
static FILE *wavf; static uint32_t wav_frames;
static void wav_u32(uint32_t v) { fputc(v & 255, wavf); fputc((v >> 8) & 255, wavf); fputc((v >> 16) & 255, wavf); fputc(v >> 24, wavf); }
static void wav_u16(uint16_t v) { fputc(v & 255, wavf); fputc(v >> 8, wavf); }
static void wav_open(const char *p)
{
    wavf = fopen(p, "wb");
    if (!wavf) { perror(p); return; }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wavf);
    wav_u32(16); wav_u16(1); wav_u16(2); wav_u32(AUDIO_RATE); wav_u32(AUDIO_RATE * 4); wav_u16(4); wav_u16(16);
    fwrite("data\0\0\0\0", 1, 8, wavf);
}
static void wav_close(void)
{
    if (!wavf) return;
    fseek(wavf, 4, SEEK_SET); wav_u32(36 + wav_frames * 4);
    fseek(wavf, 40, SEEK_SET); wav_u32(wav_frames * 4);
    fclose(wavf);
}

/* ---- keyboard / gamepad ---- */
static uint8_t key_p1, key_p2, key_sys, pad_p[2], pad_sys;

static void handle_key(SDL_Keycode k, int down)
{
    struct { SDL_Keycode k; uint8_t *dst; uint8_t bit; } map[] = {
        { SDLK_UP, &key_p1, NG_UP }, { SDLK_DOWN, &key_p1, NG_DOWN }, { SDLK_LEFT, &key_p1, NG_LEFT }, { SDLK_RIGHT, &key_p1, NG_RIGHT },
        { SDLK_z, &key_p1, NG_A }, { SDLK_x, &key_p1, NG_B }, { SDLK_c, &key_p1, NG_C }, { SDLK_v, &key_p1, NG_D },
        { SDLK_r, &key_p2, NG_UP }, { SDLK_f, &key_p2, NG_DOWN }, { SDLK_d, &key_p2, NG_LEFT }, { SDLK_g, &key_p2, NG_RIGHT },
        { SDLK_a, &key_p2, NG_A }, { SDLK_s, &key_p2, NG_B }, { SDLK_q, &key_p2, NG_C }, { SDLK_w, &key_p2, NG_D },
        { SDLK_1, &key_sys, NG_SYS_START1 }, { SDLK_2, &key_sys, NG_SYS_START2 },
        { SDLK_5, &key_sys, NG_SYS_COIN1 }, { SDLK_6, &key_sys, NG_SYS_COIN2 },
        { SDLK_9, &key_sys, NG_SYS_SERVICE }, { SDLK_F2, &key_sys, NG_SYS_TEST },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].k == k) { if (down) *map[i].dst |= map[i].bit; else *map[i].dst &= (uint8_t)~map[i].bit; }
}

static void poll_pads(SDL_GameController **pads)
{
    pad_p[0] = pad_p[1] = 0; pad_sys = 0;
    for (int i = 0; i < 2; i++) {
        SDL_GameController *c = pads[i];
        if (!c) continue;
        uint8_t v = 0;
        int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) v |= NG_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) v |= NG_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) v |= NG_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) v |= NG_RIGHT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) v |= NG_A;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) v |= NG_B;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) v |= NG_C;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) v |= NG_D;
        pad_p[i] = v;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) pad_sys |= i ? NG_SYS_START2 : NG_SYS_START1;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) pad_sys |= i ? NG_SYS_COIN2 : NG_SYS_COIN1;
    }
}

static void print_stats(void)
{
    const cpu_stats_t *s = cpu_stats();
    if (opt.quiet) return;
    fprintf(stderr, "watchdog resets %d, frames %llu, recompiled chunk calls %llu, interpreter fallbacks %llu (%llu instructions)\n",
            ng.watchdog_resets, (unsigned long long)ng.frame, (unsigned long long)s->rc_calls,
            (unsigned long long)s->interp_entries, (unsigned long long)s->interp_insns);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define TAKE (v ? (i++, v) : (usage(), exit(1), (const char *)NULL))
        if (!strcmp(a, "--roms")) opt.roms = TAKE;
        else if (!strcmp(a, "--interp")) opt.interp = 1;
        else if (!strcmp(a, "--headless")) opt.headless = 1;
        else if (!strcmp(a, "--frames")) opt.frames = atoi(TAKE);
        else if (!strcmp(a, "--dump")) opt.dump_prefix = TAKE;
        else if (!strcmp(a, "--dump-every")) opt.dump_every = atoi(TAKE);
        else if (!strcmp(a, "--wav")) opt.wav = TAKE;
        else if (!strcmp(a, "--script")) opt.script = TAKE;
        else if (!strcmp(a, "--autoplay")) opt.autoplay = 1;
        else if (!strcmp(a, "--misses")) opt.misses = TAKE;
        else if (!strcmp(a, "--nvram")) opt.nvram = TAKE;
        else if (!strcmp(a, "--scale")) opt.scale = atoi(TAKE);
        else if (!strcmp(a, "--fullscreen")) opt.fullscreen = 1;
        else if (!strcmp(a, "--quiet")) opt.quiet = 1;
        else if (!strcmp(a, "--exec-log")) exec_log = TAKE;
        else if (!strcmp(a, "--frame-sig")) { frame_sig = fopen(TAKE, "w"); if (!frame_sig) { perror("--frame-sig"); return 1; } }
        else if (!strcmp(a, "--trace-pc")) opt.trace_pc = atoi(TAKE);
        else if (!strcmp(a, "--ram-log")) opt.ram_log = TAKE;
        else if (!strcmp(a, "--ram-log-from")) opt.ram_log_from = atoi(TAKE);
        else if (!strcmp(a, "--ram-log-every")) opt.ram_log_every = atoi(TAKE);
        else if (!strcmp(a, "--rtc-fixed")) ng_rtc_base = atoll(TAKE);
        else if (!strcmp(a, "--watch-vram")) {
            char *end;
            ng_vwatch_lo = (uint32_t)strtoul(TAKE, &end, 16);
            ng_vwatch_hi = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 16) : ng_vwatch_lo;
        }
        else if (!strcmp(a, "--trace-frame")) { trace_frame = atoll(TAKE); cpu_debug_hook = break_hook; }
        else if (!strcmp(a, "--break")) {
            for (char *p = (char *)TAKE; *p && n_break < 16; ) {
                break_pc[n_break++] = (uint32_t)strtoul(p, &p, 16);
                if (*p == ',') p++;
            }
            cpu_debug_hook = break_hook;
        }
        else if (!strcmp(a, "--watch")) {
            char *end;
            ng_watch_lo = (uint32_t)strtoul(TAKE, &end, 16);
            ng_watch_hi = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 16) : ng_watch_lo + 1;
        }
        else { usage(); return !!strcmp(a, "--help"); }
    }
    if (opt.dump_prefix && !opt.dump_every) opt.dump_every = 60;
    if (opt.script) load_script(opt.script);
    if (opt.autoplay) build_autoplay();

    if (!ng_load_roms(opt.roms)) return 1;
    ng_init(opt.interp ? CPU_MODE_INTERP : CPU_MODE_RECOMP, (opt.misses && *opt.misses) ? opt.misses : NULL);
    if (exec_log) cpu_set_exec_log(exec_log);
    ng_load_nvram(opt.nvram);
    ng_reset();
    if (opt.wav) wav_open(opt.wav);

    static uint32_t fb[NG_SCREEN_W * NG_SCREEN_H];
    static int16_t abuf[8192 * 2];

    if (opt.headless) {
        FILE *ram_log = opt.ram_log ? fopen(opt.ram_log, "wb") : NULL;
        if (opt.ram_log && !ram_log) { perror(opt.ram_log); return 1; }
        for (int f = 0; !opt.frames || f < opt.frames; f++) {
            uint8_t p1 = 0, p2 = 0, sys = 0;
            script_input(f, &p1, &p2, &sys);
            ng_set_input(p1, p2, sys);
            script_pokes(f);
            ng_run_frame(fb);
            if (frame_sig) {
                /* per-frame signature: 68000 PC and cycle carry at the end of the frame + FNV-1a of work RAM */
                uint64_t h = 1469598103934665603ull;
                for (size_t i = 0; i < sizeof ng.ram; i++) h = (h ^ ng.ram[i]) * 1099511628211ull;
                fprintf(frame_sig, "%d %06x %d %016llx\n", f + 1, cpu_pc() & 0xffffff, cpu_cycles_carry(), (unsigned long long)h);
            }
            if (opt.trace_pc && (f + 1) % opt.trace_pc == 0)
                fprintf(stderr, "frame %d pc %06x sr %04x z80pc %04x\n", f + 1, cpu_pc(), m68k_get_reg(NULL, M68K_REG_SR), audio_z80_pc());
            if (ram_log && f >= opt.ram_log_from && (opt.ram_log_every <= 1 || (f - opt.ram_log_from) % opt.ram_log_every == 0))
                fwrite(ng.ram, 1, sizeof ng.ram, ram_log);
            int n;
            while ((n = audio_read_samples(abuf, 8192)) > 0)
                if (wavf) { fwrite(abuf, 4, (size_t)n, wavf); wav_frames += (uint32_t)n; }
            if (opt.dump_prefix && (f + 1) % opt.dump_every == 0) {
                char path[512];
                snprintf(path, sizeof path, "%s_%06d.ppm", opt.dump_prefix, f + 1);
                write_ppm(path, fb);
            }
        }
        if (ram_log) fclose(ram_log);
        print_stats();
        wav_close();
        ng_save_nvram(opt.nvram);
        ng_shutdown();
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("KOF97 (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       NG_SCREEN_W * opt.scale, NG_SCREEN_H * opt.scale,
                                       SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, NG_SCREEN_W, NG_SCREEN_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, NG_SCREEN_W, NG_SCREEN_H);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) fprintf(stderr, "no audio: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(adev, 0);

    SDL_GameController *pads[2] = { 0 };
    int npad = 0;
    for (int i = 0; i < SDL_NumJoysticks() && npad < 2; i++)
        if (SDL_IsGameController(i)) pads[npad++] = SDL_GameControllerOpen(i);

    uint64_t freq = SDL_GetPerformanceFrequency();
    double frame_ticks = (double)freq / NG_FPS;
    double next = (double)SDL_GetPerformanceCounter();
    int running = 1, paused = 0, shot = 0;
    for (int f = 0; running && (!opt.frames || f < opt.frames); ) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int down = ev.type == SDL_KEYDOWN;
                SDL_Keycode k = ev.key.keysym.sym;
                if (down && k == SDLK_ESCAPE) running = 0;
                else if (down && k == SDLK_p && !ev.key.repeat) paused = !paused;
                else if (down && k == SDLK_F3) ng_reset();
                else if (down && k == SDLK_F12) {
                    char path[64]; snprintf(path, sizeof path, "kof97_shot_%03d.ppm", shot++); write_ppm(path, fb);
                }
                else handle_key(k, down);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && npad < 2) {
                pads[npad++] = SDL_GameControllerOpen(ev.cdevice.which);
            }
        }
        if (!paused) {
            poll_pads(pads);
            uint8_t p1 = key_p1 | pad_p[0], p2 = key_p2 | pad_p[1], sys = key_sys | pad_sys;
            script_input(f, &p1, &p2, &sys);
            ng_set_input(p1, p2, sys);
            script_pokes(f);
            ng_run_frame(fb);
            f++;
            int n;
            while ((n = audio_read_samples(abuf, 8192)) > 0) {
                if (wavf) { fwrite(abuf, 4, (size_t)n, wavf); wav_frames += (uint32_t)n; }
                if (adev) {
                    /* keep latency bounded: drop audio if the queue grows beyond ~150 ms */
                    if (SDL_GetQueuedAudioSize(adev) > (Uint32)(AUDIO_RATE * 4 * 15 / 100)) SDL_ClearQueuedAudio(adev);
                    SDL_QueueAudio(adev, abuf, (Uint32)n * 4);
                }
            }
        }
        SDL_UpdateTexture(tex, NULL, fb, NG_SCREEN_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        next += frame_ticks;
        double now = (double)SDL_GetPerformanceCounter();
        if (next > now) SDL_Delay((Uint32)((next - now) * 1000.0 / (double)freq));
        else if (now - next > frame_ticks * 10) next = now;     /* fell behind: resync */
    }
    print_stats();
    wav_close();
    ng_save_nvram(opt.nvram);
    ng_shutdown();
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}

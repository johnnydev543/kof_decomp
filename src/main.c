/* KOF97 recompiled runtime - SDL2 frontend (window, keyboard/gamepad, audio) and headless test mode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "neogeo.h"
#include "cpu.h"
#include "audio.h"
#include "snd_native.h"
#include "spr_native.h"
#include "gpu.h"
#include "bios_hle.h"
#include "m68k.h"

static uint32_t fb[NG_SCREEN_W * NG_SCREEN_H];     /* frame buffer (ARGB) */
static int16_t abuf[8192 * 2];                     /* audio transfer buffer */

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
         "  --native-audio [DIR] play pre-rendered sound files (tools/assets/export_audio.py) instead of\n"
         "                      emulating the Z80 + YM2610 (default DIR build/assets/kof97/audio)\n"
         "  --mods DIR          remade assets (sprites/, audio/) that replace the originals (default mods/kof97)\n"
         "  --aes               home (AES) mode: mode select menu, options, no coins\n"
         "  --bios-hle N        native BIOS: 1 services, 2 + SYSTEM_IO, 3 everything (no BIOS ROM needed)\n"
         "  --bios-hle-timing T level 3 boot timing: 'real' (as the BIOS ROM) or F,B,E[,R] frames (boot delay on a\n"
         "                      fresh backup RAM, boot delay, eye-catcher length, boot delay after a watchdog\n"
         "                      reset); default 0,0,0 (fast)\n"
         "  --soft-dip S        native BIOS: soft-DIP settings 'name=value,...' ('--soft-dip list' shows them)\n"
         "  --bios-log          print the BIOS's calls into the game (USER, PLAYER_START, SYSTEM_RETURN ...)\n"
         "  --test-raster       tests: the raster line counter read does not depend on CPU timing\n"
         "  --test-cpu-scale N  tests: N times the 68000 cycles per line, so the game never runs late\n"
         "  --gpu               draw with OpenGL at the window's resolution (remade sprites at full detail)\n"
         "  --keys FILE         key bindings: lines 'action=Key Name' (actions p1_up .. p1_d, p2_up .. p2_d,\n"
         "                      p1_start, p2_start, p1_coin, p2_coin, service, test; SDL key names;\n"
         "                      gamepad: pad_a .. pad_d, pad_start, pad_coin = SDL button names a, b, x, y, ...)\n"
         "  --print-keys        print the key bindings in --keys format (after any --keys) and exit\n"
         "  --sound-log FILE    log every sound command (68k -> Z80) and reply with its frame\n"
         "  --hd S              render at S x resolution (remade sprites keep their detail)\n"
         "  --frame-render      draw each frame at once at the start of VBlank instead of line by line\n"
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

static int use_gpu;
static int bios_level;
static const char *bios_timing, *soft_dip;
static int web_worklet;          /* web page with an AudioWorklet: audio goes to Module.audioPush */

static void write_ppm_buf(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* screenshot of what is shown: the 1x frame buffer, the --hd buffer, or the GPU output */
static void write_ppm(const char *path, const uint32_t *fb1x)
{
    if (use_gpu) {
        uint32_t *buf; int w, h;
        if (gpu_read(&buf, &w, &h)) { write_ppm_buf(path, buf, w, h); free(buf); }
        return;
    }
    int s = ng_hd_fb ? ng_hd_scale : 1;
    write_ppm_buf(path, ng_hd_fb ? ng_hd_fb : fb1x, NG_SCREEN_W * s, NG_SCREEN_H * s);
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

/* ---- --sound-log ---- */
static const char *sound_log_path;
static const char *native_audio, *mods_dir = "mods/kof97";

/* audio for one frame: from the emulated driver, or rendered natively (55555 Hz / 59.185 Hz per frame) */
static int read_audio(int16_t *buf, int max)
{
    static double owed; static int given;
    if (!snd_native_enabled()) return audio_read_samples(buf, max);
    if (given) { given = 0; return 0; }
    owed += (double)AUDIO_RATE / NG_FPS;
    int n = (int)owed; if (n > max) n = max;
    owed -= n; given = 1;
    return snd_native_render(buf, n);
}
static FILE *sound_log_f;
static void sound_log_cb(int frame, int dir, uint8_t val) { fprintf(sound_log_f, "%d %s %02x\n", frame, dir ? "reply" : "cmd", val); }
static void sound_reply_cb(uint8_t val) { if (sound_log_f) fprintf(sound_log_f, "%llu reply %02x\n", (unsigned long long)ng.frame, val); }

/* ---- keyboard / gamepad ---- */
static uint8_t key_p1, key_p2, key_sys, pad_p[2], pad_sys;

/* key bindings by action name (--keys FILE, or the web page's key settings) */
typedef struct { const char *name; uint8_t *dst; uint8_t bit; SDL_Keycode key, def; } keybind;
static keybind keymap[] = {
    /* P1: W/S/A/D + J/K/U/I = A/B/C/D; P2: arrow keys + keypad 1/2/4/5 */
    { "p1_up", &key_p1, NG_UP, SDLK_w, SDLK_w }, { "p1_down", &key_p1, NG_DOWN, SDLK_s, SDLK_s },
    { "p1_left", &key_p1, NG_LEFT, SDLK_a, SDLK_a }, { "p1_right", &key_p1, NG_RIGHT, SDLK_d, SDLK_d },
    { "p1_a", &key_p1, NG_A, SDLK_j, SDLK_j }, { "p1_b", &key_p1, NG_B, SDLK_k, SDLK_k }, { "p1_c", &key_p1, NG_C, SDLK_u, SDLK_u }, { "p1_d", &key_p1, NG_D, SDLK_i, SDLK_i },
    { "p2_up", &key_p2, NG_UP, SDLK_UP, SDLK_UP }, { "p2_down", &key_p2, NG_DOWN, SDLK_DOWN, SDLK_DOWN },
    { "p2_left", &key_p2, NG_LEFT, SDLK_LEFT, SDLK_LEFT }, { "p2_right", &key_p2, NG_RIGHT, SDLK_RIGHT, SDLK_RIGHT },
    { "p2_a", &key_p2, NG_A, SDLK_KP_1, SDLK_KP_1 }, { "p2_b", &key_p2, NG_B, SDLK_KP_2, SDLK_KP_2 }, { "p2_c", &key_p2, NG_C, SDLK_KP_4, SDLK_KP_4 }, { "p2_d", &key_p2, NG_D, SDLK_KP_5, SDLK_KP_5 },
    { "p1_start", &key_sys, NG_SYS_START1, SDLK_1, SDLK_1 }, { "p2_start", &key_sys, NG_SYS_START2, SDLK_2, SDLK_2 },
    { "p1_coin", &key_sys, NG_SYS_COIN1, SDLK_5, SDLK_5 }, { "p2_coin", &key_sys, NG_SYS_COIN2, SDLK_6, SDLK_6 },
    { "service", &key_sys, NG_SYS_SERVICE, SDLK_9, SDLK_9 }, { "test", &key_sys, NG_SYS_TEST, SDLK_F2, SDLK_F2 },
};
#define NKEYS (int)(sizeof keymap / sizeof keymap[0])
/* gamepad buttons (both pads; pad 1 plays P1, pad 2 P2; directions: d-pad or left stick) */
typedef struct { const char *name; uint8_t bit; int sys; SDL_GameControllerButton button, def; } padbind;
static padbind padmap[] = {
    { "pad_a", NG_A, 0, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_A },
    { "pad_b", NG_B, 0, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_B },
    { "pad_c", NG_C, 0, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_X },
    { "pad_d", NG_D, 0, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_Y },
    { "pad_start", NG_SYS_START1, 1, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_START },
    { "pad_coin", NG_SYS_COIN1, 1, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_BACK },
};
#define NPAD (int)(sizeof padmap / sizeof padmap[0])
static int key_learn = -1;               /* action waiting for a key or pad button (web key settings);
                                            >= NKEYS: padmap[key_learn - NKEYS] */

static int key_reserved(SDL_Keycode k) { return k == SDLK_ESCAPE || k == SDLK_p || k == SDLK_F3 || k == SDLK_F12; }

/* bind action to key; another action on the same key gets this action's old key (swap) */
static int bind_key(int a, SDL_Keycode k)
{
    if (a < 0 || a >= NKEYS || k == SDLK_UNKNOWN || key_reserved(k)) return 0;
    for (int i = 0; i < NKEYS; i++) if (i != a && keymap[i].key == k) keymap[i].key = keymap[a].key;
    keymap[a].key = k;
    key_p1 = key_p2 = key_sys = 0;       /* drop held bits of the old bindings */
    return 1;
}

static int action_index(const char *name)
{
    for (int i = 0; i < NKEYS; i++) if (!strcmp(keymap[i].name, name)) return i;
    for (int i = 0; i < NPAD; i++) if (!strcmp(padmap[i].name, name)) return NKEYS + i;
    return -1;
}

static int bind_pad(int a, SDL_GameControllerButton b)
{
    if (a < 0 || a >= NPAD || b == SDL_CONTROLLER_BUTTON_INVALID) return 0;
    for (int i = 0; i < NPAD; i++) if (i != a && padmap[i].button == b) padmap[i].button = padmap[a].button;
    padmap[a].button = b;
    return 1;
}

/* "action=Key Name" items separated by newlines or ';' (SDL key names, e.g. "W", "Up", "Keypad 1");
   "default" resets everything first. Returns the number of bindings applied. */
static int set_keys(const char *spec)
{
    int n = 0;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *item = strtok(buf, ";\n"); item; item = strtok(NULL, ";\n")) {
        while (*item == ' ' || *item == '\t') item++;
        if (!*item || *item == '#') continue;
        if (!strcmp(item, "default")) {
            for (int i = 0; i < NKEYS; i++) keymap[i].key = keymap[i].def;
            for (int i = 0; i < NPAD; i++) padmap[i].button = padmap[i].def;
            continue;
        }
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        char *e = eq - 1; while (e > item && *e == ' ') *e-- = 0;
        char *v = eq + 1; while (*v == ' ') v++;
        char *ve = v + strlen(v); while (ve > v && (ve[-1] == ' ' || ve[-1] == '\r')) *--ve = 0;
        int a = action_index(item);
        if (a < 0) { fprintf(stderr, "keys: unknown action '%s'\n", item); continue; }
        if (a >= NKEYS ? bind_pad(a - NKEYS, SDL_GameControllerGetButtonFromString(v)) : bind_key(a, SDL_GetKeyFromName(v))) n++;
        else fprintf(stderr, "keys: cannot bind %s to '%s'\n", item, v);
    }
    return n;
}

static const char *get_keys(void)
{
    static char out[2048];
    size_t o = 0;
    for (int i = 0; i < NKEYS; i++)
        o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s;", keymap[i].name, SDL_GetKeyName(keymap[i].key));
    for (int i = 0; i < NPAD; i++)
        o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s;", padmap[i].name, SDL_GameControllerGetStringForButton(padmap[i].button));
    return out;
}

static void handle_key(SDL_Keycode k, int down)
{
    for (int i = 0; i < NKEYS; i++)
        if (keymap[i].key == k) { if (down) *keymap[i].dst |= keymap[i].bit; else *keymap[i].dst &= (uint8_t)~keymap[i].bit; }
}

#ifdef __EMSCRIPTEN__
/* called from the page (web/shell.html) */
EMSCRIPTEN_KEEPALIVE int kof_set_keys(const char *spec) { return set_keys(spec); }
EMSCRIPTEN_KEEPALIVE const char *kof_get_keys(void) { return get_keys(); }
EMSCRIPTEN_KEEPALIVE void kof_learn_key(const char *action) { key_learn = action ? action_index(action) : -1; }
/* soft-DIP settings of the native BIOS (key settings panel) */
EMSCRIPTEN_KEEPALIVE const char *kof_soft_dip_info(void) { return bios_hle_level >= 3 ? bios_hle_soft_dip_info() : ""; }
EMSCRIPTEN_KEEPALIVE void kof_soft_dip_set(const char *spec) { if (bios_hle_level >= 3) bios_hle_soft_dip_set_now(spec); }
/* on-screen touch buttons: press / release a keyboard action */
EMSCRIPTEN_KEEPALIVE void kof_touch(const char *action, int down)
{
    int a = action_index(action);
    if (a < 0 || a >= NKEYS) return;
    if (down) *keymap[a].dst |= keymap[a].bit; else *keymap[a].dst &= (uint8_t)~keymap[a].bit;
}
#endif

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
        for (int b = 0; b < NPAD; b++) {
            if (!SDL_GameControllerGetButton(c, padmap[b].button)) continue;
            if (!padmap[b].sys) v |= padmap[b].bit;
            else if (padmap[b].bit == NG_SYS_START1) pad_sys |= i ? NG_SYS_START2 : NG_SYS_START1;
            else pad_sys |= i ? NG_SYS_COIN2 : NG_SYS_COIN1;
        }
        pad_p[i] = v;
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

/* ---- interactive loop: one emulated frame per call (native loop or browser main loop) ---- */
static struct {
    SDL_Window *win; SDL_Renderer *ren; SDL_Texture *tex; SDL_AudioDeviceID adev;
    SDL_GameController *pads[2]; int npad;
    uint64_t freq; double frame_ticks, next;
    int running, paused, shot, f;
} ui;

static void frame_step(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) ui.running = 0;
        else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            int down = ev.type == SDL_KEYDOWN;
            SDL_Keycode k = ev.key.keysym.sym;
            if (key_learn >= 0) {               /* web key settings: this key goes to the chosen action */
                if (down && !ev.key.repeat) {
                    int a = key_learn, ok = a < NKEYS && bind_key(a, k);     /* a pad action: any key cancels */
                    key_learn = -1;
                    (void)ok;
#ifdef __EMSCRIPTEN__
                    EM_ASM({ if (Module.onKeyBound) Module.onKeyBound(UTF8ToString($0), $1); },
                           a < NKEYS ? keymap[a].name : padmap[a - NKEYS].name, ok);
#endif
                }
            }
            else if (down && k == SDLK_ESCAPE) ui.running = 0;
            else if (down && k == SDLK_p && !ev.key.repeat) ui.paused = !ui.paused;
            else if (down && k == SDLK_F3) ng_reset();
            else if (down && k == SDLK_F12) {
                char path[64]; snprintf(path, sizeof path, "kof97_shot_%03d.ppm", ui.shot++); write_ppm(path, fb);
            }
            else handle_key(k, down);
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && key_learn >= NKEYS) {   /* pad button for a pad action */
            int a = key_learn, ok = bind_pad(a - NKEYS, (SDL_GameControllerButton)ev.cbutton.button);
            key_learn = -1;
            (void)ok;
#ifdef __EMSCRIPTEN__
            EM_ASM({ if (Module.onKeyBound) Module.onKeyBound(UTF8ToString($0), $1); }, padmap[a - NKEYS].name, ok);
#endif
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED && ui.npad < 2) {
            ui.pads[ui.npad++] = SDL_GameControllerOpen(ev.cdevice.which);
        }
    }
    if (!ui.paused) {
        poll_pads(ui.pads);
        uint8_t p1 = key_p1 | pad_p[0], p2 = key_p2 | pad_p[1], sys = key_sys | pad_sys;
        script_input(ui.f, &p1, &p2, &sys);
        ng_set_input(p1, p2, sys);
        script_pokes(ui.f);
        ng_run_frame(fb);
        ui.f++;
        int n;
        while ((n = read_audio(abuf, 8192)) > 0) {
            if (wavf) { fwrite(abuf, 4, (size_t)n, wavf); wav_frames += (uint32_t)n; }
#ifdef __EMSCRIPTEN__
            if (web_worklet) EM_ASM({ Module.audioPush(HEAP16.subarray($0 >> 1, ($0 >> 1) + $1 * 2)); }, abuf, n);
#endif
            if (ui.adev) {
                /* keep latency bounded: drop audio if the queue grows beyond ~150 ms */
                if (SDL_GetQueuedAudioSize(ui.adev) > (Uint32)(AUDIO_RATE * 4 * 15 / 100)) SDL_ClearQueuedAudio(ui.adev);
                SDL_QueueAudio(ui.adev, abuf, (Uint32)n * 4);
            }
        }
    }
    if (use_gpu) {
        if (!ui.paused) gpu_frame(fb);
        gpu_present();
    } else {
        SDL_UpdateTexture(ui.tex, NULL, ng_hd_fb ? ng_hd_fb : fb, NG_SCREEN_W * ng_hd_scale * 4);
        SDL_RenderClear(ui.ren);
        SDL_RenderCopy(ui.ren, ui.tex, NULL, NULL);
        SDL_RenderPresent(ui.ren);
    }
    if (opt.dump_prefix && !ui.paused && ui.f % opt.dump_every == 0) {     /* screenshots of the window too */
        char path[600];
        snprintf(path, sizeof path, "%s_%06d.ppm", opt.dump_prefix, ui.f);
        write_ppm(path, fb);
    }
}

#ifdef __EMSCRIPTEN__
/* the backup RAM (soft DIPs, book keeping, high scores) is kept by the page in localStorage: every ~10 s, if
   it changed, hand it to Module.saveNvram (web/shell.html), which writes it back to the file before the next
   start */
static void web_save_nvram(void)
{
    static uint32_t last;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof ng.backup; i++) h = (h ^ ng.backup[i]) * 16777619u;
    if (h == last) return;
    last = h;
    EM_ASM({ if (Module.saveNvram) Module.saveNvram(HEAPU8.subarray($0, $0 + $1)); }, ng.backup, (int)sizeof ng.backup);
}

/* browser animation frames come at the display rate: run as many 59.185 Hz game frames as are due */
static void web_tick(void)
{
    double now = (double)SDL_GetPerformanceCounter();
    int n = 0;
    while (ui.next <= now && n < 4) {
        frame_step(); ui.next += ui.frame_ticks; n++;
        if (ui.f % 600 == 0) web_save_nvram();
    }
    if (now - ui.next > ui.frame_ticks * 10) ui.next = now;
    if (!n) {                                       /* still redraw so the page stays responsive */
        if (use_gpu) gpu_present();
        else { SDL_RenderClear(ui.ren); SDL_RenderCopy(ui.ren, ui.tex, NULL, NULL); SDL_RenderPresent(ui.ren); }
    }
}
#endif

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define TAKE (v ? (i++, v) : (usage(), exit(1), (const char *)NULL))
        if (!strcmp(a, "--roms")) opt.roms = TAKE;
        else if (!strcmp(a, "--frame-render")) ng_frame_render = 1;
        else if (!strcmp(a, "--gpu")) use_gpu = 1;
        else if (!strcmp(a, "--aes")) ng_aes = 1;
        else if (!strcmp(a, "--bios-hle")) bios_level = atoi(TAKE);
        else if (!strcmp(a, "--bios-hle-timing")) bios_timing = TAKE;
        else if (!strcmp(a, "--bios-log")) bios_hle_log = 1;
        else if (!strcmp(a, "--soft-dip")) soft_dip = TAKE;
        else if (!strcmp(a, "--test-raster")) ng_test_raster = 1;
        else if (!strcmp(a, "--test-cpu-scale")) { int k = atoi(TAKE); ng_test_cpu_scale = k > 0 ? k : 1; }
        else if (!strcmp(a, "--hd")) ng_hd_scale = atoi(TAKE);
        else if (!strcmp(a, "--sound-log")) sound_log_path = TAKE;
        else if (!strcmp(a, "--keys")) {
            const char *path = TAKE;
            FILE *kf = fopen(path, "r");
            char spec[2048] = "";
            if (!kf) { fprintf(stderr, "cannot open %s\n", path); return 1; }
            spec[fread(spec, 1, sizeof spec - 1, kf)] = 0;
            fclose(kf);
            set_keys(spec);
        }
        else if (!strcmp(a, "--print-keys")) {
            for (const char *k = get_keys(); *k; k++) putchar(*k == ';' ? '\n' : *k);
            return 0;
        }
        else if (!strcmp(a, "--native-audio")) native_audio = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "build/assets/kof97/audio";
        else if (!strcmp(a, "--mods")) mods_dir = TAKE;
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

    if (use_gpu && opt.headless) use_gpu = 0;
    if (use_gpu) { ng_hd_scale = 1; ng_tagged = 1; }   /* the GPU composes at any size */
    if (ng_hd_scale < 1 || ng_hd_scale > 8) ng_hd_scale = 1;
    if (ng_hd_scale > 1) ng_hd_fb = calloc((size_t)NG_SCREEN_W * NG_SCREEN_H * ng_hd_scale * ng_hd_scale, 4);
    bios_hle_init(bios_level, bios_timing);
    if (!ng_load_roms(opt.roms)) return 1;
    if (soft_dip && !strcmp(soft_dip, "list")) { bios_hle_soft_dip_list(); return 0; }
    if (soft_dip) {
        if (bios_level < 3) fprintf(stderr, "--soft-dip needs --bios-hle 3 (with the BIOS ROM: F2 menu)\n");
        bios_hle_soft_dip(soft_dip);
    }
    if (native_audio && !snd_native_init(native_audio, mods_dir ? mods_dir : "")) {
#ifdef __EMSCRIPTEN__
        fprintf(stderr, "no packed sounds: using the emulated sound chips\n");   /* build without `make audio-ogg` */
        native_audio = NULL;
#else
        return 1;
#endif
    }
    if (mods_dir) spr_native_init(mods_dir);
    if (sound_log_path && (sound_log_f = fopen(sound_log_path, "w"))) { ng_sound_log = sound_log_cb; audio_set_reply_hook(sound_reply_cb); }
    ng_init(opt.interp ? CPU_MODE_INTERP : CPU_MODE_RECOMP, (opt.misses && *opt.misses) ? opt.misses : NULL);
    if (exec_log) cpu_set_exec_log(exec_log);
    ng_load_nvram(opt.nvram);
    ng_reset();
    if (opt.wav) wav_open(opt.wav);


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
            while ((n = read_audio(abuf, 8192)) > 0)
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
    if (use_gpu) gpu_gl_attributes();
    SDL_Window *win = SDL_CreateWindow("KOF97 (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       NG_SCREEN_W * opt.scale, NG_SCREEN_H * opt.scale,
                                       SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) |
                                       (use_gpu ? SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI : 0));
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    if (use_gpu && !gpu_init(win)) {
        fprintf(stderr, "gpu: falling back to the SDL renderer\n");
        use_gpu = 0; ng_tagged = 0;
    }
    if (!use_gpu) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        SDL_RenderSetLogicalSize(ren, NG_SCREEN_W * ng_hd_scale, NG_SCREEN_H * ng_hd_scale);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                NG_SCREEN_W * ng_hd_scale, NG_SCREEN_H * ng_hd_scale);
    }

    SDL_AudioSpec want = { 0 }, have;
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID adev = 0;
#ifdef __EMSCRIPTEN__
    web_worklet = EM_ASM_INT({ return Module.audioPush ? 1 : 0; });   /* the page plays it (AudioWorklet) */
#endif
    if (!web_worklet) {
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!adev) fprintf(stderr, "no audio: %s\n", SDL_GetError());
        else SDL_PauseAudioDevice(adev, 0);
    }

    SDL_GameController *pads[2] = { 0 };
    int npad = 0;
    for (int i = 0; i < SDL_NumJoysticks() && npad < 2; i++)
        if (SDL_IsGameController(i)) pads[npad++] = SDL_GameControllerOpen(i);

    ui.win = win; ui.ren = ren; ui.tex = tex; ui.adev = adev; ui.npad = npad;
    ui.pads[0] = pads[0]; ui.pads[1] = pads[1];
    ui.freq = SDL_GetPerformanceFrequency();
    ui.frame_ticks = (double)ui.freq / NG_FPS;
    ui.next = (double)SDL_GetPerformanceCounter();
    ui.running = 1;
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(web_tick, 0, 1);      /* the browser drives the loop; never returns */
#else
    while (ui.running && (!opt.frames || ui.f < opt.frames)) {
        frame_step();
        ui.next += ui.frame_ticks;
        double now = (double)SDL_GetPerformanceCounter();
        if (ui.next > now) SDL_Delay((Uint32)((ui.next - now) * 1000.0 / (double)ui.freq));
        else if (now - ui.next > ui.frame_ticks * 10) ui.next = now;     /* fell behind: resync */
    }
#endif
    print_stats();
    wav_close();
    ng_save_nvram(opt.nvram);
    ng_shutdown();
    if (ui.adev) SDL_CloseAudioDevice(ui.adev);
    SDL_Quit();
    return 0;
}

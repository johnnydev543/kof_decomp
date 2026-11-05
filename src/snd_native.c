/* Native sound: plays pre-rendered (or remade) audio files for the game's sound commands instead of running
 * the Z80 sound driver and the YM2610. The 68000 still writes command bytes to REG_SOUND; they are parsed
 * with the driver's byte grammar and mixed by these rules (decoded from the driver, see AGENT.md):
 *   music      one player; a new song replaces the old at once; stems loop independently
 *   adpcma     3-voice pool (channels 3-5) for the 2-byte voice / sfx commands 1A-1E xx; full -> newest stolen
 *   ssg        one voice for the 1-byte UI sounds (60-7F)
 *   adpcmb     one voice (19 xx)
 *   03 reset / 04 stop all + mute / 05 stop music + mute music / 06 stop sfx + mute sfx /
 *   02 eyecatch (the BIOS sends it after 03, so it unmutes music) / 07, 08, 09 unmute both / music / sfx / 20 stop music / 0A nn fade music out / 14 = stop modifier for
 *   the next 2-byte command (stops a looping sound)
 * The Z80's reply is the last command byte (00 after a reset), which is what the BIOS and game expect.
 * Files: <assets>/<cmd>.wav (tools/assets/export_audio.py, 16-bit stereo 55555 Hz) listed in
 * <assets>/commands.txt; a file of the same name in the mods directory is used instead (remade sounds).
 * Where a .wav is missing, the .ogg of the same name is used (Ogg Vorbis, `make audio-ogg`; the web build
 * packs these). Loaded on first use and kept. */
#include "snd_native.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb/stb_vorbis.c"

#define MAX_CMDS 2048
#define MAX_FILES 4

typedef struct { int16_t *pcm; int frames; int loop_start, loop_end; char name[24]; int tried; } sample_t;
typedef struct { uint16_t cmd; char group; int nfiles; sample_t f[MAX_FILES]; } sound_t;
typedef struct { sample_t *s[MAX_FILES]; int pos[MAX_FILES]; int n; uint16_t cmd; int active; int looping; } voice_t;

static sound_t sounds[MAX_CMDS];
static int nsounds;
static char asset_dir[512], mods_dir[512];
static voice_t music, pool[3], ssg, pcmb;
static int pool_order[3];                  /* start order counter for stealing the newest */
static int pool_clock;
static int mute_music, mute_sfx;
static int fade_total, fade_left;          /* music fade in samples */
static int prefix, stop_next;              /* pending 2-byte prefix, 14 modifier */
static uint8_t reply;
static int enabled;

static int load_wav(const char *path, sample_t *s)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return 0; }
    int channels = 2, bits = 16;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) { fclose(f); return 0; }
        uint32_t len = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) { fclose(f); return 0; }
            channels = fm[2] | fm[3] << 8; bits = fm[14] | fm[15] << 8;
            fseek(f, len - 16, SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (bits != 16 || (channels != 1 && channels != 2)) { fclose(f); return 0; }
            int n = (int)(len / (2 * channels));
            s->pcm = malloc((size_t)n * 4);
            int16_t *tmp = channels == 2 ? s->pcm : malloc((size_t)n * 2);
            size_t got = fread(tmp, 2 * channels, (size_t)n, f);
            if (channels == 1) { for (size_t i = 0; i < got; i++) s->pcm[2 * i] = s->pcm[2 * i + 1] = tmp[i]; free(tmp); }
            s->frames = (int)got;
            fclose(f);
            return 1;
        } else fseek(f, len + (len & 1), SEEK_CUR);
    }
}

static int load_ogg(const char *path, sample_t *s)
{
    int channels, rate;
    short *pcm;
    int n = stb_vorbis_decode_filename(path, &channels, &rate, &pcm);
    if (n <= 0) return 0;
    if (rate != AUDIO_RATE) fprintf(stderr, "snd_native: %s is %d Hz, not %d\n", path, rate, AUDIO_RATE);
    s->pcm = malloc((size_t)n * 4);
    for (int i = 0; i < n; i++) {
        s->pcm[2 * i] = pcm[i * channels];
        s->pcm[2 * i + 1] = pcm[i * channels + (channels > 1)];
    }
    free(pcm);
    s->frames = n;
    return 1;
}

/* dir/name, or the .ogg of the same name */
static int load_sound(const char *dir, const char *name, sample_t *s)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (load_wav(p, s)) return 1;
    char *dot = strrchr(p, '.');
    if (!dot || strcmp(dot, ".wav")) return 0;
    strcpy(dot, ".ogg");
    return load_ogg(p, s);
}

static sample_t *need(sample_t *s)
{
    if (!s->tried) {
        s->tried = 1;
        if ((!mods_dir[0] || !load_sound(mods_dir, s->name, s)) && !load_sound(asset_dir, s->name, s))
            fprintf(stderr, "snd_native: cannot load %s/%s\n", asset_dir, s->name);
        if (s->loop_end > s->frames) s->loop_end = s->frames;
    }
    return s->pcm ? s : NULL;
}

int snd_native_init(const char *assets, const char *mods)
{
    char p[600];
    snprintf(asset_dir, sizeof asset_dir, "%s", assets);
    snprintf(mods_dir, sizeof mods_dir, "%s", mods ? mods : "");
    snprintf(p, sizeof p, "%s/commands.txt", assets);
    FILE *f = fopen(p, "r");
    if (!f) { fprintf(stderr, "snd_native: %s missing (run tools/assets/export_audio.py)\n", p); return 0; }
    char line[1024];
    while (fgets(line, sizeof line, f) && nsounds < MAX_CMDS) {
        sound_t *s = &sounds[nsounds];
        char cmd[16], group[16];
        int loop, n, off;
        if (sscanf(line, "%15s %15s %d %d%n", cmd, group, &loop, &n, &off) != 4) continue;
        memset(s, 0, sizeof *s);
        s->cmd = (uint16_t)strtol(cmd, NULL, 16);
        s->group = !strcmp(group, "music") ? 'm' : !strcmp(group, "ssg") ? 's' : !strcmp(group, "adpcmb") ? 'b' : 'a';
        char *q = line + off;
        for (int i = 0; i < n && i < MAX_FILES; i++) {
            int k;
            if (sscanf(q, "%23s %d %d%n", s->f[i].name, &s->f[i].loop_start, &s->f[i].loop_end, &k) != 3) break;
            q += k;
            s->nfiles = i + 1;
        }
        nsounds++;
    }
    fclose(f);
    enabled = 1;
    return nsounds;
}

int snd_native_enabled(void) { return enabled; }

static sound_t *find(uint16_t cmd)
{
    for (int i = 0; i < nsounds; i++) if (sounds[i].cmd == cmd) return &sounds[i];
    return NULL;
}

static void start(voice_t *v, sound_t *s)
{
    memset(v, 0, sizeof *v);
    v->cmd = s->cmd;
    for (int i = 0; i < s->nfiles; i++) if (need(&s->f[i])) v->s[v->n++] = &s->f[i];
    v->active = v->n > 0;
    for (int i = 0; i < v->n; i++) if (v->s[i]->loop_end > 0) v->looping = 1;
}

static void play(uint16_t cmd)
{
    sound_t *s = find(cmd);
    if (!s) return;
    switch (s->group) {
    case 'm': if (!mute_music) { start(&music, s); fade_left = 0; } break;
    case 's': if (!mute_sfx) start(&ssg, s); break;
    case 'b': if (!mute_sfx) start(&pcmb, s); break;
    default: {
        if (mute_sfx) break;
        int k = -1;
        for (int i = 0; i < 3; i++) if (!pool[i].active) { k = i; break; }
        if (k < 0) {                                  /* full: steal the newest */
            k = 0;
            for (int i = 1; i < 3; i++) if (pool_order[i] > pool_order[k]) k = i;
        }
        start(&pool[k], s);
        pool_order[k] = ++pool_clock;
    }
    }
}

static void stop_sfx(void) { memset(pool, 0, sizeof pool); memset(&ssg, 0, sizeof ssg); memset(&pcmb, 0, sizeof pcmb); }

void snd_native_command(uint8_t b)
{
    if (prefix) {                                     /* second byte of a 2-byte command */
        uint16_t cmd = (uint16_t)(prefix << 8 | b);
        int p = prefix;
        prefix = 0;
        if (p == 0x0a) { if (music.active && !fade_left) { fade_total = fade_left = 55555 * 18 / 10; } }
        else if (stop_next) {
            stop_next = 0;
            for (int i = 0; i < 3; i++) if (pool[i].active && pool[i].cmd == cmd) pool[i].active = 0;
            if (pcmb.active && pcmb.cmd == cmd) pcmb.active = 0;
        } else play(cmd);
        reply = b;
        return;
    }
    reply = b;
    if (b == 0x0a || (b >= 0x18 && b <= 0x1e)) { prefix = b; return; }
    switch (b) {
    case 0x00: case 0x01: case 0x10: break;
    case 0x02: mute_music = 0; play(b); break;      /* BIOS eyecatch jingle, sent right after the 03 reset */
    case 0x03: memset(&music, 0, sizeof music); stop_sfx(); mute_music = mute_sfx = 1; reply = 0; break;
    case 0x04: memset(&music, 0, sizeof music); stop_sfx(); mute_music = mute_sfx = 1; break;
    case 0x05: memset(&music, 0, sizeof music); mute_music = 1; break;
    case 0x06: stop_sfx(); mute_sfx = 1; break;
    case 0x07: mute_music = mute_sfx = 0; break;
    case 0x08: mute_music = 0; break;
    case 0x09: mute_sfx = 0; break;
    case 0x14: stop_next = 1; break;
    case 0x20: memset(&music, 0, sizeof music); break;
    default: play(b); break;
    }
}

uint8_t snd_native_reply(void) { return reply; }

/* mix one voice into acc (stereo int32), advancing its files; returns 0 when it ended */
static int mix_voice(voice_t *v, int32_t *acc, int n, int gain256)
{
    if (!v->active) return 0;
    int alive = 0;
    for (int i = 0; i < v->n; i++) {
        sample_t *s = v->s[i];
        int pos = v->pos[i];
        if (pos < 0) continue;
        for (int k = 0; k < n; k++) {
            if (pos >= s->frames || (s->loop_end > 0 && pos >= s->loop_end)) {
                if (s->loop_end > 0 && s->loop_start >= 0) pos = s->loop_start;
                else { pos = -1; break; }
            }
            acc[2 * k] += s->pcm[2 * pos] * gain256 >> 8;
            acc[2 * k + 1] += s->pcm[2 * pos + 1] * gain256 >> 8;
            pos++;
        }
        v->pos[i] = pos;
        if (pos >= 0) alive = 1;
    }
    if (!alive) v->active = 0;
    return alive;
}

int snd_native_render(int16_t *out, int frames)
{
    int32_t acc[2 * 1024];
    int done = 0;
    while (done < frames) {
        int n = frames - done > 1024 ? 1024 : frames - done;
        memset(acc, 0, sizeof(int32_t) * 2 * (size_t)n);
        int g = 256;
        if (fade_left > 0) {
            g = (int)(256LL * fade_left / fade_total);
            fade_left -= n;
            if (fade_left <= 0) { memset(&music, 0, sizeof music); fade_left = 0; }
        }
        mix_voice(&music, acc, n, g);
        for (int i = 0; i < 3; i++) mix_voice(&pool[i], acc, n, 256);
        mix_voice(&ssg, acc, n, 256);
        mix_voice(&pcmb, acc, n, 256);
        for (int k = 0; k < 2 * n; k++) {
            int32_t x = acc[k];
            out[2 * done + k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        done += n;
    }
    return frames;
}

/* Sprite replacement: draw a remade image (e.g. redrawn with ComfyUI) instead of the tiles of a sprite frame.
 *
 * A native hook runs where the game has chosen the frame of an object and is about to write its tiles
 * (sprite_draw_requests at 0x5d8a, game.json runtime.hooks). It records, for the hardware sprites the frame
 * will occupy, the frame's key (the P2 address of its frame header, as named by
 * tools/assets/export_sprites.py), its size in tiles and its flips. The renderer (video.c) then draws
 * mods/<game>/sprites/<key>.png, scaled to the frame's box, in place of those sprites' tiles; frames without
 * a replacement are drawn from the tiles as before. The game redraws every visible object each frame, so a
 * record older than two frames is ignored (the sprites may have been reused by other code).
 * Each record also holds the palette the frame was exported with (the same choice as export_sprites.py:
 * a player's default colour set for the character's frames, a grey ramp for everything else), so the
 * renderer can follow palette changes. */
#include "spr_native.h"
#include "neogeo.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "../third_party/stb/stb_image.h"

#define A5 0x108000u
/* sprite data (games/kof97/game.json "sprites"): frame tables (P2 bank 1), ROM palettes (P2 bank 2),
   player / effect palette start tables (P1) */
#define FRAME_TABLES 0x250000u
#define ROM_PALETTES 0x2cfff0u
#define PLAYER_PALETTES 0x483eu
#define EFFECT_PALETTES 0x4956u
#define NSETS 35
#define NSPR 448
#define CACHE 8192

typedef struct { uint32_t key; int state; spr_image img; } cache_ent;   /* state: 0 unknown, 1 loaded, -1 none */

static spr_block blocks[NSPR];            /* by first hardware sprite */
static int16_t owner[NSPR];               /* sprite -> first sprite of its block, -1 none */
static uint32_t owner_frame[NSPR];
static cache_ent cache[CACHE];
static char sprite_dir[512];
static int enabled;
static unsigned long hooks, drawn;

unsigned int m68k_read_memory_8(unsigned int a);
unsigned int m68k_read_memory_16(unsigned int a);
unsigned int m68k_read_memory_32(unsigned int a);

static uint32_t set_tables[NSETS];        /* frame table of each object set */

static uint32_t p2_off(uint32_t addr, int bank) { return addr - 0x200000u + (uint32_t)bank * 0x100000u; }
static unsigned rd8(const ng_blob *b, uint32_t o) { return o < b->size ? b->data[o] : 0; }
static unsigned rd16(const ng_blob *b, uint32_t o) { return rd8(b, o) << 8 | rd8(b, o + 1); }
static uint32_t rd32(const ng_blob *b, uint32_t o) { return (uint32_t)rd16(b, o) << 16 | rd16(b, o + 2); }

static uint32_t ng_colour(unsigned d)
{
    int dark = d >> 15;
    int r = ((d >> 14) & 1) | ((d >> 7) & 0x1e), g = ((d >> 13) & 1) | ((d >> 3) & 0x1e), b = ((d >> 12) & 1) | ((d << 1) & 0x1e);
    r = ((r << 1) | !dark) * 255 / 63; g = ((g << 1) | !dark) * 255 / 63; b = ((b << 1) | !dark) * 255 / 63;
    return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* the palette export_sprites.py drew the frame with: object set from its frame table, palette byte from
   the frame header (0x10-0x1f character colours, 0x20-0x2f character effects, colour set 0) */
static void ref_palette(uint32_t table, uint32_t key, uint32_t *ref)
{
    int set = -1;
    for (int i = 0; i < NSETS; i++) if (set_tables[i] == table) { set = i; break; }
    unsigned pal = rd8(&ngr.p2, p2_off(key, 1));
    int n = -1;
    if (set >= 0 && set < 32 && pal >= 0x10 && pal < 0x20) n = (int)rd16(&ngr.p1, PLAYER_PALETTES + (uint32_t)set * 8) + (int)pal - 0x10;
    if (set >= 0 && set < 32 && pal >= 0x20 && pal < 0x30) n = (int)rd16(&ngr.p1, EFFECT_PALETTES + (uint32_t)set * 2) + (int)pal - 0x20;
    for (int i = 0; i < 16; i++)
        ref[i] = n < 0 ? 0xff000000u | (uint32_t)(i * 17) * 0x010101u
                       : ng_colour(rd16(&ngr.p2, p2_off(ROM_PALETTES, 2) + (uint32_t)n * 32 + (uint32_t)i * 2));
}

int spr_native_init(const char *mods_dir)
{
    for (int i = 0; i < NSETS; i++) set_tables[i] = rd32(&ngr.p2, p2_off(FRAME_TABLES + (uint32_t)i * 4, 1));
    snprintf(sprite_dir, sizeof sprite_dir, "%s/sprites", mods_dir);
    for (int i = 0; i < NSPR; i++) owner[i] = -1;
    enabled = 1;
    return 1;
}

static const spr_image *image_for(uint32_t key)
{
    unsigned h = (key * 2654435761u) >> 19;              /* 13-bit hash */
    for (unsigned i = 0; i < CACHE; i++) {
        cache_ent *e = &cache[(h + i) & (CACHE - 1)];
        if (e->state && e->key != key) continue;
        if (!e->state) {
            char path[640];
            int w, hh, n;
            snprintf(path, sizeof path, "%s/%06x.png", sprite_dir, key);
            e->key = key;
            e->img.rgba = stbi_load(path, &w, &hh, &n, 4);
            e->state = e->img.rgba ? 1 : -1;
            if (e->img.rgba) { e->img.w = w; e->img.h = hh; fprintf(stderr, "sprites: %s %dx%d\n", path, w, hh); }
        }
        return e->state > 0 ? &e->img : NULL;
    }
    return NULL;
}

/* hook at sprite_draw_requests 0x5d8a: A4 = object, its frame set up by sprite_setup_frame */
static void sprite_cel(void)
{
    uint32_t obj = m68k_get_reg(NULL, M68K_REG_A4);
    uint32_t table = m68k_read_memory_32(obj + 0x3c);
    uint32_t key = m68k_read_memory_32(table + (m68k_read_memory_16(A5 + 0x64e4) & 0x3ff) * 4) & 0xffffff;
    int w = m68k_read_memory_16(A5 + 0x64be), h = m68k_read_memory_16(A5 + 0x64c0);
    int second = m68k_read_memory_8(obj + 0x3b) & 0x80;
    int first = (m68k_read_memory_16(A5 + (second ? 0x307a : 0x3070)) >> 6) + m68k_read_memory_16(A5 + (second ? 0x3080 : 0x3076));
    hooks++;
    if (first < 0 || first >= NSPR || w <= 0 || w > 32 || h <= 0 || h > 32) return;
    const spr_image *img = image_for(key);
    if (!img) {                                            /* no replacement: release these sprites */
        for (int s = first; s < first + w && s < NSPR; s++) owner[s] = -1;
        return;
    }
    spr_block *b = &blocks[first];
    if (b->img != img || b->key != key || !b->ref[0]) ref_palette(table, key, b->ref);
    b->key = key; b->first = first; b->w = w; b->h = h;
    b->flips = m68k_read_memory_8(A5 + 0x64ea) & 3;
    b->img = img;
    for (int s = first; s < first + w && s < NSPR; s++) { owner[s] = (int16_t)first; owner_frame[s] = (uint32_t)ng.frame; }
}

void rt_native_hook(uint32_t pc)
{
    (void)pc;                       /* one hook for now (sprite_draw_requests 0x5d8a) */
    if (enabled) sprite_cel();
}

const spr_block *spr_native_block(int sprite)
{
    if (!enabled || sprite < 0 || sprite >= NSPR || owner[sprite] < 0) return NULL;
    if ((uint32_t)ng.frame - owner_frame[sprite] > 2) return NULL;
    drawn++;
    return &blocks[owner[sprite]];
}

void spr_native_stats(void)
{
    if (enabled) fprintf(stderr, "sprites: %lu hooks\n", hooks);
}

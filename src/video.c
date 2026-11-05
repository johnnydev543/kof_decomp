/* Neo Geo LSPC video: per-scanline sprite + fix layer renderer (behaviour follows MAME neogeo_spr). */
#include <stdlib.h>
#include <string.h>
#include "neogeo.h"
#include "spr_native.h"

#define MAX_SPRITES_PER_LINE 96
#define MAX_SPRITES 381

static uint8_t *spr_gfx;          /* one byte per pixel, 256 bytes per 16x16 tile */
static uint32_t spr_mask;          /* address mask (power of two - 1) */

static const uint8_t zoom_x_tables[16][16] = {
    { 0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0 },
    { 0,0,1,0,1,0,0,0,1,0,0,0,1,0,0,0 },
    { 0,0,1,0,1,0,0,0,1,0,0,0,1,0,1,0 },
    { 0,0,1,0,1,0,1,0,1,0,0,0,1,0,1,0 },
    { 0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0 },
    { 1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0 },
    { 1,0,1,0,1,0,1,0,1,1,1,0,1,0,1,0 },
    { 1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,0 },
    { 1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,1 },
    { 1,0,1,1,1,0,1,1,1,1,1,0,1,0,1,1 },
    { 1,0,1,1,1,0,1,1,1,1,1,0,1,1,1,1 },
    { 1,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1 },
    { 1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1 },
    { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 },
};

static void decode_sprites(void)
{
    size_t len = ngr.crom.size;
    size_t out = len * 2;
    size_t p2 = 1;
    while (p2 < out) p2 <<= 1;
    spr_mask = (uint32_t)(p2 - 1);
    spr_gfx = calloc(p2, 1);
    const uint8_t *src = ngr.crom.data;
    uint8_t *dest = spr_gfx;
    for (size_t i = 0; i + 0x80 <= len; i += 0x80, src += 0x80) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 8; x++)
                *dest++ = (uint8_t)((((src[0x43 | (y << 2)] >> x) & 1) << 3) | (((src[0x41 | (y << 2)] >> x) & 1) << 2) |
                                    (((src[0x42 | (y << 2)] >> x) & 1) << 1) | (((src[0x40 | (y << 2)] >> x) & 1) << 0));
            for (int x = 0; x < 8; x++)
                *dest++ = (uint8_t)((((src[0x03 | (y << 2)] >> x) & 1) << 3) | (((src[0x01 | (y << 2)] >> x) & 1) << 2) |
                                    (((src[0x02 | (y << 2)] >> x) & 1) << 1) | (((src[0x00 | (y << 2)] >> x) & 1) << 0));
        }
    }
}

void video_update_pen(int bank, int idx)
{
    uint16_t d = ng.pal[bank][idx];
    int dark = d >> 15;
    int r = ((d >> 14) & 1) | ((d >> 7) & 0x1e);
    int g = ((d >> 13) & 1) | ((d >> 3) & 0x1e);
    int b = ((d >> 12) & 1) | ((d << 1) & 0x1e);
    /* 5 bits + inverted dark bit as LSB -> 6 bits */
    r = (r << 1) | !dark; g = (g << 1) | !dark; b = (b << 1) | !dark;
    r = (r * 255) / 63; g = (g * 255) / 63; b = (b * 255) / 63;
    ng.pens[bank][idx] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void video_init(void)
{
    decode_sprites();
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 0x1000; i++) video_update_pen(b, i);
}

static int sprite_on_scanline(int scanline, int y, int rows)
{
    int max_y = (y + rows * 0x10 - 1) & 0x1ff;
    return ((max_y >= y) && (scanline >= y) && (scanline <= max_y)) ||
           ((max_y < y) && ((scanline >= y) || (scanline <= max_y)));
}

/* ---- replacement images (spr_native.c), high-resolution and GPU output ----
 * A replacement is drawn column by column like the tiles it stands for: each visible pixel of a column takes
 * the image pixel at the same place in the frame's box (16w x 16h), so shrunk (zoomed out) objects follow the
 * hardware's line and column selection.
 * Tagged frames (video_render_frame, used by --hd and the GPU output): a pixel a replacement wrote holds a tag
 * (alpha 0, low byte = index into recs) instead of a colour; later sprites and the fix layer overwrite it as
 * usual. under[] keeps the colour below each tagged pixel. A rec maps screen positions to box positions
 * (origin and scale, which differ from 1 for shrunk objects), so the image can be sampled at any resolution:
 * video_compose (CPU) or gpu.c.
 * Where the column's palette has a single colour (the characters' shadows: the frame squashed vertically in a
 * flat dark palette) the image only gives the shape, filled with that colour. */
static int tagging;                              /* rendering a tagged frame */
static video_rec recs[VIDEO_MAX_RECS];
static int nrecs;
static uint32_t under[NG_SCREEN_W * NG_SCREEN_H];  /* colour under a tagged pixel */
static uint32_t onex[NG_SCREEN_W * NG_SCREEN_H];   /* the replacement's own 1x colour at a tagged pixel */
static int tag_row;                              /* first index of the current line in under[] / onex[] */

/* the image pixel at box position (fx, fy), continuous box pixels (pixel centres at .5); NULL outside */
static inline const uint8_t *repl_at(const spr_block *b, float fx, float fy)
{
    const spr_image *im = b->img;
    float bw = 16.0f * b->w, bh = 16.0f * b->h;
    if (fx < 0 || fy < 0 || fx >= bw || fy >= bh) return NULL;
    if (b->flips & 1) fx = bw - fx;
    if (b->flips & 2) fy = bh - fy;
    int ix = (int)(fx * im->w / bw), iy = (int)(fy * im->h / bh);
    if (ix >= im->w) ix = im->w - 1;
    if (iy >= im->h) iy = im->h - 1;
    return im->rgba + ((size_t)iy * im->w + ix) * 4;
}

static inline uint32_t rgba_px(const uint8_t *c) { return 0xff000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2]; }

static int find_rec(const spr_block *b, int x0, int y0, int zoom_x, int zoom_y, uint32_t fill)
{
    x0 &= 0x1ff; y0 &= 0x1ff;
    for (int i = nrecs - 1; i >= 0; i--)
        if (recs[i].b == b && recs[i].x0 == x0 && recs[i].y0 == y0 && recs[i].zoom_x == zoom_x &&
            recs[i].zoom_y == zoom_y && recs[i].fill == fill) return i;
    if (nrecs == VIDEO_MAX_RECS) return -1;
    recs[nrecs] = (video_rec){ b, x0, y0, zoom_x, zoom_y, 16.0f / (float)(zoom_x + 1), 256.0f / (float)(zoom_y + 1), fill };
    return nrecs++;
}

/* the single colour of a palette whose 15 colours are all the same, else 0 */
static uint32_t flat_colour(const uint32_t *pal)
{
    for (int i = 2; i < 16; i++) if (pal[i] != pal[1]) return 0;
    return pal[1];
}

/* one line of one column of a replacement: column c of the block, box row fy, columns chosen by zx */
static void draw_replacement_column(const spr_block *b, int c, int fy, int x, int y, int zoom_x, int zoom_y,
                                    const uint8_t *zx, uint32_t fill, uint32_t *line)
{
    int rec = tagging ? find_rec(b, x - c * (zoom_x + 1), y, zoom_x, zoom_y, fill) : -1;
    int px = x;
    for (int k = 0; k < 16; k++) {
        if (!zx[k]) continue;
        int sx = px++ & 0x1ff;
        if (sx >= NG_SCREEN_W) continue;
        const uint8_t *col = repl_at(b, c * 16 + k + 0.5f, fy + 0.5f);
        int opaque = col && col[3] >= 128;
        uint32_t v = opaque ? (fill ? fill : rgba_px(col)) : 0;
        if (rec < 0) { if (opaque) line[sx] = v; continue; }
        uint32_t *u = &under[tag_row + sx], *o = &onex[tag_row + sx];
        uint32_t below = line[sx];
        *u = (below >> 24) ? below : *o;        /* below another replacement: its 1x colour */
        *o = opaque ? v : *u;
        line[sx] = (uint32_t)rec;                 /* tag: alpha 0 */
    }
}

static void draw_sprites(int scanline, uint32_t *line, const uint32_t *pens)
{
    const uint16_t *vram = ng.vram;
    uint16_t list[MAX_SPRITES_PER_LINE];
    int count = 0;
    int y = 0, rows = 0;

    for (int sn = 0; sn < MAX_SPRITES && count < MAX_SPRITES_PER_LINE; sn++) {
        uint16_t yc = vram[0x8200 | sn];
        if (!(yc & 0x40)) {
            y = 0x200 - (yc >> 7);
            rows = yc & 0x3f;
        }
        if (rows == 0) continue;
        if (sprite_on_scanline(scanline, y, rows)) list[count++] = (uint16_t)sn;
    }

    int x = 0, zoom_x = 0, zoom_y = 0;
    y = 0; rows = 0;
    for (int i = 0; i < count; i++) {
        int sn = list[i];
        uint16_t yc = vram[0x8200 | sn];
        uint16_t zc = vram[0x8000 | sn];
        if (yc & 0x40) {
            x = (x + zoom_x + 1) & 0x1ff;
            zoom_x = (zc >> 8) & 0x0f;
        } else {
            y = 0x200 - (yc >> 7);
            x = vram[0x8400 | sn] >> 7;
            zoom_y = zc & 0xff;
            zoom_x = (zc >> 8) & 0x0f;
            rows = yc & 0x3f;
        }
        if (!sprite_on_scanline(scanline, y, rows)) continue;
        if (x >= 0x140 && x <= 0x1f0) continue;

        int sprite_line = (scanline - y) & 0x1ff;
        int zoom_line = sprite_line & 0xff;
        int invert = sprite_line & 0x100;
        if (invert) zoom_line ^= 0xff;
        if (rows > 0x20) {
            zoom_line = zoom_line % ((zoom_y + 1) << 1);
            if (zoom_line > zoom_y) {
                zoom_line = ((zoom_y + 1) << 1) - 1 - zoom_line;
                invert = !invert;
            }
        }
        uint8_t yt = ngr.lo.data[((zoom_y << 8) | zoom_line) & (ngr.lo.size - 1)];
        int sprite_y = yt & 0x0f;
        int tile = yt >> 4;
        if (invert) { sprite_y ^= 0x0f; tile ^= 0x1f; }

        const spr_block *rb = spr_native_block(sn);
        if (rb) {                                   /* remade image for this frame (spr_native.c) */
            uint16_t pal = vram[(((uint32_t)sn << 6) | ((uint32_t)tile << 1)) + 1] >> 8;
            draw_replacement_column(rb, sn - rb->first, tile * 16 + sprite_y, x, y, zoom_x, zoom_y,
                                    zoom_x_tables[zoom_x], flat_colour(&pens[pal << 4]), line);
            continue;
        }

        uint32_t offs = ((uint32_t)sn << 6) | ((uint32_t)tile << 1);
        uint16_t attr = vram[offs + 1];
        uint32_t code = (((uint32_t)attr << 12) & 0x70000) | vram[offs];
        if (!ng.auto_anim_disabled) {
            if (attr & 0x0008) code = (code & ~7u) | (ng.auto_anim_counter & 7);
            else if (attr & 0x0004) code = (code & ~3u) | (ng.auto_anim_counter & 3);
        }
        if (attr & 0x0002) sprite_y ^= 0x0f;

        const uint8_t *zx = zoom_x_tables[zoom_x];
        const uint8_t *gfx = &spr_gfx[((code << 8) | ((uint32_t)sprite_y << 4)) & spr_mask];
        const uint32_t *lp = &pens[(attr >> 8) << 4];
        int inc = 1;
        if (attr & 0x0001) { gfx += 0x0f; inc = -1; }

        int px = x;
        for (int k = 0; k < 16; k++, zx++, gfx += inc) {
            if (!*zx) continue;
            int sx = px & 0x1ff;
            if (sx < NG_SCREEN_W && *gfx) line[sx] = lp[*gfx];
            px++;
        }
    }
}

static void draw_fix(int scanline, uint32_t *line, const uint32_t *pens)
{
    static const int pix_offsets[4] = { 0x10, 0x18, 0x00, 0x08 };
    const ng_blob *rom = ng.use_cart_fix ? &ngr.s1 : &ngr.sfix;
    uint32_t mask = (uint32_t)rom->size - 1;
    int row = scanline >> 3, cy = scanline & 7;
    for (int col = 0; col < 40; col++) {
        uint16_t e = ng.vram[0x7000 + col * 32 + row];
        uint32_t code = e & 0x0fff;
        const uint32_t *lp = &pens[(e >> 12) << 4];
        uint32_t base = (code << 5) | (uint32_t)cy;
        uint32_t *dst = line + col * 8;
        for (int i = 0; i < 4; i++) {
            uint8_t g = rom->data[(base + (uint32_t)pix_offsets[i]) & mask];
            if (g & 0x0f) dst[i * 2] = lp[g & 0x0f];
            if (g >> 4) dst[i * 2 + 1] = lp[g >> 4];
        }
    }
}

void video_render_frame(uint32_t *fb)
{
    tagging = 1;
    nrecs = 0;
    for (int l = NG_VBEND; l < NG_VBEND + NG_SCREEN_H; l++) {
        tag_row = (l - NG_VBEND) * NG_SCREEN_W;
        video_render_line(l, fb + tag_row);
    }
    tagging = 0;
}

int video_recs(const video_rec **r, const uint32_t **under_out)
{
    *r = recs;
    *under_out = under;
    return nrecs;
}

/* a tagged frame at S x S output pixels per screen pixel (S = 1: plain 1x colours) */
void video_compose(const uint32_t *fb, uint32_t *out, int s)
{
    int W = NG_SCREEN_W * s;
    for (int y = 0; y < NG_SCREEN_H; y++)
        for (int x = 0; x < NG_SCREEN_W; x++) {
            uint32_t px = fb[y * NG_SCREEN_W + x], *o = out + (size_t)y * s * W + x * s;
            if (px >> 24) {                       /* plain colour: S x S block */
                for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) o[j * W + i] = px;
                continue;
            }
            const video_rec *r = &recs[px & 0xff];
            uint32_t below = under[y * NG_SCREEN_W + x];
            if (ng.shadow) below = 0xff000000u | ((below >> 1) & 0x7f7f7f);
            int lx = (x - r->x0) & 0x1ff, ly = (y + NG_VBEND - r->y0) & 0x1ff;
            for (int j = 0; j < s; j++)
                for (int i = 0; i < s; i++) {
                    const uint8_t *c = repl_at(r->b, ((float)lx + (i + 0.5f) / s) * r->scale_x,
                                               ((float)ly + (j + 0.5f) / s) * r->scale_y);
                    uint32_t v = below;
                    if (c && c[3] >= 128) {
                        v = r->fill ? r->fill : rgba_px(c);
                        if (ng.shadow) v = 0xff000000u | ((v >> 1) & 0x7f7f7f);
                    }
                    o[j * W + i] = v;
                }
        }
}

void video_render_line(int scanline, uint32_t *line)
{
    const uint32_t *pens = ng.pens[ng.pal_bank];
    uint32_t bg = pens[0x0fff];
    for (int i = 0; i < NG_SCREEN_W; i++) line[i] = bg;
    draw_sprites(scanline, line, pens);
    draw_fix(scanline, line, pens);
    if (ng.shadow)
        for (int i = 0; i < NG_SCREEN_W; i++)
            if (line[i] >> 24) line[i] = 0xff000000u | ((line[i] >> 1) & 0x7f7f7f);   /* tags: video_render_line_hd */
}

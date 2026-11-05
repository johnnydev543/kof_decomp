/* GPU output (--gpu, web ?gpu=1): the tagged 1x frame (video.c) is drawn by a fragment shader at the window's
 * resolution. Plain pixels keep their colour (sharp, nearest); a tagged pixel samples its replacement image
 * (bilinear for images larger than the frame's box, nearest for original-resolution art; premultiplied alpha) at the output pixel's exact position in the frame's box, shifts it by the
 * palette change at that pixel, and blends it over the colour below. Replacement images live in one atlas texture; this frame's records go to a small data
 * texture. OpenGL 2.1 / WebGL 1 (GLSL ES 1.00 subset), so the same code runs on Linux, macOS and the web. */
#include "gpu.h"
#include "neogeo.h"
#include "spr_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <SDL_opengl.h>
#endif

#define REC_TEXELS 16            /* texels per record (one row of the record texture) */
#define ATLAS_KEYS 8192

static SDL_Window *win;
static SDL_GLContext ctx;
static GLuint prog, vbo, tex_frame, tex_under, tex_delta, tex_recs, tex_atlas;
static GLint u_frame, u_under, u_delta, u_recs, u_atlas, u_atlas_size, u_shadow, a_pos;
static int atlas_size;
static uint8_t rec_data[VIDEO_MAX_RECS * REC_TEXELS * 4];

/* atlas: shelf packing, images by pointer; reset when full */
typedef struct { const spr_image *img; int x, y; uint32_t frame; } atlas_ent;
static atlas_ent akeys[ATLAS_KEYS];
static int shelf_x, shelf_y, shelf_h;
static uint32_t frame_no;

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_pos;\n"
    "void main() {\n"
    "    v_pos = vec2(a_pos.x * 320.0, a_pos.y * 224.0);\n"
    "    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "#ifdef GL_ES\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#endif\n"
    "uniform sampler2D u_frame, u_under, u_delta, u_recs, u_atlas;\n"
    "uniform float u_atlas_size, u_shadow;\n"
    "varying vec2 v_pos;\n"
    "vec4 rec(float i, float r) { return texture2D(u_recs, vec2((i + 0.5) / 16.0, (r + 0.5) / 256.0)); }\n"
    "float num(float i, float r) {\n"
    "    vec4 t = floor(rec(i, r) * 255.0 + 0.5);\n"
    "    return t.r + t.g * 256.0 + t.b * 65536.0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 p = floor(v_pos);\n"
    "    vec2 tc = (p + 0.5) / vec2(320.0, 224.0);\n"
    "    vec4 t = texture2D(u_frame, tc);\n"
    "    if (t.a > 0.5) { gl_FragColor = vec4(t.bgr, 1.0); return; }\n"
    "    float r = floor(t.r * 255.0 + 0.5);\n"            /* tag: the low byte, first in memory */
    "    vec3 under = texture2D(u_under, tc).bgr;\n"
    "    vec2 local = mod(v_pos + vec2(0.0, 16.0) - vec2(num(0.0, r), num(1.0, r)) + 512.0, 512.0);\n"
    "    vec2 f = local * vec2(num(2.0, r), num(3.0, r)) / 65536.0;\n"
    "    vec2 box = vec2(num(4.0, r), num(5.0, r));\n"
    "    vec2 img = vec2(num(8.0, r), num(9.0, r));\n"
    "    float fl = num(10.0, r);\n"
    "    vec2 u = f / box;\n"
    "    vec4 c = vec4(0.0);\n"
    "    if (u.x >= 0.0 && u.x < 1.0 && u.y >= 0.0 && u.y < 1.0 && img.x > 0.0) {\n"
    "        if (mod(fl, 2.0) >= 1.0) u.x = 1.0 - u.x;\n"
    "        if (fl >= 2.0) u.y = 1.0 - u.y;\n"
    "        vec2 ip = u * img;\n"
    "        if (img.x <= box.x + 0.5) ip = floor(ip) + 0.5;\n"   /* original-resolution art stays pixel-sharp */
    "        ip = clamp(ip, vec2(0.5), img - 0.5);\n"
    "        c = texture2D(u_atlas, (vec2(num(6.0, r), num(7.0, r)) + ip) / u_atlas_size);\n"
    "        vec4 fill = rec(11.0, r);\n"
    "        if (fill.a > 0.5) c.rgb = fill.rgb * c.a;\n"
    "        else if (c.a > 0.0) {\n"                              /* palette change (video.c pal_delta) */
    "            vec4 dt = floor(texture2D(u_delta, tc) * 255.0 + 0.5);\n"   /* |d| per colour, signs in alpha */
    "            vec3 sg = vec3(mod(floor(dt.a / 4.0), 2.0), mod(floor(dt.a / 2.0), 2.0), mod(dt.a, 2.0));\n"
    "            vec3 d = dt.bgr * (1.0 - 2.0 * sg) / 255.0;\n"
    "            c.rgb = clamp(c.rgb / c.a + d, 0.0, 1.0) * c.a;\n"
    "        }\n"
    "    }\n"
    "    vec3 col = c.rgb + under * (1.0 - c.a);\n"
    "    if (u_shadow > 0.5) col *= 0.5;\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gpu: shader: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint make_tex(int w, int h, GLint filter, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return t;
}

void gpu_gl_attributes(void)
{
#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

int gpu_init(SDL_Window *w)
{
    win = w;
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "gpu: no OpenGL context: %s\n", SDL_GetError()); return 0; }
    SDL_GL_SetSwapInterval(1);
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "gpu: shader link failed\n"); return 0; }
    a_pos = 0;
    u_frame = glGetUniformLocation(prog, "u_frame");
    u_under = glGetUniformLocation(prog, "u_under");
    u_delta = glGetUniformLocation(prog, "u_delta");
    u_recs = glGetUniformLocation(prog, "u_recs");
    u_atlas = glGetUniformLocation(prog, "u_atlas");
    u_atlas_size = glGetUniformLocation(prog, "u_atlas_size");
    u_shadow = glGetUniformLocation(prog, "u_shadow");

    static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    GLint max = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
    atlas_size = max >= 4096 ? 4096 : max >= 2048 ? 2048 : 1024;
    tex_frame = make_tex(NG_SCREEN_W, NG_SCREEN_H, GL_NEAREST, NULL);
    tex_under = make_tex(NG_SCREEN_W, NG_SCREEN_H, GL_NEAREST, NULL);
    tex_delta = make_tex(NG_SCREEN_W, NG_SCREEN_H, GL_NEAREST, NULL);
    tex_recs = make_tex(REC_TEXELS, 256, GL_NEAREST, NULL);
    tex_atlas = make_tex(atlas_size, atlas_size, GL_LINEAR, NULL);
    fprintf(stderr, "gpu: %s, atlas %d\n", (const char *)glGetString(GL_RENDERER), atlas_size);
    return 1;
}

static void atlas_reset(void)
{
    memset(akeys, 0, sizeof akeys);
    shelf_x = shelf_y = shelf_h = 0;
}

/* place an image in the atlas (1 pixel border, premultiplied alpha); -1 if it cannot fit */
static int atlas_put(const spr_image *im, int *ox, int *oy)
{
    unsigned h = (unsigned)(((uintptr_t)im >> 4) * 2654435761u) & (ATLAS_KEYS - 1);
    atlas_ent *e = NULL;
    for (unsigned i = 0; i < ATLAS_KEYS; i++) {
        e = &akeys[(h + i) & (ATLAS_KEYS - 1)];
        if (e->img == im) { e->frame = frame_no; *ox = e->x; *oy = e->y; return 0; }
        if (!e->img) break;
    }
    int w = im->w + 2, hh = im->h + 2;
    if (w > atlas_size || hh > atlas_size) return -1;
    if (shelf_x + w > atlas_size) { shelf_y += shelf_h; shelf_x = 0; shelf_h = 0; }
    if (shelf_y + hh > atlas_size || !e || e->img) return 1;     /* full */
    uint8_t *buf = malloc((size_t)w * hh * 4);
    for (int y = 0; y < hh; y++) {
        int sy = y - 1 < 0 ? 0 : y - 1 >= im->h ? im->h - 1 : y - 1;
        for (int x = 0; x < w; x++) {
            int sx = x - 1 < 0 ? 0 : x - 1 >= im->w ? im->w - 1 : x - 1;
            const uint8_t *s = im->rgba + ((size_t)sy * im->w + sx) * 4;
            uint8_t *d = buf + ((size_t)y * w + x) * 4;
            d[0] = (uint8_t)(s[0] * s[3] / 255); d[1] = (uint8_t)(s[1] * s[3] / 255);
            d[2] = (uint8_t)(s[2] * s[3] / 255); d[3] = s[3];
        }
    }
    glBindTexture(GL_TEXTURE_2D, tex_atlas);
    glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, w, hh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    free(buf);
    *e = (atlas_ent){ im, shelf_x + 1, shelf_y + 1, frame_no };
    *ox = e->x; *oy = e->y;
    shelf_x += w;
    if (hh > shelf_h) shelf_h = hh;
    return 0;
}

static void put24(uint8_t *t, uint32_t v) { t[0] = (uint8_t)v; t[1] = (uint8_t)(v >> 8); t[2] = (uint8_t)(v >> 16); t[3] = 255; }

void gpu_frame(const uint32_t *fb)
{
    const video_rec *recs;
    const uint32_t *under, *delta;
    int n = video_recs(&recs, &under, &delta);
    frame_no++;
    glBindTexture(GL_TEXTURE_2D, tex_frame);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NG_SCREEN_W, NG_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    if (!n) return;
    glBindTexture(GL_TEXTURE_2D, tex_under);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NG_SCREEN_W, NG_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, under);
    glBindTexture(GL_TEXTURE_2D, tex_delta);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NG_SCREEN_W, NG_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, delta);
    for (int pass = 0; pass < 2; pass++) {
        int full = 0;
        for (int i = 0; i < n && !full; i++) {
            const video_rec *r = &recs[i];
            const spr_image *im = r->b->img;
            uint8_t *t = rec_data + (size_t)i * REC_TEXELS * 4;
            int ax = 0, ay = 0, res = atlas_put(im, &ax, &ay);
            if (res > 0) { full = 1; break; }
            double sx = r->scale_x * 65536.0, sy = r->scale_y * 65536.0;
            put24(t + 0, (uint32_t)r->x0);
            put24(t + 4, (uint32_t)r->y0);
            put24(t + 8, (uint32_t)(sx > 16777215.0 ? 16777215.0 : sx + 0.5));
            put24(t + 12, (uint32_t)(sy > 16777215.0 ? 16777215.0 : sy + 0.5));
            put24(t + 16, (uint32_t)(16 * r->b->w));
            put24(t + 20, (uint32_t)(16 * r->b->h));
            put24(t + 24, (uint32_t)ax);
            put24(t + 28, (uint32_t)ay);
            put24(t + 32, res < 0 ? 0 : (uint32_t)im->w);       /* 0: too large for the atlas, show what is below */
            put24(t + 36, (uint32_t)im->h);
            put24(t + 40, (uint32_t)r->b->flips);
            uint8_t *f = t + 44;
            f[0] = (uint8_t)(r->fill >> 16); f[1] = (uint8_t)(r->fill >> 8); f[2] = (uint8_t)r->fill;
            f[3] = r->fill ? 255 : 0;
        }
        if (!full) break;
        if (pass) fprintf(stderr, "gpu: replacement images of one frame do not fit the atlas\n");
        atlas_reset();                    /* full: start over with this frame's images */
    }
    glBindTexture(GL_TEXTURE_2D, tex_recs);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, REC_TEXELS, n, GL_RGBA, GL_UNSIGNED_BYTE, rec_data);
}

void gpu_present(void)
{
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    /* keep the 320:224 picture, centred */
    int vw = w, vh = w * NG_SCREEN_H / NG_SCREEN_W;
    if (vh > h) { vh = h; vw = h * NG_SCREEN_W / NG_SCREEN_H; }
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_frame); glUniform1i(u_frame, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex_under); glUniform1i(u_under, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex_recs); glUniform1i(u_recs, 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, tex_atlas); glUniform1i(u_atlas, 3);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, tex_delta); glUniform1i(u_delta, 4);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(u_atlas_size, (float)atlas_size);
    glUniform1f(u_shadow, ng.shadow ? 1.0f : 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray((GLuint)a_pos);
    glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SDL_GL_SwapWindow(win);
}

/* the drawn picture (for screenshots): RGBA rows, top first; returns its size */
int gpu_read(uint32_t **out, int *w, int *h)
{
    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    int vw = dw, vh = dw * NG_SCREEN_H / NG_SCREEN_W;
    if (vh > dh) { vh = dh; vw = dh * NG_SCREEN_W / NG_SCREEN_H; }
    uint8_t *buf = malloc((size_t)vw * vh * 4);
    glReadPixels((dw - vw) / 2, (dh - vh) / 2, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    uint32_t *o = malloc((size_t)vw * vh * 4);
    for (int y = 0; y < vh; y++)
        for (int x = 0; x < vw; x++) {
            const uint8_t *p = buf + ((size_t)(vh - 1 - y) * vw + x) * 4;
            o[(size_t)y * vw + x] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    free(buf);
    *out = o; *w = vw; *h = vh;
    return 1;
}

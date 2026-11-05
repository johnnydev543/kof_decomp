/* Sprite replacement with remade images (see spr_native.c). */
#ifndef SPR_NATIVE_H
#define SPR_NATIVE_H
#include <stdint.h>

typedef struct { int w, h; uint8_t *rgba; } spr_image;
/* ref: the 16 colours (ARGB) the frame was exported with (tools/assets/export_sprites.py); a different palette
   in use now (hit flash, other colour set, palette animation) shifts the image by the difference */
typedef struct spr_block { uint32_t key; int first, w, h, flips; const spr_image *img; uint32_t ref[16]; } spr_block;

int spr_native_init(const char *mods_dir);          /* mods_dir/sprites/<key>.png */
const spr_block *spr_native_block(int sprite);       /* replacement block a hardware sprite belongs to, or NULL */
void spr_native_stats(void);

#endif

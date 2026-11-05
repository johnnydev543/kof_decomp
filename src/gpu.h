/* GPU output: draws the tagged frame with replacement images at the window's resolution (see gpu.c). */
#ifndef GPU_H
#define GPU_H
#include <stdint.h>
#include <SDL.h>

void gpu_gl_attributes(void);              /* before creating the window */
int  gpu_init(SDL_Window *win);            /* 0: no usable OpenGL */
void gpu_frame(const uint32_t *fb);        /* upload a tagged frame (video_render_frame) and its replacements */
void gpu_present(void);
int  gpu_read(uint32_t **out, int *w, int *h);   /* the drawn picture, ARGB, malloc'd */

#endif

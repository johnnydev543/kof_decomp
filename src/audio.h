/* Neo Geo MVS sound subsystem: Z80 @ 4 MHz + YM2610 @ 8 MHz.
 * See src/AUDIO_NOTES.md for the design and integration notes. */
#ifndef KOF97_AUDIO_H
#define KOF97_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_YM_CLOCK 8000000
#define AUDIO_Z80_CLOCK (AUDIO_YM_CLOCK/2)        /* 4 MHz */
#define AUDIO_RATE (AUDIO_YM_CLOCK/144)           /* 55555 Hz stereo output */

/* Returns 0 on success, -1 on bad arguments. The ROM buffers are copied. */
int  audio_init(const uint8_t *m1, size_t m1_size,      /* cart Z80 ROM 232-m1.bin (128KB) */
                const uint8_t *sm1, size_t sm1_size,    /* BIOS Z80 ROM ng-sm1.rom (128KB) */
                const uint8_t *vrom, size_t vrom_size); /* ADPCM ROM: v1+v2+v3 concatenated (12MB) */
void audio_shutdown(void);               /* frees everything allocated by audio_init */
void audio_reset(void);                  /* Z80 + YM2610 reset (keeps ROM selection) */
void audio_set_cart_rom(int use_cart);   /* 0 = BIOS sm1 mapped, 1 = cart m1. Power-on: 0 */
void audio_write_command(uint8_t cmd);   /* 68k byte write to 0x320000 */
uint8_t audio_read_reply(void);          /* 68k read of 0x320000 (high byte) */
void audio_run(int z80_cycles);          /* advance Z80 by N cycles and the YM2610 by the same time */
int  audio_read_samples(int16_t *stereo_out, int max_frames); /* drain stereo frames */
int  audio_samples_available(void);      /* frames currently buffered */
void audio_tap_adpcm_a(int on);          /* also keep the ADPCM-A part of each frame (sound_render --wav-pa) */
int  audio_read_samples_pa(int16_t *stereo_out, int16_t *pa_out, int max_frames);  /* drain both */
int  audio_save_state(void *buf, int max); /* not implemented: returns 0 */

/* Debug/diagnostics */
uint16_t audio_z80_pc(void);
uint64_t audio_z80_cycles(void);         /* total Z80 cycles executed */
void audio_set_reply_hook(void (*hook)(uint8_t value)); /* called on every Z80 OUT 0x0C */

#ifdef __cplusplus
}
#endif

#endif /* KOF97_AUDIO_H */

/* Native sound: pre-rendered / remade audio files instead of the Z80 driver and YM2610 (see snd_native.c). */
#ifndef SND_NATIVE_H
#define SND_NATIVE_H
#include <stdint.h>

int snd_native_init(const char *asset_dir, const char *mods_dir);   /* number of sounds, 0 on error */
int snd_native_enabled(void);
void snd_native_command(uint8_t cmd);       /* a byte written to REG_SOUND */
uint8_t snd_native_reply(void);              /* what the sound CPU would answer */
int snd_native_render(int16_t *stereo, int frames);   /* 55555 Hz stereo output */

#endif

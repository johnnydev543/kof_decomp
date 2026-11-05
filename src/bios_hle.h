/* Native (high-level) replacement for the Neo Geo MVS BIOS (see bios_hle.c). */
#ifndef BIOS_HLE_H
#define BIOS_HLE_H
#include <stdint.h>

/* level 0: the BIOS ROM runs; 1: the services the game calls (LSP_1ST, FIX_CLEAR, CREDIT_CHECK/DOWN, CARD...);
   2: also SYSTEM_IO from the game's VBlank; 3: everything (boot, VBlank, SYSTEM_RETURN, the attract cycle):
   no BIOS ROM is needed */
extern int bios_hle_level;
extern int bios_hle_log;            /* print the BIOS's calls into the game (USER, PLAYER_START ...) */

void bios_hle_init(int level, const char *timing);   /* timing: "boot_fresh,boot,eyecatch" frames (NULL: fast) */
int  bios_hle_owns(uint32_t pc);    /* the HLE runs this BIOS address */
void bios_hle_exec(void);           /* REG_PC is a BIOS address the HLE owns */
void bios_hle_watch(uint32_t pc);   /* bios_hle_log with the real BIOS: report its calls into the game */
void bios_hle_soft_dip(const char *spec);   /* "name=value,..." (names as in --soft-dip list) */
void bios_hle_soft_dip_list(void);          /* print the game's soft-DIP settings (ROMs loaded) */
void bios_hle_soft_dip_set_now(const char *spec);   /* change them while running */
const char *bios_hle_soft_dip_info(void);   /* "title|choice|...|=current;..." */
void bios_hle_make_rom(uint8_t *bios, uint32_t size);   /* a stand-in BIOS image: vectors into the HLE */
void bios_hle_lo_table(uint8_t *lo); /* the L0 zoom ROM (64 KB), generated */

#endif

/* Native replacement for the Neo Geo MVS BIOS (sp-s2, Europe), written from its disassembly (addresses in the
 * comments are the BIOS routines each part follows).
 *
 * The 68000 reaches the BIOS through its jump table ($C00402..) - calls from the game, and the vectors of the
 * BIOS vector table (reset, VBlank). When REG_PC is one of those addresses, bios_hle_exec() does the routine's
 * work natively and leaves the CPU where the routine would: back at the caller (rts / rte), in the game (USER
 * $122), or waiting for VBlank. Where the BIOS calls into the game (PLAYER_START $128, DEMO_END $12E,
 * COIN_SOUND $134) the HLE pushes a return address in the unused end of the BIOS area ($C1FF00..) and
 * continues when the game returns there.
 *
 * Levels (bios_hle_level) allow comparing with the real BIOS piece by piece:
 *   1  services the game calls; 2  also SYSTEM_IO from the game's VBlank; 3  everything, no BIOS ROM needed.
 * Kept as the BIOS does: the MVS backup-RAM layout the game reads (credits, slot table, soft DIPs, the game's
 * save block), the controller and coin rules, the start-credit gate, the attract cycle, and KOF97's need for the
 * watchdog on the first boot (its registration loop must be cut short). Left out: the self-tests, the test and
 * settings menu (soft DIPs are kept in backup RAM and set with --soft-dip), bookkeeping, LEDs, coin counters,
 * memory cards, the mahjong and 4-player controllers. The eye-catcher is drawn black (sounds and timing kept).
 * SR is set with m68ki_set_sr_noint: a pending interrupt is taken at the CPU loop, once REG_PC is where the
 * routine leaves it (m68ki_set_sr would take it at once, with the PC of the HLE entry). */
#include "bios_hle.h"
#include "neogeo.h"
#include "m68kcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bios_hle_level;
int bios_hle_log;

/* ---- addresses ---- */
enum {
    SYSRET = 0x10FD80, STATE = 0x10FD81, MVS_FLAG = 0x10FD82, GAME_DIP = 0x10FD84,
    P1STATUS = 0x10FD94, P2STATUS = 0x10FD9A, P3STATUS = 0x10FDA0, P4STATUS = 0x10FDA6,
    STATCURNT = 0x10FDAC, STATCHANGE = 0x10FDAD, USER_REQUEST = 0x10FDAE, USER_MODE = 0x10FDAF,
    CREDIT_DEC = 0x10FDB0, START_FLAG = 0x10FDB4, PLAYER_MOD = 0x10FDB6, PLAYER_MOD_OLD = 0x10FDBA,
    MESS_POINT = 0x10FDBE, MESS_BUSY = 0x10FDC2, CARD_ANSWER = 0x10FDC6, COMPULSION = 0x10FDDA,
    FRAMES = 0x10FE88, WAIT_VBL = 0x10FE8C, COIN_7F = 0x10FE8D, NO_LOCK = 0x10FEBF, TITLE_SHOWN = 0x10FEC5,
    SOUND_HS = 0x10FEDB, START_RAW = 0x10FEDC, START_EDGE = 0x10FEDD, TEST_RAW = 0x10FEDE, TEST_EDGE = 0x10FEDF,
    FORCED = 0x10FEE0, HOLDOFF = 0x10FEE1, COIN_SOUNDS = 0x10FEE2, CAL_TEST = 0x10FEE3, VBL_COUNT = 0x10FEE4,
    SP_TOP = 0x10F300,
    /* game header */
    H_NGH = 0x108, H_SAVE_PTR = 0x10E, H_SAVE_SIZE = 0x112, H_EYECATCH = 0x114, H_DIPS = 0x116,
    G_USER = 0x122, G_PLAYER_START = 0x128, G_DEMO_END = 0x12E, G_COIN_SOUND = 0x134,
    /* hardware */
    REG_P1CNT = 0x300000, REG_DIPSW = 0x300001, REG_SYSTYPE = 0x300081, REG_SOUND = 0x320000,
    REG_STATUS_A = 0x320001, REG_P2CNT = 0x340000, REG_STATUS_B = 0x380000, REG_POUTPUT = 0x380001,
    REG_SLOT = 0x380021, REG_SWPBIOS = 0x3A0003, REG_CRDLOCK2 = 0x3A0007, REG_SRAMLOCK = 0x3A000D,
    REG_PALBANK1 = 0x3A000F, REG_SWPROM = 0x3A0013, REG_CRDLOCK1 = 0x3A0015, REG_CRTFIX = 0x3A001B,
    REG_SRAMUNLOCK = 0x3A001D, REG_PALBANK0 = 0x3A001F,
    REG_VRAMADDR = 0x3C0000, REG_VRAMRW = 0x3C0002, REG_VRAMMOD = 0x3C0004, REG_LSPCMODE = 0x3C0006,
    REG_IRQACK = 0x3C000C,
    /* BIOS entry points (jump table) */
    E_RESET = 0xC00402, E_INT1 = 0xC00438, E_INT2 = 0xC0043E, E_SYSTEM_RETURN = 0xC00444, E_SYSTEM_IO = 0xC0044A,
    E_CREDIT_CHECK = 0xC00450, E_CREDIT_DOWN = 0xC00456, E_READ_CALENDAR = 0xC0045C, E_SETUP_CALENDAR = 0xC00462,
    E_CARD = 0xC00468, E_CARD_ERROR = 0xC0046E, E_HOW_TO_PLAY = 0xC00474, E_FIX_CLEAR = 0xC004C2,
    E_LSP_1ST = 0xC004C8, E_MESS_OUT = 0xC004CE, E_CONTROLLER_SETUP = 0xC004D4,
    /* continuation addresses (unused end of the BIOS area) */
    K_BASE = 0xC1FF00, K_COIN = K_BASE, K_START = K_BASE + 2, K_DEMO_END = K_BASE + 4, K_WAIT = K_BASE + 6,
    K_END = K_BASE + 10,
};
#define BK(a) (ng.backup[(a) & 0xffff])   /* backup RAM $D00000 */

/* ---- memory and CPU helpers ---- */
static unsigned rd8(uint32_t a) { return m68k_read_memory_8(a); }
static unsigned rd16(uint32_t a) { return m68k_read_memory_16(a); }
static uint32_t rd32(uint32_t a) { return m68k_read_memory_32(a); }
static void wr8(uint32_t a, unsigned v) { m68k_write_memory_8(a, v & 0xff); }
static void wr16(uint32_t a, unsigned v) { m68k_write_memory_16(a, v & 0xffff); }
static void wr32(uint32_t a, uint32_t v) { m68k_write_memory_32(a, v); }
static unsigned bk16(uint32_t a) { return (unsigned)BK(a) << 8 | BK(a + 1); }
static void bkw16(uint32_t a, unsigned v) { BK(a) = (uint8_t)(v >> 8); BK(a + 1) = (uint8_t)v; }
static void kick(void) { wr8(REG_DIPSW, 0); }
static void sound(unsigned cmd) { wr8(REG_SOUND, cmd); }
static void push32(uint32_t v) { REG_SP -= 4; wr32(REG_SP, v); }
static uint32_t pop32(void) { uint32_t v = rd32(REG_SP); REG_SP += 4; return v; }
static void rts(void) { REG_PC = pop32(); }
static void rte(void) { unsigned sr = rd16(REG_SP); REG_PC = rd32(REG_SP + 2); REG_SP += 6; m68ki_set_sr_noint(sr); }
static int mvs(void) { return rd8(MVS_FLAG) != 0; }
static int free_play(void) { return !(rd8(REG_DIPSW) & 0x40); }   /* DIP 7 on */
static unsigned credits(void) { return BK(0xD00034); }
static unsigned credits_w(void) { return bk16(0xD00034); }

/* BCD helpers on one byte, as abcd / sbcd with X clear; *carry = decimal carry / borrow */
static unsigned bcd_add(unsigned a, unsigned b, int *carry)
{
    int lo = (int)(a & 15) + (int)(b & 15), hi = (int)(a >> 4) + (int)(b >> 4);
    if (lo > 9) { lo -= 10; hi++; }
    *carry = hi > 9;
    if (hi > 9) hi -= 10;
    return (unsigned)(hi << 4 | lo);
}
static unsigned bcd_sub(unsigned a, unsigned b, int *borrow)
{
    int lo = (int)(a & 15) - (int)(b & 15), hi = (int)(a >> 4) - (int)(b >> 4);
    if (lo < 0) { lo += 10; hi--; }
    *borrow = hi < 0;
    if (hi < 0) hi += 10;
    return (unsigned)(hi << 4 | lo);
}

/* ---- timing (bios_hle_init): frames to wait at boot before the slot logic, eye-catcher length ---- */
static int boot_delay_fresh, boot_delay, boot_delay_reboot, eyecatch_frames;

/* ---- calls into the game and waits: what to do when the CPU comes back to a continuation address ---- */
typedef void (*cont_fn)(void);
static cont_fn after_coin, after_start, after_demo_end, after_wait;
static uint32_t regs_io[16], regs_int1[16];   /* registers kept across SYSTEM_IO (game call) / SYSTEM_INT1 */
static int start_pending;                     /* PLAYER_START called, io_start_done checks the answer */
static uint64_t delay_until;                  /* boot delay: wait for this frame number */

static void call_game(uint32_t addr, uint32_t cont) { push32(cont); REG_PC = addr; }
static void save_regs(uint32_t *r) { memcpy(r, REG_D, 16 * sizeof *r); }
static void restore_regs(const uint32_t *r) { uint32_t sp = REG_SP; memcpy(REG_D, r, 15 * sizeof *r); REG_SP = sp; }

static void logf_(const char *what, unsigned a, unsigned b)
{
    if (bios_hle_log) fprintf(stderr, "bios: frame %llu %s %x %x\n", (unsigned long long)ng.frame, what, a, b);
}

/* the BIOS takes this long for work the HLE does at once: spend the cycles before going on (so writes that
   follow, such as sound commands, land in the same frame as with the ROM) */
static int spend_left;
static cont_fn after_spend;
static void spend(int cycles, cont_fn then)
{
    spend_left = cycles;
    after_spend = then;
    REG_PC = K_WAIT + 3;
}

/* wait for one VBlank the way the BIOS does ($C1870C): flag $10FE8C, interrupts on, SYSTEM_INT1 clears it */
static void wait_vblank(cont_fn then)
{
    wr8(WAIT_VBL, 1);
    after_wait = then;
    REG_PC = K_WAIT;
}

/* ---- leaf services ---- */
static void vram_fill(unsigned addr, unsigned mod, unsigned value, int n)
{
    wr16(REG_VRAMMOD, mod);
    if (addr != 0xffff) wr16(REG_VRAMADDR, addr);
    while (n--) wr16(REG_VRAMRW, value);
}

/* LSP_1ST $C1839A: sprite shrink / Y / X tables to defaults, sprite 0's tiles; leaves VRAMMOD = 2 */
static void lsp_1st(void)
{
    vram_fill(0x8000, 1, 0x0FFF, 512);
    vram_fill(0xffff, 1, 0x0000, 512);
    vram_fill(0xffff, 1, 0xBE00, 512);
    vram_fill(0x0000, 2, 0x00FF, 32);
    USE_CYCLES(28512);                            /* as the BIOS routine (measured) */
}

/* FIX_CLEAR $C1835E */
static void fix_clear(void)
{
    vram_fill(0x7020, 1, 0x00FF, 1216);
    vram_fill(0xffff, 1, 0x0020, 32);
    vram_fill(0x7000, 1, 0x0020, 32);
    USE_CYCLES(23300);
}

/* the fix clear of SYSTEM_RETURN $C13A3C: $7000-$74FF = $0020 */
static void fix_clear_space(void) { vram_fill(0x7000, 1, 0x0020, 0x500); }

/* CONTROLLER_SETUP $C18462 */
static void controller_setup(void)
{
    wr8(P1STATUS, 0); wr8(P2STATUS, 0); wr8(P3STATUS, 0); wr8(P4STATUS, 0);
    if (!(rd8(REG_DIPSW) & 4)) wr8(P1STATUS, 3);
    wr8(REG_POUTPUT, 0);
}

/* CREDIT_CHECK $C117CE */
static void credit_check(void)
{
    if (!mvs()) return;
    wr16(0x10FDB2, 0);
    if (free_play()) return;
    unsigned c = credits();
    int b;
    if (rd8(USER_MODE) == 1 && rd8(CREDIT_DEC) == 0) {            /* P2 alone in title / demo needs double */
        unsigned need = bcd_add(rd8(CREDIT_DEC + 1), rd8(CREDIT_DEC + 1), &b);
        bcd_sub(c, need, &b);
        if (b) wr8(CREDIT_DEC + 1, 0);
        return;
    }
    for (int i = 0; i < 2; i++) {
        unsigned r = bcd_sub(c, rd8(CREDIT_DEC + (uint32_t)i), &b);
        if (b) { c = 0; wr8(CREDIT_DEC + (uint32_t)i, 0); } else c = r;
    }
}

/* the credits of the players in d0's low bits (bit 0 P1, bit 1 P2) - 1 each ($C12912 / $C1299A) */
static void take_credits(unsigned players, const uint8_t *amount)
{
    for (int i = 0; i < 2; i++, players >>= 1) {
        unsigned n = amount ? amount[i] : 1;
        if (!(players & 1) || !n) continue;
        if (!free_play()) {
            int b;
            unsigned c = bcd_sub(credits(), n, &b);
            BK(0xD00034) = (uint8_t)(b ? 0 : c);
        }
    }
}

/* CREDIT_DOWN $C11820 */
static void credit_down(void)
{
    wr8(FORCED, 0);
    wr8(STATE, 3);
    uint8_t dec[2] = { (uint8_t)rd8(CREDIT_DEC), (uint8_t)rd8(CREDIT_DEC + 1) };
    take_credits(3, dec);
}

/* ---- SYSTEM_IO $C1167A ---- */
static int io_from_int1;                       /* called by SYSTEM_INT1 (else by the game) */
static void rom_system_return(void) { REG_PC = 0xC112D2; }   /* levels 1-2: the ROM's SYSTEM_RETURN */
static void io_controllers(void);
static void io_after_coin(void);
static void io_start_done(void);
static void io_end(void);
static void system_return(void);

/* coins, credits, test button, COIN_SOUND ($C126B4; MVS only) */
static void io_coins(void)
{
    /* test button edge: the BIOS menu is not kept (soft DIPs: --soft-dip) */
    unsigned t = (~rd8(REG_SYSTYPE)) & 0xff, old = rd8(TEST_RAW);
    wr8(TEST_RAW, t);
    wr8(TEST_EDGE, (t ^ old) & t);
    /* coin sampling: a coin counts on the first active sample after two inactive ones */
    unsigned c = (~rd8(REG_STATUS_A)) & 0xff, h2 = BK(0xD00020), h1 = BK(0xD00021);
    BK(0xD00020) = (uint8_t)h1;
    BK(0xD00021) = (uint8_t)c;
    BK(0xD00022) = (uint8_t)((c ^ h1) & c);
    unsigned accept = c & ~(h2 | h1);
    int any = 0;
    for (int chute = 0; chute < 2; chute++) {
        if (!(accept & (1u << chute))) continue;
        any = 1;
        BK(0xD00024 + chute)++;
        BK(0xD0002C + chute)++;
        unsigned need = BK(0xD0003A + 2 * chute), give = BK(0xD0003B + 2 * chute);
        if (BK(0xD00024 + chute) >= need) {
            int carry;
            BK(0xD00024 + chute) = (uint8_t)(BK(0xD00024 + chute) - need);
            unsigned cr = bcd_add(give, credits(), &carry);
            BK(0xD00034) = (uint8_t)(carry ? 0x99 : cr);
        }
    }
    if (BK(0xD00022) & 4) {                                       /* service */
        int carry;
        unsigned cr = bcd_add(credits(), 1, &carry);
        BK(0xD00034) = (uint8_t)(carry ? 0x99 : cr);
    }
    if (any) {
        wr16(COMPULSION, bk16(0xD00044));
        wr8(COIN_SOUNDS, rd8(COIN_SOUNDS) + 1);
    }
    /* COIN_SOUND $C129D4: one per frame, when the game is not in a sound handshake */
    if (rd8(HOLDOFF) == 0 && rd8(STATE) >= 2 && rd8(SOUND_HS) == 0 && rd8(COIN_SOUNDS) > 0) {
        wr8(COIN_SOUNDS, rd8(COIN_SOUNDS) - 1);
        if (rd8(COIN_7F)) sound(0x7F);
        else { after_coin = io_after_coin; call_game(G_COIN_SOUND, K_COIN); return; }
    }
    io_after_coin();
}

static void io_after_coin(void)
{
    REG_A[5] = SP_TOP;
    if (!rd8(NO_LOCK)) wr8(REG_SRAMLOCK, 0);
    io_controllers();
}

/* one player's block ($C1866C): STATUS, PREVIOUS, CURRENT, CHANGE, REPEAT, TIMER */
static void pad_update(uint32_t blk, unsigned port)
{
    unsigned now = (~port) & 0xff, cur = rd8(blk + 2);
    if (now == cur) {
        int t = (int8_t)rd8(blk + 5) - 1;
        if (t < 0) { wr8(blk + 5, 8); wr8(blk + 4, now); } else { wr8(blk + 5, (unsigned)t); wr8(blk + 4, 0); }
    } else {
        wr8(blk + 5, 16);
        wr8(blk + 4, now);
    }
    wr8(blk + 1, cur);
    wr8(blk + 2, now);
    wr8(blk + 3, (now ^ cur) & now);
}

/* controllers ($C1850E), then the start / title logic */
static void io_controllers(void)
{
    wr8(REG_POUTPUT, 0x12);
    unsigned s1 = rd8(REG_STATUS_B);
    wr8(REG_POUTPUT, 0x1B);
    unsigned s2 = rd8(REG_STATUS_B), p2 = rd8(REG_P2CNT), p1 = rd8(REG_P1CNT);
    unsigned raw = (~((s2 & 0x0F) | ((s1 << 4) & 0xF0))) & 0xff;
    unsigned edge = (raw ^ rd8(START_RAW)) & raw;
    /* P1 (and P3, never present: cleared and its start bits masked), P2 (and P4) */
    pad_update(P1STATUS, p1);
    wr8(P3STATUS + 1, 0); wr32(P3STATUS + 2, 0);
    raw &= 0xCF; edge &= 0xCF;
    pad_update(P2STATUS, p2);
    wr8(P4STATUS + 1, 0); wr32(P4STATUS + 2, 0);
    raw &= 0x3F; edge &= 0x3F;
    wr8(START_RAW, raw);
    wr8(START_EDGE, edge);
    wr8(STATCURNT, raw & 0x55);
    wr8(STATCHANGE, edge & 0x55);
    wr8(REG_POUTPUT, 0);
    for (int i = 0; i < 6; i++) { wr8(0x10FEE8 + (uint32_t)i, rd8(P1STATUS + (uint32_t)i)); wr8(0x10FEEE + (uint32_t)i, rd8(P2STATUS + (uint32_t)i)); }

    /* start and title logic: after the hold-off, while the game owns VBlank */
    int8_t h = (int8_t)rd8(HOLDOFF);
    if (h < 0) { io_end(); return; }
    if (h > 0) { wr8(HOLDOFF, (unsigned)(h - 1)); io_end(); return; }
    if (!(rd8(SYSRET) & 0x80)) { io_end(); return; }

    /* start buttons ($C116C8) */
    if (rd8(USER_MODE) == 0) { io_start_done(); return; }
    unsigned ch = rd8(STATCHANGE), req = (ch & 1) | ((ch >> 1) & 2) | ((ch >> 2) & 4) | ((ch >> 3) & 8);
    if (!req) {
        /* game-start compulsion: with credits in the title, start by itself after $D00044 seconds */
        if (rd8(USER_MODE) != 1 || !credits_w() || BK(0xD00043)) { io_start_done(); return; }
        unsigned sec = rd8(COMPULSION);
        if (sec) {
            int f = (int8_t)rd8(COMPULSION + 1) - 1;
            wr8(COMPULSION + 1, (unsigned)f);
            if (f >= 0) { io_start_done(); return; }
            wr8(COMPULSION + 1, 59);
            int b;
            sec = bcd_sub(sec, 1, &b);
            wr8(COMPULSION, sec);
            if (sec) { io_start_done(); return; }
        }
        wr8(FORCED, 1);
        req = credits() ? 1 : 2;
    }
    wr8(START_FLAG, req);
    /* credit gate ($C1177A), MVS only */
    if (mvs()) {
        static const uint8_t need[4] = { 2, 1, 1, 2 }, flag[8] = { 3, 1, 2, 3, 0, 0, 0, 0 };
        unsigned i = req & 3;
        if (!i) wr8(START_FLAG, 0);
        else {
            if ((i & 2) && rd8(USER_MODE) == 1) i = 0;
            if (!free_play() && credits() < need[i]) i += 4;
            wr8(START_FLAG, flag[i]);
        }
    }
    if (!rd8(START_FLAG)) { io_start_done(); return; }
    REG_D[0] = rd8(START_FLAG);
    REG_A[5] = SP_TOP;
    after_start = io_start_done;
    start_pending = 1;                           /* io_start_done checks whether the game accepted it */
    call_game(G_PLAYER_START, K_START);
}

/* after the start logic: accepted start -> game state; then demo -> title ($C1183C) */
static void io_start_done(void)
{
    REG_A[5] = SP_TOP;
    if (start_pending) {
        start_pending = 0;
        unsigned f = rd8(START_FLAG) & 0x0F;
        if (f) {
            wr8(HOLDOFF, 1);
            wr16(COMPULSION, 0);
            wr8(FORCED, 0);
            wr8(STATE, 3);
            take_credits(rd8(START_FLAG), NULL);
            if (!rd8(NO_LOCK)) wr8(REG_SRAMLOCK, 0);
        }
    }
    /* demo -> title / slot switch */
    if (rd8(HOLDOFF) || rd8(USER_MODE) != 1) { io_end(); return; }
    int st = 0;
    if (rd8(FORCED)) st = 4;
    else if (credits_w() && !rd8(TITLE_SHOWN) && !BK(0xD00043)) { st = 6; wr8(TITLE_SHOWN, 1); }
    else if ((bk16(0xD00060) || BK(0xD00042) || credits_w()) && (rd8(START_EDGE) & 0x0A))
        st = (rd8(START_EDGE) & 8) ? 5 : 4;
    if (!st) { io_end(); return; }
    wr8(FORCED, 0);
    wr8(STATE, (unsigned)st);
    after_demo_end = bios_hle_level >= 3 ? system_return : rom_system_return;
    call_game(G_DEMO_END, K_DEMO_END);
}

static void int1_end(void);

static void io_end(void)
{
    /* play-time bookkeeping is not kept; the copy of PLAYER_MOD is ($C12BE6) */
    if (mvs() && rd8(USER_MODE) == 2) wr32(PLAYER_MOD_OLD, rd32(PLAYER_MOD));
    USE_CYCLES(3810);                             /* about what the BIOS routine takes (3690-3950) */
    if (io_from_int1) { int1_end(); return; }
    restore_regs(regs_io);
    rts();
}

static void system_io(int from_int1)
{
    io_from_int1 = from_int1;
    if (!from_int1) save_regs(regs_io);
    REG_A[5] = SP_TOP;
    wr32(FRAMES, rd32(FRAMES) + 1);
    if (mvs()) io_coins();
    else io_controllers();
}

/* ---- SYSTEM_INT1 $C11646 (the BIOS's VBlank) ---- */
static void int1(void)
{
    wr16(REG_IRQACK, 4);
    kick();
    if (rd8(CAL_TEST)) { int1_end(); return; }
    save_regs(regs_int1);
    system_io(1);
}

static void int1_end(void)
{
    if (!rd8(CAL_TEST)) { wr8(WAIT_VBL, 0); restore_regs(regs_int1); }
    wr8(VBL_COUNT, rd8(VBL_COUNT) + 1);
    io_from_int1 = 0;
    rte();
}

/* ---- slots, save blocks, soft DIPs ---- */
static unsigned slot_block(unsigned slot) { return bk16(0xD00124 + 4 * slot + 2) & 0xff; }

static void soft_dip_apply(void);

/* game RAM (header $10E/$112) -> save block ($C12322) */
static void save_game_ram(void)
{
    unsigned slot = BK(0xD00108);
    if (slot >= 8) return;
    uint32_t dst = 0xD00320 + ((uint32_t)slot_block(slot) << 12), src = rd32(H_SAVE_PTR);
    unsigned n = rd16(H_SAVE_SIZE);
    for (unsigned i = 0; i < n; i++) BK(dst + i) = (uint8_t)rd8(src + i);
}

/* save block -> game RAM, soft DIPs -> GAME_DIP ($C1241E) */
static void load_game_ram(void)
{
    unsigned slot = BK(0xD00108), blk = slot_block(slot);
    BK(0xD00109) = (uint8_t)slot;
    uint32_t src = 0xD00320 + ((uint32_t)blk << 12), dst = rd32(H_SAVE_PTR);
    unsigned n = rd16(H_SAVE_SIZE);
    for (unsigned i = 0; i < n; i++) wr8(dst + i, BK(src + i));
    for (unsigned i = 0; i < 16; i++) wr8(GAME_DIP + i, BK(0xD00220 + 16 * blk + i));
    soft_dip_apply();
}

/* --soft-dip: settings applied to GAME_DIP and the slot's stored copy whenever they are loaded */
static char soft_dip_spec[512];

/* the region's soft-DIP table (header $116 + 4 * region): 16-byte name, 6 time / count bytes, 10 list bytes
   (high nibble default, low nibble number of choices), then 12-byte names: each list setting's title and
   its choices */
static uint32_t dip_table(void)
{
    unsigned region = rd8(0xC00401);
    return rd32(H_DIPS + 4 * (region > 2 ? 2 : region));
}

static void dip_name(uint32_t a, char *out)
{
    int n = 0;
    for (int i = 0; i < 12; i++) {
        unsigned c = rd8(a + (uint32_t)i);
        out[n++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c == ' ' || c == '/' ? '_' : (char)c;
    }
    while (n > 0 && out[n - 1] == '_') n--;
    out[n] = 0;
}

/* each list setting: index (0..9), title, choices; calls fn for each */
static void dip_walk(void (*fn)(int idx, const char *title, int nchoices, uint32_t names, void *ctx), void *ctx)
{
    uint32_t t = dip_table(), names = t + 32;
    char title[16];
    for (int i = 0; i < 10; i++) {
        unsigned b = rd8(t + 22 + (uint32_t)i), n = b & 15;
        if (!n) break;
        dip_name(names, title);
        fn(i, title, (int)n, names + 12, ctx);
        names += 12 * (n + 1);
    }
}

static void dip_print(int idx, const char *title, int n, uint32_t names, void *ctx)
{
    (void)ctx;
    char c[16];
    printf("%s (setting %d):", title, idx);
    for (int k = 0; k < n; k++) { dip_name(names + 12 * (uint32_t)k, c); printf(" %d=%s", k, c); }
    putchar('\n');
}

void bios_hle_soft_dip_list(void) { dip_walk(dip_print, NULL); }

struct dip_set { const char *name, *value; int done; };
static void dip_apply_one(int idx, const char *title, int n, uint32_t names, void *ctx)
{
    struct dip_set *d = ctx;
    char c[16];
    if (strcmp(d->name, title)) return;
    int v = -1;
    for (int k = 0; k < n; k++) { dip_name(names + 12 * (uint32_t)k, c); if (!strcmp(c, d->value)) v = k; }
    if (v < 0 && d->value[0] >= '0' && d->value[0] <= '9') v = atoi(d->value);
    if (v < 0 || v >= n) { fprintf(stderr, "soft dip %s: no choice '%s'\n", title, d->value); d->done = 1; return; }
    wr8(GAME_DIP + 6 + (uint32_t)idx, (unsigned)v);
    d->done = 1;
}

/* apply --soft-dip "name=value,..." to GAME_DIP and to the slot's stored copy */
static void soft_dip_apply(void)
{
    if (!soft_dip_spec[0]) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", soft_dip_spec);
    for (char *it = strtok(buf, ","); it; it = strtok(NULL, ",")) {
        char *eq = strchr(it, '=');
        if (!eq) continue;
        *eq = 0;
        for (char *p = it; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        for (char *p = eq + 1; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        struct dip_set d = { it, eq + 1, 0 };
        dip_walk(dip_apply_one, &d);
        if (!d.done) fprintf(stderr, "soft dip: no setting '%s' (--soft-dip list)\n", it);
    }
    unsigned blk = slot_block(BK(0xD00108) & 7);
    for (unsigned i = 0; i < 16; i++) BK(0xD00220 + 16 * blk + i) = (uint8_t)rd8(GAME_DIP + i);
}

void bios_hle_soft_dip(const char *spec) { snprintf(soft_dip_spec, sizeof soft_dip_spec, "%s", spec); }

/* change soft DIPs now (web page): GAME_DIP and the slot's stored copy (the game reads most at a new game) */
void bios_hle_soft_dip_set_now(const char *spec)
{
    char keep[sizeof soft_dip_spec];
    memcpy(keep, soft_dip_spec, sizeof keep);
    snprintf(soft_dip_spec, sizeof soft_dip_spec, "%s", spec);
    soft_dip_apply();
    memcpy(soft_dip_spec, keep, sizeof keep);
}

/* "title|choice|choice...|=current;..." for each list setting */
static char dip_info[2048];
static size_t dip_info_n;
static void dip_info_one(int idx, const char *title, int n, uint32_t names, void *ctx)
{
    (void)ctx;
    char c[16];
    dip_info_n += (size_t)snprintf(dip_info + dip_info_n, sizeof dip_info - dip_info_n, "%s", title);
    for (int k = 0; k < n; k++) {
        dip_name(names + 12 * (uint32_t)k, c);
        dip_info_n += (size_t)snprintf(dip_info + dip_info_n, sizeof dip_info - dip_info_n, "|%s", c);
    }
    dip_info_n += (size_t)snprintf(dip_info + dip_info_n, sizeof dip_info - dip_info_n, "|=%u;", rd8(GAME_DIP + 6 + (uint32_t)idx));
}
const char *bios_hle_soft_dip_info(void)
{
    dip_info_n = 0;
    dip_info[0] = 0;
    dip_walk(dip_info_one, NULL);
    return dip_info;
}

/* the soft-DIP defaults of the BIOS's region ($C11612): EU table */
static void soft_dip_defaults(void)
{
    uint32_t t = dip_table() + 16;
    for (int i = 0; i < 6; i++) wr8(GAME_DIP + (uint32_t)i, rd8(t + (uint32_t)i));
    for (int i = 0; i < 10; i++) wr8(GAME_DIP + 6 + (uint32_t)i, rd8(t + 6 + (uint32_t)i) >> 4);
    soft_dip_apply();
}

/* Z80 handshake before a slot switch ($C11FB2): send $01 until it answers $01 (8 tries, 4 frames apart) */
static int hs_tries;
static uint32_t hs_frame;
static cont_fn hs_then;
static void handshake_poll(void);
static void handshake(cont_fn then)
{
    if (rd8(REG_SOUND) == 1) { then(); return; }
    hs_then = then;
    hs_tries = 8;
    wr8(SOUND_HS, 1);
    m68ki_set_sr_noint(0x2000);
    hs_frame = rd16(FRAMES + 2) + 4;
    sound(1);
    after_wait = handshake_poll;
    REG_PC = K_WAIT + 1;                         /* polled: see bios_hle_exec */
}
static void handshake_poll(void)
{
    kick();
    if (rd8(REG_SOUND) != 1) {
        if ((rd16(FRAMES + 2) & 0xffff) != (hs_frame & 0xffff)) return;   /* keep polling */
        if (--hs_tries > 0) { hs_frame = rd16(FRAMES + 2) + 4; sound(1); return; }
    }
    m68ki_set_sr_noint(0x2700);
    wr8(SOUND_HS, 0);
    REG_PC = K_END;
    hs_then();
}

/* select the current slot ($C11F94 after the handshake): cart fix and vectors */
static void select_slot(void)
{
    wr8(REG_SLOT, BK(0xD00108));
    wr8(REG_CRTFIX, 0);
    wr8(REG_SWPROM, 0);
}

/* ---- the BIOS state machine $C11142 and USER entry $C11286 ---- */
static void state_machine(void);
static unsigned user_req, user_state;
static void user_enter2(void);

static void enter_user(unsigned req, unsigned st)
{
    user_req = req; user_state = st;
    wr32(PLAYER_MOD, 0);
    wr32(PLAYER_MOD_OLD, 0);
    wr16(REG_IRQACK, 7);
    m68ki_set_sr_noint(0x2000);
    sound(3);
    wait_vblank(user_enter2);
}
static void user_enter2(void)
{
    m68ki_set_sr_noint(0x2700);
    wr16(REG_LSPCMODE, 0x4000);
    wr8(USER_REQUEST, user_req);
    wr8(SYSRET, user_state | 0x80);
    wr8(NO_LOCK, 0);
    wr8(REG_SWPROM, 0);
    wr8(REG_SRAMLOCK, 0);
    REG_D[0] = user_req;
    REG_D[1] = user_state | 0x80;
    REG_SP = SP_TOP;
    REG_PC = G_USER;
}

static uint64_t eyecatch_start;
static int eyecatch_jingle;
static void eyecatch_frame(void);
static void eyecatch_end(void);

/* the eye-catcher's picture: the BIOS ROM's logo is not available, so a line of text in the cartridge's
   own fix font (bank 0 holds ASCII: colour 1 the letters, colour 2 the cell) with palette 15 */
static void fix_text(int col, int row, const char *t)
{
    wr16(REG_VRAMMOD, 32);                        /* fix map: next column */
    wr16(REG_VRAMADDR, 0x7000 + (unsigned)col * 32 + (unsigned)row);
    for (; *t; t++) wr16(REG_VRAMRW, 0xF000 | (unsigned char)*t);
}
static void eyecatch_draw(void)
{
    wr16(0x400000 + (15 * 16 + 1) * 2, 0x7FFF);   /* white */
    wr16(0x400000 + (15 * 16 + 2) * 2, 0x8000);   /* black */
    fix_text(16, 13, "NEO-GEO");
    fix_text(14, 16, "MAX 330 MEGA");
    fix_text(13, 17, "PRO-GEAR SPEC");
}

/* BIOS eye-catcher $C17E26 (text instead of the ROM's logo): sound $03, the jingle $02 six frames later, SYSTEM_RETURN after
   eyecatch_frames frames; timed by frame number (woken once per VBlank) */
static void eyecatch(void)
{
    sound(3);
    if (eyecatch_frames <= 0) { eyecatch_end(); return; }
    eyecatch_start = ng.frame;
    eyecatch_jingle = 0;
    eyecatch_draw();
    m68ki_set_sr_noint(0x2000);
    wait_vblank(eyecatch_frame);
}
static void eyecatch_frame(void)
{
    /* each frame: MVS ends it when there are credits, AES on a P1 A-D or start press */
    if (mvs() ? credits_w() != 0 : (rd8(0x10FEEB) & 0xF0 || rd8(STATCHANGE) & 1)) { eyecatch_end(); return; }
    uint64_t n = ng.frame - eyecatch_start;
    if (n >= 6 && !eyecatch_jingle) { eyecatch_jingle = 1; if (!(mvs() && BK(0xD00046))) sound(2); }
    if (n >= (uint64_t)eyecatch_frames) { eyecatch_end(); return; }
    wait_vblank(eyecatch_frame);
}
static void eyecatch_end(void) { system_return(); }

static void state_machine(void)
{
    kick();
    REG_SP = SP_TOP;
    REG_A[5] = SP_TOP;
    wr8(SYSRET, rd8(SYSRET) & 0x7F);
    switch (rd8(STATE)) {
    case 0: {
        /* slot scan ($C121D6): register the cartridge in the slot table on the first boot */
        unsigned ngh = rd16(H_NGH), slot = 0;
        int registered = bk16(0xD00124) == ngh && !(bk16(0xD00126) & 0x8000);
        if (registered) {
            /* state-0 return handler with the scan complete ($C113DE): select slot $D00058, sound $03, state 1 */
            BK(0xD00109) = 8;
            BK(0xD00108) = (uint8_t)slot;
            select_slot();
            sound(3);
            wr8(STATE, 1);
            wr8(NO_LOCK, 0);
            state_machine();
            return;
        }
        bkw16(0xD00124, ngh);
        bkw16(0xD00126, 0);
        BK(0xD00108) = 0;
        BK(0xD00109) = 0;
        for (int i = 0; i < 16; i++) BK(0xD002A0 + (uint32_t)i) = (uint8_t)rd8(rd32(H_DIPS + 8) + (uint32_t)i);
        select_slot();
        soft_dip_defaults();
        wr8(USER_MODE, 0);
        enter_user(0, 0);        /* KOF97 never returns from USER(0): the watchdog resets it (bios_hle.c top) */
        return;
    }
    case 1:
        wr8(HOLDOFF, 10);
        wr8(USER_MODE, 0);
        wr8(0x10FCEC, 0);
        if (rd8(H_EYECATCH) == 1) { enter_user(1, 1); return; }
        if (rd8(H_EYECATCH) == 0) { wr8(SYSRET, 1); wr8(NO_LOCK, 0); eyecatch(); return; }
        wr16(SYSRET, 2);
        state_machine();
        return;
    default: {
        /* state 2: demo or title */
        wr8(HOLDOFF, 10);
        wr8(USER_MODE, 1);
        unsigned req = (!BK(0xD00043) && credits_w()) ? 3 : 2;
        enter_user(req, 2);
        return;
    }
    }
}

/* after a slot change: sound $03 and load ($C123C4 tail) */
static void next_slot_done(void)
{
    sound(3);
    load_game_ram();
}

/* the handlers of SYSTEM_RETURN by state ($C1137A) */
static void sysret_state2_end(void)
{
    if (BK(0xD00058) == BK(0xD00108)) wr8(STATE, 1);
    wr8(NO_LOCK, 0);
    state_machine();
}
static void sysret_next_slot(void)
{
    /* one cartridge: the next slot is the same one */
    select_slot();
    next_slot_done();
}
static void sysret_state2_slot(void) { sysret_next_slot(); sysret_state2_end(); }
static void sysret_state45_slot(void) { sysret_next_slot(); wr8(NO_LOCK, 0); state_machine(); }

static void system_return2(void);
static void system_return(void)
{
    wr8(HOLDOFF, 0xFF);
    wr16(REG_LSPCMODE, 0x4000);
    wr8(SYSRET, rd8(SYSRET) & 0x7F);
    kick();
    wr16(REG_IRQACK, 7);
    io_from_int1 = 0;
    start_pending = 0;
    m68ki_set_sr_noint(0x2000);
    logf_("SYSTEM_RETURN", rd8(STATE), credits());
    wait_vblank(system_return2);
}
static void system_return3(void);
static void system_return2(void)
{
    wr8(REG_SWPBIOS, 0);
    REG_SP = SP_TOP;
    for (uint32_t a = 0x400000; a < 0x402000; a += 2) wr16(a, 0);   /* current palette bank */
    controller_setup();
    wr8(MESS_BUSY, 0);
    wr32(MESS_POINT, 0x10FF00);
    fix_clear_space();
    vram_fill(0x8000, 1, 0x0FFF, 512);           /* LSP_1ST (its cycles are in the spend below) */
    vram_fill(0xffff, 1, 0x0000, 512);
    vram_fill(0xffff, 1, 0xBE00, 512);
    vram_fill(0x0000, 2, 0x00FF, 32);
    /* palette clear ~18800 + fix clear ~23300 + LSP_1ST 28512 cycles on the ROM */
    spend(70600, system_return3);
}
static void system_return3(void)
{
    switch (rd8(STATE)) {
    case 0:                                       /* USER(0) returned (not with KOF97) */
        for (int i = 0; i < 16; i++) BK(0xD00220 + (uint32_t)i) = (uint8_t)rd8(GAME_DIP + (uint32_t)i);
        save_game_ram();
        BK(0xD00109) = 8;
        BK(0xD00108) = 0;
        select_slot();
        sound(3);
        wr8(STATE, 1);
        break;
    case 1:                                       /* after the eye-catcher: load the slot */
        wr8(STATE, 2);
        wr8(NO_LOCK, 1);
        load_game_ram();
        wr8(NO_LOCK, 0);
        break;
    case 3:                                       /* game over */
        wr8(TITLE_SHOWN, 0);
        wr8(NO_LOCK, 1);
        wr8(STATE, 2);
        save_game_ram();
        wr8(NO_LOCK, 0);
        if (!BK(0xD00043) && credits_w()) wr16(COMPULSION, bk16(0xD00044));
        break;
    case 2:                                       /* the game ended the demo or title itself */
        wr8(STATE, 2);
        wr8(NO_LOCK, 1);
        save_game_ram();
        handshake(sysret_state2_slot);
        return;
    case 4: case 5:                               /* select: next / previous slot */
        wr8(STATE, 2);
        wr8(NO_LOCK, 1);
        save_game_ram();
        handshake(sysret_state45_slot);
        return;
    default:                                      /* 6: coin in the demo */
        wr8(STATE, 2);
        break;
    }
    state_machine();
}

/* ---- boot ($C11002 .. $C11038) ---- */
static void boot2(void);
static int booted_once;

static void boot(void)
{
    m68ki_set_sr_noint(0x2700);
    kick();
    wr16(REG_IRQACK, 7);
    wr16(REG_LSPCMODE, 0x4000);
    REG_SP = SP_TOP;
    wr8(REG_SWPBIOS, 0);
    /* the self-tests take this long on the real board; first boot of a fresh backup RAM separately */
    int fresh = memcmp(&BK(0xD00010), "BACKUP RAM OK !\x80", 16) || bk16(0xD00124) != rd16(H_NGH);
    int d = fresh ? boot_delay_fresh : booted_once ? boot_delay_reboot : boot_delay;
    booted_once = 1;
    delay_until = ng.frame + (uint64_t)(d > 0 ? d : 0);
    after_wait = boot2;
    REG_PC = K_WAIT + 2;                          /* frame delay: see bios_hle_exec */
}

static void boot2(void)
{
    REG_PC = K_END;
    /* backup RAM: signature, defaults ($C125F4) */
    if (memcmp(&BK(0xD00010), "BACKUP RAM OK !\x80", 16)) {
        memset(ng.backup, 0, sizeof ng.backup);
        memcpy(&BK(0xD00010), "BACKUP RAM OK !\x80", 16);
        static const uint8_t def[] = { 1, 1, 1, 2, 1, 1, 1, 3, 0, 0, 0x30, 0x3B, 0 };   /* $D0003A.. */
        memcpy(&BK(0xD0003A), def, sizeof def);
        BK(0xD00122) = 0x23;
        for (int i = 0; i < 8; i++) { bkw16(0xD00124 + 4 * (uint32_t)i, 0); bkw16(0xD00126 + 4 * (uint32_t)i, 0xFFFF); }
        BK(0xD001AC) = BK(0xD001AD) = BK(0xD001AE) = BK(0xD001AF) = 0xF0;
        bkw16(0xD001B0, 0xFFFF);
    }
    /* every boot ($C11DB0) */
    bkw16(0xD0015E, 0xFFFF);
    bkw16(0xD00160, 0);
    memset(&BK(0xD00148), 0, 4);
    memset(&BK(0xD00020), 0, 4);
    memset(&BK(0xD00024), 0, 0x16);              /* coin counts and credits */
    BK(0xD00047) = 6;
    BK(0xD00058) = rd8(H_EYECATCH) == 0 ? 0 : 0xFF;
    wr8(REG_SLOT, 0);
    sound(1);
    BK(0xD00109) = 0;
    /* warm init ($C11038): work RAM, palettes, fix, sprites */
    for (uint32_t a = 0; a < 0x10000; a++) ng.ram[a] = 0;
    wr8(REG_PALBANK1, 0);
    for (uint32_t a = 0x400000; a < 0x402000; a += 2) wr16(a, 0);
    wr8(REG_PALBANK0, 0);
    for (uint32_t a = 0x400000; a < 0x402000; a += 2) wr16(a, 0);
    wr8(REG_CRTFIX, 0);                          /* no BIOS fix ROM needed: the cartridge's from the start */
    fix_clear();
    wr16(REG_VRAMMOD, 1);
    wr16(REG_VRAMADDR, 0);
    for (int i = 0; i < 0x5F40 / 2; i++) { wr16(REG_VRAMRW, 0x00FF); wr16(REG_VRAMRW, 0); }
    lsp_1st();
    wr16(MVS_FLAG, ng_aes ? rd16(0xC00400) & 0x00FF : rd16(0xC00400));
    wr32(MESS_POINT, 0x10FF00);
    controller_setup();
    REG_SP = SP_TOP;
    state_machine();
}

/* ---- dispatch ---- */
int bios_hle_owns(uint32_t pc)
{
    pc &= 0xffffff;
    if (pc - 0xC00000u >= 0x20000u) return 0;
    if (bios_hle_level >= 3) return 1;
    if (pc >= K_BASE && pc < K_END + 2) return 1;
    switch (pc) {
    case E_LSP_1ST: case E_FIX_CLEAR: case E_CREDIT_CHECK: case E_CREDIT_DOWN: case E_CARD: case E_CARD_ERROR:
        return bios_hle_level >= 1;
    case E_SYSTEM_IO:
        return bios_hle_level >= 2;
    default:
        return 0;
    }
}

void bios_hle_exec(void)
{
    uint32_t pc = REG_PC & 0xffffff;
    switch (pc) {
    /* game calls */
    case E_LSP_1ST: lsp_1st(); rts(); return;
    case E_FIX_CLEAR: fix_clear(); rts(); return;
    case E_CREDIT_CHECK: credit_check(); USE_CYCLES(260); rts(); return;
    case E_CREDIT_DOWN: credit_down(); rts(); return;
    case E_CARD: wr8(0x10FCF4, 0xFF); wr8(CARD_ANSWER, 0x80); wr8(REG_CRDLOCK2, 0); wr8(REG_CRDLOCK1, 0); rts(); return;
    case E_CARD_ERROR: wr8(CARD_ANSWER + 1, rd8(CARD_ANSWER)); rts(); return;
    case E_CONTROLLER_SETUP: controller_setup(); rts(); return;
    case E_SYSTEM_IO: system_io(0); return;
    case E_MESS_OUT: case E_READ_CALENDAR: case E_SETUP_CALENDAR: case E_HOW_TO_PLAY: rts(); return;
    case E_SYSTEM_RETURN: system_return(); return;
    /* vectors */
    case E_INT1: int1(); return;
    case E_INT2: wr16(REG_IRQACK, 2); wr16(REG_LSPCMODE, 0x4000); rte(); return;
    /* continuations */
    case K_COIN: after_coin(); return;
    case K_START: after_start(); return;
    case K_DEMO_END: after_demo_end(); return;
    case K_WAIT:                                  /* VBlank wait: SYSTEM_INT1 clears the flag */
        if (rd8(WAIT_VBL)) { SET_CYCLES(0); return; }
        REG_PC = K_END;
        after_wait();
        return;
    case K_WAIT + 1:                              /* Z80 handshake: poll the reply */
        USE_CYCLES(200);
        after_wait();
        return;
    case K_WAIT + 3: {                            /* spend cycles */
        int c = GET_CYCLES();
        if (c <= 0) return;
        if (spend_left > c) { spend_left -= c; SET_CYCLES(0); return; }
        USE_CYCLES(spend_left);
        spend_left = 0;
        REG_PC = K_END;
        after_spend();
        return;
    }
    case K_WAIT + 2:                              /* boot delay in frames (the self-tests kick the watchdog) */
        kick();
        if (ng.frame < delay_until) { SET_CYCLES(0); return; }
        after_wait();
        return;
    default:
        break;
    }
    /* reset vector and the exception vectors (all lead to the reset path on this BIOS) */
    if (pc >= E_RESET && pc < E_INT1) { boot(); return; }
    if (pc == 0xC11002) { boot(); return; }
    fprintf(stderr, "bios_hle: unhandled BIOS address %06x (frame %llu)\n", pc, (unsigned long long)ng.frame);
    SET_CYCLES(0);
}

/* --bios-log: the calls into the game, seen when the CPU arrives there (with the ROM or the native BIOS) */
void bios_hle_watch(uint32_t pc)
{
    if (pc == G_USER) logf_("USER", REG_D[0] & 0xff, rd8(USER_MODE));
    else if (pc == G_PLAYER_START) logf_("PLAYER_START", rd8(START_FLAG), credits());
    else if (pc == G_DEMO_END) logf_("DEMO_END", rd8(STATE), credits());
    else if (pc == G_COIN_SOUND) logf_("COIN_SOUND", 0, 0);
    else if (pc == 0xC112D2) logf_("SYSTEM_RETURN", rd8(STATE), credits());
}

void bios_hle_init(int level, const char *timing)
{
    bios_hle_level = level;
    boot_delay_fresh = boot_delay = boot_delay_reboot = eyecatch_frames = 0;
    /* "real": as the BIOS ROM with --rtc-fixed (its calendar test waits for the 1 Hz pulse, so the length of the
       self-tests depends on the clock's phase): fresh backup RAM 267, power-on 208, after a watchdog reset 226 */
    if (timing && !strcmp(timing, "real")) { boot_delay_fresh = 267; boot_delay = 208; boot_delay_reboot = 226; eyecatch_frames = 465; }
    else if (timing) {
        int n = sscanf(timing, "%d,%d,%d,%d", &boot_delay_fresh, &boot_delay, &eyecatch_frames, &boot_delay_reboot);
        if (n < 4) boot_delay_reboot = boot_delay;
    }
}

/* a stand-in for the BIOS ROM (level 3 without one): the vector table and jump table addresses lead into the
   HLE; $C00400 = $8002 (MVS, Europe) as on sp-s2 */
void bios_hle_make_rom(uint8_t *bios, uint32_t size)
{
    memset(bios, 0xFF, size);
    static const uint32_t vec[32] = {
        0x10F300, 0xC00402, 0xC00408, 0xC0040E, 0xC00414, 0xC00426, 0xC00426, 0xC00426,
        0xC0041A, 0xC00420, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC0042C,
        0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426,
        0xC00432, 0xC00438, 0xC0043E, 0xC00426, 0xC00426, 0xC00426, 0xC00426, 0xC00426,
    };
    for (int i = 0; i < 32; i++) {
        bios[4 * i] = (uint8_t)(vec[i] >> 24); bios[4 * i + 1] = (uint8_t)(vec[i] >> 16);
        bios[4 * i + 2] = (uint8_t)(vec[i] >> 8); bios[4 * i + 3] = (uint8_t)vec[i];
    }
    bios[0x400] = 0x80; bios[0x401] = 0x02;
}

/* the L0 zoom ROM: for shrink level z the hardware shows z + 1 of a sprite's 256 lines, the set
   { bitreverse8(k) ^ $88 : k <= z } in ascending order (verified identical to 000-lo.lo) */
void bios_hle_lo_table(uint8_t *lo)
{
    for (int z = 0; z < 256; z++) {
        uint8_t used[256] = { 0 };
        for (int k = 0; k <= z; k++) {
            int r = 0;
            for (int b = 0; b < 8; b++) if (k & (1 << b)) r |= 0x80 >> b;
            used[r ^ 0x88] = 1;
        }
        int n = 0;
        for (int l = 0; l < 256; l++) if (used[l]) lo[z * 256 + n++] = (uint8_t)l;
        while (n < 256) lo[z * 256 + n++] = 0xFF;
    }
}

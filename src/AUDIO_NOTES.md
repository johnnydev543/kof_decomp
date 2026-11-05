# Sound subsystem (Z80 + YM2610)

Files: `src/audio.h`, `src/audio.cpp`, `src/z80/` (patched copy of superzazu/z80, MIT,
`src/z80/LICENSE`), `tests/audio_test.cpp`. Uses ymfm (`third_party/ymfm/src`, BSD-3) unmodified.

## Design

* One global `AudioState` (allocated by `audio_init`, freed by `audio_shutdown`). ROMs are copied.
* Time base: Z80 cycle counter `cpu.cyc` (4 MHz). YM time = 2 x Z80 cycles (8 MHz input clock).
* ymfm is run at `OPN_FIDELITY_MIN`, so one `generate()` call = 144 YM clocks = 72 Z80 cycles =
  one output frame at `AUDIO_RATE` = 55555 Hz. Since a sample is a whole number of Z80 cycles,
  sample timing is exact: `next_sample` is an absolute YM-clock timestamp, `target_z80` an absolute
  Z80 target, so instruction overshoot and the 256-cycle calls (= 3.555 samples) are carried over
  without drift (264 x 256 cycles -> 938.67 frames/frame on average).
* `audio_run(n)`: the Z80 runs in slices that end at min(target, next sample, next YM timer expiry).
  After each slice, timers whose expiry has passed are fired (in time order, interleaved with sample
  generation) via `engine_timer_expired()`. While a timer callback runs, "now" is overridden to the
  exact expiry time so the re-armed timer does not drift. Timer/IRQ latency is therefore at most one
  Z80 instruction (a few µs); slices are <= 18 µs.
* HALT with nothing able to wake it is fast-forwarded to the slice end.
* YM IRQ -> Z80 INT, level triggered: before every `z80_step`, `cpu.int_pending = irq_line`
  (IM1 -> RST 38h; `int_data = 0xFF` so IM0 would also do RST 38h).
* NMI: `audio_write_command` latches the byte and sets `nmi_pending`; the NMI line is
  `nmi_enabled && nmi_pending` and a rising edge calls `z80_gen_nmi` (MAME `audio_cpu_check_nmi`).
  `IN 0x00` returns the latch and clears `nmi_pending`; a command written while NMIs are disabled
  fires as soon as the Z80 enables them (OUT 0x08).
* Busy flag: `ymfm_set_busy_end`/`ymfm_is_busy` implemented against the same time base.
* ADPCM-A and ADPCM-B both read the V ROM (v1+v2+v3); addresses beyond the ROM wrap (`addr % size`).
* Mix: `L = out[0] + ssg*2/7`, `R = out[1] + ssg*2/7`, clamped to int16 (out[0]/[1] are ymfm's FM +
  ADPCM, already clamped; out[2] is SSG). 2/7 ~= MAME's neogeo routing ratio 0.28/0.98.
* Output ring buffer: 32768 stereo frames (~0.59 s); on overflow the oldest frame is dropped.

### Memory / I/O map (MAME neogeo.cpp)

| Z80 address | contents |
|---|---|
| 0000-7FFF | first 32 KB of selected ROM (cart m1 or BIOS sm1) |
| 8000-BFFF | window 3, 16 KB units (initial bank 0x02) |
| C000-DFFF | window 2, 8 KB units (initial 0x06) |
| E000-EFFF | window 1, 4 KB units (initial 0x0E) |
| F000-F7FF | window 0, 2 KB units (initial 0x1E) |
| F800-FFFF | 2 KB RAM |

Bank offsets index the *selected* ROM, modulo its size (the BIOS sm1 driver never banks, so the
MAME detail of windows always pointing at the cart is irrelevant in practice).

Ports (low byte decoded, mirrored over the high byte): IN 00 = command latch (+clear NMI pending),
OUT 00 = clear latch; 04-07 = YM2610 `read/write(offset & 3)`; OUT 08 = NMI enable, OUT 18 = NMI
disable; IN 08/09/0A/0B (mirror 0x00F0) = bank select window 0/1/2/3, bank = A8-A15, returns 0;
OUT 0C = reply latch for the 68k. Unmapped reads return 0.

## Patch to the Z80 core (`src/z80/z80.c`, `src/z80/z80.h`)

`port_in`/`port_out` now take a `uint16_t` port (marked `PATCH` in the source):
* `IN A,(n)` / `OUT (n),A`: port = `A << 8 | n`.
* `IN r,(C)`, `OUT (C),r`, `INI/INIR/IND/INDR`: port = BC (B before decrement).
* `OUTI/OTIR/OUTD/OTDR`: B is decremented *before* the I/O (as on real silicon), port = BC.
* `z80.h` got `extern "C"` guards. Nothing else changed.

## Build and run the test

From the project root:

```sh
mkdir -p build/audio_test
gcc -O2 -Wall -c src/z80/z80.c -o build/audio_test/z80.o
g++ -std=c++17 -O2 -Isrc -Ithird_party/ymfm/src src/audio.cpp tests/audio_test.cpp \
    third_party/ymfm/src/ymfm_opn.cpp third_party/ymfm/src/ymfm_adpcm.cpp \
    third_party/ymfm/src/ymfm_ssg.cpp third_party/ymfm/src/ymfm_misc.cpp \
    build/audio_test/z80.o -o build/audio_test/audio_test
./build/audio_test/audio_test            # curated list -> build/audio_test.wav (35.5 s)
./build/audio_test/audio_test --scan 02 FF            # one fresh driver per command, RMS table
./build/audio_test/audio_test --scan 00 FF --prefix 1D # scan 2-byte commands 1D:xx
./build/audio_test/audio_test --cmds 21,1D:12 --secs 3 # arbitrary sequences ("a:b" = 2-byte cmd)
./build/audio_test/audio_test --bios-boot ...          # start on sm1, 0x01, switch to m1, 0x03
```

ymfm needs only `ymfm_opn.cpp ymfm_adpcm.cpp ymfm_ssg.cpp ymfm_misc.cpp`. Add `-DAUDIO_TRACE`
when compiling `audio.cpp` to log YM writes, bank switches and IRQ edges to stderr.
Runs ~40x real time on this machine.

## KOF97 sound driver protocol (m1 "Sound Driver Ver 1.1 96/10/01 To SNK"), found empirically + disassembly

The NMI handler echoes every byte on port 0x0C and queues it (64-entry ring at RAM FDF5); the main
loop dispatches. Bytes 0x01, 0x03, 0x10 are handled directly in the NMI.

**Important:** after reset the driver sets "music disabled" (FDE1) and "SFX disabled" (FDE2) to 0xFF,
so every 0x20+ command is silently ignored until **0x07** is sent. The game's 68k code sends it
itself; when testing sound without the 68k you must send it (the test does, as a "prelude").

| cmd | meaning |
|---|---|
| 01 | prepare slot switch: driver parks in RAM (PC=FFFD) |
| 02 | short jingle (RMS ~760) |
| 03 | soft reset (jp 0000) |
| 04 | stop everything and mute music+SFX |
| 05 / 06 | mute music / mute SFX |
| 07 | unmute music+SFX (**required after reset**) ; 08 = unmute music, 09 = unmute SFX |
| 0A nn | fade music out, nn = speed (0x08 slow, 0x20 fast); 11 cancels fade |
| 0D | clears driver channel state (music then stays silent until 07) |
| 10 | sound-ROM checksum self test (used by BIOS test mode; leaves driver muted until 07) |
| 18..1E nn | 2-byte SFX/voice commands (prefix + id), validated by bitmaps at ROM 0x2E1C |
| 20..5F | music (table 0x3216); 00/FF and 60+ undefined ids do nothing |

RMS over 1.5 s after the command, fresh driver + 07 each time (int16 units, peak in brackets):

* **Music (FM + ADPCM-A drums + ADPCM-B):** 21 2092, 22 3048, 23 2437, 24 1785, 25 2089,
  26 1765, 27 634, 28 263, 29 1881, 2A 838, 2B 1488, 2C 1429, 2D 2009, 2E 1939, 2F 1549, 30 366,
  31 457, 32 731, 33 674, 34 728, 35 959, 36 2485/674 (L/R), 37 1805, 38 392, 39 680, 3A 1898,
  40 984, 41 2005, 42 2267, 43 1214, 44 1032, 45 1215, 46 2155, 47 1586, 48 1300, 49 1863,
  4A 1107, 4B 296, 4C 1586, 50 2596, 51 634. Silent: 20, 3B-3F, 4D-4F, 52-5F.
* **1-byte SFX (mono):** 65 1892, 70 251, 71 457, 72 654, 7A 116, 7F 377.
* **2-byte SFX/voices (ADPCM-A, mono, key-on on ADPCM-A channel 4):** prefix 1A (156 ids),
  1B (236), 1C (235), 1D (160), 1E (233) - almost every id 0x11..0xFF produces sound, e.g.
  1D:12 3864, 1D:13 4404, 1A:15 2611, 1B:16 2405, 1C:16 3724, 1E:16 2927; prefix 19 only 19:68
  (1178). Prefix 18 has no valid ids.

## Integration notes

* Call order: `audio_init(...)` once (power-on state = BIOS sm1 mapped, as on hardware), then per
  scanline `audio_run(256)`; forward 68k byte writes to 0x320000 to `audio_write_command`, reads of
  0x320000 (upper byte) to `audio_read_reply`, and REG_BRDFIX/REG_CRTFIX writes (0x3A000B / 0x3A001B)
  to `audio_set_cart_rom(0/1)`. The BIOS does 01 -> switch to cart -> 03 itself; this is verified with
  `--bios-boot` (driver comes up correctly). Drain with `audio_read_samples` (feed SDL at 55555 Hz,
  or resample).
* The Z80 lags the 68k by at most one scanline (64 µs); 68k code polling the reply latch sees the
  echo after the next `audio_run`.
* `audio_reset()` resets Z80 + YM (e.g. for a machine reset); it keeps the ROM selection, so on a full
  machine reset also call `audio_set_cart_rom(0)`.
* `audio_save_state` returns 0 (not implemented).
* Not thread-safe: if the SDL audio callback drains samples on another thread, protect
  `audio_run`/`audio_read_samples` with a mutex (or use `SDL_QueueAudio` from the main thread).

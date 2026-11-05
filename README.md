# kof_decomp

A decompilation project for SNK Neo Geo fighting games, starting with **The King of Fighters '97** (MVS).

It contains:

- **A Neo Geo emulator with a static recompiler.** The 68000 program ROM is translated to C and runs
  cycle-exactly next to a Musashi fallback interpreter, with an SDL2 front end, YM2610 sound (ymfm) and a
  Z80 core. `make test` checks the recompiled instructions against the interpreter.
- **A matching, shiftable disassembly.** `make matching` turns the P1 program ROM into GNU `as`
  source that reassembles to the original bytes (MAME CRCs verified). The source is symbolic: inserting
  code anywhere moves everything and the game still behaves the same. `make shifttest` verifies this by
  replaying 69 coverage scripts on the original image and on three shifted builds, comparing every
  VBlank, screenshot and the audio.
- **Names.** Functions, data and RAM get names from curated analysis and from observation tools: which
  character's runs execute it, which command or action triggers it, and which named function owns it.
  Official move names come from the game's move list. Coverage today: about 90% of the executed program
  code is under a name (`make namecov`).

**No ROM data is included.** You need your own `kof97.zip` and `neogeo.zip` (MAME sets). Everything built
from them (`build/`) is local only and must not be redistributed.

## Requirements

- Linux (other Unix systems may work), `make`, a C/C++ compiler, Python 3
- `pkg-config`, SDL2 development files (`libsdl2-dev`)
- 68000 binutils for the disassembly: `binutils-m68k-linux-gnu` (or `m68k-elf-*`; set `M68K_PREFIX`)

## Quick start

```sh
cp /path/to/kof97.zip /path/to/neogeo.zip roms/
make -j8            # checks the ROM CRCs, recompiles, builds build/kof97
build/kof97         # play (see build/kof97 --help for headless, scripts, logs)
make test           # recompiled code vs interpreter: 0 failed
make matching       # build/disasm/p1.s -> reassembled P1/P2, CRCs verified
make shifttest      # behaviour unchanged with code shifted by 2 bytes (long; limit with --jobs)
make namecov        # how much executed code has a name
make codecheck      # code still emitted as data / bad character labels
```

## Layout

```
src/                 emulator and runtime (Neo Geo hardware, video, audio, CPU glue)
tools/recomp/        68000 -> C recompiler, mode check (recompiled vs interpreted)
tools/disasm/        matching disassembler (mkasm.py, mkrom.py), shift test and debugging tools,
                     naming tools (charmap, actionmap, specmap, derivemap, movenames, chrcheck, namecov)
tools/rea/, ghidra/  seeds and setup for REA / Ghidra analysis
games/kof97/         everything specific to KOF97:
  game.json            ROM names and CRCs, addresses the tools need, RAM addresses, character list
  symbols/             curated.txt (hand-written names) and generated name files
  scripts/             coverage input scripts (gen_scripts.py writes them)
  moves.tsv            move list (from MAME command.dat)
  commands.tsv         decoded per-character commands (motion + buttons -> move routine)
  rea/                 REA seeds for the P1 ROM
third_party/         Musashi (68000), ymfm (YM2610), z80
docs/NEW_ROM_GUIDE.md  how to add another Neo Geo game
AGENT.md             detailed technical notes (Chinese)
```

Tools select the game with `GAME=<name>` (default `kof97`); another game gets its own `games/<name>/`
directory. The build output (`build/`, the `build/kof97` executable) currently holds one game at a time.

## Status (KOF97)

- Matching P1/P2: OK. Shift test: 207/207 runs identical.
- Decoded subsystems include the object system, sprite/palette/fix uploads, select and order screens,
  round flow, HUD, hit reactions and throws, command recognition (motion lists and per-character
  command checks), the CPU AI (4-opcode scripts selected from P2 ROM tables), stage objects, endings,
  rankings and the attract mode.

## Credits and licenses

- Musashi 68000 core by Karl Stenerud (see `third_party/Musashi/readme.txt`)
- ymfm by Aaron Giles, BSD 3-Clause (`third_party/ymfm/LICENSE`)
- z80 by Nicolas Allemand, MIT (`third_party/z80/LICENSE`)
- Move list in `games/kof97/moves.tsv`: MAME command.dat (progettosnaps.net), rebuilt by Fabricio
  Coroquer, created by Minoplis
- The King of Fighters and Neo Geo are trademarks of SNK. This project is not affiliated with SNK.

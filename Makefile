# KOF97 static recompilation build.
#   make            ROM prep -> Musashi -> recompile (uses existing REA output) -> build/kof97
#   make rea        (re)run REA/Ghidra decompile-all on P1 and BIOS (needs tools/rea/rea.sh toolchain)
#   make test       per-instruction differential test of recompiled code against Musashi
#   make recomp     regenerate build/recomp/*.c (e.g. after a run added runtime misses)
#   make clean      remove build products (keeps ROM prep and REA output)

CC      ?= gcc
CXX     ?= g++
OPT     ?= -O2
RC_OPT  ?= -O1
B       := build
NFILES  := 48
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2) $(if $(filter Darwin,$(shell uname)),-framework OpenGL,-lGL)

MUSASHI_SRC := third_party/Musashi
MUSASHI     := $(B)/musashi
MUSASHI_OBJ := $(B)/obj/m68kcpu.o $(B)/obj/m68kops.o $(B)/obj/m68kdasm.o $(B)/obj/softfloat.o
YMFM        := third_party/ymfm/src
YMFM_OBJ    := $(B)/obj/ymfm_opn.o $(B)/obj/ymfm_adpcm.o $(B)/obj/ymfm_ssg.o $(B)/obj/ymfm_misc.o
RC_C        := $(foreach i,$(shell seq -w 0 $$(($(NFILES)-1))),$(B)/recomp/rc_$(i).c) $(B)/recomp/rc_table.c
RC_OBJ      := $(patsubst $(B)/recomp/%.c,$(B)/obj/recomp/%.o,$(RC_C))
APP_OBJ     := $(B)/obj/main.o $(B)/obj/neogeo.o $(B)/obj/video.o $(B)/obj/cpu.o $(B)/obj/audio.o $(B)/obj/z80.o $(B)/obj/snd_native.o $(B)/obj/spr_native.o $(B)/obj/gpu.o

CFLAGS_COMMON := -I$(MUSASHI) -Isrc -Wall
ROMS := $(B)/roms/p1.bin

all: $(B)/kof97

# ---- ROMs -----------------------------------------------------------------------------------
# game selection: GAME=<name> uses games/<name>/ (ROM names/CRCs and per-game tool settings)
GAME ?= kof97
export GAME
GAME_ZIPS := $(shell python3 -c "import json;c=json.load(open('games/$(GAME)/game.json'));print(c['rom_zip'],c['bios_zip'])")
$(ROMS): $(GAME_ZIPS) games/$(GAME)/game.json tools/romprep.py
	python3 tools/romprep.py

# ---- Musashi (copied with the project m68kconf.h, opcode tables generated) -----------------
$(MUSASHI)/m68kops.c: src/musashi/m68kconf.h
	mkdir -p $(MUSASHI)
	cp -r $(MUSASHI_SRC)/*.c $(MUSASHI_SRC)/*.h $(MUSASHI_SRC)/softfloat $(MUSASHI)/
	cp src/musashi/m68kconf.h $(MUSASHI)/
	$(CC) -O2 -o $(MUSASHI)/m68kmake $(MUSASHI)/m68kmake.c
	cd $(MUSASHI) && ./m68kmake

$(B)/obj/%.o: $(MUSASHI)/%.c $(MUSASHI)/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -I$(MUSASHI) -c $< -o $@
$(B)/obj/softfloat.o: $(MUSASHI)/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -I$(MUSASHI) -c $(MUSASHI)/softfloat/softfloat.c -o $@

$(B)/m68kinfo: tools/recomp/m68kinfo.c $(MUSASHI_OBJ)
	$(CC) $(OPT) $(CFLAGS_COMMON) -o $@ $< $(MUSASHI_OBJ) -lm
$(B)/m68k_table.txt: $(B)/m68kinfo
	$< table > $@

# ---- REA / Ghidra ---------------------------------------------------------------------------
REA_IDX := $(B)/rea/decomp_p1/decompile-index.json $(B)/rea/decomp_bios/decompile-index.json
rea: $(ROMS)
	rm -rf $(B)/rea/decomp_p1 $(B)/rea/decomp_bios && mkdir -p $(B)/rea
	REA_GHIDRA_SEED_FILE=$(CURDIR)/games/$(GAME)/rea/seeds_p1.tsv tools/rea/rea.sh decompile-all $(CURDIR)/$(B)/roms/p1.be \
	  --target-format raw-binary --architecture m68k --entry-point 0x0 --provider ghidra \
	  --output $(CURDIR)/$(B)/rea/decomp_p1 --format json > $(B)/rea/decomp_p1.json
	REA_GHIDRA_SEED_FILE=$(CURDIR)/tools/rea/seeds_bios.tsv tools/rea/rea.sh decompile-all $(CURDIR)/$(B)/roms/bios.be \
	  --target-format raw-binary --architecture m68k --entry-point 0xc00000 --provider ghidra \
	  --output $(CURDIR)/$(B)/rea/decomp_bios --format json > $(B)/rea/decomp_bios.json

# ---- static recompiler ---------------------------------------------------------------------
RECOMP_STAMP := $(B)/recomp/.stamp
$(RECOMP_STAMP): tools/recomp/m68k_recomp.py $(B)/m68k_table.txt $(B)/m68kinfo $(ROMS) $(wildcard $(REA_IDX)) \
                 $(wildcard tools/recomp/extra_seeds.txt) $(wildcard $(B)/recomp/runtime_misses.txt) \
                 $(wildcard $(B)/disasm/p1.bin) $(wildcard $(B)/disasm/code_p1.txt)
	python3 tools/recomp/m68k_recomp.py --files $(NFILES)
	touch $@
$(RC_C) $(B)/recomp/rc_tests.c: $(RECOMP_STAMP)
	@touch -c $@
recomp:
	rm -f $(RECOMP_STAMP)
	$(MAKE) $(RECOMP_STAMP)

$(B)/obj/recomp/%.o: $(B)/recomp/%.c src/recomp_rt.h $(MUSASHI)/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(RC_OPT) $(CFLAGS_COMMON) -Wno-unused-label -c $< -o $@

# ---- runtime ---------------------------------------------------------------------------------
$(B)/obj/main.o $(B)/obj/gpu.o: $(B)/obj/%.o: src/%.c src/neogeo.h src/cpu.h src/audio.h src/gpu.h src/spr_native.h
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(CFLAGS_COMMON) $(SDL_CFLAGS) -c $< -o $@
$(B)/obj/%.o: src/%.c src/neogeo.h src/cpu.h src/recomp_rt.h $(MUSASHI)/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(CFLAGS_COMMON) -c $< -o $@
$(B)/obj/audio.o: src/audio.cpp src/audio.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 $(OPT) -Isrc -I$(YMFM) -c $< -o $@
$(B)/obj/z80.o: src/z80/z80.c src/z80/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -c $< -o $@
$(B)/obj/ymfm_%.o: $(YMFM)/ymfm_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 $(OPT) -I$(YMFM) -c $< -o $@

$(B)/kof97: $(APP_OBJ) $(RC_OBJ) $(MUSASHI_OBJ) $(YMFM_OBJ)
	$(CXX) -o $@ $^ $(SDL_LIBS) -lm

# ---- tests -------------------------------------------------------------------------------------
$(B)/rc_difftest: tests/rc_difftest.c $(B)/recomp/rc_tests.c $(MUSASHI_OBJ)
	$(CC) $(RC_OPT) $(CFLAGS_COMMON) -Wno-unused-label -o $@ tests/rc_difftest.c $(B)/recomp/rc_tests.c $(MUSASHI_OBJ) -lm
test: $(B)/rc_difftest
	$(B)/rc_difftest
# whole-program check: every coverage script on recompiled C vs pure Musashi, frame by frame
modecheck: $(B)/kof97
	python3 tools/recomp/modecheck.py --all

# ---- matching disassembly (AGENT.md 7.5) ---------------------------------------------------------
# matching:  P1 -> symbolized GNU as source -> reassembled P1/P2, verified against the MAME CRCs
# shifttest: every games/$(GAME)/scripts/*.txt + autoplay, original vs code shifted by 2 bytes
# executed-address / read maps from interpreter runs of every coverage script (stage 2 evidence)
$(B)/disasm/exec_p1.bin: $(B)/kof97 $(wildcard games/$(GAME)/scripts/*.txt)
	python3 tools/disasm/shifttest.py --all --collect-only
matching: $(ROMS) $(B)/m68kinfo $(B)/m68k_table.txt $(B)/disasm/exec_p1.bin
	python3 tools/disasm/mkasm.py --symbolize --out $(B)/disasm
	python3 tools/disasm/mkrom.py --out $(B)/disasm
	python3 tools/disasm/codemap.py
# charmap: regenerate games/$(GAME)/symbols/chars.txt (character-specific code, mirror-match runs)
charmap: $(B)/kof97 $(B)/m68kinfo $(B)/m68k_table.txt
	python3 tools/disasm/charmap.py
# actionmap: regenerate games/$(GAME)/symbols/actions.txt (shared player-action code, 2P versus runs)
actionmap: $(B)/kof97 $(B)/m68kinfo $(B)/m68k_table.txt
	python3 tools/disasm/actionmap.py
# derivemap: names for code used only by one named function (games/$(GAME)/symbols/derived.txt)
derivemap:
	python3 tools/disasm/derivemap.py
# namecov: how much executed code has a stage-3 name; biggest unnamed functions (after make matching/shifttest)
namecov:
	python3 tools/disasm/namecov.py
# codecheck: code still emitted as data (shift hazards the shift test cannot see); chrcheck: bad chr_ labels
codecheck:
	python3 tools/disasm/codecheck.py
	python3 tools/disasm/chrcheck.py
# specmap: regenerate games/$(GAME)/symbols/specials.txt (character code by command motion)
specmap: $(B)/kof97 $(B)/m68kinfo $(B)/m68k_table.txt
	python3 tools/disasm/specmap.py
# rendertest: the exported frames used as their own replacements give the original picture (needs `make assets`)
rendertest: $(B)/kof97
	python3 tools/assets/rendertest.py
shifttest: $(B)/kof97 $(B)/m68kinfo $(B)/m68k_table.txt
	python3 tools/disasm/shifttest.py --all

clean:
	rm -rf $(B)/obj $(B)/kof97 $(B)/rc_difftest $(B)/recomp/*.c $(RECOMP_STAMP)

# ---- assets for redrawing / remaking (local only: derived from the ROMs) -----------------------
$(B)/sound_render: tools/assets/sound_render.cpp $(B)/obj/audio.o $(B)/obj/z80.o $(YMFM_OBJ)
	$(CXX) -std=c++17 $(OPT) -Isrc -o $@ $^ -lm
assets: $(B)/sound_render $(B)/roms/p1.bin
	python3 tools/assets/export_sprites.py --composites --sheet
	python3 tools/assets/export_audio.py
audio-ogg:                     # Ogg Vorbis copies of the sounds for the web build (~55 MB instead of ~820 MB)
	python3 tools/assets/ogg_audio.py

# ---- web build (Emscripten in $(EMSDK)) and the LAN-only development server ------------------
EMSDK ?= $(HOME)/emsdk
web: $(RC_C) $(MUSASHI)/m68kops.c $(B)/roms/p1.bin
	bash -c 'source $(EMSDK)/emsdk_env.sh >/dev/null 2>&1 && $(MAKE) -f web.mk -j$(WEB_JOBS) $(if $(MODS),MODS=$(MODS))'
WEB_JOBS ?= 10
WEB_PORT ?= 8097
serve: web
	python3 tools/web/serve.py --port $(WEB_PORT) --dir $(B)/web

.PHONY: all rea recomp test clean web serve assets audio-ogg rendertest matching shifttest charmap actionmap specmap modecheck namecov codecheck derivemap

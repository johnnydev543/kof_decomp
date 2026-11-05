# Web build (Emscripten): the same sources as the native build -> build/web/index.{html,js,wasm,data}.
# Run through the main Makefile (`make web`), which loads the Emscripten environment ($(EMSDK)/emsdk_env.sh).
# The ROM data is packed into build/web/index.data from build/roms: a local development build only,
# never to be published (see README).

B       := build
W       := $(B)/web
NFILES  := 48
MUSASHI := $(B)/musashi
YMFM    := third_party/ymfm/src
OPT     ?= -O2
RC_OPT  ?= -O1
EMFLAGS := -sUSE_SDL=2
CFLAGS  := -I$(MUSASHI) -Isrc $(EMFLAGS)

RC_C    := $(foreach i,$(shell seq -w 0 $$(($(NFILES)-1))),$(B)/recomp/rc_$(i).c) $(B)/recomp/rc_table.c
APP_C   := src/main.c src/neogeo.c src/video.c src/cpu.c src/snd_native.c src/spr_native.c
OBJ     := $(patsubst src/%.c,$(W)/obj/%.o,$(APP_C)) \
           $(W)/obj/z80.o $(W)/obj/audio.o \
           $(W)/obj/m68kcpu.o $(W)/obj/m68kops.o $(W)/obj/m68kdasm.o $(W)/obj/softfloat.o \
           $(W)/obj/ymfm_opn.o $(W)/obj/ymfm_adpcm.o $(W)/obj/ymfm_ssg.o $(W)/obj/ymfm_misc.o \
           $(patsubst $(B)/recomp/%.c,$(W)/obj/recomp/%.o,$(RC_C))
ROMFILES := p1.bin p2.bin bios.bin s1.bin sfix.bin m1.bin sm1.bin v.bin c.bin lo.bin

all: $(W)/index.html

$(W)/obj/%.o: src/%.c src/neogeo.h src/cpu.h src/recomp_rt.h
	@mkdir -p $(dir $@)
	emcc $(OPT) $(CFLAGS) -c $< -o $@
$(W)/obj/z80.o: src/z80/z80.c src/z80/z80.h
	@mkdir -p $(dir $@)
	emcc $(OPT) -c $< -o $@
$(W)/obj/audio.o: src/audio.cpp src/audio.h
	@mkdir -p $(dir $@)
	em++ -std=c++17 $(OPT) -Isrc -I$(YMFM) -c $< -o $@
$(W)/obj/m68k%.o: $(MUSASHI)/m68k%.c
	@mkdir -p $(dir $@)
	emcc $(OPT) -I$(MUSASHI) -c $< -o $@
$(W)/obj/softfloat.o: $(MUSASHI)/softfloat/softfloat.c
	@mkdir -p $(dir $@)
	emcc $(OPT) -I$(MUSASHI) -c $< -o $@
$(W)/obj/ymfm_%.o: $(YMFM)/ymfm_%.cpp
	@mkdir -p $(dir $@)
	em++ -std=c++17 $(OPT) -I$(YMFM) -c $< -o $@
$(W)/obj/recomp/%.o: $(B)/recomp/%.c src/recomp_rt.h
	@mkdir -p $(dir $@)
	emcc $(RC_OPT) $(CFLAGS) -Wno-unused-label -c $< -o $@

# ROM data the runtime loads (packed under /build/roms, the runtime's default ROM directory)
$(W)/roms/.stamp: $(addprefix $(B)/roms/,$(ROMFILES))
	@mkdir -p $(W)/roms
	cp $^ $(W)/roms/
	touch $@

$(W)/index.html: $(OBJ) $(W)/roms/.stamp web/shell.html
	em++ $(OPT) -o $@ $(OBJ) $(EMFLAGS) -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 \
	    -sSTACK_SIZE=1048576 --shell-file web/shell.html \
	    --preload-file $(W)/roms@/build/roms --exclude-file '*/.stamp' \
	    $(if $(wildcard mods/kof97),--preload-file mods/kof97@/mods/kof97)

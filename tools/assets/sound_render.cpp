// sound_render - offline renderer for the Neo Geo sound driver (tools/assets/export_audio.py).
// Links src/audio.cpp (Z80 + ymfm YM2610) through its offline hooks; reads m1/sm1/v from the ROM dir.
//
//   sound_render [--roms build/roms] --seq "F:BB,F:BB,..." [--secs S] [--wav OUT.wav] [--ticks OUT.bin] [--ym OUT.txt]
//
// Boots the sound system exactly like the logged MVS boot + game start
// (BIOS: 01, cart switch, 03, 02 jingle; game sound_init: 03, 07), then
// writes byte BB at frame F (frames counted from the end of boot, 59.1856 Hz,
// one 68k VBlank each; the game sends at most one byte per frame).
// Records `secs` seconds of 55555 Hz stereo PCM from frame 0.
// --ticks: at each Z80 IRQ entry (PC=0x38, = driver tick), append u64 sample index + 2 KB Z80 RAM.
// --ym: every YM2610 register write "sample reg val" (reg 0x000-0x1FF).
#include "audio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>

extern "C" {
extern void (*audio_step_hook)(uint16_t pc);
extern int (*audio_ym_hook)(int port, uint8_t val);
uint8_t *audio_z80_ram(void);
uint64_t audio_frames_out(void);
}

static std::vector<uint8_t> load(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p.c_str()); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(n);
    if (fread(d.data(), 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    return d;
}

static std::vector<int16_t> pcm;
static bool recording = false;
static uint64_t rec_base = 0;          // audio_frames_out() at frame 0
static FILE *ticks_f = nullptr, *ym_f = nullptr;
static int ym_addr[2] = {0, 0};
static int zero_a = -1, zero_n = 0;      // --zero A,N: clear Z80 RAM A..A+N-1 at every driver tick (experiments)
static long zero_from = 0; static uint64_t cur_frame = 0;

static void step_hook(uint16_t pc) {
    if (pc == 0x38 && recording && zero_a >= 0 && (long)cur_frame >= zero_from) memset(audio_z80_ram() + (zero_a - 0xF800), 0, zero_n);
    if (pc == 0x38 && recording && ticks_f) {
        uint64_t s = audio_frames_out() - rec_base;
        fwrite(&s, 8, 1, ticks_f);
        fwrite(audio_z80_ram(), 1, 0x800, ticks_f);
    }
}
// --mute LIST: comma list of FM1,FM2,FM5,FM6,PA0..PA5,PB,SSG : their key-ons are dropped (stem rendering)
static unsigned mute_fm = 0, mute_pa = 0; static bool mute_pb = false, mute_ssg = false;
static int ym_hook(int port, uint8_t val) {
    int part = port >> 1;
    if (!(port & 1)) { ym_addr[part] = val; return val; }
    int reg = part * 0x100 + ym_addr[part];
    if (recording && ym_f)
        fprintf(ym_f, "%llu %03x %02x\n", (unsigned long long)(audio_frames_out() - rec_base), reg, val);
    if (reg == 0x28 && (mute_fm >> (val & 7) & 1)) return -1;
    if (reg == 0x100 && !(val & 0x80)) { val &= ~mute_pa; if (!(val & 0x3f)) return -1; }
    if (reg == 0x10 && mute_pb && (val & 0x80)) return -1;
    if (mute_ssg && reg >= 8 && reg <= 10) return val & 0xe0;   // level 0 (envelope mode bit 4 cleared too)
    return val;
}

static void run_frame() {
    static int16_t buf[4096 * 2];
    for (int l = 0; l < 264; l++) {
        audio_run(256);
        if (audio_samples_available() >= 2048) {
            int n;
            while ((n = audio_read_samples(buf, 4096)) > 0) if (recording) pcm.insert(pcm.end(), buf, buf + 2 * n);
        }
    }
    int n;
    while ((n = audio_read_samples(buf, 4096)) > 0) if (recording) pcm.insert(pcm.end(), buf, buf + 2 * n);
}

static void write_wav(const std::string &path, const std::vector<int16_t> &s) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t data = (uint32_t)(s.size() * 2);
    fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(AUDIO_RATE); u32(AUDIO_RATE * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(data);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

int main(int argc, char **argv) {
    std::string roms = "build/roms/";
    std::string wav, ticks, ym, seq;
    double secs = 10;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--roms") roms = std::string(argv[++i]) + "/";
        else if (a == "--seq") seq = argv[++i];
        else if (a == "--secs") secs = atof(argv[++i]);
        else if (a == "--wav") wav = argv[++i];
        else if (a == "--ticks") ticks = argv[++i];
        else if (a == "--ym") ym = argv[++i];
        else if (a == "--mute") {
            std::string m = argv[++i];
            for (size_t p = 0; p < m.size();) {
                size_t q = m.find(',', p); if (q == std::string::npos) q = m.size();
                std::string t = m.substr(p, q - p); p = q + 1;
                if (t.rfind("FM", 0) == 0) mute_fm |= 1u << atoi(t.c_str() + 2);
                else if (t == "PB") mute_pb = true;
                else if (t.rfind("PA", 0) == 0) mute_pa |= 1u << atoi(t.c_str() + 2);
                else if (t == "SSG") mute_ssg = true;
            }
        }
        else if (a == "--zero") { zero_a = (int)strtol(argv[++i], 0, 16); zero_n = atoi(argv[++i]); zero_from = atol(argv[++i]); }
        else { fprintf(stderr, "bad arg %s\n", a.c_str()); return 2; }
    }
    std::multimap<long, int> sched;   // frame -> byte (in order)
    for (const char *p = seq.c_str(); *p;) {
        char *q; long f = strtol(p, &q, 10);
        if (q == p || *q != ':') break;
        int b = (int)strtol(q + 1, &q, 16);
        sched.insert({f, b});
        p = q; if (*p == ',') p++;
    }
    auto m1 = load(roms + "m1.bin"), sm1 = load(roms + "sm1.bin"), v = load(roms + "v.bin");
    audio_init(m1.data(), m1.size(), sm1.data(), sm1.size(), v.data(), v.size());
    audio_step_hook = step_hook;
    audio_ym_hook = ym_hook;
    if (!ticks.empty()) ticks_f = fopen(ticks.c_str(), "wb");
    if (!ym.empty()) ym_f = fopen(ym.c_str(), "w");

    // boot, as logged from build/kof97 (frame: event)
    // 204: 01, cart; 208: 03 03; 214: 02; 675: 03 (BIOS); 678: 03, 684: 07 (game sound_init)
    for (long f = 0; f < 700; f++) {
        if (f == 204) { audio_write_command(0x01); audio_set_cart_rom(1); }
        if (f == 205) audio_set_cart_rom(0);
        if (f == 208) { audio_set_cart_rom(1); audio_write_command(0x03); audio_write_command(0x03); }
        if (f == 214) audio_write_command(0x02);
        if (f == 675 || f == 678) audio_write_command(0x03);
        if (f == 684) audio_write_command(0x07);
        run_frame();
    }
    recording = true;
    rec_base = audio_frames_out();
    long nframes = (long)(secs * 59.185606);
    for (long f = 0; f < nframes; f++) {
        auto r = sched.equal_range(f);
        cur_frame = f;
        for (auto it = r.first; it != r.second; ++it) audio_write_command((uint8_t)it->second);
        run_frame();
    }
    if (!wav.empty()) write_wav(wav, pcm);
    if (ticks_f) fclose(ticks_f);
    if (ym_f) fclose(ym_f);
    // summary
    double s2 = 0; int peak = 0; size_t last = 0;
    for (size_t i = 0; i < pcm.size(); i++) { int x = abs(pcm[i]); s2 += (double)x * x; if (x > peak) peak = x; if (x > 8) last = i / 2; }
    printf("frames %zu rms %.1f peak %d last_nonsilent_sample %zu (%.3f s)\n", pcm.size() / 2,
           pcm.empty() ? 0 : __builtin_sqrt(s2 / pcm.size()), peak, last, last / (double)AUDIO_RATE);
    return 0;
}

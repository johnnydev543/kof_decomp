// Standalone test for the Neo Geo sound subsystem (src/audio.cpp).
//
//   audio_test                 run the curated command list, write build/audio_test.wav
//   audio_test --scan [a b]    reset + init + send each command a..b (hex), print RMS only
//   audio_test --cmds 21,40,.. run the given hex commands (2.5 s each) and write the WAV
//
// ROMs are loaded from build/roms/ (run from the project root, or pass --roms DIR).

#include "audio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> load(const std::string &path) {
    std::vector<uint8_t> d;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    d.resize((size_t)n);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) { std::fprintf(stderr, "read error %s\n", path.c_str()); std::exit(1); }
    std::fclose(f);
    return d;
}

static std::vector<int16_t> g_wav;     // all output (interleaved stereo)
static bool g_record = true;
static std::vector<uint8_t> g_replies;

static void reply_hook(uint8_t v) { g_replies.push_back(v); }

struct Stats { double rms_l = 0, rms_r = 0; int peak = 0; size_t frames = 0; };

// Run for `seconds` of emulated time using the frontend's per-scanline cadence.
static Stats run_seconds(double seconds) {
    const double lines = seconds * 59.185606 * 264.0;
    static int16_t buf[4096 * 2];
    double sl = 0, sr = 0;
    Stats st;
    for (long i = 0; i < (long)lines; i++) {
        audio_run(256);
        if (audio_samples_available() >= 2048 || i + 1 == (long)lines) {
            int n;
            while ((n = audio_read_samples(buf, 4096)) > 0) {
                for (int k = 0; k < n; k++) {
                    int l = buf[2 * k], r = buf[2 * k + 1];
                    sl += (double)l * l;
                    sr += (double)r * r;
                    if (std::abs(l) > st.peak) st.peak = std::abs(l);
                    if (std::abs(r) > st.peak) st.peak = std::abs(r);
                }
                st.frames += (size_t)n;
                if (g_record) g_wav.insert(g_wav.end(), buf, buf + 2 * n);
            }
        }
    }
    if (st.frames) { st.rms_l = std::sqrt(sl / st.frames); st.rms_r = std::sqrt(sr / st.frames); }
    return st;
}

static void write_wav(const std::string &path, const std::vector<int16_t> &s) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t data = (uint32_t)(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(AUDIO_RATE); u32(AUDIO_RATE * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
}

static std::string replies_str() {
    std::string r;
    char b[8];
    for (size_t i = 0; i < g_replies.size() && i < 12; i++) { std::snprintf(b, sizeof b, "%02X ", g_replies[i]); r += b; }
    if (g_replies.size() > 12) r += "...";
    g_replies.clear();
    return r.empty() ? "-" : r;
}

// Commands sent right after the 2 s driver init. 0x07 clears the driver's
// "music disabled" (FDE1) and "SFX disabled" (FDE2) flags, which the KOF97
// driver sets to 0xFF at reset; without it every 0x20+ command is ignored.
static std::vector<int> g_prelude = {0x07};

static bool g_bios_boot = false;

static void boot(const std::vector<uint8_t> &m1, const std::vector<uint8_t> &sm1, const std::vector<uint8_t> &v) {
    audio_init(m1.data(), m1.size(), sm1.data(), sm1.size(), v.data(), v.size());
    audio_set_reply_hook(reply_hook);
    if (!g_bios_boot) {
        audio_set_cart_rom(1);
        return;
    }
    // Emulate the MVS BIOS hand-over: power on with sm1 mapped, 0x01 = prepare
    // slot switch, map the cart m1, 0x03 = soft reset into the cart driver.
    run_seconds(0.5);
    std::printf("bios: sm1 running, z80 pc %04X\n", audio_z80_pc());
    audio_write_command(0x01);
    run_seconds(0.02);
    std::printf("bios: after 0x01 z80 pc %04X reply %02X\n", audio_z80_pc(), audio_read_reply());
    audio_set_cart_rom(1);
    audio_write_command(0x03);
    run_seconds(0.02);
    std::printf("bios: after 0x03 z80 pc %04X reply %02X\n", audio_z80_pc(), audio_read_reply());
}

static void send_prelude() {
    for (int c : g_prelude) { audio_write_command((uint8_t)c); run_seconds(0.05); }
}

// Send a command sequence ("19:40" = 0x19 then 0x40). Multi-byte commands are
// written ~1 ms apart, the way the 68k game code does it.
static void send_seq(const std::vector<int> &seq) {
    for (size_t k = 0; k < seq.size(); k++) {
        audio_write_command((uint8_t)seq[k]);
        if (k + 1 < seq.size()) run_seconds(0.001);
    }
}

static std::vector<int> parse_seq(const char *p, const char **end) {
    std::vector<int> v;
    for (;;) {
        char *q;
        long c = strtol(p, &q, 16);
        if (q == p) break;
        v.push_back((int)c);
        p = q;
        if (*p != ':') break;
        p++;
    }
    *end = p;
    return v;
}

static std::string seq_str(const std::vector<int> &seq) {
    std::string r;
    char b[8];
    for (size_t k = 0; k < seq.size(); k++) { std::snprintf(b, sizeof b, k ? ":%02X" : "%02X", seq[k]); r += b; }
    return r;
}

int main(int argc, char **argv) {
    std::string roms = "build/roms/";
    std::string out = "build/audio_test.wav";
    bool scan = false;
    int scan_a = 0x02, scan_b = 0xFF, scan_prefix = -1;
    double secs = 2.5;
    std::vector<std::vector<int>> cmds;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--roms") && i + 1 < argc) { roms = argv[++i]; if (roms.back() != '/') roms += '/'; }
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--bios-boot")) g_bios_boot = true;
        else if (!std::strcmp(argv[i], "--secs") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--prefix") && i + 1 < argc) scan_prefix = (int)strtol(argv[++i], 0, 16);
        else if (!std::strcmp(argv[i], "--scan")) {
            scan = true;
            if (i + 2 < argc && argv[i + 1][0] != '-') { scan_a = (int)strtol(argv[++i], 0, 16); scan_b = (int)strtol(argv[++i], 0, 16); }
        } else if (!std::strcmp(argv[i], "--pre") && i + 1 < argc) {
            g_prelude.clear();
            const char *p = argv[++i];
            while (*p) { if (*p == ',') { p++; continue; } char *q; int c = (int)strtol(p, &q, 16); if (q == p) break; g_prelude.push_back(c); p = q; }
        } else if (!std::strcmp(argv[i], "--cmds") && i + 1 < argc) {
            const char *p = argv[++i];
            while (*p) {
                std::vector<int> seq = parse_seq(p, &p);
                if (seq.empty()) break;
                cmds.push_back(seq);
                if (*p == ',') p++;
            }
        } else {
            std::fprintf(stderr, "usage: %s [--roms DIR] [--out WAV] [--pre 07] [--cmds 21,19:40,...] [--secs S] [--bios-boot]\n"
                                 "       %s --scan [FIRST LAST] [--prefix 19]   (hex)\n", argv[0], argv[0]);
            return 2;
        }
    }

    std::vector<uint8_t> m1 = load(roms + "232-m1.bin");
    std::vector<uint8_t> sm1 = load(roms + "ng-sm1.rom");
    std::vector<uint8_t> v = load(roms + "232-v1.bin");
    for (const char *n : {"232-v2.bin", "232-v3.bin"}) { auto x = load(roms + n); v.insert(v.end(), x.begin(), x.end()); }
    std::printf("m1 %zu, sm1 %zu, vrom %zu bytes, rate %d Hz\n", m1.size(), sm1.size(), v.size(), AUDIO_RATE);

    if (scan) {
        // each command gets a fresh driver: init, prelude, command, measure 1.5 s
        g_record = false;
        std::printf("cmd      rmsL    rmsR    peak  replies\n");
        for (int c = scan_a; c <= scan_b; c++) {
            boot(m1, sm1, v);
            run_seconds(0.5);
            send_prelude();
            g_replies.clear();
            std::vector<int> seq;
            if (scan_prefix >= 0) seq.push_back(scan_prefix);
            seq.push_back(c);
            send_seq(seq);
            Stats s = run_seconds(1.5);
            std::printf("%-6s %7.1f %7.1f %6d  %s\n", seq_str(seq).c_str(), s.rms_l, s.rms_r, s.peak, replies_str().c_str());
            std::fflush(stdout);
        }
        audio_shutdown();
        return 0;
    }

    boot(m1, sm1, v);
    Stats s = run_seconds(2.0);
    std::printf("init (2.0 s): frames %zu rms %.1f/%.1f peak %d, z80 pc %04X, replies %s\n",
                s.frames, s.rms_l, s.rms_r, s.peak, audio_z80_pc(), replies_str().c_str());
    send_prelude();
    std::printf("prelude %s sent, replies %s\n", seq_str(g_prelude).c_str(), replies_str().c_str());

    struct Step { std::vector<int> seq; double secs; std::string what; };
    std::vector<Step> steps;
    if (!cmds.empty()) {
        for (auto &c : cmds) steps.push_back({c, secs, ""});
    } else {
        // curated list (see src/AUDIO_NOTES.md), chosen from --scan results
        steps = {
            {{0x21}, 4.0, "music"},
            {{0x04}, 0.5, "stop all + mute"},
            {{0x07}, 0.1, "unmute music+sfx"},
            {{0x2D}, 4.0, "music"},
            {{0x0A, 0x20}, 2.0, "fade out (0A:speed)"},
            {{0x41}, 4.0, "music"},
            {{0x04}, 0.5, "stop all + mute"},
            {{0x07}, 0.1, "unmute music+sfx"},
            {{0x65}, 1.2, "sfx (1-byte, type 5)"},
            {{0x72}, 1.2, "sfx (1-byte, type 5)"},
            {{0x1D, 0x12}, 1.5, "ADPCM-A voice/sfx"},
            {{0x1D, 0x13}, 1.5, "ADPCM-A voice/sfx"},
            {{0x1A, 0x15}, 1.2, "ADPCM-A voice/sfx"},
            {{0x1B, 0x16}, 1.2, "ADPCM-A voice/sfx"},
            {{0x1C, 0x16}, 1.2, "ADPCM-A voice/sfx"},
            {{0x1E, 0x16}, 1.2, "ADPCM-A voice/sfx"},
            {{0x19, 0x68}, 1.5, "ADPCM-A voice/sfx"},
            {{0x22}, 2.0, "music"},
            {{0x1D, 0x13}, 2.0, "voice over music"},
            {{0x0A, 0x08}, 2.5, "slow music fade"},
        };
    }
    for (const Step &st : steps) {
        send_seq(st.seq);
        Stats r = run_seconds(st.secs);
        std::printf("cmd %-6s %-24s %.1fs: rms L %7.1f R %7.1f peak %6d  replies %s\n",
                    seq_str(st.seq).c_str(), st.what.c_str(), st.secs, r.rms_l, r.rms_r, r.peak, replies_str().c_str());
    }
    write_wav(out, g_wav);
    std::printf("wrote %s (%zu frames, %.1f s)\n", out.c_str(), g_wav.size() / 2, g_wav.size() / 2.0 / AUDIO_RATE);
    audio_shutdown();
    return 0;
}

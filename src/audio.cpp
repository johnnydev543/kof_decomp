// Neo Geo MVS sound subsystem: Z80 (superzazu core, patched for 16-bit ports)
// + YM2610 (ymfm). Hardware mapping follows MAME's neogeo.cpp.
// See src/AUDIO_NOTES.md.

#include "audio.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "ymfm_opn.h"

extern "C" {
#include "z80/z80.h"
}

#ifdef AUDIO_TRACE
#include <cstdio>
#define TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) do {} while (0)
#endif

namespace {

constexpr int YM_CLOCKS_PER_SAMPLE = 144;   // OPN_FIDELITY_MIN: one output per 144 input clocks
constexpr int YM_CLOCKS_PER_Z80 = AUDIO_YM_CLOCK / AUDIO_Z80_CLOCK;  // 2
constexpr int RING_FRAMES = 1 << 15;        // 32768 frames = ~0.59 s
constexpr uint64_t NEVER = ~0ull;

// SSG mix level relative to FM/ADPCM (MAME neogeo: SSG 0.28, FM 0.98).
constexpr int SSG_MIX_NUM = 2;   // 2/7 ~= 0.286
constexpr int SSG_MIX_DEN = 7;

struct AudioState;
AudioState *g = nullptr;
void (*g_reply_hook)(uint8_t) = nullptr;   // survives audio_init/shutdown

class YmIntf : public ymfm::ymfm_interface {
public:
    explicit YmIntf(AudioState &s) : st(s) {}
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;
    void ymfm_set_busy_end(uint32_t clocks) override;
    bool ymfm_is_busy() override;
    void ymfm_update_irq(bool asserted) override;
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override;
    ymfm::ymfm_engine_callbacks *engine() { return m_engine; }
private:
    AudioState &st;
};

struct AudioState {
    std::vector<uint8_t> m1, sm1, vrom;
    int use_cart = 0;

    z80 cpu;
    uint32_t bank_off[4];          // byte offset of window 0..3 into the selected ROM
    uint8_t ram[0x800];

    uint8_t cmd_latch = 0;
    uint8_t reply_latch = 0;
    bool nmi_enabled = false;
    bool nmi_pending = false;      // command written and not yet read via IN 0x00
    bool nmi_line = false;
    bool irq_line = false;

    YmIntf intf;
    ymfm::ym2610 ym;

    // timing, in YM input clocks (8 MHz)
    uint64_t target_z80 = 0;       // absolute Z80 cycle target
    uint64_t next_sample = YM_CLOCKS_PER_SAMPLE;
    uint64_t timer_expiry[2] = {NEVER, NEVER};
    uint64_t busy_end = 0;
    uint64_t time_override = NEVER;  // "now" while firing a timer callback

    // output ring buffer (stereo frames)
    int16_t ring[RING_FRAMES * 2];
    unsigned ring_rd = 0, ring_wr = 0;  // free-running frame counters

    AudioState() : intf(*this), ym(intf) {}

    uint64_t now() const {
        return time_override != NEVER ? time_override : (uint64_t)cpu.cyc * YM_CLOCKS_PER_Z80;
    }

    const std::vector<uint8_t> &rom() const { return (use_cart && !m1.empty()) ? m1 : sm1; }

    void update_nmi() {
        bool line = nmi_enabled && nmi_pending;
        if (line && !nmi_line) z80_gen_nmi(&cpu);   // Z80 NMI is edge triggered
        nmi_line = line;
    }

    void push_frame(int16_t l, int16_t r) {
        if (ring_wr - ring_rd >= (unsigned)RING_FRAMES) ring_rd++;  // drop oldest
        unsigned i = (ring_wr % RING_FRAMES) * 2;
        ring[i] = l;
        ring[i + 1] = r;
        ring_wr++;
    }

    void generate_sample() {
        ymfm::ym2610::output_data out;
        ym.generate(&out, 1);
        int ssg = out.data[2] * SSG_MIX_NUM / SSG_MIX_DEN;
        int l = out.data[0] + ssg;
        int r = out.data[1] + ssg;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        push_frame((int16_t)l, (int16_t)r);
    }

    void fire_timers(uint64_t t) {
        for (;;) {
            int tn = -1;
            uint64_t best = NEVER;
            for (int i = 0; i < 2; i++)
                if (timer_expiry[i] <= t && timer_expiry[i] < best) { best = timer_expiry[i]; tn = i; }
            if (tn < 0) break;
            timer_expiry[tn] = NEVER;
            time_override = best;          // re-arm relative to the exact expiry time
            intf.engine()->engine_timer_expired(tn);
            time_override = NEVER;
        }
    }
};

// ---------------------------------------------------------------- ymfm glue

void YmIntf::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    if (tnum > 1) return;
    st.timer_expiry[tnum] = duration_in_clocks < 0 ? NEVER : st.now() + (uint64_t)duration_in_clocks;
}

void YmIntf::ymfm_set_busy_end(uint32_t clocks) { st.busy_end = st.now() + clocks; }

bool YmIntf::ymfm_is_busy() { return st.now() < st.busy_end; }

void YmIntf::ymfm_update_irq(bool asserted) {
    TRACE("[%10lu] YM irq %d\n", (unsigned long)st.cpu.cyc, asserted);
    st.irq_line = asserted;
}

uint8_t YmIntf::ymfm_external_read(ymfm::access_class type, uint32_t address) {
    if (type != ymfm::ACCESS_ADPCM_A && type != ymfm::ACCESS_ADPCM_B) return 0;
    const size_t n = st.vrom.size();
    if (n == 0) return 0;
    return st.vrom[address < n ? address : address % n];
}

// ---------------------------------------------------------------- Z80 bus

uint8_t z80_read(void *, uint16_t addr) {
    AudioState &s = *g;
    const std::vector<uint8_t> &rom = s.rom();
    const size_t n = rom.size();
    if (addr >= 0xF800) return s.ram[addr & 0x7FF];
    if (n == 0) return 0xFF;
    size_t off;
    if (addr < 0x8000)      off = addr;
    else if (addr < 0xC000) off = s.bank_off[3] + (addr & 0x3FFF);
    else if (addr < 0xE000) off = s.bank_off[2] + (addr & 0x1FFF);
    else if (addr < 0xF000) off = s.bank_off[1] + (addr & 0x0FFF);
    else                    off = s.bank_off[0] + (addr & 0x07FF);
    return rom[off % n];
}

void z80_write(void *, uint16_t addr, uint8_t val) {
    if (addr >= 0xF800) g->ram[addr & 0x7FF] = val;
    // ROM writes ignored
}

uint8_t z80_in(z80 *, uint16_t port) {
    AudioState &s = *g;
    switch (port & 0xFF) {
    case 0x00: {                       // command latch; clears pending NMI
        s.nmi_pending = false;
        s.update_nmi();
        return s.cmd_latch;
    }
    case 0x04: case 0x05: case 0x06: case 0x07:
        return s.ym.read(port & 3);
    default:
        break;
    }
    // bank select: 0x08-0x0B mirrored over 0x00F0, bank number = A8-A15
    if ((port & 0x0C) == 0x08) {
        static const uint32_t unit[4] = {0x800, 0x1000, 0x2000, 0x4000};
        int w = port & 3;
        s.bank_off[w] = (uint32_t)(port >> 8) * unit[w];
        TRACE("[%10lu] pc=%04X bank%d = %02X\n", (unsigned long)s.cpu.cyc, s.cpu.pc, w, port >> 8);
        return 0;
    }
    return 0;
}

void z80_out(z80 *, uint16_t port, uint8_t val) {
    AudioState &s = *g;
    switch (port & 0xFF) {
    case 0x00:                         // clear command latch
        s.cmd_latch = 0;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        TRACE("[%10lu] pc=%04X YM w%d %02X\n", (unsigned long)s.cpu.cyc, s.cpu.pc, port & 3, val);
        s.ym.write(port & 3, val);
        break;
    case 0x08:                         // NMI enable
        s.nmi_enabled = true;
        s.update_nmi();
        break;
    case 0x18:                         // NMI disable
        s.nmi_enabled = false;
        s.update_nmi();
        break;
    case 0x0C:                         // reply to 68k
        s.reply_latch = val;
        if (g_reply_hook) g_reply_hook(val);
        break;
    default:
        break;
    }
}

void setup_cpu(AudioState &s) {
    z80_init(&s.cpu);
    s.cpu.read_byte = z80_read;
    s.cpu.write_byte = z80_write;
    s.cpu.port_in = z80_in;
    s.cpu.port_out = z80_out;
    s.cpu.userdata = &s;
}

void do_reset(AudioState &s) {
    unsigned long cyc = s.cpu.cyc;     // keep the time base monotonic
    setup_cpu(s);
    s.cpu.cyc = cyc;
    s.bank_off[0] = 0x1E * 0x800;
    s.bank_off[1] = 0x0E * 0x1000;
    s.bank_off[2] = 0x06 * 0x2000;
    s.bank_off[3] = 0x02 * 0x4000;
    std::memset(s.ram, 0, sizeof(s.ram));
    s.cmd_latch = 0;
    s.reply_latch = 0;
    s.nmi_enabled = false;
    s.nmi_pending = false;
    s.nmi_line = false;
    s.irq_line = false;
    s.timer_expiry[0] = s.timer_expiry[1] = NEVER;
    s.busy_end = 0;
    s.ym.reset();
}

} // namespace

// ---------------------------------------------------------------- public API

extern "C" int audio_init(const uint8_t *m1, size_t m1_size,
                          const uint8_t *sm1, size_t sm1_size,
                          const uint8_t *vrom, size_t vrom_size) {
    if ((!m1 || !m1_size) && (!sm1 || !sm1_size)) return -1;
    audio_shutdown();
    g = new AudioState();
    if (m1 && m1_size) g->m1.assign(m1, m1 + m1_size);
    if (sm1 && sm1_size) g->sm1.assign(sm1, sm1 + sm1_size);
    if (vrom && vrom_size) g->vrom.assign(vrom, vrom + vrom_size);
    if (g->sm1.empty()) g->use_cart = 1;
    g->ym.set_fidelity(ymfm::OPN_FIDELITY_MIN);   // output rate = clock / 144
    g->cpu.cyc = 0;
    do_reset(*g);
    return 0;
}

extern "C" void audio_shutdown(void) {
    delete g;
    g = nullptr;
}

extern "C" void audio_reset(void) {
    if (g) do_reset(*g);
}

extern "C" void audio_set_cart_rom(int use_cart) {
    if (!g) return;
    g->use_cart = (use_cart && !g->m1.empty()) || g->sm1.empty();
}

extern "C" void audio_write_command(uint8_t cmd) {
    if (!g) return;
    g->cmd_latch = cmd;
    g->nmi_pending = true;
    g->update_nmi();
}

extern "C" uint8_t audio_read_reply(void) {
    return g ? g->reply_latch : 0;
}

extern "C" void audio_run(int z80_cycles) {
    if (!g || z80_cycles <= 0) return;
    AudioState &s = *g;
    z80 &cpu = s.cpu;
    s.target_z80 += (uint64_t)z80_cycles;
    // if a previous instruction overshot, the debt is carried in cpu.cyc vs target_z80
    while ((uint64_t)cpu.cyc < s.target_z80) {
        // slice end = min(target, next sample, next timer expiry), in Z80 cycles
        uint64_t end = s.target_z80;
        uint64_t ev = s.next_sample;
        if (s.timer_expiry[0] < ev) ev = s.timer_expiry[0];
        if (s.timer_expiry[1] < ev) ev = s.timer_expiry[1];
        uint64_t ev_z80 = (ev + YM_CLOCKS_PER_Z80 - 1) / YM_CLOCKS_PER_Z80;
        if (ev_z80 < end) end = ev_z80;

        while ((uint64_t)cpu.cyc < end) {
            // HALT with nothing that can wake it: skip to the slice end
            if (cpu.halted && !cpu.nmi_pending && cpu.iff_delay == 0 &&
                !(s.irq_line && cpu.iff1)) {
                cpu.cyc = (unsigned long)end;
                break;
            }
            cpu.int_pending = s.irq_line;   // level-triggered INT (IM1 -> RST 38h)
            cpu.int_data = 0xFF;
            z80_step(&cpu);
        }

        const uint64_t t = (uint64_t)cpu.cyc * YM_CLOCKS_PER_Z80;
        // produce samples and fire timers in time order up to t
        for (;;) {
            uint64_t te = s.timer_expiry[0] < s.timer_expiry[1] ? s.timer_expiry[0] : s.timer_expiry[1];
            if (te <= t && te <= s.next_sample) {
                s.fire_timers(te);
            } else if (s.next_sample <= t) {
                s.generate_sample();
                s.next_sample += YM_CLOCKS_PER_SAMPLE;
            } else {
                break;
            }
        }
    }
}

extern "C" int audio_read_samples(int16_t *stereo_out, int max_frames) {
    if (!g || !stereo_out || max_frames <= 0) return 0;
    AudioState &s = *g;
    int n = 0;
    while (n < max_frames && s.ring_rd != s.ring_wr) {
        unsigned i = (s.ring_rd % RING_FRAMES) * 2;
        stereo_out[n * 2] = s.ring[i];
        stereo_out[n * 2 + 1] = s.ring[i + 1];
        s.ring_rd++;
        n++;
    }
    return n;
}

extern "C" int audio_samples_available(void) {
    return g ? (int)(g->ring_wr - g->ring_rd) : 0;
}

extern "C" int audio_save_state(void *, int) { return 0; }

extern "C" uint16_t audio_z80_pc(void) { return g ? g->cpu.pc : 0; }

extern "C" uint64_t audio_z80_cycles(void) { return g ? (uint64_t)g->cpu.cyc : 0; }

extern "C" void audio_set_reply_hook(void (*hook)(uint8_t)) {
    g_reply_hook = hook;
}

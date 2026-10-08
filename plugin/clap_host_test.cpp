// Minimal CLAP host: loads bin/LowLatConv.clap, drives it through the real CLAP API and checks the audio.
//   build: g++ -std=c++17 clap_host_test.cpp -I<DPF>/distrho/src -ldl -pthread -o clap_host_test
//   run  : ./clap_host_test bin/LowLatConv.clap
#include "clap/entry.h"
#include "clap/plugin-factory.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/audio-ports.h"
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
typedef std::vector<float> V;

// ---- tiny 24-bit mono WAV writer -------------------------------------------------------------------------
static void writeWav(const char* path, const V& h, uint32_t sr) {
    std::vector<uint8_t> d; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (8 * i))); }; auto p16 = [&](uint16_t v) { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); };
    std::vector<uint8_t> pcm; for (float f : h) { int32_t s = int32_t(std::lround(f * 8388607.0)); pcm.push_back(uint8_t(s)); pcm.push_back(uint8_t(s >> 8)); pcm.push_back(uint8_t(s >> 16)); }
    d.insert(d.end(), {'R', 'I', 'F', 'F'}); p32(uint32_t(36 + pcm.size())); d.insert(d.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); p32(16); p16(1); p16(1); p32(sr); p32(sr * 3); p16(3); p16(24);
    d.insert(d.end(), {'d', 'a', 't', 'a'}); p32(uint32_t(pcm.size())); d.insert(d.end(), pcm.begin(), pcm.end());
    FILE* f = std::fopen(path, "wb"); std::fwrite(d.data(), 1, d.size(), f); std::fclose(f);
}

// ---- host ---------------------------------------------------------------------------------------------------
static const void* hostExt(const clap_host_t*, const char*) { return nullptr; }
static void hostNoop(const clap_host_t*) {}
static clap_host_t gHost = {CLAP_VERSION, nullptr, "mini-host", "test", "", "1.0", hostExt, hostNoop, hostNoop, hostNoop};

struct Events { std::vector<clap_event_param_value_t> ev; };
static uint32_t evSize(const clap_input_events_t* l) { return uint32_t(static_cast<Events*>(l->ctx)->ev.size()); }
static const clap_event_header_t* evGet(const clap_input_events_t* l, uint32_t i) { return &static_cast<Events*>(l->ctx)->ev[i].header; }
static bool evPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

struct Stream { std::vector<char> data; size_t pos = 0; };
static int64_t sWrite(const clap_ostream_t* s, const void* b, uint64_t n) { auto* st = static_cast<Stream*>(s->ctx); st->data.insert(st->data.end(), (const char*)b, (const char*)b + n); return int64_t(n); }
static int64_t sRead(const clap_istream_t* s, void* b, uint64_t n) { auto* st = static_cast<Stream*>(s->ctx); const size_t k = std::min<size_t>(size_t(n), st->data.size() - st->pos); std::memcpy(b, st->data.data() + st->pos, k); st->pos += k; return int64_t(k); }

struct Rig {
    const clap_plugin_t* pl = nullptr; const clap_plugin_params_t* params = nullptr; const clap_plugin_state_t* state = nullptr;
    uint32_t inCh = 0, outCh = 0; double sr = 48000.0;

    clap_id findParam(const char* name) const {
        for (uint32_t i = 0; i < params->count(pl); ++i) { clap_param_info_t in; params->get_info(pl, i, &in); if (std::strcmp(in.name, name) == 0) return in.id; }
        return CLAP_INVALID_ID;
    }
    double getParam(const char* name) const { double v = 0; params->get_value(pl, findParam(name), &v); return v; }
    void setParams(const std::vector<std::pair<const char*, double>>& kv) {
        Events e; for (auto& p : kv) { clap_event_param_value_t ev; std::memset(&ev, 0, sizeof ev); ev.header = {sizeof ev, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}; ev.param_id = findParam(p.first); ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.value = p.second; e.ev.push_back(ev); }
        clap_input_events_t in = {&e, evSize, evGet}; clap_output_events_t out = {nullptr, evPush};
        params->flush(pl, &in, &out);
    }
    void loadIR(const std::string& path) {
        Stream st; auto add = [&](const std::string& s) { st.data.insert(st.data.end(), s.begin(), s.end()); st.data.push_back('\0'); };
        add("__dpf_state_begin__"); add("ir_path"); add(path); add("__dpf_state_end__"); st.data.push_back('\xfe');
        clap_istream_t is = {&st, sRead}; CHECK(state->load(pl, &is), "state load");
    }
    std::string savedIR() {
        Stream st; clap_ostream_t os = {&st, sWrite}; state->save(pl, &os);
        const std::string s(st.data.begin(), st.data.end()); const size_t k = s.find(std::string("ir_path\0", 8)); if (k == std::string::npos) return "";
        return std::string(s.c_str() + k + 8);
    }
    bool waitStatus(int want, int ms = 5000) { for (int t = 0; t < ms; t += 20) { if (int(getParam("IR status") + 0.5) == want) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); } return false; }

    // Process L/R (identical channel layouts for in and out); block sizes cycle through `blocks`.
    void run(const V& L, const V& R, V& yL, V& yR, const std::vector<int>& blocks) {
        yL.assign(L.size(), 0.f); yR.assign(L.size(), 0.f); size_t pos = 0, bi = 0;
        Events none; clap_input_events_t ie = {&none, evSize, evGet}; clap_output_events_t oe = {nullptr, evPush};
        while (pos < L.size()) {
            const uint32_t n = uint32_t(std::min<size_t>(size_t(blocks[bi++ % blocks.size()]), L.size() - pos));
            V a(L.begin() + long(pos), L.begin() + long(pos + n)), b(R.begin() + long(pos), R.begin() + long(pos + n)); V oa(n), ob(n);
            float* ic[2] = {a.data(), b.data()}; float* oc[2] = {oa.data(), ob.data()};
            clap_audio_buffer_t in = {}, out = {};
            if (inCh == 2) { in.channel_count = 2; in.data32 = ic; } else { in.channel_count = 1; in.data32 = ic; }
            if (outCh == 2) { out.channel_count = 2; out.data32 = oc; } else { out.channel_count = 1; out.data32 = oc; }
            clap_process_t pr = {}; pr.frames_count = n; pr.audio_inputs = &in; pr.audio_inputs_count = 1; pr.audio_outputs = &out; pr.audio_outputs_count = 1; pr.in_events = &ie; pr.out_events = &oe;
            if (inCh == 1) { /* two mono ports */ }
            pl->process(pl, &pr);
            for (uint32_t i = 0; i < n; ++i) { yL[pos + i] = oa[i]; yR[pos + i] = ob[i]; }
            pos += n;
        }
    }
};

static V noise(size_t n, unsigned seed) { std::mt19937 r(seed); std::uniform_real_distribution<float> d(-0.5f, 0.5f); V v(n); for (auto& x : v) x = d(r); return v; }
static V conv(const V& x, const V& h) { V y(x.size()); for (size_t n = 0; n < x.size(); ++n) { double s = 0; for (size_t k = 0; k < h.size() && k <= n; ++k) s += double(h[k]) * x[n - k]; y[n] = float(s); } return y; }

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "bin/LowLatConv.clap";
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("cannot load %s: %s\n", path, dlerror()); return 2; }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    CHECK(entry && entry->init(path), "clap_entry.init");
    auto* fac = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    CHECK(fac && fac->get_plugin_count(fac) == 1, "plugin factory");
    const clap_plugin_descriptor_t* d = fac->get_plugin_descriptor(fac, 0);
    std::printf("[host] plugin: %s (%s), clap %u.%u\n", d->name, d->id, d->clap_version.major, d->clap_version.minor);

    Rig r; r.pl = fac->create_plugin(fac, &gHost, d->id);
    CHECK(r.pl && r.pl->init(r.pl), "plugin init");
    r.params = static_cast<const clap_plugin_params_t*>(r.pl->get_extension(r.pl, CLAP_EXT_PARAMS));
    r.state = static_cast<const clap_plugin_state_t*>(r.pl->get_extension(r.pl, CLAP_EXT_STATE));
    auto* ports = static_cast<const clap_plugin_audio_ports_t*>(r.pl->get_extension(r.pl, CLAP_EXT_AUDIO_PORTS));
    CHECK(r.params && r.state && ports, "extensions");
    { clap_audio_port_info_t pi; ports->get(r.pl, 0, true, &pi); r.inCh = pi.channel_count; ports->get(r.pl, 0, false, &pi); r.outCh = pi.channel_count;
      std::printf("[host] audio ports: %u in / %u out, input port channels %u, output port channels %u\n", ports->count(r.pl, true), ports->count(r.pl, false), r.inCh, r.outCh); }
    CHECK(ports->count(r.pl, true) == 1 && r.inCh == 2 && r.outCh == 2, "expected one stereo in and one stereo out port");

    const uint32_t np = r.params->count(r.pl);
    std::printf("[host] %u parameters\n", np);
    CHECK(np == 59, "parameter count %u", np);
    { clap_param_info_t in; r.params->get_info(r.pl, r.findParam("Wet") , &in); }
    int automatable = 0, nonAuto = 0; for (uint32_t i = 0; i < np; ++i) { clap_param_info_t in; r.params->get_info(r.pl, i, &in); if (in.flags & CLAP_PARAM_IS_AUTOMATABLE) ++automatable; else ++nonAuto; }
    CHECK(automatable == 28, "automatable parameters %d", automatable);
    std::printf("[host] automatable %d, not automatable %d (IR edit, meter window, outputs)\n", automatable, nonAuto);
    CHECK(r.findParam("IR reverse") != CLAP_INVALID_ID && r.findParam("EQ2 Freq") != CLAP_INVALID_ID && r.findParam("HP Slope") != CLAP_INVALID_ID, "parameter names");

    CHECK(r.pl->activate(r.pl, r.sr, 16, 4096) && r.pl->start_processing(r.pl), "activate / start_processing");

    // 1. no IR: bit-exact bypass
    const size_t N = 60000; V L = noise(N, 1), R = noise(N, 2), yL, yR;
    r.run(L, R, yL, yR, {64});
    CHECK(yL == L && yR == R, "without IR the plugin must be a bit-exact bypass");
    std::printf("[host] bypass without IR: %s\n", (yL == L && yR == R) ? "bit-exact" : "NOT exact");

    // 1b. meters: 1 kHz sine at -20 dBFS peak on both channels, EBU reference -> -20.0 LUFS, RMS -23.01 dB
    { V sn(4 * 48000); for (size_t i = 0; i < sn.size(); ++i) sn[i] = float(0.1 * std::sin(2 * M_PI * 1000.0 * double(i) / 48000.0));
      r.setParams({{"Meter RMS window", 300.0}}); r.run(sn, sn, yL, yR, {64});
      const double pk = r.getParam("IN peak L"), rm = r.getParam("IN RMS R"), lm = r.getParam("OUT LUFS-M"), ls = r.getParam("IN LUFS-S");
      std::printf("[host] meters (sine -20 dBFS): peak %.2f dB, RMS %.2f dB, LUFS-M %.2f, LUFS-S %.2f\n", pk, rm, lm, ls);
      CHECK(std::fabs(pk + 20.0) < 0.1 && std::fabs(rm + 23.01) < 0.1 && std::fabs(lm + 20.0) < 0.2 && std::fabs(ls + 20.0) < 0.2, "meter values");
      clap_param_info_t pi; r.params->get_info(r.pl, r.findParam("IN peak L"), &pi);
      CHECK(pi.flags & CLAP_PARAM_IS_READONLY, "meters must be read-only parameters");   // (the DPF CLAP wrapper does not forward kParameterIsHidden)
      std::printf("[host] meter parameters are %s in the host list\n", (pi.flags & CLAP_PARAM_IS_HIDDEN) ? "hidden" : "visible (read-only)"); }

    // 1c. peak: instant attack, hold, release, exact peak watcher, reset trigger
    { V sp(48000, 0.f); sp[100] = 0.5f; V zz(48000, 0.f);
      CHECK(std::fabs(r.getParam("Meter peak decay") - 24.0) < 1e-6, "default peak decay %g", r.getParam("Meter peak decay"));
      r.setParams({{"Meter peak decay", 40.0}, {"Meter reset", 1.0}}); r.run(zz, zz, yL, yR, {64});
      r.run(sp, zz, yL, yR, {64});                                          // 1 s block train; the spike is 100 samples in
      const double after = r.getParam("IN peak L"), mx = r.getParam("IN peak max L"), mxR = r.getParam("IN peak max R");
      std::printf("[host] 1 s after a -6.02 dBFS spike: peak %.2f dB (envelope released), peak max %.3f dB, R %.0f\n", after, mx, mxR);
      CHECK(std::fabs(mx + 6.0206) < 0.01 && mxR <= -119.9, "peak watcher %.3f / %.1f", mx, mxR);
      CHECK(after < -30.0, "the peak must have fallen at 40 dB/s (%.2f)", after);
      V s2(64, 0.f); s2[5] = 0.25f; r.run(s2, zz, yL, yR, {64});
      CHECK(r.getParam("IN peak L") > -12.5, "a new transient is shown immediately (%.2f)", r.getParam("IN peak L"));
      r.setParams({{"Meter reset", 1.0}}); r.run(zz, zz, yL, yR, {64}); r.run(zz, zz, yL, yR, {64});
      CHECK(r.getParam("IN peak max L") <= -119.9, "reset trigger must clear the peak watcher (%.2f)", r.getParam("IN peak max L"));
      CHECK(r.getParam("Meter reset") == 0.0, "the trigger must fall back to 0 (%g)", r.getParam("Meter reset")); }

    // 2. load an IR through the state, check the result against a direct convolution
    V h(3000); { std::mt19937 g(5); std::uniform_real_distribution<float> u(-1, 1); for (size_t i = 0; i < h.size(); ++i) h[i] = u(g) * std::exp(-4.0f * float(i) / 3000.0f); h[0] = 0.9f; }
    writeWav("/tmp/llc_host_ir.wav", h, 48000);
    V hq(h.size()); for (size_t i = 0; i < h.size(); ++i) hq[i] = float(std::lround(h[i] * 8388607.0)) / 8388607.0f;     // what the WAV really holds
    double e = 0; for (float v : hq) e += double(v) * v; const float g = float(1.0 / std::sqrt(e));                      // Energy normalisation
    V ref(hq); for (auto& v : ref) v *= g;
    r.loadIR("/tmp/llc_host_ir.wav");
    CHECK(r.waitStatus(2), "IR did not load (status %g)", r.getParam("IR status"));
    std::printf("[host] IR loaded: %.1f ms, status %g\n", r.getParam("IR length"), r.getParam("IR status"));
    CHECK(std::fabs(r.getParam("IR length") - 62.5) < 0.5, "IR length %g ms", r.getParam("IR length"));
    CHECK(r.savedIR() == "/tmp/llc_host_ir.wav", "saved state must contain the IR path ('%s')", r.savedIR().c_str());
    r.setParams({{"Dry", -100.0}, {"Wet", 0.0}, {"Input mode", 0.0}});            // Stereo input
    // The worker thread reacts in wall-clock time (20 ms poll + 60 ms debounce) while this host runs faster than real time.
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); r.waitStatus(2);
    r.run(V(6000, 0.f), V(6000, 0.f), yL, yR, {64});                               // lets the 1024-sample crossfade finish
    r.run(L, R, yL, yR, {16, 32, 64, 128, 1, 500});
    V eL = conv(L, ref), eR = conv(R, ref); double err = 0; for (size_t i = 5000; i < N; ++i) err = std::max(err, std::max(std::fabs(double(yL[i]) - eL[i]), std::fabs(double(yR[i]) - eR[i])));
    std::printf("[host] convolution through the CLAP API vs direct computation: max error %.2e\n", err);
    CHECK(err < 2e-4, "convolution error %g", err);

    // 3. automatable parameter: wet -6 dB
    r.setParams({{"Wet", -6.0}}); r.run(V(3000, 0.f), V(3000, 0.f), yL, yR, {64});
    r.run(L, R, yL, yR, {64});
    const double gw = std::pow(10.0, -6.0 / 20.0); err = 0; for (size_t i = 5000; i < N; ++i) err = std::max(err, std::fabs(double(yL[i]) - gw * eL[i]));
    std::printf("[host] wet -6 dB: max error %.2e\n", err);
    CHECK(err < 2e-4, "wet gain error %g", err);

    // 4. a parameter that rebuilds the IR: reverse
    r.setParams({{"Wet", 0.0}, {"IR reverse", 1.0}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); r.waitStatus(2);
    r.run(V(6000, 0.f), V(6000, 0.f), yL, yR, {64});
    V rev(h.size()); for (size_t i = 0; i < h.size(); ++i) rev[i] = hq[h.size() - 1 - i]; double er = 0; for (float v : rev) er += double(v) * v; for (auto& v : rev) v *= float(1.0 / std::sqrt(er));
    r.run(L, R, yL, yR, {64});
    V rL = conv(L, rev); err = 0; for (size_t i = 5000; i < N; ++i) err = std::max(err, std::fabs(double(yL[i]) - rL[i]));
    std::printf("[host] reversed IR rebuilt in the background: max error %.2e\n", err);
    CHECK(err < 2e-4, "reverse error %g", err);

    // 5. bad file, then empty path
    r.loadIR("/tmp/does_not_exist.wav"); CHECK(r.waitStatus(3), "missing file must give status 3 (got %g)", r.getParam("IR status"));
    r.loadIR(""); CHECK(r.waitStatus(0), "empty path must unload (status %g)", r.getParam("IR status"));
    r.run(V(8000, 0.f), V(8000, 0.f), yL, yR, {64}); r.run(L, R, yL, yR, {64});
    CHECK(err < 1 && yL == L, "after unloading the IR the plugin is a bypass again");

    // 6. clean shutdown (worker thread joined)
    r.pl->stop_processing(r.pl); r.pl->deactivate(r.pl);
    const auto t0 = std::chrono::steady_clock::now(); r.pl->destroy(r.pl);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[host] destroy took %.0f ms\n", ms); CHECK(ms < 1000, "destroy too slow");
    entry->deinit(); dlclose(lib);
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL CLAP HOST TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}

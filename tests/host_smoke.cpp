// Black-box validation harness for the Polar Reconstruction CLAP plugin.
//
// Loads the compiled .clap through the plain CLAP C ABI (as a host would),
// then feeds it deterministic audio with random buffer sizes, parameter
// events, host-bypass buffers and reset triggers, and compares the output
// BIT-EXACTLY against a direct instance of the recovered engine that is
// driven in exact 64-frame blocks (the engine's contract).
//
// The plugin's wrapper adds a constant 64-frame latency (pre-filled output
// ring), so plugin_out[i + 64] must equal reference_out[i].

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "params.hpp"
#include "polar_processor.hpp"

using polar_clap::PARAM_COUNT;
using polar_clap::kParams;

// ------------------------------------------------------------------ host

static uint32_t g_flushRequests = 0;

static const void* CLAP_ABI host_params_get_extension(const clap_host_t*, const char* id);
static void CLAP_ABI host_request_restart(const clap_host_t*) {}
static void CLAP_ABI host_request_process(const clap_host_t*) {}
static void CLAP_ABI host_request_callback(const clap_host_t*) {}

static void CLAP_ABI host_params_rescan(const clap_host_t*, clap_param_rescan_flags) {}
static void CLAP_ABI host_params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void CLAP_ABI host_params_request_flush(const clap_host_t*) { ++g_flushRequests; }
static const clap_host_params_t s_host_params = {
    host_params_rescan,
    host_params_clear,
    host_params_request_flush,
};

static const void* CLAP_ABI host_params_get_extension(const clap_host_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_host_params;
    return nullptr;
}

static const clap_host_t kHostTemplate = {
    CLAP_VERSION,               // clap_version
    nullptr,                    // host_data
    "polar-clap-test-host",     // name
    "polar reconstruction test",// vendor
    "",                         // url
    "1.0",                      // version
    host_params_get_extension,  // get_extension
    host_request_restart,       // request_restart
    host_request_process,       // request_process
    host_request_callback,      // request_callback
};

// --------------------------------------------------------- event plumbing

struct InputEvents {
    std::vector<clap_event_header_t*> events;
    static uint32_t CLAP_ABI size(const clap_input_events_t* e) {
        auto* self = static_cast<InputEvents*>(e->ctx);
        return (uint32_t)self->events.size();
    }
    static const clap_event_header_t* CLAP_ABI get(const clap_input_events_t* e, uint32_t i) {
        auto* self = static_cast<InputEvents*>(e->ctx);
        return self->events[i];
    }
    clap_input_events_t iface = {};
    std::vector<clap_event_param_value_t> values;
    std::vector<clap_event_param_mod_t> mods;
    InputEvents() {
        iface.ctx = this;
        iface.size = size;
        iface.get = get;
    }
    void addValue(uint32_t time, clap_id param, double value) {
        clap_event_param_value_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = time;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = param;
        ev.cookie = nullptr;
        ev.value = value;
        values.push_back(ev);
    }
    void addMod(uint32_t time, clap_id param, double amount) {
        clap_event_param_mod_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = time;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_MOD;
        ev.header.flags = 0;
        ev.param_id = param;
        ev.cookie = nullptr;
        ev.amount = amount;
        mods.push_back(ev);
    }
    // Must be called after the last add: builds the header pointer list
    // (vectors must not grow afterwards, or pointers would dangle).
    void finalize() {
        events.clear();
        events.reserve(values.size() + mods.size());
        for (auto& v : values) events.push_back(&v.header);
        for (auto& m : mods) events.push_back(&m.header);
        std::stable_sort(events.begin(), events.end(),
                         [](const clap_event_header_t* a, const clap_event_header_t* b) {
                             return a->time < b->time;
                         });
    }
};

struct OutputEvents {
    std::vector<double> values;
    std::vector<clap_id> ids;
    static bool CLAP_ABI try_push(const clap_output_events_t* e, const clap_event_header_t* h) {
        auto* self = static_cast<OutputEvents*>(e->ctx);
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            self->ids.push_back(ev->param_id);
            self->values.push_back(ev->value);
        }
        return true;
    }
    clap_output_events_t iface = {};
    OutputEvents() {
        iface.ctx = this;
        iface.try_push = try_push;
    }
};

// ------------------------------------------------------------- scenarios

struct Ev {
    uint64_t frame;
    uint32_t param;
    double value;
    bool isMod;
};

struct Scenario {
    const char* name;
    double sampleRate = 48000.0;
    uint64_t frames;
    std::vector<Ev> events;
    std::vector<uint32_t> bufferSizes;
    bool compare = true;                 // bit-exact vs reference engine
    bool expectSilence = false;
};

static uint32_t g_failures = 0;
static void fail(const char* what) {
    ++g_failures;
    printf("  FAIL: %s\n", what);
}

// deterministic xorshift
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 32);
}

static const clap_plugin_entry_t* loadEntry(const char* path, void*& handle) {
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        printf("  dlopen failed: %s\n", dlerror());
        return nullptr;
    }
    // Look up the symbol exactly like a real host (Bitwig, REAPER, ...):
    // the CLAP specification name is "clap_entry". Do NOT fall back to
    // other spellings here — this test must fail if that symbol is gone.
    auto entry = (const clap_plugin_entry_t*)dlsym(handle, "clap_entry");
    if (!entry) {
        printf("  dlsym(clap_entry) failed — binary does not export the "
               "spec-mandated entry symbol; hosts would report "
               "\"Not a plug-in file\"\n");
        return nullptr;
    }
    if (entry->init(nullptr) == false) {
        printf("  entry init failed\n");
        return nullptr;
    }
    return entry;
}

struct PluginHandle {
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_audio_ports_t* ports = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    clap_host_t host;
};

static bool createPlugin(const clap_plugin_entry_t* entry, PluginHandle& out) {
    out.host = kHostTemplate;
    auto factory = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) != 1) return false;
    out.plugin = factory->create_plugin(factory, &out.host, "com.polar-reconstruction.polar");
    if (!out.plugin) return false;
    if (!out.plugin->init(out.plugin)) return false;
    out.params = (const clap_plugin_params_t*)out.plugin->get_extension(out.plugin, CLAP_EXT_PARAMS);
    out.state = (const clap_plugin_state_t*)out.plugin->get_extension(out.plugin, CLAP_EXT_STATE);
    out.ports = (const clap_plugin_audio_ports_t*)out.plugin->get_extension(out.plugin, CLAP_EXT_AUDIO_PORTS);
    out.latency = (const clap_plugin_latency_t*)out.plugin->get_extension(out.plugin, CLAP_EXT_LATENCY);
    out.gui = (const clap_plugin_gui_t*)out.plugin->get_extension(out.plugin, CLAP_EXT_GUI);
    return out.params && out.state && out.ports && out.latency;
}

static void runScenario(const Scenario& sc, const clap_plugin_entry_t* entry) {
    printf("scenario: %s (%.0f Hz, %llu frames)\n", sc.name, sc.sampleRate,
           (unsigned long long)sc.frames);

    // A fresh plugin instance per scenario, exactly like a host adding a
    // new device; the reference engine is also fresh.
    PluginHandle ph;
    if (!createPlugin(entry, ph)) { fail("plugin creation failed"); return; }

    const uint32_t latency = ph.latency->get(ph.plugin);
    if (latency != 64) fail("latency != 64");

    // ---- input signal
    std::vector<float> inL(sc.frames), inR(sc.frames);
    s_rng = 0x1234567 + sc.frames;
    for (uint64_t i = 0; i < sc.frames; ++i) {
        double t = (double)i / sc.sampleRate;
        float v = 0.35f * (float)std::sin(2 * 3.14159265 * (220.0 + 60.0 * std::sin(0.7 * t)) * t);
        v += 0.05f * ((float)(int)(rnd() % 2001) - 1000.f) / 1000.f;
        if ((i % 48000u) < 300u) v += 0.3f; // periodic transient
        inL[i] = v;
        inR[i] = -0.8f * v + 0.1f * (float)std::sin(2 * 3.14159265 * 330.0 * t);
    }
    if (sc.expectSilence) {
        std::fill(inL.begin(), inL.end(), 0.f);
        std::fill(inR.begin(), inR.end(), 0.f);
    }

    if (!ph.plugin->activate(ph.plugin, sc.sampleRate, 1, 4096)) fail("activate failed");
    if (!ph.plugin->start_processing(ph.plugin)) fail("start_processing failed");

    // ---- feed the plugin
    std::vector<float> outL(sc.frames), outR(sc.frames);
    {
        std::vector<Ev> events = sc.events;
        // stable by frame
        for (size_t i = 1; i < events.size(); ++i) {
            Ev key = events[i];
            size_t j = i;
            while (j > 0 && events[j - 1].frame > key.frame) {
                events[j] = events[j - 1];
                --j;
            }
            events[j] = key;
        }

        uint64_t cursor = 0;
        uint32_t bufferIndex = 0;
        size_t nextEvent = 0;
        while (cursor < sc.frames) {
            uint32_t n = sc.bufferSizes[bufferIndex % sc.bufferSizes.size()];
            if (n == 0) n = 64;
            if (cursor + n > sc.frames) n = (uint32_t)(sc.frames - cursor);

            InputEvents in;
            while (nextEvent < events.size() && events[nextEvent].frame < cursor + n) {
                const Ev& e = events[nextEvent];
                uint32_t t = (uint32_t)(e.frame - cursor);
                if (e.isMod) in.addMod(t, e.param, e.value);
                else in.addValue(t, e.param, e.value);
                ++nextEvent;
            }
            in.finalize();

            clap_audio_buffer_t inBuf[1], outBuf[1];
            float* inCh[2] = { inL.data() + cursor, inR.data() + cursor };
            float* outCh[2] = { outL.data() + cursor, outR.data() + cursor };
            inBuf[0].data32 = inCh;
            inBuf[0].channel_count = 2;
            inBuf[0].latency = 0;
            inBuf[0].constant_mask = 0;
            outBuf[0].data32 = outCh;
            outBuf[0].channel_count = 2;
            outBuf[0].latency = 0;
            outBuf[0].constant_mask = 0;

            clap_process_t pr{};
            pr.frames_count = n;
            pr.audio_inputs = inBuf;
            pr.audio_outputs = outBuf;
            pr.audio_inputs_count = 1;
            pr.audio_outputs_count = 1;
            pr.in_events = &in.iface;
            pr.transport = nullptr;

            ph.plugin->process(ph.plugin, &pr);

            cursor += n;
            ++bufferIndex;
        }
        if (nextEvent < events.size()) fail("not all events were delivered");
    }

    ph.plugin->stop_processing(ph.plugin);
    ph.plugin->deactivate(ph.plugin);
    ph.plugin->destroy(ph.plugin);

    // sanity: finite output
    for (uint64_t i = 0; i < sc.frames; ++i) {
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) {
            fail("non-finite output sample");
            break;
        }
    }

    if (!sc.compare) {
        // silence expectation only
        if (sc.expectSilence) {
            for (uint64_t i = 0; i < sc.frames; ++i)
                if (outL[i] != 0.f || outR[i] != 0.f) { fail("silence scenario produced audio"); break; }
        }
        return;
    }

    // ---- reference: direct engine in exact 64-frame blocks
    polar_recovered::PolarProcessor ref((float)sc.sampleRate);
    float base[PARAM_COUNT], mod[PARAM_COUNT];
    for (int i = 0; i < PARAM_COUNT; ++i) { base[i] = kParams[i].def; mod[i] = 0.f; }
    bool resetLatched = false, resetPending = false;

    std::vector<Ev> events = sc.events;
    for (size_t i = 1; i < events.size(); ++i) {
        Ev key = events[i];
        size_t j = i;
        while (j > 0 && events[j - 1].frame > key.frame) { events[j] = events[j - 1]; --j; }
        events[j] = key;
    }

    size_t nextEvent = 0;
    uint64_t blocks = sc.frames / 64;
    std::vector<float> refL(sc.frames), refR(sc.frames);
    double peak = 0;
    for (uint64_t k = 0; k < blocks; ++k) {
        uint64_t start = k * 64, end = start + 64;
        // events with frame in [start, end) apply before this block
        while (nextEvent < events.size() && events[nextEvent].frame < end) {
            const Ev& e = events[nextEvent];
            if (e.isMod) {
                if (kParams[e.param].kind == polar_clap::kFloat)
                    mod[e.param] = (float)e.value;  // raw amount; sum is clamped
                else
                    mod[e.param] = 0.f;
            } else if (kParams[e.param].kind == polar_clap::kTrigger) {
                if (e.value >= 0.5) { if (!resetLatched) { resetPending = true; resetLatched = true; } }
                else resetLatched = false;
                base[e.param] = (float)e.value;
            } else {
                base[e.param] = polar_clap::clampToParam(kParams[e.param], (float)e.value);
            }
            ++nextEvent;
        }
        if (resetPending) { ref.reset(); resetPending = false; }
        polar_clap::applyAllToEngine(base, mod, ref.parameters());
        float oL[64], oR[64];
        ref.processBlock(inL.data() + start, inR.data() + start, oL, oR);
        for (int s = 0; s < 64; ++s) {
            refL[start + s] = oL[s];
            refR[start + s] = oR[s];
            peak = std::fmax(peak, std::fmax(std::fabs((double)oL[s]), std::fabs((double)oR[s])));
        }
    }
    // trailing partial block (engine pads internally with zeros; the wrapper
    // holds it in the carry and emits it only when more audio arrives, so the
    // comparison region stops at the last full block)
    uint64_t comparable = blocks * 64;

    printf("  reference peak: %.6f, comparing %llu frames\n", peak,
           (unsigned long long)comparable);

    // pre-fill: first 64 output frames are zero
    for (uint64_t i = 0; i < 64 && i < sc.frames; ++i)
        if (outL[i] != 0.f || outR[i] != 0.f) { fail("latency pre-fill not silent"); break; }

    for (uint64_t i = 0; i + 64 < sc.frames && i < comparable; ++i) {
        if (outL[i + 64] != refL[i] || outR[i + 64] != refR[i]) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "output mismatch at frame %llu: plugin %.9g/%.9g vs ref %.9g/%.9g",
                     (unsigned long long)i, (double)outL[i + 64], (double)outR[i + 64],
                     (double)refL[i], (double)refR[i]);
            fail(buf);
            break;
        }
    }
}

// ------------------------------------------------------------------ main

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: polar_clap_test <path-to-plugin.clap>\n");
        return 2;
    }
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = loadEntry(argv[1], handle);
    if (!entry) return 2;

    PluginHandle ph;
    if (!createPlugin(entry, ph)) {
        printf("  plugin creation failed\n");
        return 2;
    }
    printf("plugin created, extensions resolved (gui=%s)\n", ph.gui ? "present" : "absent");
#if defined(__APPLE__)
    if (!ph.gui) fail("gui extension missing on macOS");
#else
    if (ph.gui) fail("gui extension should not be present on this platform");
#endif

    // ---- parameter introspection
    if (ph.params->count(ph.plugin) != (uint32_t)PARAM_COUNT) fail("param count mismatch");
    for (uint32_t i = 0; i < (uint32_t)PARAM_COUNT; ++i) {
        clap_param_info_t info{};
        if (!ph.params->get_info(ph.plugin, i, &info)) { fail("get_info failed"); break; }
        if (info.id != i) { fail("param id != index"); break; }
        if (!info.name[0] || !info.module[0]) { fail("param name/module empty"); break; }
        if (info.min_value > info.default_value || info.default_value > info.max_value) {
            fail("default outside range"); break;
        }
    }
    // value_to_text / text_to_value round trip
    for (uint32_t i = 0; i < (uint32_t)PARAM_COUNT; ++i) {
        clap_param_info_t info{};
        ph.params->get_info(ph.plugin, i, &info);
        char text[128];
        double probe = info.default_value;
        if (!ph.params->value_to_text(ph.plugin, i, probe, text, sizeof(text))) {
            fail("value_to_text failed"); break;
        }
        double back = 0;
        if (!ph.params->text_to_value(ph.plugin, i, text, &back)) {
            fail("text_to_value failed"); break;
        }
        double tol = (info.max_value - info.min_value) * 5e-3 + 1e-6;
        if (std::fabs(back - probe) > tol) {
            char buf[200];
            snprintf(buf, sizeof(buf), "round trip drift on '%s': %s -> %g", info.name, text, back);
            fail(buf);
            break;
        }
    }

    // ---- audio ports
    if (ph.ports->count(ph.plugin, true) != 1 || ph.ports->count(ph.plugin, false) != 1)
        fail("audio port counts");
    clap_audio_port_info_t pinfo{};
    ph.ports->get(ph.plugin, 0, true, &pinfo);
    if (pinfo.channel_count != 2) fail("input not stereo");
    ph.ports->get(ph.plugin, 0, false, &pinfo);
    if (pinfo.channel_count != 2) fail("output not stereo");

    // ---- scenarios
    Scenario silence{"silence", 48000, 48000, {}, {64, 128, 256}, true, true};
    runScenario(silence, entry);

    Scenario defaults{"defaults", 48000, 96000, {},
                      {64, 128, 1, 63, 65, 127, 256, 441, 512, 3, 100}};
    runScenario(defaults, entry);

    Scenario events{"events", 48000, 192000,
        {
            {5000, polar_clap::P_DRY, 1.0, false},
            {20000, polar_clap::P_PITCH_1, 7.0, false},
            {40000, polar_clap::P_ENABLE_2, 1.0, false},
            {41000, polar_clap::P_PITCH_2, -5.0, false},
            {60000, polar_clap::P_STEREO, 1.0, false},
            {70000, polar_clap::P_ALGORITHM, 0.0, false},
            {90000, polar_clap::P_SPEED, 0.0, false},
            {90001, polar_clap::P_OUTPUT_DB, -12.0, false},
            {100000, polar_clap::P_ALGORITHM, 1.0, false},
            {120000, polar_clap::P_SPEED, 2.0, false},
            {130000, polar_clap::P_LOCK, 1.0, false},
            {150000, polar_clap::P_LOCK, 0.0, false},
            {160000, polar_clap::P_ALGORITHM, 3.0, false},
            {170000, polar_clap::P_FILTER_ON, 1.0, false},
            {171000, polar_clap::P_FILTER_MODE, 3.0, false},
            {175000, polar_clap::P_GATE, 1.0, false},
            {180000, polar_clap::P_RESET, 1.0, false},
            {181000, polar_clap::P_RESET, 0.0, false},
            {183000, polar_clap::P_PITCH_1, 4.0, true},   // mod +4 st
            {185000, polar_clap::P_CUTOFF, -0.2, true},   // mod -20 %
            {186000, polar_clap::P_PAN_1, 0.25, true},
            {165000, polar_clap::P_BYPASS, 1.0, false},   // CLAP bypass param
            {168000, polar_clap::P_BYPASS, 0.0, false},
        },
        {128, 64, 100, 7, 256, 63, 65, 441, 1, 512, 3, 90}};
    runScenario(events, entry);

    // ---- randomized soak with full comparison
    {
        Scenario soak{"soak-30s", 48000, 30ull * 48000ull, {}, {}};
        s_rng = 0xDEADBEEF;
        for (uint64_t f = 1000; f < soak.frames; f += 300 + (rnd() % 3000)) {
            uint32_t p = rnd() % PARAM_COUNT;
            const auto& d = kParams[p];
            if (d.kind == polar_clap::kTrigger) {
                soak.events.push_back({f, p, 1.0, false});
                soak.events.push_back({f + 50, p, 0.0, false});
                continue;
            }
            double v = d.min + (d.max - d.min) * ((double)(rnd() % 100001) / 100000.0);
            bool asMod = (d.kind == polar_clap::kFloat) && ((rnd() & 3) == 0);
            soak.events.push_back({f, p, v, asMod});
        }
        for (int i = 0; i < 512; ++i) soak.bufferSizes.push_back(1 + (rnd() % 700));
        runScenario(soak, entry);
    }

    // ---- state round trip
    {
        printf("scenario: state round-trip\n");
        PluginHandle s;
        if (!createPlugin(entry, s)) fail("create for state test");
        // set params while deactivated through params->flush
        InputEvents in;
        in.addValue(0, polar_clap::P_PITCH_1, 5.0);
        in.addValue(0, polar_clap::P_LEVEL_1, 0.33);
        in.addValue(0, polar_clap::P_ALGORITHM, 1.0);
        in.finalize();
        s.params->flush(s.plugin, &in.iface, nullptr);

        struct MemStream {
            std::vector<char> data;
            size_t pos = 0;
            static int64_t CLAP_ABI read(const clap_istream_t* s, void* buf, uint64_t n) {
                auto* m = static_cast<MemStream*>(s->ctx);
                uint64_t avail = m->data.size() - m->pos;
                uint64_t take = n < avail ? n : avail;
                memcpy(buf, m->data.data() + m->pos, take);
                m->pos += take;
                return (int64_t)take;
            }
            static int64_t CLAP_ABI write(const clap_ostream_t* s, const void* buf, uint64_t n) {
                auto* m = static_cast<MemStream*>(s->ctx);
                const char* p = static_cast<const char*>(buf);
                m->data.insert(m->data.end(), p, p + n);
                return (int64_t)n;
            }
            clap_istream_t in{};
            clap_ostream_t out{};
            MemStream() { in.ctx = this; in.read = read; out.ctx = this; out.write = write; }
        } ms;
        if (!s.state->save(s.plugin, &ms.out)) fail("state save failed");
        if (ms.data.size() < 100) fail("state chunk suspiciously small");

        // scramble
        InputEvents junk;
        junk.addValue(0, polar_clap::P_PITCH_1, -20.0);
        junk.addValue(0, polar_clap::P_LEVEL_1, 0.05);
        junk.addValue(0, polar_clap::P_ALGORITHM, 3.0);
        junk.finalize();
        s.params->flush(s.plugin, &junk.iface, nullptr);

        if (!s.state->load(s.plugin, &ms.in)) fail("state load failed");
        double v = 0;
        s.params->get_value(s.plugin, polar_clap::P_PITCH_1, &v);
        if (std::fabs(v - 5.0) > 1e-6) fail("pitch_1 not restored");
        s.params->get_value(s.plugin, polar_clap::P_LEVEL_1, &v);
        if (std::fabs(v - 0.33) > 1e-6) fail("level_1 not restored");
        s.params->get_value(s.plugin, polar_clap::P_ALGORITHM, &v);
        if (std::fabs(v - 1.0) > 1e-6) fail("algorithm not restored");
        s.plugin->destroy(s.plugin);
    }

    // ---- sample rate change sanity (no reference compare)
    {
        Scenario sr{"sample-rate-44100", 44100, 44100, {}, {128, 100, 64, 7}};
        sr.compare = false;
        sr.expectSilence = false;
        runScenario(sr, entry);
    }

    ph.plugin->destroy(ph.plugin);
    dlclose(handle);

    if (g_failures) {
        printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}

// White-box tests: exercises the C++ layer directly.
//  - member-pointer parameter table must match the engine's public
//    set(name, value) string API (bit-exact audio after identical input)
//  - GUI edits reach the host as gesture begin/value/end from flush() and process()
//  - host modulation routing (post-smoother offsets, stepped rounding, clamping)
//  - host transport tempo drives the LFO
//  - text formatting / parsing special cases

#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "plugin.hpp"
#include "params.hpp"
#include "polar_processor.hpp"

using polar_clap::PolarPlugin;
using polar_clap::kParams;
using polar_clap::PARAM_COUNT;

static uint32_t g_failures = 0;
static void fail(const char* what) {
    ++g_failures;
    printf("  FAIL: %s\n", what);
}

// ---------------------------------------------------------------- mock host

static const void* CLAP_ABI host_get_extension(const clap_host_t*, const char*) { return nullptr; }
static void CLAP_ABI host_request_restart(const clap_host_t*) {}
static void CLAP_ABI host_request_process(const clap_host_t*) {}
static void CLAP_ABI host_request_callback(const clap_host_t*) {}

static const clap_host_t kHost = {
    CLAP_VERSION, nullptr, "whitebox", "test", "", "1.0",
    host_get_extension, host_request_restart, host_request_process, host_request_callback,
};

struct OutEvents {
    std::vector<clap_id> ids;
    std::vector<double> values;
    std::vector<uint16_t> types;
    bool wildcard = true;
    static bool CLAP_ABI try_push(const clap_output_events_t* e, const clap_event_header_t* h) {
        auto* self = static_cast<OutEvents*>(e->ctx);
        self->types.push_back(h->type);
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            self->ids.push_back(ev->param_id);
            self->values.push_back(ev->value);
            self->wildcard = self->wildcard && ev->note_id == -1 && ev->port_index == -1 && ev->channel == -1 && ev->key == -1;
        }
        return true;
    }
    clap_output_events_t iface{};
    OutEvents() { iface.ctx = this; iface.try_push = try_push; }
};

// ------------------------------------------------------------------- main

int main() {
    // ---- 1. table vs string API equivalence
    {
        polar_recovered::PolarProcessor viaTable(48000);
        polar_recovered::PolarProcessor viaString(48000);

        float base[PARAM_COUNT], mod[PARAM_COUNT];
        for (int i = 0; i < PARAM_COUNT; ++i) { base[i] = kParams[i].def; mod[i] = 0.f; }

        // a sweep of interesting values, including every stepped choice
        for (int pass = 0; pass < 4; ++pass) {
            for (int i = 0; i < PARAM_COUNT; ++i) {
                const auto& d = kParams[i];
                if (d.kind == polar_clap::kTrigger) continue;
                float t = (float)(pass + 1) / 5.f;
                base[i] = d.min + (d.max - d.min) * t;
                if (d.kind != polar_clap::kFloat)
                    base[i] = std::floor(base[i] + 0.5f);
            }
            polar_clap::applyAllToEngine(base, mod, viaTable.parameters());
            for (int i = 0; i < PARAM_COUNT; ++i) {
                const auto& d = kParams[i];
                if (d.kind == polar_clap::kTrigger) continue;
                if (!viaString.set(d.engine_name, base[i])) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "engine set() rejected '%s'", d.engine_name);
                    fail(buf);
                }
            }
            // run audio through both and compare
            for (int blk = 0; blk < 200; blk++) {
                float a[64], b[64], o1[64], o2[64], q1[64], q2[64];
                for (int s = 0; s < 64; ++s) {
                    double t = (blk * 64 + s) / 48000.0;
                    a[s] = 0.4f * (float)std::sin(2 * 3.14159265 * 300.0 * t) + 0.05f * (float)(s % 7 - 3) / 3.f;
                    b[s] = -0.6f * a[s];
                }
                viaTable.processBlock(a, b, o1, q1);
                viaString.processBlock(a, b, o2, q2);
                for (int s = 0; s < 64; ++s) {
                    if (o1[s] != o2[s] || q1[s] != q2[s]) {
                        fail("table apply diverges from set() API (audio mismatch)");
                        blk = 1000;
                        break;
                    }
                }
            }
        }
        printf("table-vs-set(): ok\n");
    }

    // ---- 2. GUI gestures reach the host from flush() and from process()
    {
        clap_plugin_t* cp = PolarPlugin::create(&kHost);
        if (!cp) { fail("plugin create"); return 1; }
        PolarPlugin* pl = PolarPlugin::fromClap(cp);
        cp->init(cp);
        pl->uiBeginGesture(polar_clap::P_PITCH_1);
        pl->uiSetValue(polar_clap::P_PITCH_1, 7.0f);
        pl->uiEndGesture(polar_clap::P_PITCH_1);
        pl->uiTap(polar_clap::P_ALGORITHM, 1.0f);

        OutEvents out;
        pl->flushParams(nullptr, &out.iface);
        const uint16_t want[] = {CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_PARAM_GESTURE_END,
                                 CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_PARAM_GESTURE_END};
        if (out.types.size() != 6) fail("flush did not emit begin/value/end for both edits");
        for (size_t i = 0; i < out.types.size() && i < 6; ++i) if (out.types[i] != want[i]) fail("gesture event order");
        if (out.ids.size() != 2 || out.ids[0] != (clap_id)polar_clap::P_PITCH_1 || out.values[0] != 7.0 ||
            out.ids[1] != (clap_id)polar_clap::P_ALGORITHM || out.values[1] != 1.0) fail("value events wrong");
        if (!out.wildcard) fail("value events must be global (note/port/channel/key = -1)");
        OutEvents out2;
        pl->flushParams(nullptr, &out2.iface);
        if (!out2.types.empty()) fail("second flush emitted spurious events");

        // While the host runs process(), GUI edits leave through process()'s output queue.
        cp->activate(cp, 48000, 1, 512);
        pl->uiSetValue(polar_clap::P_LEVEL_1, 0.5f);
        float zeros[256] = {}, oL[256], oR[256];
        float* inCh[2] = {zeros, zeros}; float* outCh[2] = {oL, oR};
        clap_audio_buffer_t inBuf{}, outBuf{};
        inBuf.data32 = inCh; inBuf.channel_count = 2; outBuf.data32 = outCh; outBuf.channel_count = 2;
        clap_input_events_t noEvents{}; noEvents.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
        noEvents.get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; };
        OutEvents out3;
        clap_process_t pr{}; pr.frames_count = 256; pr.audio_inputs = &inBuf; pr.audio_inputs_count = 1;
        pr.audio_outputs = &outBuf; pr.audio_outputs_count = 1; pr.in_events = &noEvents; pr.out_events = &out3.iface;
        pl->process(&pr);
        if (out3.ids.size() != 1 || out3.ids[0] != (clap_id)polar_clap::P_LEVEL_1) fail("process() did not report the GUI edit");

        // clamping: garbage UI value must clamp, not store
        pl->uiSetValue(polar_clap::P_PITCH_1, 999.f);
        if (std::fabs(pl->uiBase(polar_clap::P_PITCH_1) - 36.f) > 1e-6) fail("uiSetValue clamp");

        // ---- host transport: tempo reaches the LFO
        clap_event_transport_t tr{};
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
        tr.tempo = 137.5; tr.song_pos_beats = 3 * CLAP_BEATTIME_FACTOR;
        pr.transport = &tr;
        pl->uiSetValue(polar_clap::P_LFO_SYNC, 1.f);
        pl->process(&pr);
        if (pl->processor().parameters().tempo != 137.5f || pl->uiTempo() != 137.5f) fail("host tempo not used by the LFO");
        cp->deactivate(cp);
        cp->destroy(cp);
        printf("gui gestures / process events / transport: ok\n");
    }

    // ---- 2b. host modulation routing
    {
        float base[PARAM_COUNT], mod[PARAM_COUNT];
        for (int i = 0; i < PARAM_COUNT; ++i) { base[i] = kParams[i].def; mod[i] = 0.f; }
        polar_recovered::PolarEngine::Parameters p;
        base[polar_clap::P_PITCH_1] = 30; mod[polar_clap::P_PITCH_1] = 20;   // clamps at +36: offset 6
        mod[polar_clap::P_TUNE_1] = 50;                                      // +0.5 st
        mod[polar_clap::P_POSITION] = 0.25f; mod[polar_clap::P_CUTOFF] = -0.2f; mod[polar_clap::P_RESONANCE] = 0.1f;
        mod[polar_clap::P_ALGORITHM] = 1.4f;                                 // stepped: 2 + 1.4 -> 3
        mod[polar_clap::P_LOCK] = 0.6f;                                      // bool: 0 + 0.6 -> on
        mod[polar_clap::P_LEVEL_1] = -0.3f;                                  // ordinary: base + mod
        polar_clap::applyAllToEngine(base, mod, p);
        if (p.pitch_1 != 30 || p.pitch_mod_1 != 6.5f) fail("pitch modulation must bypass the smoother, clamped to range");
        if (p.position != kParams[polar_clap::P_POSITION].def || p.position_mod != 0.25f) fail("position modulation routing");
        if (std::fabs(p.cutoff_mod + 0.2f) > 1e-6f || std::fabs(p.resonance_mod - 0.1f) > 1e-6f) fail("filter modulation routing");
        if (p.algorithm != 3 || !p.lock) fail("stepped/bool modulation");
        if (std::fabs(p.level_1 - 0.5f) > 1e-6f) fail("level modulation");
        for (uint32_t i = 0; i < (uint32_t)PARAM_COUNT; ++i)
            if (polar_clap::isModulatable(i) == (i == polar_clap::P_RESET || i == polar_clap::P_BYPASS)) fail("modulatable flag policy");
        // With zero modulation the engine sees exactly the base values (verified signal path unchanged).
        for (int i = 0; i < PARAM_COUNT; ++i) mod[i] = 0.f;
        polar_clap::applyAllToEngine(base, mod, p);
        if (p.pitch_mod_1 != 0 || p.pitch_mod_2 != 0 || p.position_mod != 0 || p.cutoff_mod != 0 || p.resonance_mod != 0)
            fail("zero modulation must leave post-smoother offsets at zero");
        printf("modulation routing: ok\n");
    }

    // ---- 3. text formatting special cases
    {
        struct Case { uint32_t param; double value; const char* text; } cases[] = {
            {polar_clap::P_ALGORITHM, 0.0, "Smooth"},
            {polar_clap::P_ALGORITHM, 3.0, "Rev Loop"},
            {polar_clap::P_FILTER_MODE, 2.0, "HP"},
            {polar_clap::P_LFO_WAVE, 1.0, "Triangle"},
            {polar_clap::P_SPEED, 1.0, "Medium"},
            {polar_clap::P_LOCK, 1.0, "On"},
            {polar_clap::P_LOCK, 0.0, "Off"},
            {polar_clap::P_PAN_1, -0.35, "L 35 %"},
            {polar_clap::P_PAN_2, 0.5, "R 50 %"},
            {polar_clap::P_PAN_1, 0.0, "C"},
            {polar_clap::P_CUTOFF, 0.75, "75 %"},
            {polar_clap::P_OUTPUT_DB, -6.0, "-6.0 dB"},
            {polar_clap::P_PITCH_1, 7.0, "+7.0 st"},
            {polar_clap::P_TUNE_1, -50.0, "-50 ct"},
        };
        for (const auto& c : cases) {
            char buf[64];
            polar_clap::formatValue(kParams[c.param], c.value, buf, sizeof(buf));
            if (strcmp(buf, c.text) != 0) {
                char msg[160];
                snprintf(msg, sizeof(msg), "format %s -> '%s' (want '%s')",
                         kParams[c.param].engine_name, buf, c.text);
                fail(msg);
            }
        }
        // parsing
        struct PCase { uint32_t param; const char* text; double want; } pcases[] = {
            {polar_clap::P_PAN_1, "L 35 %", -0.35},
            {polar_clap::P_PAN_1, "R 20", 0.2},
            {polar_clap::P_CUTOFF, "75 %", 0.75},
            {polar_clap::P_CUTOFF, "0.8", 0.8},
            {polar_clap::P_ALGORITHM, "Rev Loop", 3.0},
            {polar_clap::P_ALGORITHM, "2", 2.0},
            {polar_clap::P_LOCK, "on", 1.0},
            {polar_clap::P_PITCH_1, "+7.0 st", 7.0},
        };
        for (const auto& c : pcases) {
            double v = 0;
            if (!polar_clap::parseValue(kParams[c.param], c.text, &v)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "parse failed '%s' for %s",
                         c.text, kParams[c.param].engine_name);
                fail(msg);
                continue;
            }
            if (std::fabs(v - c.want) > 1e-6) {
                char msg[160];
                snprintf(msg, sizeof(msg), "parse '%s' -> %g (want %g)", c.text, v, c.want);
                fail(msg);
            }
        }
        // out-of-range parse must clamp
        double v = 0;
        polar_clap::parseValue(kParams[polar_clap::P_PITCH_1], "999", &v);
        if (std::fabs(v - 36.0) > 1e-6) fail("parse clamp");
        printf("text formats: ok\n");
    }

    if (g_failures) {
        printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("\nWHITEBOX TESTS PASSED\n");
    return 0;
}

// White-box tests: exercises the C++ layer directly.
//  - member-pointer parameter table must match the engine's public
//    set(name, value) string API (bit-exact audio after identical input)
//  - GUI value path: uiSetValue marks dirty, params_flush emits values
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
    static bool CLAP_ABI try_push(const clap_output_events_t* e, const clap_event_header_t* h) {
        auto* self = static_cast<OutEvents*>(e->ctx);
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            self->ids.push_back(ev->param_id);
            self->values.push_back(ev->value);
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

    // ---- 2. uiSetValue / params_flush
    {
        clap_plugin_t* cp = PolarPlugin::create(&kHost);
        if (!cp) { fail("plugin create"); return 1; }
        PolarPlugin* pl = PolarPlugin::fromClap(cp);
        pl->uiSetValue(polar_clap::P_PITCH_1, 7.0f);
        pl->uiSetValue(polar_clap::P_LEVEL_1, 0.5f);

        OutEvents out;
        pl->flushParams(nullptr, &out.iface);
        if (out.ids.size() != 2) fail("flush did not emit exactly the two dirty params");
        bool foundPitch = false, foundLevel = false;
        for (size_t i = 0; i < out.ids.size(); ++i) {
            if (out.ids[i] == (clap_id)polar_clap::P_PITCH_1 && std::fabs(out.values[i] - 7.0) < 1e-9) foundPitch = true;
            if (out.ids[i] == (clap_id)polar_clap::P_LEVEL_1 && std::fabs(out.values[i] - 0.5) < 1e-9) foundLevel = true;
        }
        if (!foundPitch) fail("pitch_1 value event wrong/missing");
        if (!foundLevel) fail("level_1 value event wrong/missing");
        // second flush must be clean
        OutEvents out2;
        pl->flushParams(nullptr, &out2.iface);
        if (!out2.ids.empty()) fail("second flush emitted spurious events");

        // clamping: garbage UI value must clamp, not store
        pl->uiSetValue(polar_clap::P_PITCH_1, 999.f);
        if (std::fabs(pl->uiBase(polar_clap::P_PITCH_1) - 36.f) > 1e-6) fail("uiSetValue clamp");

        cp->destroy(cp);
        printf("ui/flush: ok\n");
    }

    // ---- 3. text formatting special cases
    {
        struct Case { uint32_t param; double value; const char* text; } cases[] = {
            {polar_clap::P_ALGORITHM, 0.0, "Spectral"},
            {polar_clap::P_ALGORITHM, 3.0, "Reverse"},
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
            {polar_clap::P_ALGORITHM, "Reverse", 3.0},
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

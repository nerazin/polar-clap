#include "params.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace polar_clap {

static const char* kAlgoEnums[] = {"Spectral", "Periodic", "Forward", "Reverse", nullptr};
static const char* kSpeedEnums[] = {"Slow", "Medium", "Fast", nullptr};
static const char* kFilterEnums[] = {"LP", "BP", "HP", "Comb", nullptr};
static const char* kWaveEnums[] = {"Sine", "Triangle", "Saw", "Square", nullptr};

// Order, ranges and defaults follow the recovered engine parameter struct and
// the published control table (Polar reconstruction 0.2).
const ParamDesc kParams[PARAM_COUNT] = {
    // --- Algorithm / capture ---
    {"algorithm", "Algorithm", "Algorithm", "Algorithm", 0, 3, 2, kInt, U_NONE, kAlgoEnums, nullptr, &EngineParams::algorithm, nullptr},
    {"speed", "FFT Speed", "Speed", "Algorithm", 0, 2, 1, kInt, U_NONE, kSpeedEnums, nullptr, &EngineParams::speed, nullptr},
    {"reverse", "Force Reverse", "Reverse", "Algorithm", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::reverse},
    {"stereo", "Stereo", "Stereo", "Algorithm", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::stereo},
    {"lock", "Lock Capture", "Lock", "Algorithm", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::lock},
    // --- Grain source ---
    {"position", "Position", "Position", "Grain", 0.0f, 1.7f, 0.20f, kFloat, U_SEC, nullptr, &EngineParams::position, nullptr, nullptr},
    {"loop", "Loop Length", "Loop", "Grain", 0.0f, 1.0f, 0.60f, kFloat, U_NONE, nullptr, &EngineParams::loop, nullptr, nullptr},
    {"position_lfo", "Position LFO", "Pos LFO", "Grain", -0.5f, 0.5f, 0, kFloat, U_SEC, nullptr, &EngineParams::position_lfo, nullptr, nullptr},
    {"position_env", "Position Env", "Pos Env", "Grain", -0.5f, 0.5f, 0, kFloat, U_SEC, nullptr, &EngineParams::position_env, nullptr, nullptr},
    // --- Voice 1 ---
    {"pitch_1", "Pitch 1", "Pitch", "Voice 1", -36, 36, 0, kFloat, U_SEMI, nullptr, &EngineParams::pitch_1, nullptr, nullptr},
    {"tune_1", "Tune 1", "Tune", "Voice 1", -100, 100, 0, kFloat, U_CENT, nullptr, &EngineParams::tune_1, nullptr, nullptr},
    {"level_1", "Level 1", "Level", "Voice 1", 0, 1, 0.8f, kFloat, U_PERCENT, nullptr, &EngineParams::level_1, nullptr, nullptr},
    {"pan_1", "Pan 1", "Pan", "Voice 1", -1, 1, -0.35f, kFloat, U_PAN, nullptr, &EngineParams::pan_1, nullptr, nullptr},
    {"feedback_1", "Feedback 1", "Feedbk", "Voice 1", 0, 0.99f, 0, kFloat, U_PERCENT, nullptr, &EngineParams::feedback_1, nullptr, nullptr},
    {"pitch_lfo_1", "Pitch LFO 1", "Pt LFO", "Voice 1", -24, 24, 0, kFloat, U_SEMI, nullptr, &EngineParams::pitch_lfo_1, nullptr, nullptr},
    {"pitch_env_1", "Pitch Env 1", "Pt Env", "Voice 1", -24, 24, 0, kFloat, U_SEMI, nullptr, &EngineParams::pitch_env_1, nullptr, nullptr},
    // --- Voice 2 ---
    {"enable_2", "Enable Voice 2", "Enable", "Voice 2", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::enable_2},
    {"pitch_2", "Pitch 2", "Pitch", "Voice 2", -36, 36, 7, kFloat, U_SEMI, nullptr, &EngineParams::pitch_2, nullptr, nullptr},
    {"tune_2", "Tune 2", "Tune", "Voice 2", -100, 100, 0, kFloat, U_CENT, nullptr, &EngineParams::tune_2, nullptr, nullptr},
    {"level_2", "Level 2", "Level", "Voice 2", 0, 1, 0.6f, kFloat, U_PERCENT, nullptr, &EngineParams::level_2, nullptr, nullptr},
    {"pan_2", "Pan 2", "Pan", "Voice 2", -1, 1, 0.35f, kFloat, U_PAN, nullptr, &EngineParams::pan_2, nullptr, nullptr},
    {"feedback_2", "Feedback 2", "Feedbk", "Voice 2", 0, 0.99f, 0, kFloat, U_PERCENT, nullptr, &EngineParams::feedback_2, nullptr, nullptr},
    {"delay_2", "Delay 2", "Delay", "Voice 2", 0, 1000, 0, kFloat, U_MS, nullptr, &EngineParams::delay_2, nullptr, nullptr},
    {"pitch_lfo_2", "Pitch LFO 2", "Pt LFO", "Voice 2", -24, 24, 0, kFloat, U_SEMI, nullptr, &EngineParams::pitch_lfo_2, nullptr, nullptr},
    {"pitch_env_2", "Pitch Env 2", "Pt Env", "Voice 2", -24, 24, 0, kFloat, U_SEMI, nullptr, &EngineParams::pitch_env_2, nullptr, nullptr},
    // --- Filter ---
    {"filter_on", "Filter On", "On", "Filter", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::filter_on},
    {"filter_mode", "Filter Mode", "Mode", "Filter", 0, 3, 0, kInt, U_NONE, kFilterEnums, nullptr, &EngineParams::filter_mode, nullptr},
    {"cutoff", "Cutoff", "Cutoff", "Filter", 0, 1, 0.75f, kFloat, U_PERCENT, nullptr, &EngineParams::cutoff, nullptr, nullptr},
    {"resonance", "Resonance", "Reso", "Filter", 0, 1, 0.2f, kFloat, U_PERCENT, nullptr, &EngineParams::resonance, nullptr, nullptr},
    {"filter_lfo", "Filter LFO", "Flt LFO", "Filter", -1, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::filter_lfo, nullptr, nullptr},
    {"filter_env", "Filter Env", "Flt Env", "Filter", -1, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::filter_env, nullptr, nullptr},
    // --- LFO ---
    {"lfo_rate", "LFO Rate", "Rate", "LFO", 0.02f, 20, 0.30f, kFloat, U_HZ, nullptr, &EngineParams::lfo_rate, nullptr, nullptr},
    {"lfo_sync", "LFO Sync", "Sync", "LFO", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::lfo_sync},
    {"tempo", "Tempo", "Tempo", "LFO", 40, 250, 120, kFloat, U_BPM, nullptr, &EngineParams::tempo, nullptr, nullptr},
    {"division", "LFO Division", "Div", "LFO", 0.25f, 8, 1, kFloat, U_NONE, nullptr, &EngineParams::division, nullptr, nullptr},
    {"lfo_wave", "LFO Wave", "Wave", "LFO", 0, 3, 0, kInt, U_NONE, kWaveEnums, nullptr, &EngineParams::lfo_wave, nullptr},
    {"lfo_phase", "LFO Phase", "Phase", "LFO", 0, 1, 0, kFloat, U_PERCENT, nullptr, &EngineParams::lfo_phase, nullptr, nullptr},
    {"pan_lfo", "Pan LFO Depth", "Pan LFO", "LFO", 0, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::pan_lfo, nullptr, nullptr},
    // --- Envelope ---
    {"gate", "Gate", "Gate", "Envelope", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::gate},
    {"attack", "Attack", "Attack", "Envelope", 0, 2, 0.02f, kFloat, U_SEC, nullptr, &EngineParams::attack, nullptr, nullptr},
    {"decay", "Decay", "Decay", "Envelope", 0, 2, 0.2f, kFloat, U_SEC, nullptr, &EngineParams::decay, nullptr, nullptr},
    {"sustain", "Sustain", "Sustain", "Envelope", 0, 1, 0.7f, kFloat, U_PERCENT, nullptr, &EngineParams::sustain, nullptr, nullptr},
    {"release", "Release", "Release", "Envelope", 0, 2, 0.5f, kFloat, U_SEC, nullptr, &EngineParams::release, nullptr, nullptr},
    {"env_to_amp", "Env to Amp", "Env>Amp", "Envelope", 0, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::env_to_amp, nullptr, nullptr},
    // --- Output ---
    {"dry", "Dry", "Dry", "Output", 0, 1, 0, kFloat, U_PERCENT, nullptr, &EngineParams::dry, nullptr, nullptr},
    {"dry_delay", "Dry Delay", "Dry Dly", "Output", 0, 1000, 0, kFloat, U_MS, nullptr, &EngineParams::dry_delay, nullptr, nullptr},
    {"output_db", "Output", "Output", "Output", -60, 6, -6, kFloat, U_DB, nullptr, &EngineParams::output_db, nullptr, nullptr},
    {"bypass", "Bypass", "Bypass", "Output", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::bypass},
    // --- External block-rate modulation (kept for parity with the Pd panel) ---
    {"external_modulation", "External Mod", "Ext Mod", "External", 0, 1, 0, kBool, U_NONE, nullptr, nullptr, nullptr, &EngineParams::external_modulation},
    {"external_lfo", "External LFO", "Ext LFO", "External", -1, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::external_lfo, nullptr, nullptr},
    {"external_envelope", "External Env", "Ext Env", "External", 0, 1, 0, kFloat, U_DEPTH, nullptr, &EngineParams::external_envelope, nullptr, nullptr},
    // --- Trigger (no engine parameter; handled by the plugin core) ---
    {"reset", "Reset Capture", "Reset", "Algorithm", 0, 1, 0, kTrigger, U_NONE, nullptr, nullptr, nullptr, nullptr},
};

float clampToParam(const ParamDesc& d, float v) {
    if (!(v >= d.min)) return d.min; // also catches NaN
    if (v > d.max) return d.max;
    return v;
}

float effectiveValue(const ParamDesc& d, float base, float mod) {
    // Stepped parameters do not accept fractional modulation; the clamped raw
    // value is still quantized by applyToEngine.
    return clampToParam(d, base + mod);
}

void applyToEngine(const ParamDesc& d, EngineParams& p, float value) {
    value = clampToParam(d, value);
    switch (d.kind) {
        case kInt:
            if (d.i) p.*d.i = (int)std::lround(value);
            break;
        case kBool:
        case kTrigger:
            if (d.b) p.*d.b = value >= 0.5f;
            break;
        default:
            if (d.f) p.*d.f = value;
            break;
    }
}

void applyAllToEngine(const float* base, const float* mod, EngineParams& p) {
    for (int i = 0; i < PARAM_COUNT; ++i) {
        const ParamDesc& d = kParams[i];
        applyToEngine(d, p, effectiveValue(d, base[i], mod[i]));
    }
}

bool formatValue(const ParamDesc& d, double v, char* out, size_t cap) {
    float q = clampToParam(d, (float)v);
    switch (d.kind) {
        case kTrigger:
        case kBool:
            snprintf(out, cap, "%s", q >= 0.5f ? "On" : "Off");
            return true;
        case kInt: {
            int iv = (int)std::lround(q);
            if (d.enums) {
                for (int e = 0; d.enums[e]; ++e) {
                    if (e == iv) { snprintf(out, cap, "%s", d.enums[e]); return true; }
                }
            }
            snprintf(out, cap, "%d", iv);
            return true;
        }
        default:
            break;
    }
    switch (d.unit) {
        case U_SEMI:  snprintf(out, cap, "%+.1f st", q); break;
        case U_CENT:  snprintf(out, cap, "%+.0f ct", q); break;
        case U_MS:    snprintf(out, cap, "%.1f ms", q); break;
        case U_HZ:    snprintf(out, cap, "%.2f Hz", q); break;
        case U_DB:    snprintf(out, cap, "%+.1f dB", q); break;
        case U_SEC:   snprintf(out, cap, "%.3f s", q); break;
        case U_BPM:   snprintf(out, cap, "%.1f BPM", q); break;
        case U_PAN: {
            float p2 = clampToParam(d, q);
            if (p2 < -0.005f) snprintf(out, cap, "L %.0f %%", -p2 * 100.f);
            else if (p2 > 0.005f) snprintf(out, cap, "R %.0f %%", p2 * 100.f);
            else snprintf(out, cap, "C");
            break;
        }
        case U_PERCENT:
        case U_DEPTH: snprintf(out, cap, "%.0f %%", q * 100.f); break;
        default:      snprintf(out, cap, "%.3f", q); break;
    }
    return true;
}

static bool parseOnOff(const char* s) {
    if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "yes")) return true;
    if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcasecmp(s, "no")) return true;
    return false;
}
static bool isOn(const char* s) {
    return !strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "yes");
}

bool parseValue(const ParamDesc& d, const char* text, double* out) {
    if (!text) return false;
    while (*text == ' ') ++text;
    if (!*text) return false;
    switch (d.kind) {
        case kTrigger:
        case kBool:
            if (parseOnOff(text)) { *out = isOn(text) ? 1 : 0; return true; }
            break;
        case kInt: {
            char* end = nullptr;
            long iv = strtol(text, &end, 10);
            if (end != text) { *out = (double)clampToParam(d, (float)iv); return true; }
            for (int e = 0; d.enums && d.enums[e]; ++e) {
                if (!strcasecmp(text, d.enums[e])) { *out = e; return true; }
            }
            return false;
        }
        default:
            break;
    }
    char* end = nullptr;
    // Pan accepts "L 35" / "R 35" / "C" (percent by default) in addition to -1..1
    if (d.unit == U_PAN && (*text == 'L' || *text == 'l' || *text == 'R' || *text == 'r')) {
        bool left = (*text == 'L' || *text == 'l');
        const char* num = text + 1;
        while (*num == ' ') ++num;
        double pv = strtod(num, &end);
        if (end == num) return false;
        pv /= 100.0;  // L/R-prefixed input is percent, matching the display
        *out = (double)clampToParam(d, (float)(left ? -pv : pv));
        return true;
    }
    double v = strtod(text, &end);
    if (end == text) return false;
    // percent-style input for 0..1 parameters: "75 %" or "75%"
    if ((d.unit == U_PERCENT || d.unit == U_DEPTH || d.unit == U_PAN) &&
        (fabs(v) > 1.0001 || strchr(text, '%'))) {
        v /= 100.0;
    }
    *out = (double)clampToParam(d, (float)v);
    return true;
}

} // namespace polar_clap

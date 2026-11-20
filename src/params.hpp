#pragma once
// Polar Reconstruction CLAP — parameter model shared by plugin core, GUI and tests.
#include <cstddef>
#include <cstdint>
#include "polar_engine.hpp"

namespace polar_clap {

using EngineParams = polar_recovered::PolarEngine::Parameters;

enum ParamKind { kFloat, kInt, kBool, kTrigger };
enum ParamUnit {
    U_NONE,     // plain number
    U_SEMI,     // semitones
    U_CENT,     // cents
    U_MS,       // milliseconds
    U_HZ,       // hertz
    U_DB,       // decibels
    U_SEC,      // seconds
    U_PERCENT,  // 0..1 shown as percent
    U_BPM,      // beats per minute
    U_PAN,      // -1..1 shown as L/R percent
    U_DEPTH     // 0..1 modulation depth shown as percent
};

struct ParamDesc {
    const char* engine_name;  // canonical name, also used for state files
    const char* name;         // full display name for hosts
    const char* label;        // compact label for the custom GUI
    const char* module;       // CLAP module path / GUI section
    float       min, max, def;
    ParamKind   kind;
    ParamUnit   unit;
    const char* const* enums; // kInt: display names, null-terminated
    // engine target (null for kTrigger)
    float EngineParams::* f;
    int   EngineParams::* i;
    bool  EngineParams::* b;
};

enum ParamId {
    P_ALGORITHM = 0,
    P_SPEED,
    P_FORCE_REVERSE,
    P_STEREO,
    P_LOCK,
    P_POSITION,
    P_LOOP,
    P_POSITION_LFO,
    P_POSITION_ENV,
    P_PITCH_1,
    P_TUNE_1,
    P_LEVEL_1,
    P_PAN_1,
    P_FEEDBACK_1,
    P_PITCH_LFO_1,
    P_PITCH_ENV_1,
    P_ENABLE_2,
    P_PITCH_2,
    P_TUNE_2,
    P_LEVEL_2,
    P_PAN_2,
    P_FEEDBACK_2,
    P_DELAY_2,
    P_PITCH_LFO_2,
    P_PITCH_ENV_2,
    P_FILTER_ON,
    P_FILTER_MODE,
    P_CUTOFF,
    P_RESONANCE,
    P_FILTER_LFO,
    P_FILTER_ENV,
    P_LFO_RATE,
    P_LFO_SYNC,
    P_TEMPO,
    P_DIVISION,
    P_LFO_WAVE,
    P_LFO_PHASE,
    P_PAN_LFO,
    P_GATE,
    P_ATTACK,
    P_DECAY,
    P_SUSTAIN,
    P_RELEASE,
    P_ENV_TO_AMP,
    P_DRY,
    P_DRY_DELAY,
    P_OUTPUT_DB,
    P_BYPASS,
    P_EXTERNAL_MOD,
    P_EXTERNAL_LFO,
    P_EXTERNAL_ENV,
    P_RESET,
    PARAM_COUNT
};

extern const ParamDesc kParams[PARAM_COUNT];

// Quantizes + clamps a raw value into the parameter's domain.
float clampToParam(const ParamDesc& d, float v);
// Effective value = base + mod, clamped; stepped kinds are rounded by applyToEngine.
float effectiveValue(const ParamDesc& d, float base, float mod);
// Writes a value into the engine parameter struct.
void applyToEngine(const ParamDesc& d, EngineParams& p, float value);
// Applies a whole base[]+mod[] table. Pitch, tune, position, cutoff and resonance
// keep their base value on the engine's smoothed path and receive the (range-clamped)
// modulation on the engine's post-smoother path; every other parameter gets base + mod.
void applyAllToEngine(const float* base, const float* mod, EngineParams& p);

// CLAP flags policy: host modulation reaches every parameter except Reset and Bypass.
bool isModulatable(uint32_t index);
// Kept for state compatibility, not shown to hosts or in the GUI.
bool isHidden(uint32_t index);

// Text formatting matching ParamUnit; parse is the inverse (also accepts enum
// names and on/off). Both clamp to the parameter domain.
bool formatValue(const ParamDesc& d, double v, char* out, size_t cap);
bool parseValue(const ParamDesc& d, const char* text, double* out);

} // namespace polar_clap

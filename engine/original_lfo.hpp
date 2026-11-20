#pragma once
// Polar's LFO: the original CLFOModule (vtable 0x3be70, tick 0x20fb0), ported operation by
// operation. One value per 64-sample graph tick, which is how the engine consumes it.
//
// Device wiring (graph node 70): wave via a selector of constants {1,3,9,2,4,10}, LFORate
// mapped to -0.9..0.1, LFOSync, the synced-rate index (LFORateDummy, 0..17) and LFOPhase.
// Shapes 0, 5, 6, 7 and 8 exist in the code but the device exposes only the six above.
// Proven against the original routine by the lab harness (see README, "Accuracy").
#include <cmath>
#include <cstdint>
#include "bundled_math.hpp"

namespace polar_recovered {

struct OriginalLfo {
    // UI wave index -> internal shape (selector constants at nodes 134..139). Index 6 adds
    // the binary's own sine shape (internal 0), which the device panel does not offer.
    static constexpr int kWaveCount = 7;
    static int shapeForWave(int wave) {
        static const int map[kWaveCount] = {1, 3, 9, 2, 4, 10, 0};
        return map[wave < 0 ? 0 : wave >= kWaveCount ? kWaveCount - 1 : wave];
    }
    // Synced cycle lengths in beats (table at 0x38220).
    static float syncBeats(int index) {
        static const float table[18] = {32.0f, 16.0f, 12.0f, 8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f,
                                        2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.3333333432674408f,
                                        0.25f, 0.125f, 0.0625f};
        return table[index < 0 ? 0 : index > 17 ? 17 : index];
    }

    float sample_rate = 48000;  // +0x110
    std::uint32_t seed = 0;     // +0x154 (the original seeds with its own address)
    float phase = 0;            // +0x158 (stored with the phase offset included)
    float held = 0;             // +0x15c random value
    float previous = 0;         // +0x160 phase of the previous tick
    float smooth = 0;           // +0x16c smoothed random
    float value = 0;            // +0x10c

    void reset() { value = 0; phase = 0; smooth = 0; }

    // shape: internal shape number; rate: mapped LFORate (-0.9..0.1); sync: LFOSync;
    // syncRate: LFORateDummy mapped to 0..1; phaseOffset: LFOPhase. tempo, playing and
    // positionPpq are the host transport (Reason: 15360 PPQ).
    float tick(int shape, float rate, float sync, float syncRate, float phaseOffset,
               double tempo, bool playing, double positionPpq) {
        float step;
        if (sync > 0.5f) {
            const int raw = int(syncRate * 17.899999618530273f);
            const int index = raw < 0 ? 0 : (raw >= 18 ? 17 : raw);
            const float beats = syncBeats(index);
            float denominator = sample_rate * beats;
            denominator = denominator * 60.0f;
            step = float(tempo) / denominator;
            if (playing && 1.0499999523162842f > sync) {
                float cycle = beats * 15360.0f;
                cycle = cycle + cycle;
                const double q = positionPpq / double(cycle);
                const double fraction = q - std::floor(q);
                phase = float(fraction + fraction);
            }
        } else {
            step = bundled::powf(2.0f, rate * 7.0f) * 8.175799369812012f / sample_rate;
        }
        float offset = 0.0f;
        if (!(0.0f > phaseOffset)) offset = phaseOffset > 1.0f ? 1.0f : phaseOffset;
        step = 0.5f < step ? 0.5f : step;  // minss 0.5, step
        float x = phase - offset;
        if (0.0f > x) x += 1.0f;
        float p = 64.0f * step + x;
        if (p > 1.0f) p += -1.0f;
        float stored = offset + p;
        if (stored > 1.0f) stored += -1.0f;
        phase = stored;

        if (shape < 0 || shape > 10) return value;
        auto draw = [&] {
            seed = seed * 0x0bb38435u + 0x3619636bu;
            held = float(seed) * 4.656612873077393e-10f + -1.0f;
        };
        switch (shape) {
        case 0: {  // sine (cubic in z around the peak)
            float q = p > 0.75f ? p + -1.25f : p + -0.25f;
            const float z = q * q;
            float v = -61.0011100769043f * z + 61.571754455566406f;
            v = v * z + -19.589359283447266f;
            v = v * z + 1.001099944114685f;
            value = v;
            break;
        }
        case 1: {  // triangle
            if (0.25f > p) { value = p * 4.0f; break; }
            const float t = p * 4.0f;
            value = 0.75f > p ? 2.0f - t : t + -4.0f;
            break;
        }
        case 2: value = p > 0.5f ? -1.0f : 1.0f; break;   // square
        case 3: value = (p + p) + -1.0f; break;          // saw up
        case 4: {                                         // random, sample and hold
            if (previous - p > 0.5f) draw();
            previous = p;
            value = held;
            break;
        }
        case 5: { const float t = 1.0f - p; value = (t * t) * t; break; }
        case 6: value = float(int(p * 3.0f)) + -1.0f; break;
        case 7: value = float(std::uint32_t(std::int64_t(p * 4.0f))) * 0.6666666865348816f + -1.0f; break;
        case 8: value = 0.25f > p ? 1.0f : (0.5f > p ? 0.0f : (0.75f > p ? -1.0f : 0.0f)); break;
        case 9: value = p * -2.0f + 1.0f; break;          // saw down
        default: {                                        // 10: smooth random
            const float k = step * 64.0f;
            if (previous - p > 0.5f) draw();
            previous = p;
            const float r = held * k;
            smooth = (1.0f - k) * smooth + r;
            value = smooth;
            break;
        }
        }
        return value;
    }
};

}  // namespace polar_recovered

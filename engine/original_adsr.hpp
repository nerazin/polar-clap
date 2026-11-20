#pragma once
// Polar's envelope: the original CADSRModule (vtable 0x3b770), audio path 0x1e7c0 with its
// per-sample state machine 0x1e190, ported operation by operation. Consumers read the whole
// block (Env to Amp) or its first sample (pitch, position and filter depths), as the graph's
// junction nodes do. A, D, S, R are the raw 0..1 knobs: attack 3a^3 s (linear), decay and
// release time constants 10x^3 s (exponential), sustain s^3.
// Gate: EnvGate (0/1) plus the MIDI gate (1.01 while a key is held). Attack restarts when the
// gate rises above 1/127 and exceeds its previous value, so legato keys do not retrigger.
#include <cmath>
#include <cstdint>
#include "bundled_math.hpp"

namespace polar_recovered {

struct OriginalAdsr {
    enum State { kAttack = 0, kDecay = 1, kRelease = 3, kIdle = 4 };
    float sample_rate = 48000;       // +0x140
    float level = 0;                 // +0x144
    float attack_rate = 0;           // +0x148
    float decay_coefficient = 0;     // +0x14c
    float sustain_level = 0;         // +0x150
    float release_coefficient = 0;   // +0x154
    float release_step = 0;          // +0x158
    int state = kIdle;               // +0x15c
    float last_attack = -1, last_decay = -1, last_sustain = 0, last_release = -1;  // +0x160..+0x16c
    float last_gate = 0;             // +0x170

    void reset() {  // 0x1e4a0
        last_attack = last_decay = last_release = -1.0f;
        last_gate = 0;
        state = kIdle;
        level = 0;
    }

    void process(float gate, float attack, float decay, float sustain, float release, float* out) {
        if (gate > 0.007874015718698502f) {
            if (gate > last_gate) state = kAttack;
        } else if (last_gate > 0.007874015718698502f) {
            state = kRelease;
        }
        last_gate = gate;
        if (attack != last_attack) {
            last_attack = attack;
            float t = (attack * attack) * attack;
            t = t * 3.0f + 9.999999747378752e-05f;
            if (2.9999999242136255e-05f > t) attack_rate = 1.0099999904632568f;
            else attack_rate = 1.0f / (t * sample_rate);
        }
        if (decay != last_decay) {
            last_decay = decay;
            float t = (decay * decay) * decay;
            t = t * 10.0f + 9.999999747378752e-05f;
            decay_coefficient = float(bundled::exp(double(-2.299999952316284f / (sample_rate * t))));
        }
        if (sustain != last_sustain) {
            last_sustain = sustain;
            sustain_level = (sustain * sustain) * sustain;
        }
        if (release != last_release) {
            last_release = release;
            float t = (release * release) * release;
            t = t * 10.0f + 9.999999747378752e-05f;
            release_coefficient = float(bundled::exp(double(-2.299999952316284f / (sample_rate * t))));
            release_step = 0.03999999910593033f / sample_rate;
        }
        run(out);
    }

private:
    void run(float* out) {  // 0x1e190
        int i = 0;
        if (state == kAttack) {
            const float rate = attack_rate;
            const int n = int((1.0f - level) / rate);
            if (n > 63) {
                for (; i < 64; ++i) { out[i] = level; level = level + rate; }
                return;
            }
            for (; i < n; ++i) { out[i] = level; level = level + rate; }
            level = 1.0f;
            state = kDecay;
        }
        if (state == kDecay) {
            const float c = decay_coefficient;
            const float target = (1.0f - c) * sustain_level;
            for (; i < 64; ++i) { level = level * c + target; out[i] = level; }
            return;
        }
        if (state == kRelease) {
            for (; i < 64; ++i) {
                float v = level * release_coefficient - release_step;
                level = v;
                if (0.0f > v) { level = 0; state = kIdle; v = 0; }
                out[i] = v;
            }
            return;
        }
        for (; i < 64; ++i) out[i] = 0.0f;  // idle
    }
};

}  // namespace polar_recovered

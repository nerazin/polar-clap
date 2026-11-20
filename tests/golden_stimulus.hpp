#pragma once
// Stimulus for the LFO, envelope and exp goldens. The lab harness feeds the same sequences
// to the ORIGINAL routines to produce the pinned hashes; tests/golden.cpp feeds the ports.
#include <cstdint>
#include <cstring>
#include <functional>

namespace golden {

inline uint64_t mixWord(uint64_t h, uint32_t w, uint64_t i) {
    return h + (uint64_t(w) + 1) * (0x9e3779b97f4a7c15ull ^ (i * 0xbf58476d1ce4e5b9ull));
}
inline uint32_t floatWord(float f) { uint32_t x; std::memcpy(&x, &f, 4); return f != f ? 0x7fc00000u : x; }

// LFO at 48 kHz, seed 0x1234567: the seven waves x free / synced stopped / synced playing,
// segments of 250 ticks sweeping rate, synced rate, phase and tempo.
struct LfoIn { int shape; float rate, sync, syncRate, phase; double tempo; bool playing; double ppq; };
inline uint64_t lfoHash(const std::function<float(const LfoIn&)>& tick) {
    static const int shapes[7] = {1, 3, 9, 2, 4, 10, 0};
    LfoIn in{};
    uint64_t h = 0;
    double ppq = 0;
    for (uint64_t t = 0; t < 210000; ++t) {
        const uint64_t s = t / 250;
        const int mode = int(s / 7 % 3);
        in.shape = shapes[s % 7];
        in.rate = -0.9f + float(s * 37 % 101) / 100.0f;
        in.sync = mode ? 1.0f : 0.0f;
        in.syncRate = float(s * 5 % 18) / 17.0f;
        in.phase = float(s * 13 % 11) / 10.0f;
        in.tempo = 60.0 + double(s * 17 % 181);
        in.playing = mode == 2;
        if (in.playing) ppq += 64.0 / 48000.0 * in.tempo / 60.0 * 15360.0;
        in.ppq = ppq;
        h = mixWord(h, floatWord(tick(in)), t);
    }
    return h;
}

// Envelope at 48 kHz: knobs stepping through 0, 0.1 .. 1; gate held, released, retriggered
// (EnvGate 1, MIDI 1.01, both 2.01, and a 0.5 that stays below a held MIDI gate).
struct AdsrIn { float gate, attack, decay, sustain, release; };
inline uint64_t adsrHash(const std::function<const float*(const AdsrIn&)>& block) {
    static const float gates[4] = {1.0f, 1.01f, 2.01f, 0.5f};
    uint64_t h = 0, n = 0;
    for (uint64_t t = 0; t < 24000; ++t) {
        const uint64_t s = t / 60, ph = t % 60;
        AdsrIn in{};
        in.attack = float(s * 7 % 11) / 10.0f;
        in.decay = float(s * 3 % 11) / 10.0f;
        in.sustain = float(s * 5 % 11) / 10.0f;
        in.release = float(s * 9 % 11) / 10.0f;
        in.gate = ph < 25 ? gates[s % 4] : (ph < 30 && s % 5 == 0 ? 2.01f : 0.0f);
        const float* o = block(in);
        for (int i = 0; i < 64; ++i, ++n) h = mixWord(h, floatWord(o[i]), n);
    }
    return h;
}

// exp(double): the envelope's domain [-0.5, 0] and a wide range with overflow and underflow.
inline uint64_t expHash(const std::function<double(double)>& f) {
    uint64_t h = 0;
    for (uint64_t i = 0; i < (1u << 22); ++i) {
        const double u = double(i) / 4194304.0;
        const double y = f(i & 1 ? (u - 0.5) * 1500.0 : -u * 0.5);
        uint64_t p; std::memcpy(&p, &y, 8);
        h = mixWord(h, uint32_t(p ^ (p >> 32)), i);
    }
    return h;
}

}  // namespace golden

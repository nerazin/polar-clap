// Golden tests: pin the original binary's behaviour without shipping the binary.
//  - The ported bundled libm (engine/bundled_math.hpp) was proven against the original
//    x86-64 routines on every 32-bit input of expf/atanf, every cosf input |x| <= 2^7*pi/2,
//    every powf(128, t) with t in [0,1], and 650M random powf/atan2f/sin/cos inputs (0 differ).
//    These hashes over strided subsets were printed by running the same stimulus through the
//    ORIGINAL routines (the lab harness's "golden" mode), as the playbook's golden step does.
//  - The LFO (CLFOModule), envelope (CADSRModule) and binary64 exp ports were proven against
//    the original routines with randomized stimulus and every state word compared (0 differ);
//    their hashes over tests/golden_stimulus.hpp were printed by the original routines too.
//  - The FFT trig/Hann/fade tables generated with it must keep the pinned hash of the
//    original binary's table data.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#define private public  // read the period processor's generated tables
#include "fft0.hpp"
#include "fft1.hpp"
#undef private
#include "golden_stimulus.hpp"
#include "original_adsr.hpp"
#include "original_lfo.hpp"

namespace bm = polar_recovered::bundled;
static int g_failures = 0;
static const uint64_t kTablesHash = 0xfcce2b39e2d662c7ull;  // 74,432 entries, checked one by one before pinning
static void fail(const char* what) { ++g_failures; printf("  FAIL: %s\n", what); }
static float bits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
static uint32_t word(float f) { uint32_t x; std::memcpy(&x, &f, 4); return f != f ? 0x7fc00000u : x; }
static uint64_t hashOver(uint64_t n, const std::function<uint32_t(uint64_t)>& fn) {
    uint64_t h = 0;
    for (uint64_t i = 0; i < n; ++i) h += (uint64_t(fn(i)) + 1) * (0x9e3779b97f4a7c15ull ^ (i * 0xbf58476d1ce4e5b9ull));
    return h;
}

int main(int argc, char** argv) {
    const bool print = argc > 1 && !strcmp(argv[1], "--print");
    struct Golden { const char* name; uint64_t expect; std::function<uint64_t()> run; } goldens[] = {
        {"exp", 0x2fdb79945fa7a1bfull, [] { return golden::expHash([](double x) { return bm::exp(x); }); }},
        {"lfo", 0x387c776235062df3ull, [] {
            polar_recovered::OriginalLfo l; l.sample_rate = 48000; l.seed = 0x1234567u; l.reset();
            return golden::lfoHash([&](const golden::LfoIn& in) {
                return l.tick(in.shape, in.rate, in.sync, in.syncRate, in.phase, in.tempo, in.playing, in.ppq); });
        }},
        {"adsr", 0x39924d945eb34f16ull, [] {
            polar_recovered::OriginalAdsr e; e.sample_rate = 48000; e.reset(); static float out[64];
            return golden::adsrHash([&](const golden::AdsrIn& in) {
                e.process(in.gate, in.attack, in.decay, in.sustain, in.release, out); return (const float*)out; });
        }},
        {"expf", 0x456faf1b87888edaull, [] { return hashOver(1u << 22, [](uint64_t i) { return word(bm::expf(bits(uint32_t(i * 1021u)))); }); }},
        {"atanf", 0x388b5aeb815e9d82ull, [] { return hashOver(1u << 22, [](uint64_t i) { return word(bm::detail::atanf(bits(uint32_t(i * 1021u)))); }); }},
        {"cosf", 0x6b5f80d08aae090dull, [] { return hashOver(1u << 22, [](uint64_t i) { return word(bm::cosf(bits(uint32_t(i * 257u) % 0x43490f81u | (i & 1 ? 0x80000000u : 0)))); }); }},
        {"powf(128,t)", 0xb8c89a483ca4b0e3ull, [] { return hashOver(1u << 22, [](uint64_t i) { return word(bm::powf(128.f, bits(uint32_t(i * 253u) % 0x3f800001u))); }); }},
        {"atan2f", 0xffa32930f5d5a724ull, [] { return hashOver(1u << 22, [](uint64_t i) { uint64_t r = i * 0x9e3779b97f4a7c15ull;
             return word(bm::atan2f(bits(uint32_t(r >> 32)), bits(uint32_t(r)))); }); }},
        {"sin/cos", 0xc9cfad6f5795d6d1ull, [] { return hashOver(1u << 22, [](uint64_t i) { double x = (double(i) / double(1u << 22) - 0.5) * 8 * 3.141592653589793;
             double a = bm::sin(x), b = bm::cos(x); uint64_t p, q; std::memcpy(&p, &a, 8); std::memcpy(&q, &b, 8); return uint32_t(p ^ (p >> 32) ^ q ^ (q >> 29)); }); }},
    };
    for (auto& g : goldens) {
        const uint64_t h = g.run();
        if (print) printf("        {\"%s\", 0x%016llxull, ...}\n", g.name, (unsigned long long)h);
        else if (h != g.expect) { char m[96]; snprintf(m, sizeof m, "%s hash %016llx", g.name, (unsigned long long)h); fail(m); }
    }
    if (!print) printf("bundled libm, LFO and envelope golden hashes: %s\n", g_failures ? "FAILED" : "ok");

    // Tables generated at start-up. Their hash was pinned after checking every entry against
    // the table data the reconstruction had copied out of the original binary (and the harness
    // compared the FFT0 trig table with the original's memory); the copied data is not shipped.
    uint64_t th = 0, count = 0;
    auto mixIn = [&](const float* v, int n) { for (int i = 0; i < n; ++i, ++count) th += (uint64_t(word(v[i])) + 1) * (0x9e3779b97f4a7c15ull ^ (count * 0xbf58476d1ce4e5b9ull)); };
    auto noop = [](float*, int) {};
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        polar_recovered::PolarFFT0 a(noop, noop, [](int, int, float*, float*) {});
        a.prepare(sr);
        mixIn(a.trig.data(), 2048);
        for (int half = 32; 2 * half <= a.maxN; half *= 2) { a.setWindowHalf(half); mixIn(a.window.data(), a.n); }
        polar_recovered::OriginalAlg1Reference b(sr, noop, noop, [](int, int) { return 0.f; });
        for (int lag = b.capacity_ / 32; lag <= b.capacity_ / 2; lag *= 2) {
            if (!b.configureMaxLag(lag)) { fail("FFT1 configure"); continue; }
            mixIn(b.window_.data(), b.n_);
            mixIn(b.fade_.data(), b.fadeLength_);
        }
    }
    if (print) { printf("tables %016llx over %llu entries\n", (unsigned long long)th, (unsigned long long)count); return 0; }
    if (th != kTablesHash) fail("generated FFT tables differ from the original's");
    printf("generated tables (%llu entries): %s\n", (unsigned long long)count, th == kTablesHash ? "ok" : "FAILED");
    if (g_failures) { printf("\n%d FAILURE(S)\n", g_failures); return 1; }
    printf("\nGOLDEN TESTS PASSED\n");
    return 0;
}

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "bundled_math.hpp"

// Static reconstruction of Polar's original AlgType 0 (x86_64 image).
// Build with normal IEEE arithmetic, without fast-math/FMA contraction.
// The injected transform owns the SDK's real-FFT packing and normalization.
// It receives N floats in place and log2(N); packed complex pairs k=1..N/2-1
// are interlaced real/imaginary. Pair 0 deliberately survives the pack pass.
namespace polar_recovered {

class PolarFFT0 {
public:
    using Transform = std::function<void(float*, int order)>;
    // Read a contiguous N-sample window at signed source position. right is
    // null for mono. Original SourceReader methods are vtable+0x10 / +0x20.
    using Source = std::function<void(int start, int count, float* left, float* right)>;
    Transform forward, inverse;
    Source source;

    static constexpr float pi = 3.1415927410125732421875f;
    static constexpr float twoPi = 6.283185482025146484375f;
    static constexpr float invPi = 0.3183098733425140380859375f;
    static constexpr float phaseToTable = 162.9746551513671875f;

    // Public state permits image-oracle comparisons against offsets in CHECKPOINT.md.
    float maxSeconds = 0.07999999821186066f; // ctor argument at image 0x375dc
    int n = 0, maxN = 0, firstBin = 1, binEnd = 0, order = 0;
    int analysisStart = 0;
    std::array<int, 2> synthWrite{{0, 0}};
    std::array<double, 2> readPosition{{0.0, 0.0}};
    std::array<float, 2048> trig{};
    std::vector<float> window, transientHold;
    std::array<std::vector<float>, 2> work, analysisPhase, magnitude, omega;
    std::array<std::array<std::vector<float>, 2>, 2> ola, synthPhase;

    PolarFFT0(Transform f, Transform i, Source s, float seconds = 0.07999999821186066f)
        : forward(std::move(f)), inverse(std::move(i)), source(std::move(s)), maxSeconds(seconds) {
        // 0x43a0: LUT generated in double precision and stored as float, with the
        // original's bundled sin/cos (bit-exact with the original table).
        double theta = -3.141592654;
        for (int k = 0; k < 1024; ++k) {
            trig[2*k] = static_cast<float>(bundled::sin(theta));
            trig[2*k+1] = static_cast<float>(bundled::cos(theta));
            theta += 0.00613592315234375;
        }
    }

    static int nextPowerOfTwo(int value) {
        int result = 2;
        while (result < value) result *= 2;
        return value == 1 ? 1 : result;
    }

    // 0x4da0. Call setWindowHalf after this when first preparing a processor.
    void prepare(float sampleRate) {
        maxN = nextPowerOfTwo(static_cast<int>(sampleRate * maxSeconds));
        window.assign(maxN, 0.0f);
        transientHold.assign(maxN/2, 0.0f);
        for (int ch = 0; ch < 2; ++ch) {
            work[ch].assign(maxN, 0.0f);
            analysisPhase[ch].assign(maxN/2, 0.0f);
            magnitude[ch].assign(maxN/2, 0.0f);
            omega[ch].assign(maxN/2, 0.0f);
            for (int v = 0; v < 2; ++v) {
                ola[v][ch].assign(2*maxN, 0.0f);
                synthPhase[v][ch].assign(maxN/2, 0.0f);
            }
        }
    }

    // 0x4f50. The argument is N/2, as supplied by the original parent core.
    bool setWindowHalf(int halfWindow) {
        const int requestedN = nextPowerOfTwo(2*halfWindow);
        if (requestedN == n || requestedN < 64 || requestedN > maxN) return false;
        n = requestedN;
        binEnd = n/2;
        order = 0;
        for (int i = n; i > 1; i /= 2) ++order;
        std::fill(window.begin(), window.end(), 0.0f);
        // 0x24c10, type 0: periodic Hann; retain source float operation order.
        for (int i = 0; i < n; ++i) {
            float angle = static_cast<float>(i) * twoPi;
            angle /= static_cast<float>(n);
            float c = bundled::cosf(angle);
            c *= 0.5f;
            window[i] = 0.5f-c;
        }
        resetAt(static_cast<double>(analysisStart));
        return true;
    }

    // 0x4680 + 0x4fd0. Window and LUT survive; all signal arrays are cleared.
    void resetAt(double anchor) {
        std::fill(transientHold.begin(), transientHold.end(), 0.0f);
        for (int ch = 0; ch < 2; ++ch) {
            for (auto* a : {&work[ch], &analysisPhase[ch], &magnitude[ch], &omega[ch]})
                std::fill(a->begin(), a->end(), 0.0f);
            for (int v = 0; v < 2; ++v) {
                std::fill(ola[v][ch].begin(), ola[v][ch].end(), 0.0f);
                std::fill(synthPhase[v][ch].begin(), synthPhase[v][ch].end(), 0.0f);
            }
        }
        analysisStart = static_cast<int>(anchor)-n;
        synthWrite.fill(analysisStart);
        readPosition.fill(static_cast<double>(analysisStart));
    }

    // Source-ring coordinate rebase at 0x5020, and OLA rebase at 0x5030.
    void rebaseSource(int period) { analysisStart -= period/2; }
    void rebaseSynthesis(int threshold) {
        for (int v = 0; v < 2; ++v) if (synthWrite[v] > threshold) {
            synthWrite[v] -= threshold/2;
            readPosition[v] -= static_cast<double>(threshold/2);
        }
    }

    // 0x1f0b0 / 0x1f230 share the same even-integer phase reduction rule.
    static float wrapEvenPi(float x) {
        int q = static_cast<int>(x*invPi);
        if (q > 0) q += q&1;
        else q &= ~1;
        float correction = static_cast<float>(q)*(-pi);
        return x+correction;
    }

    // 0x5080 / 0x52a0: at most ONE analysis hop per invocation.
    bool analyse(double anchor, double sourceStep, bool stereo) {
        const int difference = static_cast<int>(anchor)-analysisStart;
        const int distance = std::abs(difference);
        int hop = static_cast<int>(static_cast<float>(n)*0.25f);
        float oversampling = 4.0f;
        if (sourceStep < 0.10000000149011612 && distance < hop) {
            hop = static_cast<int>(static_cast<float>(n)*0.0625f);
            oversampling = 16.0f;
        }
        if (distance <= hop) return false;
        if (difference < 0) oversampling = -oversampling;
        const int start = analysisStart+(difference > 0 ? hop : -hop);
        source(start, n, work[0].data(), stereo ? work[1].data() : nullptr);
        const int channels = stereo ? 2 : 1;
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i) work[ch][i] *= window[i];
        for (int ch = 0; ch < channels; ++ch) forward(work[ch].data(), order);
        for (int ch = 0; ch < channels; ++ch) unpack(ch, oversampling);
        // Stereo uses ONE hold detector on magL+magR, resetting both channels'
        // phases together, but each to its own analysis phase (0x5542..0x5598).
        for (int k = firstBin; k < binEnd; ++k) {
            float m = magnitude[0][k];
            if (stereo) m += magnitude[1][k];
            float hold = transientHold[k];
            if (m > hold) {
                if (m > hold*1.4500000476837158f) {
                    transientHold[k] = m*1.4500000476837158f;
                    for (int v = 0; v < 2; ++v)
                        for (int ch = 0; ch < channels; ++ch)
                            synthPhase[v][ch][k] = analysisPhase[ch][k];
                } else transientHold[k] = m;
            } else transientHold[k] = hold*0.8799999952316284f;
        }
        analysisStart = start;
        return true;
    }

    // Original block functions 0x5e10/0x5f00/0x6030/0x61e0. Outputs are raw
    // individual pitch voices; parent graph supplies levels, pan and routing.
    void process(double anchor, double sourceStep, float ratio0, float ratio1,
                 int count, bool stereo, bool dual,
                 float* out0L, float* out0R = nullptr,
                 float* out1L = nullptr, float* out1R = nullptr) {
        if (n == 0 || !forward || !inverse || !source)
            throw std::logic_error("PolarFFT0 must be prepared with callbacks and a window");
        analyse(anchor, sourceStep, stereo);
        const std::array<float, 2> ratios{{ratio0, ratio1}};
        const int voices = dual ? 2 : 1;
        for (int v = 0; v < voices; ++v) {
            const float advance = static_cast<float>(count)*ratios[v];
            synthesise(v, readPosition[v]+static_cast<double>(advance), stereo);
        }
        const std::array<std::array<float*, 2>, 2> outputs{{{{out0L, out0R}}, {{out1L, out1R}}}};
        const int mask = 2*maxN-1;
        for (int i = 0; i < count; ++i) for (int v = 0; v < voices; ++v) {
            const int index = static_cast<int>(readPosition[v]);
            // This float(index) -> double conversion is present in the image.
            const float fraction = static_cast<float>(readPosition[v]-static_cast<double>(static_cast<float>(index)));
            const float oneMinus = 1.0f-fraction;
            for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
                const float a = oneMinus*ola[v][ch][(index-1)&mask];
                const float b = fraction*ola[v][ch][index&mask];
                outputs[v][ch][i] = a+b;
            }
            readPosition[v] += static_cast<double>(ratios[v]);
        }
    }

    // 0x5c90 / 0x5d30. The transition engine can use these single-sample
    // virtual methods. Their synthesis horizon is CURRENT readPosition,
    // whereas process(count=1) looks one ratio step ahead.
    std::array<float, 2> processOne(double anchor, double sourceStep, float ratio, bool stereo) {
        analyse(anchor, sourceStep, stereo);
        synthesise(0, readPosition[0], stereo);
        const int index = static_cast<int>(readPosition[0]);
        const float fraction = static_cast<float>(readPosition[0]-static_cast<double>(static_cast<float>(index)));
        const float oneMinus = 1.0f-fraction;
        const int mask = 2*maxN-1;
        std::array<float, 2> result{{0.0f, 0.0f}};
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
            const float a = oneMinus*ola[0][ch][(index-1)&mask];
            const float b = fraction*ola[0][ch][index&mask];
            result[ch] = a+b;
        }
        readPosition[0] += static_cast<double>(ratio);
        return result;
    }

private:
    void unpack(int ch, float oversampling) {
        const float residualScale = oversampling/twoPi;
        const float expected = twoPi/oversampling;
        const float binOmega = twoPi/static_cast<float>(n);
        for (int k = firstBin; k < binEnd; ++k) {
            const float re = work[ch][2*k], im = work[ch][2*k+1];
            float m = 0.0f, phase = 0.0f;
            if (re != 0.0f) { // intentional original check, including NaN
                const float squareR = re*re, squareI = im*im;
                m = std::sqrt(squareR+squareI);
                phase = bundled::atan2f(im, re);
            }
            const float bin = static_cast<float>(k);
            float residual = phase-analysisPhase[ch][k];
            analysisPhase[ch][k] = phase;
            residual -= bin*expected;
            residual = wrapEvenPi(residual);
            magnitude[ch][k] = m;
            residual *= residualScale;
            omega[ch][k] = (bin+residual)*binOmega;
        }
    }

    void lockPeaks(int v, int ch) {
        int k = firstBin+1;
        while (k < binEnd-2) {
            if (magnitude[ch][k] > magnitude[ch][k+1] && magnitude[ch][k] > magnitude[ch][k-1]) {
                if (std::abs(omega[ch][k]-omega[ch][k-1]) < 0.0010000000474974513f)
                    synthPhase[v][ch][k-1] = synthPhase[v][ch][k]+pi;
                if (std::abs(omega[ch][k]-omega[ch][k+1]) < 0.0010000000474974513f)
                    synthPhase[v][ch][k+1] = synthPhase[v][ch][k]+pi;
                k += 2;
            } else ++k;
        }
    }

    void pack(int v, int ch) {
        const float frequencyToBin = static_cast<float>(n)/twoPi;
        const float phasePerBin = twoPi/4.0f;
        for (int k = firstBin; k < binEnd; ++k) {
            const float bin = static_cast<float>(k);
            float delta = omega[ch][k]*frequencyToBin-bin;
            const float phase = synthPhase[v][ch][k];
            const int quantised = static_cast<int>(phase*phaseToTable);
            const int index = (2*quantised+1024)&2046;
            work[ch][2*k] = magnitude[ch][k]*trig[index+1];
            work[ch][2*k+1] = magnitude[ch][k]*trig[index];
            delta *= phasePerBin;
            float advance = bin*phasePerBin;
            advance += delta;
            advance += phase;
            synthPhase[v][ch][k] = wrapEvenPi(advance);
        }
    }

    void synthesise(int v, double futureReadPosition, bool stereo) {
        const int end = static_cast<int>(futureReadPosition);
        const int channels = stereo ? 2 : 1, hop = n/4, mask = 2*maxN-1;
        while (synthWrite[v] <= end) {
            for (int ch = 0; ch < channels; ++ch) lockPeaks(v, ch);
            for (int ch = 0; ch < channels; ++ch) pack(v, ch);
            for (int ch = 0; ch < channels; ++ch) inverse(work[ch].data(), order);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i) work[ch][i] *= window[i];
            for (int i = 0; i < n; ++i) {
                const int index = (synthWrite[v]+i)&mask;
                for (int ch = 0; ch < channels; ++ch) {
                    const float sample = work[ch][i]*0.7071067690849304f;
                    if (i < n-hop) ola[v][ch][index] = sample+ola[v][ch][index];
                    else ola[v][ch][index] = sample;
                }
            }
            synthWrite[v] += hop;
        }
    }
};
} // namespace polar_recovered

#pragma once
// Static reconstruction of Polar AlgType 1. See findings.md and its REA ID.
// Production copy of the validated isolated reference implementation.
// Compile without fast-math or fused multiply/add for instruction-level ordering.
// FFT callbacks use the explicit packed service contract in packed_fft.hpp.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "original_math_tables.hpp"
#include "original_fade_tables.hpp"

namespace polar_recovered {

class OriginalAlg1Reference {
public:
    // FFT arguments: N floats in the original packed format, log2(N).
    using FFT = std::function<void(float*, int)>;
    // Read one sample from the already-populated, absolute-indexed source ring.
    using Source = std::function<float(int, int)>; // sample index, channel 0/1
    using Cosine = std::function<float(float)>;
    using Outputs = std::array<std::array<float*, 2>, 2>; // voice, channel

    OriginalAlg1Reference(float sampleRate, FFT forward, FFT inverse,
                          Source source, Cosine cosine = {})
        : forward_(std::move(forward)), inverse_(std::move(inverse)),
          source_(std::move(source)), cosine_(std::move(cosine)) {
        if (!cosine_) cosine_ = [](float x) { return std::cos(x); };
        capacity_ = nextPowerOfTwo(static_cast<int>(sampleRate * 0.07999999821186066f));
        ringMask_ = 2 * capacity_ - 1;
        for (auto& voice : ring_)
            for (auto& channel : voice) channel.resize(2 * capacity_, 0.0f);
        work_.resize(capacity_, 0.0f);
        window_.resize(capacity_, 0.0f);
        fade_.resize(capacity_ / 4, 0.0f);
        reset(0.0);
    }

    // Exactly the Alg1 maxLag setter, except it reports rejected requests.
    bool configureMaxLag(int maxLag) {
        if (maxLag < capacity_ / 32 || maxLag > capacity_ / 2) return false;
        maxLag_ = maxLag;
        minLag_ = maxLag / 4;
        fadeLength_ = minLag_;
        n_ = 2 * maxLag;
        log2N_ = ceilLog2(n_);
        const float step = 3.1415927410125732f / static_cast<float>(fadeLength_);
        float theta = step;
        for (int j = 0; j < fadeLength_; ++j) {
            float a = cosine_(theta);
            a *= 0.5f;
            a += 0.5f;
            fade_[j] = a;
            theta += step;
        }
        std::fill(window_.begin(), window_.end(), 0.0f);
        for (int j = 0; j < n_; ++j) {
            float thetaJ = static_cast<float>(j) * 6.2831854820251465f;
            thetaJ /= static_cast<float>(n_);
            float a = cosine_(thetaJ);
            a *= 0.5f;
            window_[j] = 0.5f - a;
        }
        // Deterministic initializer DATA from the reviewed original routines.
        // Processing equations above are unchanged from the verified port.
        fillOriginalHann(window_.data(), n_);
        fillOriginalFade(fade_.data(), fadeLength_);
        return true;
    }

    // Root 0x1c390 computes this N before invoking configureMaxLag.
    bool configureSpeed(float seconds, float sampleRate) {
        float requested = seconds * sampleRate;
        requested += requested;
        return configureMaxLag(nextPowerOfTwo(static_cast<int>(requested)) / 2);
    }

    void reset(double anchor) {
        for (auto& voice : ring_)
            for (auto& channel : voice) std::fill(channel.begin(), channel.end(), 0.0f);
        std::fill(work_.begin(), work_.end(), 0.0f);
        const int k = static_cast<int>(anchor) - 1;
        lower_ = upper_ = k;
        for (int v = 0; v < 2; ++v) {
            position_[v] = static_cast<double>(k);
            write_[v] = lastEpoch_[v] = k;
        }
    }

    // One outer block, corresponding to 0x1b770/870/9a0/bb70. The original root
    // uses count=64. Anchor is read once; this method does not advance it.
    void process(double anchor, float ratio1, float ratio2, int count,
                 int voices, bool stereo, const Outputs& out) {
        if (n_ == 0) throw std::logic_error("configureMaxLag must precede processing");
        if (voices != 1 && voices != 2) throw std::invalid_argument("voices must be 1 or 2");
        bracket(anchor, stereo);
        const std::array<float, 2> ratio{ratio1, ratio2};
        const int channels = stereo ? 2 : 1;
        for (int v = 0; v < voices; ++v) {
            const float demand = static_cast<float>(count) * ratio[v];
            const int target = static_cast<int>(position_[v] + static_cast<double>(demand) + 4.0);
            fill(v, target, channels);
        }
        for (int i = 0; i < count; ++i) {
            for (int v = 0; v < voices; ++v) {
                const int k = static_cast<int>(position_[v]); // truncates, not floor
                const float f = static_cast<float>(position_[v] - static_cast<double>(static_cast<float>(k)));
                const float other = 1.0f - f;
                for (int c = 0; c < channels; ++c) {
                    const float current = f * ring_[v][c][index(k)];
                    const float previous = other * ring_[v][c][index(k - 1)];
                    out[v][c][i] = previous + current;
                }
                position_[v] += static_cast<double>(ratio[v]);
            }
        }
    }

    // Original 0x1ada0: caller decides when source-ring absolute indices rebase.
    void rebaseSource(int adjustment) {
        lower_ -= adjustment / 2;
        upper_ -= adjustment / 2;
    }

    // Original 0x1adc0: source-epoch markers are deliberately not changed here.
    void rebaseOutput(int threshold) {
        for (int v = 0; v < 2; ++v)
            if (write_[v] > threshold) {
                write_[v] -= threshold / 2;
                position_[v] -= static_cast<double>(threshold / 2);
            }
    }

    int fftSize() const { return n_; }
    int minimumLag() const { return minLag_; }
    int maximumLag() const { return maxLag_; }
    int lowerEpoch() const { return lower_; }
    int upperEpoch() const { return upper_; }
    double position(int voice) const { return position_.at(voice); }
    int writeCursor(int voice) const { return write_.at(voice); }

private:
    static int ceilLog2(int n) {
        if (n == 1) return 0;
        int power = 2, result = 1;
        while (power < n) { power += power; ++result; }
        return result;
    }
    static int nextPowerOfTwo(int n) { return 1 << ceilLog2(n); }
    int index(int absoluteIndex) const { return absoluteIndex & ringMask_; }

    int detectPeriod(int start, bool stereo) {
        for (int j = 0; j < n_; ++j) {
            float x = source_(start + j, 0);
            if (stereo) x += source_(start + j, 1);
            work_[j] = x * window_[j];
        }
        forward_(work_.data(), log2N_);
        for (int i = 0; i < n_; i += 2) {
            const float im2 = work_[i + 1] * work_[i + 1];
            float re2 = work_[i] * work_[i];
            re2 += im2;
            work_[i] = re2;
            work_[i + 1] = 0.0f;
        }
        inverse_(work_.data(), log2N_);
        const float dt = 1.0f / static_cast<float>(n_);
        float t = 0.0f;
        for (int j = 0; j < n_ / 2; ++j) {
            const float z = t * t;
            t += dt;
            float d = -30.50055503845215f * z;
            d += 30.785877227783203f;
            d *= z;
            d += -9.794684410095215f;
            d *= z;
            d += 0.9989709854125977f;
            d *= 0.6200000047683716f;
            d += 0.3799999952316284f;
            work_[j] /= d;
        }
        int best = minLag_;
        float value = 0.0f;
        for (int j = minLag_; j < maxLag_; ++j) {
            if (work_[j] > value) best = j;
            // SSE maxss returns its second operand for unordered comparison.
            value = work_[j] > value ? work_[j] : value;
        }
        work_[0] = work_[1] = 100.0f;
        work_[best] = work_[best - 1] = -100.0f;
        return best;
    }

    void bracket(double anchor, bool stereo) {
        while (anchor > static_cast<double>(upper_)) {
            const int p = detectPeriod(upper_, stereo);
            lower_ = upper_;
            upper_ += p;
        }
        while (static_cast<double>(lower_) > anchor) {
            const int p = detectPeriod(upper_ - n_, stereo);
            upper_ = lower_;
            lower_ -= p;
        }
    }

    void fill(int voice, int target, int channels) {
        while (write_[voice] < target) {
            const int period = upper_ - lower_;
            if (lower_ != lastEpoch_[voice]) {
                for (int j = 0; j < fadeLength_; ++j) {
                    const int k = index(write_[voice] + j);
                    const float w = fade_[j];
                    const float other = 1.0f - w;
                    for (int c = 0; c < channels; ++c) {
                        const float oldValue = w * ring_[voice][c][k];
                        const float newValue = other * source_(lower_ + j, c);
                        ring_[voice][c][k] = newValue + oldValue;
                    }
                }
            }
            for (int j = 0; j < period; ++j)
                for (int c = 0; c < channels; ++c)
                    ring_[voice][c][index(write_[voice] + fadeLength_ + j)] = source_(lower_ + fadeLength_ + j, c);
            write_[voice] += period;
            lastEpoch_[voice] = upper_;
        }
    }

    FFT forward_, inverse_;
    Source source_;
    Cosine cosine_;
    int capacity_ = 0, ringMask_ = 0;
    int minLag_ = 0, maxLag_ = 0, fadeLength_ = 0, n_ = 0, log2N_ = 0;
    std::array<std::array<std::vector<float>, 2>, 2> ring_;
    std::vector<float> work_, window_, fade_;
    int lower_ = -1, upper_ = -1;
    std::array<int, 2> write_{-1, -1}, lastEpoch_{-1, -1};
    std::array<double, 2> position_{-1.0, -1.0};
};

} // namespace polar_recovered

#pragma once

// Portable 64-sample Polar reconstruction engine.
// Recovered: granular kernels, mode/Lock timing, control/gain smoothers,
// feedback topology and visible output-filter recurrences.
// Adapted: public parameter units/defaults, LFO, ADSR, manual tempo, bypass.
// The FFT algorithms are supplied through PitchRenderer by the application.
// Compile without fast-math/FMA contraction for instruction-order comparisons.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>
#include "bundled_math.hpp"

namespace polar_recovered {
namespace engine_detail {

constexpr std::size_t block_size = 64;
using Block = std::array<float, block_size>;
template<class T> inline T clamp(T x, T lo, T hi) {
    return std::max(lo, std::min(hi, x));
}
inline float add(float a, float b) { volatile float r = a + b; return r; }
inline float sub(float a, float b) { volatile float r = a - b; return r; }
inline float mul(float a, float b) { volatile float r = a * b; return r; }
inline float divide(float a, float b) { volatile float r = a / b; return r; }
inline std::size_t next_power_of_two(std::size_t n) {
    std::size_t p = 64;
    while (p < n) p <<= 1;
    return p;
}

// 0x206d0 scalar and 0x20b90 smoothed-gain recurrences. Each object must
// be evaluated once at its selected graph point, not once per consumer.
struct Smoother {
    float scalar_output = 0, position = 0, velocity = 0, settled_target = 0;
    float omega = 0;
    std::int32_t countdown = 0;
    std::uint32_t last_tick = static_cast<std::uint32_t>(-666);
    void set_rate(float rate) { omega = divide(mul(35.0f, 6.2831854820251465f), rate); }
    void reset() {
        scalar_output = position = velocity = 0;
        countdown = 0;
        settled_target = 0;
        last_tick = static_cast<std::uint32_t>(-666);
    }
    void begin(std::uint32_t tick, float target) {
        if (last_tick + 1u != tick) {
            position = settled_target = target;
            velocity = 0;
            countdown = 0;
        }
    }
    void step(float target, float w) {
        const float old_velocity = velocity;
        position = add(mul(old_velocity, w), position);
        velocity = add(mul(add(mul(-2.0f, old_velocity), sub(target, position)), w),
                       old_velocity);
    }
    void finish(float target, float previous) {
        if (position == previous) {
            countdown -= 64;
            if (countdown == 0) { position = target; velocity = 0; }
            settled_target = target;
        } else countdown = 128;
    }
    float scalar(std::uint32_t tick, float target) {
        begin(tick, target);
        if (countdown == 0 && !(target == scalar_output)) countdown = 128;
        if (countdown > 0) {
            const float previous = position;
            step(target, mul(omega, 64.0f));
            scalar_output = position; // original writes this before snap
            finish(target, previous);
        }
        last_tick = tick;
        return scalar_output;
    }
    void gain(std::uint32_t tick, float target, const float* input, float* output) {
        begin(tick, target);
        if (countdown == 0 && !(target == settled_target)) countdown = 128;
        if (countdown > 0) {
            const float previous = position;
            for (std::size_t i = 0; i < block_size; ++i) {
                step(target, omega);
                output[i] = mul(position, input[i]);
            }
            finish(target, previous); // does not refill already rendered audio
        } else {
            for (std::size_t i = 0; i < block_size; ++i) output[i] = mul(input[i], target);
        }
        last_tick = tick;
    }
};

struct Grain {
    bool active = false;
    double position = 0, lo = 0, hi = 0, fade = 0;
    float ratio = 0;
    void reset(double anchor) { active = false; position = lo = hi = anchor; }
    void rebase(double amount) { position -= amount; lo -= amount; hi -= amount; }
    void launch(double anchor, double span, double edge, float playback) {
        ratio = playback;
        fade = edge;
        lo = anchor - span - edge;
        hi = anchor;
        position = playback > 0.0f ? lo : hi;
        active = true;
    }
    std::pair<float, float> read(const float* left, const float* right,
                                 std::uint32_t mask, bool stereo) {
        if (!active) return {0, 0};
        const std::int64_t k = static_cast<std::int64_t>(position);
        const float f = static_cast<float>(position - static_cast<double>(static_cast<float>(k)));
        const std::uint32_t a = static_cast<std::uint32_t>(k - 1) & mask;
        const std::uint32_t b = static_cast<std::uint32_t>(k) & mask;
        const float old_weight = sub(1.0f, f);
        float l = add(mul(old_weight, left[a]), mul(f, left[b]));
        float r = stereo ? add(mul(old_weight, right[a]), mul(f, right[b])) : l;
        if (position < lo + fade) {
            const float weight = static_cast<float>((position - lo) / fade);
            l = mul(l, weight); r = mul(r, weight);
        } else if (position > hi - fade) {
            const float weight = static_cast<float>((hi - position) / fade);
            l = mul(l, weight); r = mul(r, weight);
        }
        position += static_cast<double>(ratio);
        if (position < lo || position > hi) active = false;
        return {l, r};
    }
};

struct GrainBank {
    std::array<Grain, 3> grains{};
    int index = 0;
    float counter = 0, threshold = 0;
    void reset(double anchor) {
        for (auto& grain : grains) grain.reset(anchor);
        index = 0; counter = threshold = 0;
    }
    void rebase(double amount) { for (auto& grain : grains) grain.rebase(amount); }
    std::pair<float, float> step(const float* l, const float* r, std::uint32_t mask,
                                bool stereo, double anchor, float ratio,
                                double span, double fade) {
        counter = add(counter, std::abs(ratio));
        if (counter > threshold) {
            counter = 0;
            threshold = static_cast<float>(span);
            if (!grains[index].active) grains[index].launch(anchor, span, fade, ratio);
            index = (index + 1) % 3;
        }
        const auto a = grains[0].read(l, r, mask, stereo);
        const auto b = grains[1].read(l, r, mask, stereo);
        const auto c = grains[2].read(l, r, mask, stereo);
        return {add(add(a.first, b.first), c.first), add(add(a.second, b.second), c.second)};
    }
};

// Original delay nodes173/174 select interpolation type0: one integer
// delay for each64-sample block. Buffer is written before delayed reads.
struct IntegerDelay {
    std::vector<float> ring;
    std::uint32_t mask = 0, cursor = 0;
    std::uint32_t last_tick = static_cast<std::uint32_t>(-666);
    void configure(float sample_rate) {
        ring.assign(next_power_of_two(static_cast<std::size_t>(sample_rate) + 128), 0);
        mask = static_cast<std::uint32_t>(ring.size() - 1); cursor = 0;
        last_tick = static_cast<std::uint32_t>(-666);
    }
    void reset() {
        std::fill(ring.begin(), ring.end(), 0); cursor = 0;
        last_tick = static_cast<std::uint32_t>(-666);
    }
    void process(const float* input, float* output, float frames, std::uint32_t tick) {
        // 0x17e5b..0x17e6e clears delay history when this branch has not
        // been pulled on the preceding graph tick.
        if (last_tick + 1u != tick) std::fill(ring.begin(), ring.end(), 0);
        const auto delay = static_cast<std::uint32_t>(clamp(frames, 0.0f,
                                      static_cast<float>(ring.size() - block_size)));
        for (std::size_t i = 0; i < block_size; ++i) ring[(cursor + i) & mask] = input[i];
        for (std::size_t i = 0; i < block_size; ++i) output[i] = ring[(cursor + i - delay) & mask];
        cursor += 64;
        last_tick = tick;
    }
};

struct OutputFilter {
    float low = 0, band = 0, comb_smooth = 0;
    std::array<float, 16384> comb{};
    std::uint32_t comb_cursor = 0;
    void reset() { low = band = comb_smooth = 0; comb.fill(0); comb_cursor = 0; }
    void process(const float* input, float* output, int mode, float cutoff,
                 float resonance, float sample_rate) {
        const float c = clamp(cutoff, -4.0f, 4.0f);
        const float r = clamp(resonance, 0.0f, 1.0f);
        if (mode == 3) {
            const float base = mul(divide(44100.0f, sample_rate),
                                    bundled::expf(mul(4.852030277252197f, c)));
            for (std::size_t i = 0; i < block_size; ++i) {
                comb_smooth = add(mul(0.9980000257492065f, comb_smooth),
                                  mul(0.0020000000949949026f, base));
                const float d = clamp(divide(168.56149291992188f, comb_smooth),
                                      1.0f, mul(0.08529999852180481f, sample_rate));
                const std::uint32_t k = static_cast<std::uint32_t>(d);
                const float a = sub(d, static_cast<float>(k));
                const float read = add(mul(sub(1.0f, a), comb[(comb_cursor - k) & 16383u]),
                                       mul(a, comb[(comb_cursor - k - 1u) & 16383u]));
                const float written = add(input[i], mul(mul(0.9959999918937683f, r), read));
                comb[comb_cursor++ & 16383u] = written;
                output[i] = add(written, read);
            }
        } else {
            const float gain = add(1.0f, mul(-0.6000000238418579f, r));
            const float f0 = std::min(2.0f, mul(divide(44100.0f, sample_rate),
                                bundled::expf(sub(mul(4.809999942779541f, c), 3.2920000553131104f))));
            const float damp = std::max(mul(0.15000000596046448f, f0),
                         std::min(sub(2.0f, mul(0.5f, f0)), sub(2.0f, mul(2.0f, r))));
            const float f = clamp(f0, 0.0f, divide(2.0f, add(1.0f, damp)));
            for (std::size_t i = 0; i < block_size; ++i) {
                low = add(low, mul(f, band));
                const float high = sub(sub(mul(gain, input[i]), low), mul(damp, band));
                band = add(band, mul(f, high));
                output[i] = mode == 0 ? low : (mode == 1 ? band : high);
            }
        }
        // Only the selected branch above advances its stored state.
    }
};

inline float soft_feedback(float x) {
    const float a = std::abs(x), t = 0.4000000059604645f;
    float y;
    if (a <= t) y = a;
    else if (a <= 1.0f) {
        const float d = sub(a, t);
        const float k = divide(1.0f, mul(2.0f, sub(1.0f, t)));
        y = sub(a, mul(mul(k, d), d));
    } else {
        const float tail = sub(1.0f, t);
        const float k = divide(1.0f, mul(2.0f, tail));
        y = sub(1.0f, mul(mul(tail, tail), k));
    }
    return std::copysign(y, x);
}

// Authored portable ADSR adapter. The original envelope class and Reason
// host/transport behavior are not represented as recovered equivalence.
struct Envelope {
    enum Stage { idle, attack_stage, decay_stage, sustain_stage, release_stage } stage = idle;
    float value = 0, release_step = 0;
    bool previous_gate = false;
    void reset() { stage = idle; value = release_step = 0; previous_gate = false; }
    float step(bool gate, float attack, float decay, float sustain, float release, float rate) {
        sustain = clamp(sustain, 0.0f, 1.0f);
        if (gate && !previous_gate) stage = attack_stage;
        if (!gate && previous_gate) {
            stage = release_stage;
            release_step = value / std::max(1.0f, release * rate);
        }
        previous_gate = gate;
        switch (stage) {
        case idle: value = 0; break;
        case attack_stage:
            value += 1.0f / std::max(1.0f, attack * rate);
            if (value >= 1) { value = 1; stage = decay_stage; }
            break;
        case decay_stage:
            value -= (1 - sustain) / std::max(1.0f, decay * rate);
            if (value <= sustain) { value = sustain; stage = sustain_stage; }
            break;
        case sustain_stage: value = sustain; break;
        case release_stage:
            value -= release_step;
            if (value <= 0) { value = 0; stage = idle; }
            break;
        }
        return value;
    }
};

} // namespace engine_detail

class PolarEngine {
public:
    static constexpr std::size_t block_size = 64;
    using Block = std::array<float, block_size>;
    using PitchOutput = std::array<Block, 4>; // voice1L/R, voice2L/R

    struct Parameters {
        // Defaults and units intentionally retain the authored v0.1 panel.
        int algorithm = 2;       // 0/1 recovered FFT families,2 forward,3 reverse
        int speed = 1;           // FFT speed selector0..2
        bool reverse = false;   // compatibility: overrides algorithm2 to3
        bool lock = false, stereo = false;
        float position = 0.20f, loop = 0.60f;
        float position_lfo = 0, position_env = 0;
        float pitch_1 = 0, tune_1 = 0, feedback_1 = 0, level_1 = 0.8f, pan_1 = -0.35f;
        float pitch_lfo_1 = 0, pitch_env_1 = 0;
        bool enable_2 = false;
        float pitch_2 = 7, tune_2 = 0, feedback_2 = 0, level_2 = 0.6f, pan_2 = 0.35f;
        float delay_2 = 0, pitch_lfo_2 = 0, pitch_env_2 = 0;
        bool filter_on = false;
        int filter_mode = 0;     // LP,BP,HP,comb
        float cutoff = 0.75f, resonance = 0.2f, filter_lfo = 0, filter_env = 0;
        float lfo_rate = 0.30f;
        bool lfo_sync = false;
        float tempo = 120, division = 1;
        int lfo_wave = 0;        // sine,triangle,saw,square (portable adapter)
        float lfo_phase = 0, pan_lfo = 0;
        bool gate = false;
        float attack = 0.02f, decay = 0.2f, sustain = 0.7f, release = 0.5f, env_to_amp = 0;
        float dry = 0, dry_delay = 0, output_db = -6;
        bool bypass = false;
        // Optional external scalar modulation for host adapters or validation.
        bool external_modulation = false;
        float external_lfo = 0, external_envelope = 0;
        // Host (DAW) modulation, added after the control smoothers where the LFO and
        // envelope enter, so it acts at once instead of through the 35 Hz knob smoothing.
        // Adapter: zero leaves the recovered signal path bit-identical.
        float pitch_mod_1 = 0, pitch_mod_2 = 0;    // semitones
        float position_mod = 0;                    // seconds
        float cutoff_mod = 0, resonance_mod = 0;   // normalized cutoff / resonance units
    };

    struct PitchRequest {
        int algorithm = 0, internal_mode = 0, speed = 0;
        float sample_rate = 48000;
        bool stereo = false, dual_voice = false;
        const float* ring_left = nullptr;
        const float* ring_right = nullptr;
        std::uint32_t ring_mask = 0, write_cursor = 8192;
        double anchor = 0, source_step = 1;
        float ratio_1 = 1, ratio_2 = 1;
        int fft_half_window = 1024;
        bool configuration_changed = false, reset = false;
        std::uint64_t reset_generation = 0;
        // Apply this to retained FFT absolute cursors/positions before render.
        int rebase_frames = 0;
    };
    using PitchRenderer = std::function<void(const PitchRequest&, PitchOutput&)>;

    explicit PolarEngine(float sample_rate = 48000.0f) { setSampleRate(sample_rate); }
    Parameters& parameters() { return parameters_; }
    const Parameters& parameters() const { return parameters_; }
    float sampleRate() const { return sample_rate_; }
    std::uint64_t blockCount() const { return blocks_; }
    std::uint32_t writeCursor() const { return write_cursor_; }
    bool captureLocked() const { return locked_; }
    int lockTransitionSamples() const { return transition_; }
    bool hasPitchRenderer() const { return static_cast<bool>(pitch_renderer_); }
    const PitchOutput& pitchOutput() const { return pitch_output_; }
    // Host transport sync for the (adapter) LFO: phase in cycles, applied to the next block.
    void setLfoPhase(double phase) { lfo_phase_ = phase - std::floor(phase); }
    void setPitchRenderer(PitchRenderer renderer) {
        pitch_renderer_ = std::move(renderer); ++reset_generation_;
    }

    void setSampleRate(float rate);
    void reset();
    void processBlock(const float* input_left, const float* input_right,
                      float* output_left, float* output_right);

private:
    enum ScalarControl { position_control, pitch1_control, pitch2_control,
                         cutoff_control, resonance_control, p2delay_control,
                         drydelay_control, scalar_control_count };
    enum GainControl { voice1_left_gain, voice1_right_gain, voice2_left_gain,
                       voice2_right_gain, dry_left_gain, dry_right_gain,
                       feedback1_left_gain, feedback1_right_gain,
                       feedback2_left_gain, feedback2_right_gain,
                       output_left_gain, output_right_gain, gain_control_count };
    Parameters parameters_{};
    float sample_rate_ = 48000;
    std::array<engine_detail::Smoother, scalar_control_count> scalar_controls_{};
    std::array<engine_detail::Smoother, gain_control_count> gain_controls_{};
    std::vector<float> ring_left_, ring_right_;
    std::uint32_t ring_mask_ = 0, write_cursor_ = 8192, tick_ = 0;
    std::uint64_t blocks_ = 0, reset_generation_ = 0;
    int transition_ = 256, previous_mode_ = -1, fft_half_window_ = 1024;
    bool locked_ = false;
    float previous_speed_ = 0.02f, min_offset_ = 64, max_offset_ = 144000;
    std::array<engine_detail::GrainBank, 2> grain_banks_{};
    std::array<engine_detail::IntegerDelay, 4> delays_{}; // P2L/R,dryL/R
    std::array<engine_detail::OutputFilter, 2> filters_{};
    engine_detail::Envelope envelope_{};
    double lfo_phase_ = 0;
    PitchRenderer pitch_renderer_{};
    PitchOutput pitch_output_{};
    void configureMode(int mode, float speed, double anchor);
    void renderGrains(int algorithm, bool stereo, bool dual, double anchor,
                      double source_step, float ratio1, float ratio2,
                      float loop_seconds, float position_seconds);
    void capture(const float* input_left, const float* input_right);
};

inline void PolarEngine::setSampleRate(float rate) {
    sample_rate_ = engine_detail::clamp(rate, 8000.0f, 384000.0f);
    const auto size = engine_detail::next_power_of_two(
                       static_cast<std::size_t>(sample_rate_ * 3.5f));
    ring_left_.assign(size, 0); ring_right_.assign(size, 0);
    ring_mask_ = static_cast<std::uint32_t>(size - 1);
    for (auto& s : scalar_controls_) s.set_rate(sample_rate_);
    for (auto& s : gain_controls_) s.set_rate(sample_rate_);
    for (auto& d : delays_) d.configure(sample_rate_);
    reset();
}

inline void PolarEngine::reset() {
    std::fill(ring_left_.begin(), ring_left_.end(), 0);
    std::fill(ring_right_.begin(), ring_right_.end(), 0);
    write_cursor_ = 8192; transition_ = 256; locked_ = false;
    previous_mode_ = -1; previous_speed_ = 0.02f;
    min_offset_ = 64; max_offset_ = sample_rate_ * 3;
    fft_half_window_ = 1024;
    tick_ = 0; blocks_ = 0; ++reset_generation_;
    for (auto& b : grain_banks_) b.reset(4096);
    for (auto& s : scalar_controls_) s.reset();
    for (auto& s : gain_controls_) s.reset();
    for (auto& d : delays_) d.reset();
    for (auto& f : filters_) f.reset();
    for (auto& channel : pitch_output_) channel.fill(0);
    envelope_.reset(); lfo_phase_ = 0;
    // Original sample-rate setup configures stored mode-1/speed0.02 even
    // before the first real mode is selected, establishing the old bounds.
    configureMode(-1, 0.02f, 4096.0);
}

inline void PolarEngine::configureMode(int mode, float speed, double anchor) {
    using namespace engine_detail;
    previous_speed_ = speed;
    if (mode <= 7) {
        const float frames = mul(speed, sample_rate_);
        const int desired = static_cast<int>(add(frames, frames));
        int n = desired == 1 ? 1 : 2;
        while (n < desired) n *= 2;
        fft_half_window_ = n / 2;
        min_offset_ = static_cast<float>(n + 8);
        max_offset_ = static_cast<float>(static_cast<int>(mul(sample_rate_, 3.5f)) - (n + 8));
    } else { min_offset_ = 64; max_offset_ = mul(3.0f, sample_rate_); }
    if (mode != previous_mode_) {
        previous_mode_ = mode;
        ++reset_generation_;
        for (auto& bank : grain_banks_) bank.reset(anchor);
    }
}

inline void PolarEngine::renderGrains(int algorithm, bool stereo, bool dual, double anchor,
                                     double source_step, float ratio1, float ratio2,
                                     float loop_seconds, float position_seconds) {
    using namespace engine_detail;
    const float fade_seconds = std::min(0.02f, mul(0.9f, loop_seconds));
    const double fade = static_cast<double>(mul(fade_seconds, sample_rate_));
    const auto span = [&](float ratio) {
        const float seconds = std::min(mul(sub(3.0f, position_seconds), std::abs(ratio)),
                                       add(loop_seconds, fade_seconds));
        return (static_cast<double>(seconds) - static_cast<double>(fade_seconds))
                * static_cast<double>(sample_rate_);
    };
    const double span1 = span(ratio1), span2 = span(ratio2);
    if (algorithm == 3) { ratio1 = -ratio1; ratio2 = -ratio2; }
    for (std::size_t i = 0; i < block_size; ++i) {
        const auto a = grain_banks_[0].step(ring_left_.data(), ring_right_.data(), ring_mask_,
                                           stereo, anchor, ratio1, span1, fade);
        pitch_output_[0][i] = a.first; pitch_output_[1][i] = a.second;
        if (dual) {
            const auto b = grain_banks_[1].step(ring_left_.data(), ring_right_.data(), ring_mask_,
                                               stereo, anchor, ratio2, span2, fade);
            pitch_output_[2][i] = b.first; pitch_output_[3][i] = b.second;
        }
        anchor += source_step;
    }
}

inline void PolarEngine::capture(const float* input_left, const float* input_right) {
    using namespace engine_detail;
    if (locked_ && transition_ > 255) return;
    const auto start = write_cursor_ + (locked_ ? transition_ : 0);
    if (!locked_ && transition_ <= 255) {
        float weight = mul(static_cast<float>(transition_), 1.0f / 256.0f);
        for (std::size_t i = 0; i < block_size; ++i) {
            const auto j = (start + i) & ring_mask_;
            const float old = sub(1.0f, weight);
            ring_left_[j] = add(mul(ring_left_[j], old), mul(input_left[i], weight));
            ring_right_[j] = add(mul(ring_right_[j], old), mul(input_right[i], weight));
            weight = add(weight, 1.0f / 256.0f);
        }
    } else {
        for (std::size_t i = 0; i < block_size; ++i) {
            ring_left_[(start + i) & ring_mask_] = input_left[i];
            ring_right_[(start + i) & ring_mask_] = input_right[i];
        }
    }
    if (!locked_) write_cursor_ += 64;
    if (transition_ <= 255) transition_ += 64;
}

inline void PolarEngine::processBlock(const float* input_left, const float* input_right,
                                      float* output_left, float* output_right) {
    using namespace engine_detail;
    const auto& p = parameters_;
    // Pd hosts may alias inlet/outlet vectors. Preserve live capture inputs
    // until the deferred write after output rendering.
    Block original_left, original_right;
    std::copy_n(input_left, block_size, original_left.begin());
    std::copy_n(input_right, block_size, original_right.begin());
    input_left = original_left.data(); input_right = original_right.data();
    const std::uint32_t tick = ++tick_;
    ++blocks_;
    int rebase_frames = 0;
    if ((tick & 127u) == 0 && write_cursor_ > 0x100000u) {
        rebase_frames = 0x80000;
        write_cursor_ -= static_cast<std::uint32_t>(rebase_frames);
        for (auto& bank : grain_banks_) bank.rebase(rebase_frames);
    }
    if (p.lock != locked_) { locked_ = p.lock; transition_ = 0; }

    // These modulation sources are explicit portable host adapters. The
    // instruction-derived pitch/filter equations below consume block scalars.
    Block envelope;
    for (std::size_t i = 0; i < block_size; ++i)
        envelope[i] = p.external_modulation ? clamp(p.external_envelope, 0.0f, 1.0f)
            : envelope_.step(p.gate, p.attack, p.decay, p.sustain, p.release, sample_rate_);
    double phase = lfo_phase_ + p.lfo_phase;
    phase -= std::floor(phase);
    float lfo;
    switch (clamp(p.lfo_wave, 0, 3)) {
    case 1: lfo = static_cast<float>(1.0 - 4.0 * std::abs(phase - 0.5)); break;
    case 2: lfo = static_cast<float>(2.0 * phase - 1.0); break;
    case 3: lfo = phase < 0.5 ? 1.0f : -1.0f; break;
    default: lfo = static_cast<float>(std::sin(6.2831853071795864769 * phase)); break;
    }
    if (p.external_modulation) lfo = clamp(p.external_lfo, -1.0f, 1.0f);
    const float lfo_rate = p.lfo_sync ? clamp(p.tempo, 1.0f, 999.0f) / 60.0f * p.division
                                     : clamp(p.lfo_rate, 0.02f, 20.0f);
    lfo_phase_ += static_cast<double>(lfo_rate) * block_size / sample_rate_;
    lfo_phase_ -= std::floor(lfo_phase_);
    const float env = envelope[0];

    const float octave1 = scalar_controls_[pitch1_control].scalar(tick,
                             divide(add(p.pitch_1, divide(p.tune_1, 100.0f)), 12.0f));
    const float octave2 = scalar_controls_[pitch2_control].scalar(tick,
                             divide(add(p.pitch_2, divide(p.tune_2, 100.0f)), 12.0f));
    const float mod1 = divide(add(add(mul(p.pitch_lfo_1, lfo), mul(p.pitch_env_1, env)), p.pitch_mod_1), 12.0f);
    const float mod2 = divide(add(add(mul(p.pitch_lfo_2, lfo), mul(p.pitch_env_2, env)), p.pitch_mod_2), 12.0f);
    const float ratio1 = bundled::expf(mul(0.6931471824645996f, clamp(add(octave1, mod1), -4.0f, 4.0f)));
    const float ratio2 = bundled::expf(mul(0.6931471824645996f, clamp(add(octave2, mod2), -4.0f, 4.0f)));
    const float base_position = scalar_controls_[position_control].scalar(tick, p.position);
    const float position = std::min(1.7000000476837158f,
                   add(add(add(base_position, mul(p.position_lfo, lfo)), mul(p.position_env, env)), p.position_mod));
    // Deliberately clamp with the PREVIOUS configuration. This ordering is
    // visible at1cdda..1ce83 and matters for the transition block's reset anchor.
    const float offset = clamp(mul(position, sample_rate_), min_offset_, max_offset_);
    const double anchor = std::max(0.0, static_cast<double>(
                               sub(static_cast<float>(write_cursor_), offset)));
    int algorithm = clamp(p.algorithm, 0, 3);
    if (algorithm == 2 && p.reverse) algorithm = 3;
    const int internal_mode = 4 * algorithm + 2 * static_cast<int>(p.enable_2)
                             + static_cast<int>(!p.stereo);
    const int speed_index = clamp(p.speed, 0, 2);
    const float loop_seconds = mul(1.0f / 128.0f,
                                  bundled::powf(128.0f, clamp(p.loop, 0.0f, 1.0f)));
    const float speed = algorithm >= 2 ? loop_seconds
                        : 0.042663998901844025f / static_cast<float>(1u << speed_index);
    const bool mode_changed = previous_mode_ != internal_mode;
    const bool configuration_changed = mode_changed || !(speed == previous_speed_);
    if (configuration_changed) configureMode(internal_mode, speed, anchor);
    for (auto& channel : pitch_output_) channel.fill(0);
    const double source_step = locked_ ? 0.0 : 1.0;
    if (algorithm >= 2) {
        renderGrains(algorithm, p.stereo, p.enable_2, anchor, source_step,
                     ratio1, ratio2, loop_seconds, position);
    } else if (pitch_renderer_) {
        PitchRequest request;
        request.algorithm = algorithm; request.internal_mode = internal_mode;
        request.speed = speed_index; request.sample_rate = sample_rate_;
        request.stereo = p.stereo; request.dual_voice = p.enable_2;
        request.ring_left = ring_left_.data(); request.ring_right = ring_right_.data();
        request.ring_mask = ring_mask_; request.write_cursor = write_cursor_;
        request.anchor = anchor; request.source_step = source_step;
        request.ratio_1 = ratio1; request.ratio_2 = ratio2;
        request.fft_half_window = fft_half_window_;
        request.configuration_changed = configuration_changed; request.reset = mode_changed;
        request.reset_generation = reset_generation_; request.rebase_frames = rebase_frames;
        pitch_renderer_(request, pitch_output_);
        if (!p.enable_2) { pitch_output_[2].fill(0); pitch_output_[3].fill(0); }
    }

    Block p2_left{}, p2_right{}, dry_left, dry_right;
    const bool capture_needed = !locked_ || transition_ <= 255;
    // P2 delay is shared by audible and feedback branches. A disabled P2
    // can retain a delay tail in feedback while its audible selector is off.
    if (p.enable_2 || capture_needed) {
        const float delay_ms = scalar_controls_[p2delay_control].scalar(tick,
                                      clamp(p.delay_2, 0.0f, 1000.0f));
        const float frames = mul(delay_ms, mul(sample_rate_, 0.001f));
        delays_[0].process(pitch_output_[2].data(), p2_left.data(), frames, tick);
        delays_[1].process(pitch_output_[3].data(), p2_right.data(), frames, tick);
    }
    const float dry_delay_ms = scalar_controls_[drydelay_control].scalar(tick,
                                      clamp(p.dry_delay, 0.0f, 1000.0f));
    const float dry_frames = mul(dry_delay_ms, mul(sample_rate_, 0.001f));
    delays_[2].process(input_left, dry_left.data(), dry_frames, tick);
    delays_[3].process(input_right, dry_right.data(), dry_frames, tick);

    // Original pan law is linear: level*(1-pan), level*pan. Here public
    // bipolar pan is converted to the original0..1 interval before that law.
    const float pan1 = clamp(mul(add(add(p.pan_1, mul(p.pan_lfo, lfo)), 1.0f), 0.5f), 0.0f, 1.0f);
    const float pan2 = clamp(mul(add(add(p.pan_2, mul(p.pan_lfo, lfo)), 1.0f), 0.5f), 0.0f, 1.0f);
    const float level1 = clamp(p.level_1, 0.0f, 1.0f), level2 = clamp(p.level_2, 0.0f, 1.0f);
    Block voice1_left, voice1_right, voice2_left{}, voice2_right{}, dry_gain_left, dry_gain_right;
    gain_controls_[voice1_left_gain].gain(tick, mul(sub(1.0f, pan1), level1),
                                         pitch_output_[0].data(), voice1_left.data());
    gain_controls_[voice1_right_gain].gain(tick, mul(level1, pan1),
                                          pitch_output_[1].data(), voice1_right.data());
    if (p.enable_2) {
        gain_controls_[voice2_left_gain].gain(tick, mul(sub(1.0f, pan2), level2),
                                             p2_left.data(), voice2_left.data());
        gain_controls_[voice2_right_gain].gain(tick, mul(level2, pan2),
                                              p2_right.data(), voice2_right.data());
    }
    gain_controls_[dry_left_gain].gain(tick, clamp(p.dry, 0.0f, 1.0f),
                                      dry_left.data(), dry_gain_left.data());
    gain_controls_[dry_right_gain].gain(tick, clamp(p.dry, 0.0f, 1.0f),
                                       dry_right.data(), dry_gain_right.data());
    Block mixed_left, mixed_right;
    const float env_amount = clamp(p.env_to_amp, 0.0f, 1.0f);
    for (std::size_t i = 0; i < block_size; ++i) {
        const float amp = add(sub(1.0f, env_amount), mul(env_amount, envelope[i]));
        mixed_left[i] = mul(add(voice1_left[i], voice2_left[i]), amp);
        mixed_right[i] = mul(add(voice1_right[i], voice2_right[i]), amp);
    }
    Block filtered_left, filtered_right;
    if (p.filter_on) {
        const float cutoff = scalar_controls_[cutoff_control].scalar(tick,
                         add(-0.30000001192092896f, mul(1.100000023841858f,
                                                     clamp(p.cutoff, 0.0f, 1.0f))));
        const float resonance = clamp(add(scalar_controls_[resonance_control].scalar(tick,
                                    clamp(p.resonance, 0.0f, 1.0f)), p.resonance_mod), 0.0f, 1.0f);
        const float c = add(add(add(cutoff, mul(p.filter_lfo, lfo)), mul(p.filter_env, env)),
                            mul(1.100000023841858f, p.cutoff_mod));
        const int filter_mode = clamp(p.filter_mode, 0, 3);
        filters_[0].process(mixed_left.data(), filtered_left.data(), filter_mode, c,
                            resonance, sample_rate_);
        filters_[1].process(mixed_right.data(), filtered_right.data(), filter_mode, c,
                            resonance, sample_rate_);
    } else { filtered_left = mixed_left; filtered_right = mixed_right; }
    // Graph sums55/56 add the dry path after the voice envelope/filter path.
    for (std::size_t i = 0; i < block_size; ++i) {
        filtered_left[i] = add(dry_gain_left[i], filtered_left[i]);
        filtered_right[i] = add(dry_gain_right[i], filtered_right[i]);
    }
    const float master = std::pow(10.0f, clamp(p.output_db, -60.0f, 6.0f) / 20.0f);
    Block final_left, final_right;
    gain_controls_[output_left_gain].gain(tick, master, filtered_left.data(), final_left.data());
    gain_controls_[output_right_gain].gain(tick, master, filtered_right.data(), final_right.data());
    for (std::size_t i = 0; i < block_size; ++i) {
        output_left[i] = p.bypass ? input_left[i] : final_left[i];
        output_right[i] = p.bypass ? input_right[i] : final_right[i];
    }

    // Host deferred action: capture follows sink rendering and uses CURRENT
    // pitch/delayed-P2 buffers. There is no added feedback-return delay.
    if (capture_needed) {
        Block fb1_left, fb1_right, fb2_left, fb2_right, capture_left, capture_right;
        const float fb1 = clamp(p.feedback_1, 0.0f, 0.99f), fb2 = clamp(p.feedback_2, 0.0f, 0.99f);
        gain_controls_[feedback1_left_gain].gain(tick, fb1, pitch_output_[0].data(), fb1_left.data());
        gain_controls_[feedback1_right_gain].gain(tick, fb1, pitch_output_[1].data(), fb1_right.data());
        gain_controls_[feedback2_left_gain].gain(tick, fb2, p2_left.data(), fb2_left.data());
        gain_controls_[feedback2_right_gain].gain(tick, fb2, p2_right.data(), fb2_right.data());
        for (std::size_t i = 0; i < block_size; ++i) {
            capture_left[i] = add(input_left[i], soft_feedback(add(fb1_left[i], fb2_left[i])));
            capture_right[i] = add(input_right[i], soft_feedback(add(fb1_right[i], fb2_right[i])));
        }
        capture(capture_left.data(), capture_right.data());
    }
}

} // namespace polar_recovered

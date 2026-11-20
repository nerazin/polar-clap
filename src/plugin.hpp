#pragma once
// Polar Reconstruction CLAP — plugin instance.
#include <atomic>
#include <cstdint>
#include <memory>
#include <clap/clap.h>

#include "params.hpp"
#include "polar_processor.hpp"

namespace polar_clap {

// Constant 64-frame latency: the engine consumes exactly 64-sample blocks, and
// the wrapper pre-fills its output ring with 64 frames on activation.
static constexpr uint32_t kLatencyFrames = 64;

// GUI -> audio thread: parameter gestures and values the host must hear about.
// Single producer (main thread), single consumer (process or flush).
struct GuiEvent { uint16_t type; uint32_t param; float value; };
class GuiEventQueue {
public:
    bool push(const GuiEvent& e) {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= kSize) return false;
        items_[w % kSize] = e;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(GuiEvent& e) {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        e = items_[r % kSize];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
private:
    static constexpr uint32_t kSize = 1024;
    GuiEvent items_[kSize]{};
    std::atomic<uint32_t> write_{0}, read_{0};
};

class PolarPlugin {
public:
    // Creates the CLAP C object; returns null on allocation failure.
    // The returned plugin_t is owned by the caller (host) and must be
    // destroyed via plugin->destroy(plugin).
    static clap_plugin_t* create(const clap_host_t* host);

    static PolarPlugin* fromClap(const clap_plugin_t* p) {
        return static_cast<PolarPlugin*>(p->plugin_data);
    }

    // ---- GUI-facing API (main thread) -------------------------------------
    uint32_t paramCount() const { return (uint32_t)PARAM_COUNT; }
    const ParamDesc& desc(uint32_t index) const { return kParams[index]; }
    // Effective value: the current base plus the offset modulation added in the engine's last
    // block, so a GUI edit never shows as a stale modulation arc.
    float uiValue(uint32_t index) const {
        return base_[index].load(std::memory_order_relaxed) + uiOffset_[index].load(std::memory_order_relaxed);
    }
    // Current host modulation offset for the UI ring display.
    float uiMod(uint32_t index) const { return mod_[index].load(std::memory_order_relaxed); }
    float uiBase(uint32_t index) const { return base_[index].load(std::memory_order_relaxed); }
    // Edits from the GUI. Wrap value changes in begin/end so hosts record them as
    // one gesture; uiTap sends begin, value and end for clicks and toggles.
    void uiBeginGesture(uint32_t index);
    void uiSetValue(uint32_t index, float value);
    void uiEndGesture(uint32_t index);
    void uiTap(uint32_t index, float value);
    // Reset Capture from the GUI: a momentary action, not a host-visible value change.
    void uiTriggerReset() { resetPending_.store(true); }
    // Tempo the LFO uses (host tempo when the host provides it).
    float uiTempo() const { return tempo_.load(std::memory_order_relaxed); }
    // Sounding MIDI key while one is held, else -1.
    int uiKey() const { return key_.load(std::memory_order_relaxed); }
    // LFO scope: the last cycle of LFO output in kScopeSize columns, indexed by cycle position.
    static constexpr int kScopeSize = 64;
    float uiScope(int column) const { return scope_[column].load(std::memory_order_relaxed); }
    float uiLfoPosition() const { return lfoPosition_.load(std::memory_order_relaxed); }

    polar_recovered::PolarProcessor& processor() { return processor_; }
    const clap_host_t* host() const { return host_; }

    // Opaque pointer used by the macOS GUI layer (PolarGuiController).
    void* guiState = nullptr;

    // Lifetime token for GUI objects that may outlive the plugin briefly
    // (e.g. a delayed trigger release retained by the runloop).
    std::shared_ptr<std::atomic<bool>> guiAlive = std::make_shared<std::atomic<bool>>(true);

private:
    PolarPlugin() = default;

public:
    // Internal: used by the C callbacks and the GUI layer. Do not call directly.
    bool init();
    ~PolarPlugin();

    void activateMain(double sampleRate);
    void deactivateMain();
    void process(const clap_process_t* process);
    void publishLfo();
    void flushParams(const clap_input_events_t* in, const clap_output_events_t* out);
    void resetEngine();
    bool saveState(const clap_ostream_t* s) const;
    bool loadState(const clap_istream_t* s);

    void handleEvent(const clap_event_header_t* ev);
    void handleNoteEvent(const clap_event_header_t* ev);
    void emitGuiEvents(const clap_output_events_t* out);
    void requestFlush();

    polar_recovered::PolarProcessor processor_{48000.0f};
    const clap_host_t* host_ = nullptr;
    const clap_host_params_t* hostParams_ = nullptr;

    // Parameter values. base_: host/GUI value; mod_: host modulation offsets.
    std::atomic<float> base_[PARAM_COUNT]{};
    std::atomic<float> mod_[PARAM_COUNT]{};
    std::atomic<float> uiOffset_[PARAM_COUNT]{};  // effective - base, published per block
    GuiEventQueue guiEvents_;
    std::atomic<float> tempo_{120.0f};
    std::atomic<int> key_{-1};
    std::atomic<float> scope_[kScopeSize]{};
    std::atomic<float> lfoPosition_{0.0f};
    int scopeColumn_ = -1;  // column written by the previous block (audio thread)
    // MIDI pitch bend (-1..1 = +-1 octave) and CLAP tuning expression (semitones), added to
    // the Pitch Wheel parameter. Audio thread only.
    float bend_ = 0.f, tuning_ = 0.f;

    // 64-frame input carry (engine contract: exactly 64 samples per call).
    float carryL_[64]{}, carryR_[64]{};
    int carryFill_ = 0;
    // Output ring; pre-filled with 64 zero frames on activate so the plugin
    // reports a constant 64-frame latency.
    float ringL_[128]{}, ringR_[128]{};
    int ringHead_ = 0, ringLen_ = 0;
    double sampleRate_ = 48000.0;

    std::atomic<bool> resetPending_{false};  // engine reset queued for the next block (GUI or host)
    bool resetLatched_ = false;  // trigger re-arm latch
    bool processing_ = false;
    bool failed_ = false;        // engine threw; emit silence

    // Filled in by create(); returned to the host.
    clap_plugin_t clap_{};
};

} // namespace polar_clap

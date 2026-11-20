#include "plugin.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "polar_engine.hpp"

#ifdef __APPLE__
#include "gui/polar_gui.hpp"
#endif

namespace polar_clap {

static constexpr uint32_t kBlockSize = 64;      // engine contract
static constexpr const char kStateMagic[] = "polar-clap-state";
static constexpr uint32_t kStateVersion = 1;

// ---------------------------------------------------------------- lifecycle

bool PolarPlugin::init() {
    float base[PARAM_COUNT], mod[PARAM_COUNT];
    for (int i = 0; i < PARAM_COUNT; ++i) {
        base[i] = kParams[i].def;
        mod[i] = 0.0f;
        base_[i].store(kParams[i].def, std::memory_order_relaxed);
        mod_[i].store(0.0f, std::memory_order_relaxed);
        ui_[i].store(kParams[i].def, std::memory_order_relaxed);
    }
    applyAllToEngine(base, mod, processor_.parameters());
    if (host_ && host_->get_extension)
        hostParams_ = static_cast<const clap_host_params_t*>(
            host_->get_extension(host_, CLAP_EXT_PARAMS));
    return true;
}

PolarPlugin::~PolarPlugin() {
#ifdef __APPLE__
    // Safety net: the host must destroy the GUI before the plugin, but never
    // let a live Cocoa controller outlive the C++ object it points at.
    gui::destroy(*this);
#endif
    guiAlive->store(false);
}

void PolarPlugin::activateMain(double sampleRate) {
    if (sampleRate > 0 && sampleRate < 4000000) {
        sampleRate_ = sampleRate;
        try {
            processor_.sampleRate((float)sampleRate);
        } catch (...) {
            failed_ = true;
            return;
        }
    }
    failed_ = false;
    resetEngine();
}

void PolarPlugin::deactivateMain() {
    processing_ = false;
}

void PolarPlugin::resetEngine() {
    try {
        processor_.reset();
    } catch (...) {
        failed_ = true;
    }
    carryFill_ = 0;
    ringHead_ = 0;
    ringLen_ = kBlockSize;  // pre-fill: constant 64-frame latency
    for (uint32_t i = 0; i < kBlockSize; ++i) { ringL_[i] = 0.f; ringR_[i] = 0.f; }
    resetPending_ = false;
    resetLatched_ = false;
}

// ---------------------------------------------------------------- params

void PolarPlugin::requestFlush() {
    if (hostParams_ && hostParams_->request_flush) hostParams_->request_flush(host_);
}

void PolarPlugin::uiBeginGesture(uint32_t index) {
    if (index >= (uint32_t)PARAM_COUNT) return;
    guiEvents_.push({(uint16_t)CLAP_EVENT_PARAM_GESTURE_BEGIN, index, 0.f});
    requestFlush();
}

void PolarPlugin::uiSetValue(uint32_t index, float value) {
    if (index >= (uint32_t)PARAM_COUNT) return;
    const float v = clampToParam(kParams[index], value);
    base_[index].store(v, std::memory_order_relaxed);  // the engine follows at once
    if (index == (uint32_t)P_RESET && v >= 0.5f) resetPending_ = true;
    guiEvents_.push({(uint16_t)CLAP_EVENT_PARAM_VALUE, index, v});
    requestFlush();
}

void PolarPlugin::uiEndGesture(uint32_t index) {
    if (index >= (uint32_t)PARAM_COUNT) return;
    guiEvents_.push({(uint16_t)CLAP_EVENT_PARAM_GESTURE_END, index, 0.f});
    requestFlush();
}

void PolarPlugin::uiTap(uint32_t index, float value) {
    uiBeginGesture(index);
    uiSetValue(index, value);
    uiEndGesture(index);
}

void PolarPlugin::handleEvent(const clap_event_header_t* ev) {
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (ev->type == CLAP_EVENT_PARAM_MOD) {
        const auto* pm = reinterpret_cast<const clap_event_param_mod_t*>(ev);
        if (pm->param_id >= (clap_id)PARAM_COUNT || !isModulatable(pm->param_id)) return;
        // Global modulation only: this is an effect without voices, so any
        // note/channel-specific amount also applies to the whole device.
        mod_[pm->param_id].store(std::isfinite(pm->amount) ? (float)pm->amount : 0.f, std::memory_order_relaxed);
        return;
    }
    if (ev->type != CLAP_EVENT_PARAM_VALUE) return;
    const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
    if (pv->param_id >= (clap_id)PARAM_COUNT || !std::isfinite(pv->value)) return;
    const uint32_t index = (uint32_t)pv->param_id;
    const ParamDesc& d = kParams[index];
    if (d.kind == kTrigger) {
        if (pv->value >= 0.5) {
            if (!resetLatched_) { resetPending_ = true; resetLatched_ = true; }
        } else {
            resetLatched_ = false;
        }
    }
    base_[index].store(clampToParam(d, (float)pv->value), std::memory_order_relaxed);
}

void PolarPlugin::emitGuiEvents(const clap_output_events_t* out) {
    GuiEvent e;
    while (guiEvents_.pop(e)) {
        if (!out || !out->try_push) continue;
        if (e.type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = e.param;
            ev.cookie = nullptr;
            ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;  // global, not per note
            ev.value = (double)e.value;
            out->try_push(out, &ev.header);
        } else {
            clap_event_param_gesture_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, e.type, 0};
            ev.param_id = e.param;
            out->try_push(out, &ev.header);
        }
    }
}

void PolarPlugin::flushParams(const clap_input_events_t* in, const clap_output_events_t* out) {
    if (in) {
        uint32_t n = in->size(in);
        for (uint32_t e = 0; e < n; ++e) handleEvent(in->get(in, e));
    }
    emitGuiEvents(out);
}

// ---------------------------------------------------------------- audio

static void applyParams(PolarPlugin& pl) {
    auto& ep = pl.processor().parameters();
    // Trigger: engine reset before this block's parameters are applied. The
    // engine keeps parameters; only DSP state (capture ring, cursors, filters)
    // is cleared, matching the Pd panel's `reset` message.
    if (pl.resetPending_) {
        try {
            pl.processor().reset();
        } catch (...) {
            pl.failed_ = true;
        }
        pl.resetPending_ = false;
    }
    float base[PARAM_COUNT], mod[PARAM_COUNT];
    for (int i = 0; i < PARAM_COUNT; ++i) {
        base[i] = pl.base_[i].load(std::memory_order_relaxed);
        mod[i] = pl.mod_[i].load(std::memory_order_relaxed);
    }
    applyAllToEngine(base, mod, ep);
    for (int i = 0; i < PARAM_COUNT; ++i) {
        const ParamDesc& d = kParams[i];
        float v = effectiveValue(d, base[i], mod[i]);
        if (d.kind == kInt) v = std::round(v);
        else if (d.kind != kFloat) v = v >= 0.5f ? 1.f : 0.f;
        pl.ui_[i].store(v, std::memory_order_relaxed);
    }
}

void PolarPlugin::process(const clap_process_t* pr) {
    const uint32_t frames = pr->frames_count;
    if (frames == 0) return;

    const float* inL = nullptr;
    const float* inR = nullptr;
    float* outL = nullptr;
    float* outR = nullptr;
    if (pr->audio_inputs_count > 0 && pr->audio_inputs && pr->audio_inputs[0].data32) {
        const clap_audio_buffer_t& b = pr->audio_inputs[0];
        inL = b.data32[0];
        inR = b.channel_count > 1 ? b.data32[1] : b.data32[0];
    }
    if (pr->audio_outputs_count > 0 && pr->audio_outputs && pr->audio_outputs[0].data32) {
        clap_audio_buffer_t& b = pr->audio_outputs[0];
        outL = b.data32[0];
        outR = b.channel_count > 1 ? b.data32[1] : b.data32[0];
    }
    if (!outL) return;
    if (!inL) { /* free-running render: feed silence */ }

    emitGuiEvents(pr->out_events);

    // Host transport drives the LFO's tempo sync: host tempo replaces the fallback
    // Tempo parameter, and while playing the phase locks to the song position.
    const clap_event_transport_t* tr = pr->transport;
    const bool hostTempo = tr && (tr->flags & CLAP_TRANSPORT_HAS_TEMPO) && tr->tempo > 0;
    const bool beatLock = hostTempo && (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
                          (tr->flags & CLAP_TRANSPORT_IS_PLAYING);
    const double beat0 = beatLock ? (double)tr->song_pos_beats / (double)CLAP_BEATTIME_FACTOR : 0.0;

    const uint32_t evCount = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
    uint32_t ev = 0;

    for (uint32_t i = 0; i < frames; ++i) {
        carryL_[carryFill_] = inL ? inL[i] : 0.f;
        carryR_[carryFill_] = inR ? inR[i] : 0.f;
        ++carryFill_;

        if (carryFill_ == (int)kBlockSize) {
            // All events scheduled at or before this frame take effect for the
            // block that is about to render (matches the original 64-frame panel).
            for (; ev < evCount; ++ev) {
                const clap_event_header_t* e = pr->in_events->get(pr->in_events, ev);
                if ((int32_t)i < (int32_t)e->time) break;
                handleEvent(e);
            }
            applyParams(*this);
            auto& ep = processor_.parameters();
            if (hostTempo) ep.tempo = (float)tr->tempo;
            tempo_.store(ep.tempo, std::memory_order_relaxed);
            if (beatLock && ep.lfo_sync) {
                // Beat at the first input frame of this engine block (may lie in the previous buffer).
                const double beat = beat0 + ((double)i - 63.0) / sampleRate_ * tr->tempo / 60.0;
                processor_.setLfoPhase(beat * (double)ep.division);
            }
            float bl[64], br[64];
            bool ok = true;
            try {
                processor_.processBlock(carryL_, carryR_, bl, br);
            } catch (...) {
                ok = false;
                failed_ = true;
            }
            if (ok) {
                for (int k = 0; k < (int)kBlockSize; ++k) {
                    ringL_[(ringHead_ + ringLen_) & 127] = bl[k];
                    ringR_[(ringHead_ + ringLen_) & 127] = br[k];
                    ++ringLen_;
                }
            } else {
                for (int k = 0; k < (int)kBlockSize; ++k) {
                    ringL_[(ringHead_ + ringLen_) & 127] = 0.f;
                    ringR_[(ringHead_ + ringLen_) & 127] = 0.f;
                    ++ringLen_;
                }
            }
            carryFill_ = 0;
        }

        // One output frame per input frame; the 64-frame pre-fill from
        // activate() is the reported plugin latency.
        outL[i] = ringL_[ringHead_];
        outR[i] = ringR_[ringHead_];
        ringHead_ = (ringHead_ + 1) & 127;
        --ringLen_;
    }

    // Events in the tail of a short buffer apply from the next block on.
    for (; ev < evCount; ++ev)
        handleEvent(pr->in_events->get(pr->in_events, ev));
}

// ---------------------------------------------------------------- state

bool PolarPlugin::saveState(const clap_ostream_t* s) const {
    if (!s || !s->write) return false;
    char line[160];
    int n = snprintf(line, sizeof(line), "%s %u\n", kStateMagic, kStateVersion);
    if (s->write(s, line, (uint64_t)n) != (int64_t)n) return false;
    for (int i = 0; i < PARAM_COUNT; ++i) {
        float v = base_[i].load(std::memory_order_relaxed);
        n = snprintf(line, sizeof(line), "%s %.9g\n", kParams[i].engine_name, (double)v);
        if (n <= 0 || s->write(s, line, (uint64_t)n) != (int64_t)n) return false;
    }
    return true;
}

bool PolarPlugin::loadState(const clap_istream_t* s) {
    if (!s || !s->read) return false;
    char buf[8192];
    size_t total = 0;
    for (;;) {
        int64_t got = s->read(s, buf + total, (uint64_t)(sizeof(buf) - 1 - total));
        if (got < 0) return false;
        if (got == 0) break;
        total += (size_t)got;
        if (total >= sizeof(buf) - 1) break;
    }
    buf[total] = 0;

    char name[64];
    double value;
    const char* cursor = buf;
    char magic[64];
    unsigned version = 0;
    if (sscanf(cursor, "%63s %u", magic, &version) != 2) return false;
    if (strcmp(magic, kStateMagic) != 0) return false;
    if (version != kStateVersion) return false;
    cursor = strchr(cursor, '\n');
    if (!cursor) return false;
    ++cursor;

    bool touched[PARAM_COUNT] = {};
    while (*cursor) {
        if (sscanf(cursor, "%63s %lf", name, &value) != 2) break;
        for (int i = 0; i < PARAM_COUNT; ++i) {
            if (!strcmp(name, kParams[i].engine_name)) {
                base_[i].store(clampToParam(kParams[i], (float)value), std::memory_order_relaxed);
                touched[i] = true;
                break;
            }
        }
        const char* nl = strchr(cursor, '\n');
        if (!nl) break;
        cursor = nl + 1;
    }
    // Any parameter absent from the chunk reverts to its default.
    for (int i = 0; i < PARAM_COUNT; ++i)
        if (!touched[i])
            base_[i].store(kParams[i].def, std::memory_order_relaxed);
    base_[(int)P_RESET].store(0.f, std::memory_order_relaxed);
    // state.load runs on the main thread: tell the host to re-read every value.
    if (hostParams_ && hostParams_->rescan) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

} // namespace polar_clap

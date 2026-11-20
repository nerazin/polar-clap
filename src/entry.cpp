// Polar Reconstruction CLAP — C ABI layer: plugin callbacks, extensions,
// factory and library entry point.
#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "plugin.hpp"
#include "params.hpp"

#ifdef __APPLE__
#include "gui/polar_gui.hpp"
#endif

using polar_clap::PolarPlugin;
using polar_clap::kParams;
using polar_clap::PARAM_COUNT;

static const char* const s_features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    CLAP_PLUGIN_FEATURE_PITCH_SHIFTER,
    CLAP_PLUGIN_FEATURE_GRANULAR,
    nullptr,
};

static const clap_plugin_descriptor_t s_descriptor = {
    CLAP_VERSION_INIT,                            // clap_version
    "com.polar-reconstruction.polar",             // id
    "Polar Reconstruction",                       // name
    "Polar Reconstruction Project",               // vendor
    "",                                           // url
    "",                                           // manual_url
    "",                                           // support_url
    "0.3.0",                                      // version
    "Spectral / periodic / granular pitch reconstruction", // description
    s_features,                                   // features
};

// ------------------------------------------------------------------ params

static uint32_t CLAP_ABI params_count(const clap_plugin_t*) {
    return (uint32_t)PARAM_COUNT;
}

static bool CLAP_ABI params_get_info(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (!info || index >= (uint32_t)PARAM_COUNT) return false;
    const polar_clap::ParamDesc& d = kParams[index];
    uint32_t flags = 0;
    if (d.kind != polar_clap::kFloat) {
        flags |= CLAP_PARAM_IS_STEPPED;
        if (d.enums) flags |= CLAP_PARAM_IS_ENUM;
    }
    if (d.kind != polar_clap::kTrigger) flags |= CLAP_PARAM_IS_AUTOMATABLE;
    // Every parameter except Reset and Bypass takes host modulation; stepped ones round
    // base + amount. Host modulators, the internal LFO and the envelope all stack.
    if (polar_clap::isModulatable(index)) flags |= CLAP_PARAM_IS_MODULATABLE;
    if (polar_clap::isHidden(index)) flags |= CLAP_PARAM_IS_HIDDEN;
    if (index == (uint32_t)polar_clap::P_BYPASS) flags |= CLAP_PARAM_IS_BYPASS;
    info->id = index;
    info->flags = flags;
    info->cookie = (void*)(uintptr_t)index;
    snprintf(info->module, sizeof(info->module), "%s", d.module);
    snprintf(info->name, sizeof(info->name), "%s", d.name);
    info->min_value = (double)d.min;
    info->max_value = (double)d.max;
    info->default_value = (double)d.def;
    return true;
}

static bool CLAP_ABI params_get_value(const clap_plugin_t* p, clap_id id, double* out) {
    auto* pl = PolarPlugin::fromClap(p);
    if (!pl || id >= (clap_id)PARAM_COUNT || !out) return false;
    *out = (double)polar_clap::clampToParam(kParams[id], pl->uiBase((uint32_t)id));
    return true;
}

static bool CLAP_ABI params_value_to_text(const clap_plugin_t*, clap_id id, double value,
                                          char* out, uint32_t out_capacity) {
    if (id >= (clap_id)PARAM_COUNT || !out || out_capacity == 0) return false;
    return polar_clap::formatValue(kParams[id], value, out, out_capacity);
}

static bool CLAP_ABI params_text_to_value(const clap_plugin_t*, clap_id id,
                                          const char* text, double* out) {
    if (id >= (clap_id)PARAM_COUNT || !out) return false;
    return polar_clap::parseValue(kParams[id], text, out);
}

static void CLAP_ABI params_flush(const clap_plugin_t* p,
                                  const clap_input_events_t* in,
                                  const clap_output_events_t* out) {
    PolarPlugin::fromClap(p)->flushParams(in, out);
}

static const clap_plugin_params_t s_params = {
    params_count,
    params_get_info,
    params_get_value,
    params_value_to_text,
    params_text_to_value,
    params_flush,
};

// ------------------------------------------------------------ audio ports

static uint32_t CLAP_ABI audio_ports_count(const clap_plugin_t*, bool is_input) {
    (void)is_input;
    return 1;
}

static bool CLAP_ABI audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                                     clap_audio_port_info_t* info) {
    if (index != 0 || !info) return false;
    info->id = 0;
    info->name[0] = 0;
    strncat(info->name, is_input ? "Input" : "Output", sizeof(info->name) - 1);
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
    audio_ports_count,
    audio_ports_get,
};

// --------------------------------------------------------- remote controls
// Bitwig shows these as the device's 8-knob pages, ready for its modulators.

struct RemotePage { const char* name; polar_clap::ParamId ids[CLAP_REMOTE_CONTROLS_COUNT]; };
static const RemotePage s_pages[] = {
    {"Main", {polar_clap::P_ALGORITHM, polar_clap::P_POSITION, polar_clap::P_LOOP, polar_clap::P_PITCH_1,
              polar_clap::P_PITCH_2, polar_clap::P_FEEDBACK_1, polar_clap::P_DRY, polar_clap::P_OUTPUT_DB}},
    {"Shifters", {polar_clap::P_PITCH_1, polar_clap::P_TUNE_1, polar_clap::P_LEVEL_1, polar_clap::P_PAN_1,
                  polar_clap::P_PITCH_2, polar_clap::P_TUNE_2, polar_clap::P_LEVEL_2, polar_clap::P_PAN_2}},
    {"Feedback & Delay", {polar_clap::P_FEEDBACK_1, polar_clap::P_FEEDBACK_2, polar_clap::P_DELAY_2, polar_clap::P_ENABLE_2,
                          polar_clap::P_LOCK, polar_clap::P_STEREO, polar_clap::P_SPEED, polar_clap::P_DRY_DELAY}},
    {"Filter", {polar_clap::P_FILTER_ON, polar_clap::P_FILTER_MODE, polar_clap::P_CUTOFF, polar_clap::P_RESONANCE,
                polar_clap::P_FILTER_LFO, polar_clap::P_FILTER_ENV, polar_clap::P_ENV_TO_AMP, polar_clap::P_OUTPUT_DB}},
    {"LFO", {polar_clap::P_LFO_RATE, polar_clap::P_LFO_SYNC, polar_clap::P_DIVISION, polar_clap::P_LFO_WAVE,
             polar_clap::P_PITCH_LFO_1, polar_clap::P_PITCH_LFO_2, polar_clap::P_POSITION_LFO, polar_clap::P_PAN_LFO}},
    {"Envelope", {polar_clap::P_GATE, polar_clap::P_ATTACK, polar_clap::P_DECAY, polar_clap::P_SUSTAIN,
                  polar_clap::P_RELEASE, polar_clap::P_PITCH_ENV_1, polar_clap::P_PITCH_ENV_2, polar_clap::P_POSITION_ENV}},
};

static uint32_t CLAP_ABI remote_count(const clap_plugin_t*) { return sizeof(s_pages) / sizeof(s_pages[0]); }

static bool CLAP_ABI remote_get(const clap_plugin_t*, uint32_t index, clap_remote_controls_page_t* page) {
    if (!page || index >= remote_count(nullptr)) return false;
    const RemotePage& p = s_pages[index];
    snprintf(page->section_name, sizeof(page->section_name), "%s", "Polar");
    page->page_id = index;
    snprintf(page->page_name, sizeof(page->page_name), "%s", p.name);
    for (uint32_t i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; ++i) page->param_ids[i] = (clap_id)p.ids[i];
    page->is_for_preset = false;
    return true;
}

static const clap_plugin_remote_controls_t s_remote = { remote_count, remote_get };

// ---------------------------------------------------------------- latency

static uint32_t CLAP_ABI latency_get(const clap_plugin_t*) {
    return polar_clap::kLatencyFrames;
}

static const clap_plugin_latency_t s_latency = { latency_get };

// ------------------------------------------------------------------ state

static bool CLAP_ABI state_save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    return PolarPlugin::fromClap(p)->saveState(stream);
}

static bool CLAP_ABI state_load(const clap_plugin_t* p, const clap_istream_t* stream) {
    return PolarPlugin::fromClap(p)->loadState(stream);
}

static const clap_plugin_state_t s_state = { state_save, state_load };

// -------------------------------------------------------------------- gui

#ifdef __APPLE__

static bool CLAP_ABI gui_is_api_supported(const clap_plugin_t*, const char* api, bool) {
    return api && !strcmp(api, CLAP_WINDOW_API_COCOA);
}

static bool CLAP_ABI gui_get_preferred_api(const clap_plugin_t*, const char** api,
                                           bool* is_floating) {
    if (!api || !is_floating) return false;
    *api = CLAP_WINDOW_API_COCOA;
    *is_floating = false;
    return true;
}

static bool CLAP_ABI gui_create(const clap_plugin_t* p, const char* api, bool) {
    if (!api || strcmp(api, CLAP_WINDOW_API_COCOA) != 0) return false;
    return polar_clap::gui::create(*PolarPlugin::fromClap(p));
}

static void CLAP_ABI gui_destroy(const clap_plugin_t* p) {
    polar_clap::gui::destroy(*PolarPlugin::fromClap(p));
}

static bool CLAP_ABI gui_set_scale(const clap_plugin_t*, double) { return false; }

static bool CLAP_ABI gui_get_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    return polar_clap::gui::getSize(*PolarPlugin::fromClap(p), w, h);
}

static bool CLAP_ABI gui_can_resize(const clap_plugin_t*) { return false; }

static bool CLAP_ABI gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t*) {
    return false;
}

static bool CLAP_ABI gui_adjust_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    return polar_clap::gui::getSize(*PolarPlugin::fromClap(p), w, h);
}

static bool CLAP_ABI gui_set_size(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    return polar_clap::gui::setSize(*PolarPlugin::fromClap(p), w, h);
}

static bool CLAP_ABI gui_set_parent(const clap_plugin_t* p, const clap_window_t* window) {
    if (!window || !window->cocoa) return false;
    return polar_clap::gui::setParent(*PolarPlugin::fromClap(p), window->cocoa);
}

static bool CLAP_ABI gui_set_transient(const clap_plugin_t*, const clap_window_t*) { return false; }

static void CLAP_ABI gui_suggest_title(const clap_plugin_t*, const char*) {}

static bool CLAP_ABI gui_show(const clap_plugin_t* p) {
    return polar_clap::gui::show(*PolarPlugin::fromClap(p), true);
}

static bool CLAP_ABI gui_hide(const clap_plugin_t* p) {
    return polar_clap::gui::show(*PolarPlugin::fromClap(p), false);
}

static const clap_plugin_gui_t s_gui = {
    gui_is_api_supported,
    gui_get_preferred_api,
    gui_create,
    gui_destroy,
    gui_set_scale,
    gui_get_size,
    gui_can_resize,
    gui_get_resize_hints,
    gui_adjust_size,
    gui_set_size,
    gui_set_parent,
    gui_set_transient,
    gui_suggest_title,
    gui_show,
    gui_hide,
};

#endif // __APPLE__

// ----------------------------------------------------------- plugin vtable

static bool CLAP_ABI plugin_init(const clap_plugin_t* p) {
    return PolarPlugin::fromClap(p)->init();
}

static void CLAP_ABI plugin_destroy(const clap_plugin_t* p) {
    PolarPlugin* pl = PolarPlugin::fromClap(p);
    delete pl;
}

static bool CLAP_ABI plugin_activate(const clap_plugin_t* p, double sample_rate,
                                     uint32_t, uint32_t) {
    PolarPlugin::fromClap(p)->activateMain(sample_rate);
    return true;
}

static void CLAP_ABI plugin_deactivate(const clap_plugin_t* p) {
    PolarPlugin::fromClap(p)->deactivateMain();
}

static bool CLAP_ABI plugin_start_processing(const clap_plugin_t* p) {
    PolarPlugin::fromClap(p)->processing_ = true;
    return true;
}

static void CLAP_ABI plugin_stop_processing(const clap_plugin_t* p) {
    PolarPlugin::fromClap(p)->processing_ = false;
}

static void CLAP_ABI plugin_reset(const clap_plugin_t*) {
    // Deliberate no-op: the capture buffer holds audio material, not
    // timeline state; transport jumps must not clear it. Use the
    // "Reset Capture" parameter to clear deliberately.
}

static clap_process_status CLAP_ABI plugin_process(const clap_plugin_t* p,
                                                   const clap_process_t* process) {
    PolarPlugin::fromClap(p)->process(process);
    return CLAP_PROCESS_CONTINUE;
}

static const void* CLAP_ABI plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &s_remote;
#ifdef __APPLE__
    if (!strcmp(id, CLAP_EXT_GUI)) return &s_gui;
#endif
    return nullptr;
}

static void CLAP_ABI plugin_on_main_thread(const clap_plugin_t*) {}

clap_plugin_t* PolarPlugin::create(const clap_host_t* host) {
    if (!host || !host->get_extension || !host->request_callback) return nullptr;
    PolarPlugin* pl = new (std::nothrow) PolarPlugin();
    if (!pl) return nullptr;
    pl->host_ = host;
    pl->clap_.desc = &s_descriptor;
    pl->clap_.plugin_data = pl;
    pl->clap_.init = plugin_init;
    pl->clap_.destroy = plugin_destroy;
    pl->clap_.activate = plugin_activate;
    pl->clap_.deactivate = plugin_deactivate;
    pl->clap_.start_processing = plugin_start_processing;
    pl->clap_.stop_processing = plugin_stop_processing;
    pl->clap_.reset = plugin_reset;
    pl->clap_.process = plugin_process;
    pl->clap_.get_extension = plugin_get_extension;
    pl->clap_.on_main_thread = plugin_on_main_thread;
    return &pl->clap_;
}

// ----------------------------------------------------------------- factory

static uint32_t CLAP_ABI factory_plugin_count(const clap_plugin_factory_t*) { return 1; }

static const clap_plugin_descriptor_t* CLAP_ABI factory_describe(
    const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &s_descriptor : nullptr;
}

static const clap_plugin_t* CLAP_ABI factory_create(const clap_plugin_factory_t*,
                                                    const clap_host_t* host,
                                                    const char* plugin_id) {
    if (!plugin_id || strcmp(plugin_id, s_descriptor.id) != 0) return nullptr;
    return PolarPlugin::create(host);
}
static const clap_plugin_factory_t s_factory = {
    factory_plugin_count,
    factory_describe,
    factory_create,
};

// ------------------------------------------------------------------- entry

static bool CLAP_ABI entry_init(const char*) { return true; }
static void CLAP_ABI entry_deinit() {}

static const void* CLAP_ABI entry_get_factory(const char* factory_id) {
    if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
    return nullptr;
}

// The symbol hosts resolve with dlsym() after loading the binary.
// clap-sdk/include/clap/entry.h:
//     CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;
// It MUST be called "clap_entry" — exporting it under the struct's name
// (clap_plugin_entry) makes hosts reject the library with
// "Not a plug-in file" (Bitwig) because the symbol lookup fails.
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    entry_init,
    entry_deinit,
    entry_get_factory,
};

// Compatibility alias under the struct's type name, for hosts that
// predate the final symbol name. Both names resolve to the same table.
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_plugin_entry = clap_entry;

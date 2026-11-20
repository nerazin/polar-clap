// Renders the plugin GUI to a PNG (for review without a host):
//   polar_gui_snapshot out.png [modulated]
// "modulated" applies a few host modulation amounts first, to show the violet rings.
#import <AppKit/AppKit.h>
#include <clap/clap.h>
#include <cstdio>
#include <cstring>
#include "plugin.hpp"
#include "gui/polar_gui.hpp"

static const void* CLAP_ABI ext(const clap_host_t*, const char*) { return nullptr; }
static void CLAP_ABI noop(const clap_host_t*) {}
static const clap_host_t kHost = {CLAP_VERSION, nullptr, "snapshot", "test", "", "1.0", ext, noop, noop, noop};

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s out.png [modulated]\n", argv[0]); return 2; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        clap_plugin_t* cp = polar_clap::PolarPlugin::create(&kHost);
        cp->init(cp);
        auto* pl = polar_clap::PolarPlugin::fromClap(cp);
        cp->activate(cp, 48000, 1, 512);
        if (argc > 2 && !strcmp(argv[2], "modulated")) {
            const struct { polar_clap::ParamId id; double amount; } mods[] = {
                {polar_clap::P_PITCH_1, 9.0}, {polar_clap::P_POSITION, 0.35}, {polar_clap::P_CUTOFF, -0.3},
                {polar_clap::P_ALGORITHM, 1.0}, {polar_clap::P_LEVEL_2, -0.25}};
            for (auto& m : mods) {
                clap_event_param_mod_t ev{};
                ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
                ev.param_id = m.id; ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.amount = m.amount;
                pl->handleEvent(&ev.header);
            }
            pl->uiSetValue(polar_clap::P_ENABLE_2, 1);
            pl->uiSetValue(polar_clap::P_FILTER_ON, 1);
            pl->uiSetValue(polar_clap::P_PITCH_LFO_1, 3);
            pl->uiSetValue(polar_clap::P_LOCK, 1);
        }
        float z[64] = {}, oL[64], oR[64];
        float* in[2] = {z, z}; float* out[2] = {oL, oR};
        clap_audio_buffer_t ib{}, ob{}; ib.data32 = in; ib.channel_count = 2; ob.data32 = out; ob.channel_count = 2;
        clap_process_t pr{}; pr.frames_count = 64; pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        for (int k = 0; k < 3000; ++k) pl->process(&pr);  // effective values, and a full LFO cycle for the scope
        if (!polar_clap::gui::create(*pl) || !polar_clap::gui::snapshot(*pl, argv[1])) { printf("snapshot failed\n"); return 1; }
        polar_clap::gui::destroy(*pl);
        cp->deactivate(cp);
        cp->destroy(cp);
    }
    printf("wrote %s\n", argv[1]);
    return 0;
}

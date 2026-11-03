#pragma once
// macOS Cocoa GUI for the Polar Reconstruction CLAP plugin.
// Implemented in polar_gui.mm; compiled only on Apple platforms.

#include <cstdint>

namespace polar_clap {

class PolarPlugin;

namespace gui {

bool create(PolarPlugin& plugin);                       // build view tree
void destroy(PolarPlugin& plugin);
bool getSize(PolarPlugin& plugin, uint32_t* w, uint32_t* h);
bool setSize(PolarPlugin& plugin, uint32_t w, uint32_t h);
bool setParent(PolarPlugin& plugin, const void* nsView); // NSView*
bool show(PolarPlugin& plugin, bool shown);

} // namespace gui
} // namespace polar_clap

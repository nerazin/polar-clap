# Polar Reconstruction — CLAP plugin

A CLAP audio plugin wrapping the recovered **Polar** DSP engine (Polar
reconstruction 0.2): spectral and periodic FFT pitch shifting, forward and
reverse granular playback, dual pitch voices with feedback, filtering, LFO
and envelope modulation — the same C++17 engine as the Pure Data external,
now loadable directly as an instrument-effect device in Bitwig Studio and
other CLAP hosts.

- **Plugin ID:** `com.polar-reconstruction.polar`
- **Version:** 0.2.0
- **Ports:** stereo in / stereo out
- **Latency:** constant 64 samples (reported to the host; Bitwig compensates)
- **GUI:** built-in macOS Cocoa panel (knob grid); Bitwig's generic panel
  shows the same parameters grouped by module

## Build & install (macOS, Apple Silicon)

One command:

```sh
sh build-mac.sh
```

This configures CMake for `arm64`, builds the release bundle, installs it to
`~/Library/Audio/Plug-Ins/CLAP/Polar Reconstruction.clap`, applies an
ad-hoc code signature and then **verifies the installed bundle the way a
host loads it** (Info.plist → executable → exported `clap_entry` symbol →
code signature → a live `dlopen` + factory query). If all checks pass, the
bundle will load; if any step fails the script says exactly what and why.
Restart Bitwig, then add **Polar Reconstruction** from the Audio Effects
category.

Requirements:

- macOS 11 or newer on Apple Silicon (arm64)
- Apple Command Line Tools: `xcode-select --install`
- CMake 3.16+: `brew install cmake`

Manual build, if you prefer:

```sh
cmake -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-mac --parallel
cp -R build-mac/PolarReconstruction.clap ~/Library/Audio/Plug-Ins/CLAP/
```

Uninstall: `rm -rf ~/Library/Audio/Plug-Ins/CLAP/"Polar Reconstruction.clap"`

> For an Intel or Universal 2 build, change `-DCMAKE_OSX_ARCHITECTURES` to
> `x86_64` or `"arm64;x86_64"`. Everything else is identical.

### Using it in Bitwig

1. Restart Bitwig so the plugin is scanned (Bitwig has loaded CLAP since 2022;
   any current version works).
2. Add **Polar Reconstruction** as an audio FX device on any track.
3. Let audio flow for a moment (the engine captures up to 3 seconds), then
   change **Pitch**, switch **Algorithm**, or engage **Lock** to freeze the
   captured material.
4. Every float parameter accepts Bitwig modulators (see *Modulation* below).

## Troubleshooting

**"Not a plug-in file" when scanning / drag-and-drop.**
This Bitwig error means the bundle was found and its binary loaded, but the
CLAP entry point could not be resolved. Version 0.2.0 of this wrapper
exported the entry point under the wrong name (`clap_plugin_entry` instead
of the spec-mandated `clap_entry`, see `clap-sdk/include/clap/entry.h`) —
fixed in the current source; both symbols are now exported. If you ever see
this error again, check the binary directly:

```sh
nm -gU ~/Library/Audio/Plug-Ins/CLAP/"Polar Reconstruction.clap"/Contents/MacOS/PolarReconstruction | grep clap_entry
```

A line containing `_clap_entry` must appear. Rebuild with `sh build-mac.sh`
if in doubt — its verification stage fails loudly on this.

**Plugin does not appear in the browser.** Restart Bitwig (its indexer
re-reads CLAP bundles at startup), or force a re-scan in
*Settings → Plug-ins → Re-scan All*. Bitwig reports individual load failures
under the plug-in browser's error button rather than in a dialog.

**"Could not load executable"** (a different Bitwig error) means the binary
itself failed to `dlopen`: wrong architecture, broken signature or missing
dependencies. Re-run `sh build-mac.sh`, which now checks the signature and
actually loads the binary after installing.

## Controls

All values take effect at the next 64-sample boundary, exactly like the
original Pd panel. Group = CLAP module / GUI section.

| Group | Parameter | Range / values |
| --- | --- | --- |
| Algorithm | Algorithm | Spectral · Periodic · Forward · Reverse |
| Algorithm | FFT Speed | Slow · Medium · Fast |
| Algorithm | Force Reverse | on/off (compat: algorithm 2 → 3) |
| Algorithm | Stereo | on/off capture |
| Algorithm | Lock Capture | on/off |
| Algorithm | Reset Capture | trigger: clears buffer + running state |
| Grain | Position | 0 … 1.7 s |
| Grain | Loop Length | 0 … 1 (normalized; ~7.8 ms … 1 s exponential) |
| Grain | Position LFO / Env | ±0.5 s modulation depth |
| Voice 1 | Pitch 1 | ±36 st |
| Voice 1 | Tune 1 | ±100 ct |
| Voice 1 | Level 1 | 0 … 100 % |
| Voice 1 | Pan 1 | L … R |
| Voice 1 | Feedback 1 | 0 … 99 % |
| Voice 1 | Pitch LFO / Env 1 | ±24 st modulation depth |
| Voice 2 | Enable Voice 2 | on/off |
| Voice 2 | Pitch 2 | ±36 st (default +7) |
| Voice 2 | Tune 2 | ±100 ct |
| Voice 2 | Level 2 | 0 … 100 % |
| Voice 2 | Pan 2 | L … R |
| Voice 2 | Feedback 2 | 0 … 99 % |
| Voice 2 | Delay 2 | 0 … 1000 ms |
| Voice 2 | Pitch LFO / Env 2 | ±24 st modulation depth |
| Filter | Filter On / Mode | LP · BP · HP · Comb |
| Filter | Cutoff / Resonance | 0 … 100 % (normalized) |
| Filter | Filter LFO / Env | ±100 % modulation depth |
| LFO | Rate | 0.02 … 20 Hz |
| LFO | Sync | on/off (uses manual Tempo/Division; no DAW transport link) |
| LFO | Tempo / Division | 40 … 250 BPM / 0.25 … 8 cyc-per-beat |
| LFO | Wave | Sine · Triangle · Saw · Square |
| LFO | Phase / Pan LFO | 0 … 100 % / 0 … 100 % depth |
| Envelope | Gate | on/off (manual ADSR gate) |
| Envelope | Attack / Decay / Release | 0 … 2 s |
| Envelope | Sustain | 0 … 100 % |
| Envelope | Env to Amp | 0 … 100 % |
| Output | Dry / Dry Delay | 0 … 100 % / 0 … 1000 ms |
| Output | Output | −60 … +6 dB |
| Output | Bypass | on/off — passes input while processing continues |
| External | External Mod / LFO / Env | block-rate external scalar inputs (parity with the Pd panel) |

Behaviour notes:

- **Modulation.** Stepped parameters (switches, enums) do not accept
  fractional modulation, matching the engine's integer controls. All float
  parameters report `IS_MODULATABLE`; the mod amount is added to the base
  value and clamped to the parameter range. The GUI draws modulation as a
  violet arc around the knob.
- **Bypass** follows the CLAP bypass parameter: audio passes through while
  the engine keeps processing (preserving the captured buffer), identical to
  the original panel's bypass.
- **Host transport resets are ignored** on purpose: the capture buffer holds
  audio material, not timeline state. Use *Reset Capture* to clear it.
- **State.** Project state (all parameters) is saved and restored with the
  Bitwig project / presets. Runtime capture content is intentionally not
  part of the state chunk.
- **Failure safety.** Any internal engine exception results in silence for
  that block rather than a host crash; the engine retries on the next block.

## Validation

The wrapper was developed and validated on Linux against the engine directly
(the DSP code is platform-independent C++17; only the GUI layer is
macOS-specific and was reviewed but not executed here):

- **Entry-point loading:** the black-box harness resolves the plugin through
  `dlsym("clap_entry")` — the exact symbol name real hosts look up — so a
  wrongly-named entry symbol fails the suite instead of passing.
- **Bit-exact equivalence:** the plugin, loaded through the plain CLAP C ABI
  (`dlopen` + factory) with random host buffer sizes (1 … 700 frames),
  parameter events at arbitrary sample offsets, param-mod events, bypass
  toggles and reset triggers, produces **bit-identical** output to a direct
  instance of `PolarProcessor` driven in exact 64-sample blocks — across a
  30-second randomized soak (1.44 M frames) plus scripted scenarios
  (silence, defaults, algorithm/speed/window changes, lock transitions,
  filter modes, dual voices, feedback, dry path, output gain changes).
- **Parameter table** cross-checked against the engine's public
  `set(name, value)` string API (bit-exact audio after identical input).
- **State round-trip**, parameter text formatting/parsing, params_flush
  dirty tracking, latency (64) and stereo port declaration all covered by
  automated tests.
- **Address/UB sanitizers** pass on the full suite (LeakSanitizer disabled
  in this environment).
- **Original engine test suite** (`test_engine`, `test_complete`,
  `test_transition_reference` from the Polar reconstruction archive) passes
  against the `engine/` sources in this project, confirming they are
  unchanged.

To run the same suite on Linux:

```sh
cmake -B build-test -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-test
./build-test/polar_whitebox_test
./build-test/polar_clap_test build-test/libPolarReconstruction.clap
```

macOS-specific (Cocoa GUI, bundle packaging, code signing) was built to
Apple's documented APIs but could not be executed in this Linux
environment; if anything looks off, file the symptom alongside
`build-mac.sh` output.

## Scope

This wraps the independently reconstructed engine ("Polar reconstruction
0.2"). Per that project's README: the FFT comparisons were validated
conditional on the shared FFT adapter and do not establish full Reason-host
equivalence; the packed `Polar.data` resources, original factory presets
and original UI are not reconstructed; panel defaults, LFO/ADSR generators,
manual tempo and units are authored host adapters. LFO Sync uses the manual
Tempo parameter — there is no automatic DAW transport connection. See
`EVIDENCE.md` in the original archive for provenance details.

## Project layout

```
CMakeLists.txt          build for macOS (.clap bundle) and Linux (.clap + tests)
build-mac.sh            one-command Apple Silicon build + install
engine/                 recovered DSP engine, unchanged from Polar_Complete/src
src/params.{hpp,cpp}    parameter model shared by core, GUI and tests
src/plugin.{hpp,cpp}    plugin instance: buffering, params, state, process
src/entry.cpp           CLAP C ABI: callbacks, extensions, factory, entry
src/gui/polar_gui.{hpp,mm}  macOS Cocoa panel
clap-sdk/               vendored CLAP 1.2 headers (MIT)
tests/host_smoke.cpp    black-box host harness + bit-exact engine comparison
tests/whitebox.cpp      C++-level unit tests
cmake/Info.plist.in     bundle metadata
```

## Licenses

- CLAP SDK headers: MIT (see `clap-sdk/LICENSE`).
- Engine and wrapper: part of the Polar reconstruction project; see
  `THIRD_PARTY_NOTICES.md` and the original archive's notices. No license to
  the original Polar product is implied.

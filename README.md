# Polar Reconstruction — CLAP plugin

A CLAP audio plugin wrapping the recovered **Polar** DSP engine: the Smooth
(phase-vocoder) and Classic (period-splice) FFT pitch shifters, forward and
reverse granular Loop modes, dual shifters with feedback, filter, LFO,
envelope and MIDI keyboard input (gate, keyboard pitch, pitch wheel). The Smooth
and Classic cores, the LFO, the envelope, and every math routine they share with
the original, are checked bit-for-bit against the original binary (see
*Accuracy*).

- **Plugin ID:** `com.polar-reconstruction.polar`
- **Version:** 0.4.0
- **Ports:** stereo in / stereo out, one note input (CLAP or MIDI)
- **Latency:** constant 64 samples (reported to the host; Bitwig compensates)
- **GUI:** built-in macOS Cocoa panel laid out like the original (Algorithm,
  Delay Buffer, two Shifters, Dry Signal, Envelope, LFO, Filter, Keys); Bitwig
  also gets seven remote-control pages

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
4. Every parameter except Reset and Bypass accepts Bitwig modulators, switches
   and menus included (see *Modulation* below). The seven remote-control pages
   (Main, Shifters, Feedback & Delay, Filter, LFO, Envelope, Keyboard) put the
   useful targets on the device's 8 knobs.
5. For MIDI, place it on an instrument track (after the instrument, or alone)
   so the track's notes reach its note input; see *Keyboard* below.

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

Controls take effect at the next 64-sample boundary, like the original (Reason
renders in 64-frame batches). Parameter ids are unchanged from 0.2; the GUI uses
the original panel's labels. 0.4 turns LFO Rate, LFO Sync Rate (was Division),
LFO Wave and A/D/S/R into the original's knobs; projects saved by 0.3 are
converted on load, automation of those parameters is not.

| GUI section | Control (host name) | Range / values |
| --- | --- | --- |
| Algorithm | Slow · Med · Fast (FFT Speed) | FFT window 42.7 / 21.3 / 10.7 ms; Smooth and Classic only |
| Algorithm | Smooth · Classic · Loop · Rev Loop (Algorithm) | FFT spectral · FFT period · forward grains · reverse grains |
| Algorithm | Length (Loop Length) | 0 … 1 → 7.8 ms … 1 s, exponential; Loop modes only |
| Delay Buffer | Lock (Lock Capture), Delay (Position), Stereo | freeze capture; 0 … 1.7 s behind the input; stereo or summed |
| Delay Buffer | LFO, Env (Position LFO / Env) | ±0.5 s depth |
| Shifter 1 / 2 | Shift, Fine (Pitch, Tune) | ±36 st, ±100 ct |
| Shifter 1 / 2 | F.Back, Pan, Volume (Feedback, Pan, Level) | 0 … 99 %, L … R, 0 … 100 % |
| Shifter 1 / 2 | LFO, Env (Pitch LFO / Env) | ±24 st depth |
| Shifter 2 | enable LED, Delay (Enable Voice 2, Delay 2) | on/off, 0 … 1000 ms |
| Dry Signal | Delay, Volume (Dry Delay, Dry) | 0 … 1000 ms, 0 … 100 % |
| Envelope | Gate, A D S R, Amp (Env to Amp) | button gate (MIDI keys gate it too); attack 0.1 ms … 3 s linear, decay and release time constants 0.1 ms … 10 s, sustain level, all cubic knobs as in the original; 0 … 100 % |
| LFO | Rate, Synced (LFO Sync Rate), Phase, Pan (Pan LFO Depth) | 0.11 … 13.3 Hz exponential; 8 bars … 1/64 (18 steps, incl. 7/4, 5/4, 3/16, 1/8 T) |
| LFO | Tri · Up · Down · Sqr · Rnd · Smth · Sine (LFO Wave), Sync (LFO Sync) | the original's six waves (triangle, saw up, saw down, square, random, smooth random) plus the binary's own unexposed sine; tempo sync |
| Filter | enable LED, Freq (Cutoff), Res, LP · BP · HP · Comb | 0 … 100 % |
| Filter | LFO, Env (Filter LFO / Env) | ±100 % depth |
| Output | Output, Reset (Reset Capture), Bypass | −60 … +6 dB; clears the buffer; passes input |
| Keys | P.Wheel (Pitch Wheel), Pitch (Keyboard Pitch), Flt Kbd (Filter Kbd) | ±12 st on both shifters; key − C4 on both shifters; filter key tracking 0 … 100 % |

Cyan knobs are internal LFO/envelope depths: the one LFO and envelope reach the
delay position, both shifters' pitch, the filter, the pan and the amplitude.

Not in the GUI: *Force Reverse* (old alias of Rev Loop), *Fallback Tempo* (used
only when the host sends no tempo) and the three hidden *External* parameters
(kept for state compatibility).

### Keyboard (MIDI)

The original device's keyboard input, ported from its note handling:

- **Mono, last key wins.** A new key takes over. Releasing the sounding key falls
  back to the nearest held key above it, then below it, without retriggering the
  envelope (legato); with no key left the envelope releases and the pitch stays.
- **Keys gate the envelope**, so one key press drives everything the envelope
  reaches: delay position, both shifters' pitch, the filter and (Env to Amp) the
  level. The Gate button still works; pressing it while a key is held retriggers.
- **Keyboard Pitch** transposes both shifters by key − 60 semitones (C4 = no
  change). **Filter Kbd** adds the key to the cutoff (100 % tracks about one octave per
  octave), with or without Keyboard Pitch.
- **Pitch wheel**: ±12 semitones on both shifters, added after the knob smoothers.
  MIDI pitch bend and the CLAP tuning expression add to the Pitch Wheel parameter,
  which can also be automated or modulated.
- The status line shows the sounding key.

### Modulation (Bitwig and other CLAP hosts)

- **Everything modulates.** All parameters except Reset and Bypass report
  `CLAP_PARAM_IS_MODULATABLE`. Stepped ones (algorithm, speed, filter mode,
  wave, switches) round base + amount, so a Bitwig modulator can switch modes.
- **Modulation acts at once.** Knob moves and automation go through the
  original's 35 Hz control smoothers, as in Reason. Host modulation of Shift,
  Fine, Delay (position), Freq and Res is added *after* those smoothers, at the
  point where the internal LFO and envelope enter, so fast modulators are not
  low-pass filtered. Levels, pans, feedback and delays stay on the per-sample
  gain smoothers to avoid zipper noise. With no modulation the signal path is
  bit-identical to the recovered one.
- **Ranges hold.** base + amount is limited to the parameter range; the
  internal LFO/envelope and host modulation add up.
- **GUI edits are gestures.** Dragging a knob sends gesture begin, values and
  gesture end, from `process()` while audio runs and from `flush()` otherwise,
  so Bitwig records automation and its own knobs follow. The GUI draws host
  modulation as a violet arc and dot (switches: a violet outline).
- **Tempo.** LFO Sync follows the host tempo; while the transport plays, the LFO
  reads the song position as the original reads Reason's, so it stays in phase
  with the bar.

### Other behaviour

- **Bypass** follows the CLAP bypass parameter: audio passes through while
  the engine keeps processing (preserving the captured buffer).
- **Host transport resets are ignored** on purpose: the capture buffer holds
  audio material, not timeline state. Use *Reset* to clear it.
- **State.** All parameters are saved with the project; the host is asked to
  re-read values after a load. Captured audio is not part of the state.
- **Failure safety.** An internal engine exception gives silence for that
  block rather than a host crash; the engine retries on the next block.

## Accuracy

The original x86-64 Polar binary was run under an instrumented loader (Rosetta on
an Apple Silicon Mac; the lab harness is private and not in this repository) next
to this engine. Comparisons are bitwise; "differ" counts any changed bit.

| Check | Result |
| --- | --- |
| Bundled `expf`, every 32-bit input (4,294,967,296) | 0 differ |
| Bundled `atanf`, every 32-bit input | 0 differ |
| Bundled `cosf`, every input with \|x\| ≤ 2⁷·π/2 (2,257,723,138) | 0 differ |
| Bundled `powf(128, t)`, every t in [0, 1] (1,065,353,217) | 0 differ |
| Bundled `powf`, `atan2f`, `sin`/`cos`, 650 M random inputs | 0 differ |
| FFT trig, Hann and fade tables generated at start-up (74,432 entries) | equal to the original's |
| Same math on arm64 and x86-64 (exhaustive hashes) | identical (NaN payloads aside) |
| Smooth (FFT0) core, randomized controls, all mono/stereo × 1/2-voice variants, 44.1/48/96 kHz, every state word compared | 480,000 blocks, 0 differ |
| Classic (FFT1) core, same stimulus | 480,000 blocks, 0 differ |
| LFO (`CLFOModule`), every wave incl. random and smooth random, free and tempo-synced, transport stopped and playing, 44.1/48/96 kHz, every state word compared | 12 × 500,000 ticks, 0 differ |
| Envelope (`CADSRModule`), random knobs and gates (button, MIDI 1.01, both), all 64 samples and the state every block | 12 × 500,000 blocks, 0 differ |
| Bundled binary64 `exp` (the envelope's coefficients) | 20 M inputs, 0 differ |
| Harness can fail: a 1-ulp change in one constant | caught in block 0–1 (FFT cores); each of 10 LFO/envelope constants caught |
| `polar_golden_test`: the original's math, LFO and envelope (hashes taken by running the original) and tables, no binary needed | pass (planted 1-ulp `expf` and LFO changes fail it) |
| `polar_clap_test`: plugin through the CLAP ABI vs the engine in exact 64-frame blocks, random buffer sizes, events, modulation, 30 s soak | bit-identical |
| `polar_whitebox_test`: gestures, event ids, modulation routing, host tempo, key priority, notes and bend through the plugin, keyboard pitch and wheel on both shifters, 0.3 state conversion, text formats | pass |
| All tests with `-fsanitize=address,undefined`, and as an Intel build under Rosetta | pass |

Why the math matters: the original carries its own (newlib fdlibm) math library.
A platform `expf`/`atan2f`/`powf` differs from it in 10–14 % of results, which
moved about 10 % of pitch settings by one float step and made the Smooth core
drift from the original from its first analysis frame. `engine/bundled_math.hpp`
is the ported library; the engine no longer calls `<cmath>` where the original
used its own routines.

### Not verified

- The FFT itself: Reason supplies it to the device and it was not measured. Both
  sides of every comparison above use this engine's binary64 radix-2 transform.
  The SDK documents size (log2 N), the packed N/2 complex pairs and the 1/N
  forward scaling; it does not document where Nyquist sits, the sign convention
  or the arithmetic. A different but correct float FFT in the original's own code
  changes Classic's period choices within seconds and Smooth by −50 … −72 dBFS,
  so sample identity with real Reason output is not established.
- Granular Loop/Rev Loop, the filters, feedback clipper, delays, pan, dry path and
  the whole engine graph were recovered from the disassembly and tested against
  references, not executed against the original.
- The key handling (priority, legato, note value) and the wiring of wheel, keyboard
  pitch and filter tracking were read from the disassembly and unit-tested, not
  executed against the original. Public units and defaults are authored.
- No real Reason renders were compared.

## Tests

```sh
cmake -B build-test -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-test
./build-test/polar_golden_test
./build-test/polar_whitebox_test
./build-test/polar_clap_test build-test/PolarReconstruction.clap/Contents/MacOS/PolarReconstruction   # Linux: build-test/PolarReconstruction.clap
./build-test/polar_gui_snapshot panel.png modulated   # macOS: render the GUI to a PNG
```

## Scope

This wraps the independently reconstructed engine. The packed `Polar.data`
resources, original factory presets and original artwork are not reconstructed;
the GUI is drawn from scratch after the original panel's layout. See *Not
verified* above for what the comparisons do and do not establish.

## Project layout

```
CMakeLists.txt          build for macOS (.clap bundle) and Linux (.clap + tests)
build-mac.sh            one-command Apple Silicon build + install
engine/                 recovered DSP engine; bundled_math.hpp = the original's libm;
                        original_lfo/original_adsr/mono_keyboard.hpp = its LFO, envelope, keys
src/params.{hpp,cpp}    parameter model shared by core, GUI and tests
src/plugin.{hpp,cpp}    plugin instance: buffering, params, state, process
src/entry.cpp           CLAP C ABI: callbacks, extensions, factory, entry
src/gui/polar_gui.{hpp,mm}  macOS Cocoa panel
clap-sdk/               vendored CLAP 1.2 headers (MIT)
tests/host_smoke.cpp    black-box host harness + bit-exact engine comparison
tests/whitebox.cpp      C++-level unit tests
tests/golden.cpp        the original's math, LFO, envelope and tables, pinned by hashes
tests/golden_stimulus.hpp  the stimulus those hashes were taken over
tests/gui_snapshot.mm   renders the GUI to a PNG
cmake/Info.plist.in     bundle metadata
```

## Licenses

- CLAP SDK headers: MIT (see `clap-sdk/LICENSE`).
- `engine/bundled_math.hpp`: fdlibm notice (Sun Microsystems), see
  `THIRD_PARTY_NOTICES.md`.
- Engine and wrapper: part of the Polar reconstruction project; see
  `THIRD_PARTY_NOTICES.md` and the original archive's notices. No license to
  the original Polar product is implied.

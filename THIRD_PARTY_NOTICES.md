# Third-party notices

## CLAP SDK

`clap-sdk/include/` contains the CLAP 1.2 headers from
<https://github.com/free-audio/clap> (revision `a47f6badb49d948fd009998f28309cdab78979c9`,
2026-07-28), covered by the MIT license in `clap-sdk/LICENSE`.
Only the header-only include directory is vendored; nothing else from that
repository is used or distributed here.

## Recovered Polar engine

`engine/` is copied unchanged from the `Polar_Complete/src` directory of the
Polar reconstruction 0.2 archive (`Polar_Reconstruction.zip`). Its
third-party notices apply:

- The standalone C++ DSP, FFT adapter, wrapper and tools have no compiled-in
  FFT/library dependency.
- Original Polar product names identify the supplied analysis target.
  Original executable/resource files and factory assets are not included.
  The mathematical initializer tables in `engine/original_*_tables.hpp`
  carry their extraction provenance in their headers.
- The independently reconstructed source is supplied for study and porting
  work; no license to the original product is implied.
- Validation scope (Reason-host equivalence unverified, resource-defined
  defaults not reconstructed) is documented in the original archive's
  README and EVIDENCE.md.

## This wrapper

The CLAP glue, parameter model and macOS GUI in `src/` were written for this
port. They add no third-party code beyond the Apple system frameworks
(AppKit via Cocoa), linked dynamically at build time.

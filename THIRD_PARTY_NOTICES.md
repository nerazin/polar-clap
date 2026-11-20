# Third-party notices

## CLAP SDK

`clap-sdk/include/` contains the CLAP 1.2 headers from
<https://github.com/free-audio/clap> (revision `a47f6badb49d948fd009998f28309cdab78979c9`,
2026-07-28), covered by the MIT license in `clap-sdk/LICENSE`.
Only the header-only include directory is vendored; nothing else from that
repository is used or distributed here.

## Recovered Polar engine

`engine/` started as the `Polar_Complete/src` directory of the Polar
reconstruction 0.2 archive (`Polar_Reconstruction.zip`). Since 0.3 it differs:
the FFT trig/Hann/fade tables are generated at start-up instead of copied, the
engine calls the ported math library below instead of `<cmath>` where the
original used its own routines, and host modulation enters after the control
smoothers. Its notices apply:

- Original Polar product names identify the analysis target. Original
  executable/resource files, table data and factory assets are not included.
- The independently reconstructed source is supplied for study and porting
  work; no license to the original product is implied.

## fdlibm (engine/bundled_math.hpp)

`engine/bundled_math.hpp` re-implements the newlib/fdlibm float and double math
routines that the original binary links statically. fdlibm notice:

> Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
>
> Developed at SunPro, a Sun Microsystems, Inc. business.
> Permission to use, copy, modify, and distribute this
> software is freely granted, provided that this notice
> is preserved.

The float versions were converted by Ian Lance Taylor, Cygnus Support.

## This wrapper

The CLAP glue, parameter model and macOS GUI in `src/` were written for this
port. They add no third-party code beyond the Apple system frameworks
(AppKit via Cocoa), linked dynamically at build time.

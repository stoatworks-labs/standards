# Attributions

Standards is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Field machinery — Stoatworks cadence

<https://github.com/stoatworks-labs/cadence>  
Licence: MIT  
Copyright: Stoatworks Labs

A field as a slice of time as much as a slice of lines, the top-field-first parity rule, the rule that no pass samples between two rows, the frame-based weave and Bob Linear are cadence's, re-expressed here for two line standards instead of one.

### A ring of timestamped frames — Stoatworks afterglow

<https://github.com/stoatworks-labs/afterglow>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of a ring of timestamped recent frames is afterglow's.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of rebate.

### Harness shape, --pipe contract and negative controls — Stoatworks rebate

<https://github.com/stoatworks-labs/rebate>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract and the negative-control pattern are pitch's and rebate's; the host clock-unit voting is readout's by way of cadence; --offline and check-shaders.sh are slowscan's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Field-store television standards converters

The 625-line/50-field and 525-line/59.94-field standards, with 576 and 480 active lines, top field first, and field-store conversion by temporal and vertical interpolation as the digital converters of the 1970s and 80s did it; block matching by sum of absolute differences for the crude motion compensation. Built from broadcast engineering; no manufacturer's design, coefficients or name is used.

## Standards and published specifications

What the implementation is measured against.

- **Robert G. Keys, "Cubic Convolution Interpolation for Digital Image Processing" (IEEE Trans. ASSP, 1981)** — Cubic convolution with a = -1/2, i.e. Catmull-Rom, for the four-tap apertures; the Lanczos windowed sinc for eight.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the harness uses for its noise frames, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

# Attributions

Standards is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Standards is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Within the fleet

Not third-party, but owed a line. The field machinery is **cadence**'s
(`github.com/stoatworks-labs/cadence`, MIT, Stoatworks Labs): a field as a slice of
time as much as a slice of lines, the top-field-first parity rule, the rule that no
pass ever samples between two rows, the frame-based weave that shows the latest
complete pair a field late, and Bob Linear -- re-expressed here for two line
standards instead of one, with attribution rather than a second invention. The idea
of a ring of timestamped recent frames is **afterglow**'s. `PassBuffer` is
**tinsel**'s, by way of **rebate**. The harness shape, the `--pipe` contract and the
negative-control pattern are **pitch**'s and **rebate**'s; the host clock-unit voting
is **readout**'s by way of cadence; `--offline` and `check-shaders.sh` are
**slowscan**'s. The PCG output mix the harness uses for its noise frames is the
well-known `pcg_hash` construction, written out here rather than copied from
anyone's source.

## Science, not code

The model is built from broadcast engineering rather than from anyone's
implementation: the 625-line/50-field and 525-line/59.94-field (60000/1001) television
standards, with 576 and 480 active lines, top field first; field-store standards
conversion by temporal and vertical interpolation, as the digital converters of the
1970s and 80s did it; Keys' cubic convolution (1981) with a = -1/2, i.e. Catmull-Rom,
for the four-tap apertures; the Lanczos windowed sinc for eight taps; and block
matching by sum of absolute differences for the crude motion compensation. No
manufacturer's design, coefficients or name is used.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

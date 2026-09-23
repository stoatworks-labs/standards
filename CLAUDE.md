# standards

A field-store standards converter — 625/50 to 525/59.94 and back — as an FFGL
**effect** for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the field arithmetic (`Model.*`), the schedule, the
vertical filter table or the display.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/sttest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Direction=1" --set "Temporal=2" --set "Motion Comp=1"`
  (0..1 for sliders, the element index for options)
- List parameters, kinds, defaults and ranges: `./build/sttest --list`
- The exact GLSL the plugin compiles: `./build/sttest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps, default 60; the
  conversion is a function of time, so this matters) and an optional `--script` of
  `frame Parameter Name value` cues, linearly interpolated between a name's cues and
  held before the first and after the last — an option index interpolated passes
  through the options between, so key a cut two cues a frame apart. A cue naming no
  parameter exits 2 before any frame; a partial frame at the end of stdin ends the
  stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/sttest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~50 s)
- Each output field's levels are its temporal weights; 6-field cycle; 59.94 creep:
  `./build/sttest --weights`
- The repeat pattern and the double image, whole-pixel and fractional: `./build/sttest --judder`
- Marked lines land where the geometry says; impulse and zone-plate response are the
  stated taps': `./build/sttest --lines`
- Same, Drop/Repeat, one tap is the fielded input byte for byte: `./build/sttest --same`
- Motion Comp smooths a pan and tears where predicted: `./build/sttest --mc`
- The field store survives a resize: `./build/sttest --resize`
- The checks can fail: `./build/sttest --negative`; one perturbation verbosely:
  `./build/sttest --lines --perturb 2` (bits in `Model.h`)
- No GL (what CI runs): `./build/sttest --offline` = `--model --clock --names` and their
  negative controls
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/sttest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/sttest --bench` (best of three, with and without Motion Comp)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Standards.bundle`

## Notes
- **The CPU decides every number, the GPU every pixel.** `Model.cpp` holds field
  positions (exact rationals: p_j = 5j/6 for 50 → 60), temporal weights and the
  vertical filter; `Schedule.cpp` decides which fields happen on a host frame. A
  wrong weight is a C++ fix; a wrong line is usually a table fix, not a GLSL one.
- **Never sample between rows.** Every read is `texelFetch` at an integer coordinate
  computed in integers; every buffer is Nearest. Two rows of a frame are two moments.
- **Buffers hold lines top-first**: texel row i is line i. Only the capture and the
  display know GL's row 0 is the bottom.
- **The harness never re-types the model.** Standards, kernels (Catmull-Rom in
  Hermite form) and the time/line geometry are stated in `sttest` from the
  definitions; it reads the plugin through the picture, plus read-only hooks for which
  field is on show and for the field store itself.
- **No per-frame clock clamp.** The harness drives 600 frames a second so field
  instants are host frames; a delta is believed unless it is backwards or over 0.5 s.
- **A resize carries the store**: `resizeStore` resamples every held field to the new
  width. A direction change empties and re-primes it.
- **`Perturb` bits are test hooks**, always 0 in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1; options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1).
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `standards_core` is an OBJECT library, not STATIC — the plugin registers itself from
  a file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `ST01`, display name `SW Standards`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. The Windows build is CI-only and has never run.
- Never seen on footage, only on synthetic cards.
- No user guide, no OpenFX port, no browser demo, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/standards/standards.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\standards\logs\standards.YYYY-MM-DD.log   (Windows)

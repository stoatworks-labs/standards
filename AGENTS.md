# AGENTS.md — Standards

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A field-store standards converter — 625/50 to 525/59.94 and back — as an FFGL 2.1
effect (`ST01`, shown as `SW Standards`) for Resolume Arena and Avenue. C++17 +
GLSL 4.10, CMake, universal macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/standards`.

Built 2026-09-23 in one session from the fleet's templates and
`specs/SPEC-standards.md` (with `BRIEF.md` and `BRIEF-ADDENDUM.md`): cadence for the
field machinery, afterglow for the ring of timestamped frames, rebate and pitch for
the harness, the verify script, the `--pipe` contract and the negative controls,
slowscan for `--offline` and the GL-less CI, tinsel for `PassBuffer` and the trap
list, graticule for the notes. `residual` (block motion estimation) did not exist in
`~/dev`, so the motion search here is written fresh.

---

## The one idea

**A standards converter interpolates twice and knows nothing about motion.**

Each output field is made from the input fields either side of it in time, weighted
by where it falls between them; each output line from nearby lines of the field being
read. Put the clip into fields of the source standard at that standard's own
instants, do those two interpolations, and every artefact anybody remembers is a
consequence:

| the stage | what comes out |
| --- | --- |
| destination field j at source position p_j = j Rd⁻¹ Rs, exact | **the judder cycle**: 50 → 60 is p_j = 5j/6, so the weights repeat every six fields; at 59.94 it is 1001j/1200 and the cycle **creeps** 0.005 of a field every six |
| two fields weighted by distance (Linear) | **double images** where p_j lands between fields; Drop/Repeat trades them for a repeated field |
| taps on the lines that exist in the field being read | **the soft look**: a field's lines are two frame lines apart, and few taps across them lose vertical detail; Softness adds the old boxes' pre-filter |
| block vectors between the two bracketing fields | **crude motion compensation**: a pan goes smooth, a block holding two motions tears |

### The arithmetic, in one place

    time:   source field n at n / Rs, destination field j at j / Rd (seconds after
            the first frame); p_j = j Rd.den Rs.num / ( Rd.num Rs.den ), held as
            n + rem / den in integers
    space:  frame line m of L active lines is centred at ( m + 1/2 ) / L of the
            height, top first; field parity q holds m = 2i + q; so destination line
            m, read from a source field of parity p, sits at field-line coordinate
            u = ( ( 2m + 1 ) Ls - Ld - 2 p Ld ) / ( 4 Ld )

Both are exact rationals; nothing that decides a field or a line is a float.

### What does not fall out, and is the honest limit

- **The input is progressive.** A source field is the nearest host frame sampled onto
  the standard's lines, as a camera with a shutter would take it — not a field that
  was shot. Nothing reads a real interlaced stream.
- **Horizontal resolution is the host's own.** Only the line structure and the time
  base are converted; a Rec. 601 horizontal sampling would soften every picture and is
  not what an operator reaching for "NTSC judder" wants.
- **The capture is a point sample.** Frame line l takes the host row under its centre.
  A host picture taller than the standard (720 rows into 576 lines) aliases
  vertically, as an unfiltered camera would; Softness is the pre-filter.
- **The motion compensation is crude by design**: integer vectors, one per 16×8
  block, vertical fetches rounded to the nearest field line, one vector pair applied to
  the whole aperture.
- **The display adds its own cadence.** Destination fields are shown at host frames,
  so 50 fields on a 60 Hz host picks up a 5-in-6 display cadence as well as the
  conversion's. That is true of any 50 Hz picture on a 60 Hz screen.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.{h,cpp}` | The conversion as arithmetic: the three standards, the four directions, exact field positions, temporal apertures, line positions, the kernels, the vertical filter with Softness, the `Perturb` bits. No GL. |
| `source/Schedule.{h,cpp}` | Which fields happen on this host frame: source fields at their instants from the nearer host frame, destination fields at theirs plus two source fields, the priming, the stall caps. No GL. |
| `source/Clock.{h,cpp}` | The field clock: unit voting (readout's), origin + offset in double, no per-frame clamp, a jump steps one nominal frame. |
| `source/Controls.{h,cpp}` | Options by index; Softness. |
| `source/Shaders.{h,cpp}` | Eight shaders: vertex, capture, field, resample, motion SAD, motion pick, convert, display. |
| `source/Standards.{h,cpp}` | The plugin: parameters, the store, the passes, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/sttest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

The store, per host frame:

1. **capture** — the host picture onto the source standard's frame lines (W × Ls,
   half float), into whichever of two buffers does not hold the previous frame.
2. **field** — each source field now due, from whichever of this frame and the
   previous one is nearer its instant, as its parity's lines (W × Ls/2) into a ring
   of eight.
3. **motion** (Motion Comp only) — one SAD per (block, candidate), then the least per
   block, for the pair (floor p, floor p + 1); cached per pair.
4. **convert** — each destination field now due, from 1, 2 or 4 source fields, each
   filtered by a table of (first line, count, ten weights) per destination line and
   source parity that the CPU built in double, summed with the CPU's temporal
   weights (W × Ld/2, a ring of four).
5. **display** — the latest complete pair woven, or the latest field bobbed, onto the
   host's framebuffer at the host's height or at native lines.

---

## Traps

Roughly in the order they will bite.

### ☠️ One fragment looping over the whole search is 41 ms at every raster

The first bench put Motion Comp at **41.4 ms at 720p and 42.9 ms at 4K** — two and a
half 60 fps frames, and nearly flat across rasters, which is the tell: it was not the
work, it was the wait. One fragment per block looped over all 165 candidates × 64
samples, so a few thousand fragments each ran a long serial loop and the GPU had
nothing to hide the latency behind. Spread across fragments — one SAD per (block,
candidate), then a pick pass in the same scan order so ties resolve as before — it is
0.57 ms at 720p. Do not fold the passes back together.

### ☠️ A SAD that samples every other column cannot see a one-pixel line — and the check could not see that

The motion search first summed every other column of a block. On a 3 px bar that
works. `--mc`'s Linear pan used exactly that bar, moving 3 px a field, so the two
halves of a double image *touched* and read as one run: with the vectors estimated
and never applied (`kPerturbMcZero`), the "one image" assertion still passed. That
was found by reading the negative controls' detail, not their verdict — the verdict
was "caught", by the Drop/Repeat case alone. With a 1 px bar the check can tell, and
the plugin then failed it: a one-pixel line on an odd column matched nothing and the
vectors came back zero. The SAD now reads all sixteen columns (Motion Comp 0.77 ms at
720p, 1.80 ms at 4K). Read what a negative control failed on, not only that it did.

### ☠️ This driver does not round to nearest when it stores half floats

The weights check measured errors up to **4.86e-4** on weights in [0.5, 1), where a
round-to-nearest store would allow 2.44e-4 (half a half-float ULP). GL allows a
conversion to a narrower float to round either way, and this one evidently does not
round to nearest. Every tolerance on a value read out of the RGBA16F store is
therefore **one full half-float ULP** at that value, not half of one. A tolerance
fitted to round-to-nearest would have failed here and been "fixed" by widening it
without a reason.

### Same has only two parity pairs

`--lines` first required all four (destination parity, source parity) pairs for every
direction, and failed Same: Same reads source field j into destination field j, so
only q = p ever happens. The requirement was wrong, not the plugin; it is two for
Same and four for a conversion.

### The zone plate's phase classes alias

Destination lines sharing a phase (5 classes for 625 → 525, 6 for 525 → 625) sit 6
(or 5) source field lines apart, so at frequencies near a multiple of 1/12 cycle per
field line their samples of the zone plate are all the same phase and G(f) cannot be
fitted. The tolerance is derived from the fit's own design matrix, so it goes large
exactly there; fits whose tolerance exceeds 0.05 are skipped and at least 40 are
required. That is a limit of the measurement, stated, not a pass by omission.

### Mutation-test only a committed tree

pitch's trap, observed rather than repeated: the mutation below was applied to a
clean, committed tree and reverted with `git checkout`.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the display, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` and the resize resampling
happen before anything binds a texture; `FFGLFBO::Release()` leaks the colour
texture, which is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo` clamps
a STANDARD default into 0..1; an option's range reads back 0..1 whatever its element
count; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `FFGLShader::Set` has no integer-vector overload, so every
integer uniform is set one at a time; the host clock's unit is voted on and the
harness declares its own; Resolume's clock overflows a float, so nothing absolute
reaches a shader and field indices are integers; the store is primed on the first
frame (seven fields of that picture), so nothing reads as risen from black after a
clip trigger; a resize must not clear the store (it is carried to the new width);
`nm | grep -q` fails under pipefail when grep succeeds; never sample between two rows
(cadence's first trap) — every read here is `texelFetch` at an integer coordinate.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`, and at 333×187 and 1920×1080 by hand.

What makes most of them rasteriser-proof by construction: **every coordinate is an
integer computed in integers** (`gl_FragCoord` and the host viewport, never an
interpolated uv), **every read is `texelFetch`** (no texture unit's filtering
precision), and **every weight is computed on the CPU in double** from exact
rationals and handed over as a uniform. The GPU only multiplies and adds. The one
float operation whose result is implementation-defined is the store's conversion to
half, and every tolerance allows a full half ULP for it (see the trap above).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--weights` per field | the four channels of a one-hot field-number patch, each exactly one field's weight | **one half-float ULP at the weight** (the RGBA16F store, either rounding) + 1e-7 (the double weight's rounding into a float uniform); the aperture is identified from a second patch at n/1024, resolution ⅛ field in half, accepted within ¼ | none: the patches are uniform and read at their centres; the host rate (600 or 600000/1001 fps) puts every source field on a host frame whatever the raster |
| `--weights` six-field cycle | fields j and j + 6 at 50 → 60 | **exact, bit for bit**: the same rational fraction is the same float and the same stored half | none |
| `--weights` creep | the later field's weight six fields on, minus now | the two weights' ULPs + 2e-7, against 6 × 1001/1200 − 5 = 0.005 | none |
| `--judder` whole pixels | component centroids and masses of a bar at 4 and 8 px/field | position **1e-9 px**: every value is 0, 1 or w, and the pixel-coverage centroid of a box of integer width is exactly its centre; weight: ULP(w) + 1e-7 | the bar starts 8 px in and travels ≤ 170 px: needs ≥ ~180 px of width (verify's smallest is 320); positions are in pixels, the same at any width |
| `--judder` fractional | the same at 2.3 and 7.3 px/field | position: Σ ‖x − c‖ ULP(v_x) / Σ v, the store's rounding of each edge's coverage, computed per component; weight as above | as above; the bar's coverage is computed on the CPU, never rasterised |
| `--judder` repeats | fields whose bar did not move, against fields whose stated source field repeated | exact count | none |
| `--lines` line map | which output rows are lit, for marked host rows | **exact set equality**: three integer maps (capture row, nearest field line, display line) composed | marks are chosen as rows a source line samples, so they exist at any height; every row is checked; the maps are integer at any raster |
| `--lines` impulse | every destination line's value against the stated composite taps | ULP(stated) + 2e-7: exactly one tap sees an impulse (they are 20 field lines apart) | **raster-free vertically by design**: the host picture is Ls rows so each row is one source line; --size sets the width only |
| `--lines` zone plate | G(f) per phase class, least squares | ‖(XᵀX)⁻¹Xᵀ‖ × e_max from the fit's own design, e_max = Σ‖w‖ ULP(0.9) + ULP(1) + 1e-6; fits with tolerance > 0.05 skipped, ≥ 40 required | as the impulse; the frequencies are column fractions of the width, so the set of f differs by raster and each is checked on its own |
| `--same` output | every byte of 78 woven frames against the nearest host frames' rows | **exact**: 8-bit in, half in the store (error < 2^-11, under half an 8-bit step), 8-bit out | the expected image is computed from the integer maps at the raster given |
| `--same` store | destination field bits against source field bits, 1/2/4/8 taps | **exact**: at φ = 0 every kernel is one tap of weight 1 (Lanczos' sin(πk) zeros are forced to 0) | none |
| `--mc` pan | per-field step and position of a 1 px bar at 3 px/field | **1 px** — the spec's, and the method's: vectors are integer pixels and a block boundary can split the bar's fetch | positions are in pixels from x = 40; blocks are 16 px from the left edge at any raster |
| `--mc` without | the same material's step without Motion Comp | must stray > 1 px (it strays 2.5): the judder is there to remove | none |
| `--mc` occlusion | error against the ideal picture at τ_j | outside two blocks (32 px) of the bar's extents at n, n + 1, the nearest field and τ_j: ULP(ideal) + 1e-6 (an integer-position copy); inside: a tear > 0.25 must appear | none: the background is point-sampled from a function of the pixel index |
| `--resize` | the weights across a mid-run doubling | as `--weights` | the check doubles whatever raster it is given |
| `--model` | the plugin's apertures and filters against the harness's statement | 1e-12: two statements of one definition in double (Catmull-Rom as Keys' cubic against its Hermite form) | none (no GL) |
| `--clock` | a six-day ms clock and a fresh s clock's field decisions | exact equality | none (no GL) |

Deliberately NOT relied on: round-to-nearest in the half store; `mix(a, b, 1) == b`
(the display returns early at Mix 1); interpolated varyings (`uv` is used only for the
Mix < 1 input sample); a texture unit's bilinear precision (none is used); GLSL
integer division of negative operands (the one negative offset, Native's centring, is
computed on the CPU with a floor division).

What might still differ on another rasteriser: nothing measured here should, beyond
the half-store rounding the tolerances allow. The software context CI would fall back
to is a different compiler for the same GLSL; `check-shaders.sh` covers the syntax
there and the rendered checks run with `--allow-no-gl` so a runner without a context
skips loudly.

### The negative controls

`sttest --negative` runs ten against rendered checks and `--offline` six against the
model; `--perturb BITS` runs any check verbosely against one. Each perturbs the
*plugin's* model — a `Perturb` bit the shipped plugin carries at zero — never the
harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| temporal weights swapped (the spec's) | `--weights`: field 0 reads mean field 1.000 against 0.000; Four Field 1.333 against 1.667 |
| 59.94 treated as 60 | `--weights`: channel weight 0.1666 against 0.1658; creep 0 over 0 pairs |
| Drop/Repeat takes the field at or before | `--judder`: the bar at 9.5 against 13.5; 0 repeats where the geometry states 4 |
| weights swapped, on the moving bar | `--judder`: a component of weight 0.667 where 0.333 is stated |
| taps ignore the field's parity (the spec's) | `--lines`: a row dark that the geometry says is lit; impulse off by a line; G(f) 150–195× its tolerance |
| Catmull-Rom detuned to a = −3/4 | `--lines`: impulse −0.0018 against −0.0012; G(f) 13× its tolerance |
| capture takes the frame at or before, not the nearest | `--same`: 44 of 78 frames differ |
| motion vectors estimated and never applied | `--mc`: Drop/Repeat steps stray 2.5 px; Linear shows one image in only 4 of 24 fields |
| motion vectors applied with the wrong sign | `--mc`: steps stray 5.0 px (Drop/Repeat) and 4.5 px (Linear) |
| a resize that re-primes the store | `--resize`: the first field after it reads mean field 40.000 against 39.167 |
| offline: weights swapped, parity ignored, floor for nearest, 59.94 as 60, Catmull-Rom detuned | `--model`: apertures and filters differ from the statement |
| offline: the clock kept in float | `--clock`: 2,999 of 3,000 frames make different fields at a six-day origin |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the convert pass,
`fetchField( k, x, first + t )` → `first - t` (the vertical taps read backwards from
the first line). Caught by `--lines` — the impulse response at 2, 4 and 8 taps and
with Softness, in both directions, and every zone-plate fit (14 checks) — and by
`--same`'s store identity at 4 and 8 taps (2), at both rasters. `--weights`,
`--judder`, `--mc` and `--resize` passed, correctly: they run one tap, where t is only
ever 0. So did Same at 2 taps, where the second tap's weight is exactly 0. Reverted
with `git checkout source/Shaders.cpp`; the tree was clean before and after.

---

## Decisions taken without asking

- **The standards**: 625/50 with 576 active lines, 525/59.94 (60000/1001) with 480,
  and 525/60 the monochrome rate; **top field first** in both. "Same" is 625/50 →
  625/50.
- **Horizontal is the host's**; only lines and time are converted.
- **Capture**: frame line l takes host row floor((2l + 1) H / (2 Ls)), a point sample
  under the line's centre; no aperture filter (Softness is the pre-filter).
- **Nearest host frame** for a source field, as the spec says, with a tie to the
  earlier. That needs the previous frame on the source lines, which is the second of
  the two capture buffers.
- **Latency two source fields**: destination field j is released at j / Rd + 2 / Rs,
  and only once its aperture's last field exists. Released by time, not by arrival:
  releasing by arrival hands the display fields in bursts and adds a judder of its
  own. The same latency for every aperture, so changing Temporal does not jump.
- **Priming**: the first frame (and the first after a direction change) fills the
  store with seven fields of that picture, so there is an output on the first frame.
- **Ties**: Drop/Repeat repeats the earlier field at exactly half-way; one vertical
  tap rounds half up.
- **Four Field and four taps are Catmull-Rom** (Keys, a = −1/2); **eight taps are
  Lanczos-4**, normalised to sum 1 at every phase, with its zeros forced exactly to 0
  on a line. **Softness** is [a, 1 − 2a, a], a = Softness / 4, convolved in front.
- **The vertical filter is a table** built on the CPU in double per (destination line,
  destination parity, source parity), so no kernel (and no `sin`) is evaluated on the
  GPU; it is rebuilt when the direction, taps or Softness change.
- **Motion Comp**: 16 px × 8 field-line blocks, ±16 px and ±2 field lines, SAD on
  luma over every pixel, a tie-break of 1e-3 per unit of ‖v‖ towards zero; vectors from
  (floor p, floor p + 1) applied to every field of the aperture at its distance from
  p; horizontal fetches linear between two columns, vertical rounded to the nearest
  field line.
- **Display**: Weave shows the latest complete pair (2i, 2i + 1), a field late as a
  real monitor is (cadence's rule); Bob is cadence's Bob Linear on the latest field.
  Native Lines centres the destination frame with black above and below, or crops.
- **The store is RGBA16F**: an 8-bit code value survives exactly, negative lobes
  survive, and 4K is about 135 MB.
- **A width change carries the store** (every held field resampled to the new width,
  nearest column); **a direction change empties it** — it is a different machine.
- **The clock has no per-frame clamp**; a delta that is backwards or over 0.5 s steps
  one nominal frame instead. The fleet's [1/240, 1/24] clamp would have run the
  harness's 600 fps clock at a quarter speed.
- **No factory presets, no audio input** — the spec has neither.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove `--pipe`
  exits 1 on a failed render; the plugin itself only fails on an input no ffmpeg
  sends. `--pipe` ignores SIGPIPE so a closed stdout is a failed write and exit 1.
- **`--lines`' impulse and zone-plate halves run at the standard's own height** (see
  the table); the line-map half runs at `--size`.
- **Provisional About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) are
  hand copies adapted from rebate's with `guide=""`; the button count, and so the
  parameter count, does not change when the fleet's sync regenerates them.
- **Commit trailers name the model that did the work** (`Claude Opus 5.5`), as the
  session's instructions said, not the brief's `Claude Fable 5.1`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 333×187 and 1920×1080
by hand.

- **Weights.** 772 destination fields over six direction/aperture cases, every weight
  within one half-float ULP of the time geometry (worst 4.86e-4). 50 → 60 repeats bit
  for bit every six fields (123 pairs Linear, 121 Four Field); 50 → 59.94 creeps
  0.004997 per cycle against 0.005 over 120 pairs.
- **Judder.** Whole-pixel positions exact, fractional within 1.7e-4 px; 4 repeats seen
  where 4 are stated; Linear's two components at the stated weights.
- **Lines.** Marked rows exact at both output sizes in both directions and Same; the
  impulse response at every destination line (960 or 1,152 per tap count) within a
  half-float ULP; 3,036 zone-plate fits of G(f) within a quarter of their tolerance —
  |G(0.25)| 0.95 / 0.996 / 0.9996 for 2 / 4 / 8 taps at 625 → 525.
- **Same.** 78 frames byte for byte; the store bit for bit at 1, 2, 4 and 8 taps.
- **Motion Comp.** A pan's per-field step within 0.5 px of 2.5 (Drop/Repeat) and
  0.3 px (Linear, one image); a tear of 0.72 inside the predicted blocks and 4.9e-4
  outside them.
- **Resize.** Weights exact across a mid-run doubling, including fields captured
  before it.
- **Offline.** Apertures and filters to 1e-12; a six-day millisecond clock makes the
  same fields as a fresh seconds one on 3,000 frames; a scrub and a jump move the
  field clock by at most one nominal frame, forwards.
- **Negative controls.** All sixteen fail their check.
- **Mutation.** Caught by sixteen checks (above).
- **No dead controls**, all 8, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, as the plugin hands it to the driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.standards`, ad-hoc signs, and `oxbow` reports `SW Standards` /
  `ST01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame | with Motion Comp |
  | --- | --- | --- | --- |
  | 1280×720 | 0.10 | 0.6% | 0.77 |
  | 1920×1080 | 0.19 | 1.1% | 1.04 |
  | 3840×2160 | 0.43 | 2.6% | 1.80 |

  Before the motion search was spread across fragments, Motion Comp was 41 ms at every
  raster.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture so far is synthetic. Whether the judder
  and the double images read as "a 1980s transatlantic feed" to someone who watched
  one is unjudged.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
  Arena at 60 fps against a 59.94 destination will show a field twice every ~17 s;
  that is the display, and it is untested in a host.
- **The Windows build is CI-only** and CI cannot run yet.
- **Not verified at 4K**, only benchmarked there.
- **The motion compensation is only checked on a pan and one occlusion**, both
  horizontal. Vertical vectors are estimated and applied (rounded to a field line) but
  no check moves anything vertically.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies** with
  `guide=""`; register the project and re-run the syncs before the first release.
- **Nothing has been through a show.**

---

## Open questions

- **Should the capture have a line aperture?** A point sample aliases when the host
  is taller than the standard (720 → 576). A box over the line's height is more like a
  camera and would soften further; it would also make `--same` a comparison against
  an averaged picture rather than bytes.
- **Should the store be Rec. 601 wide (720 samples)?** More authentic, cheaper at 4K,
  and immune to resizes by construction — and horizontally soft on every picture.
- **Should the motion search be hierarchical?** ±16 px a field is 800 px/s at 50
  fields; a fast pan at 4K outruns it.
- **A Motion Comp vertical check** would want a vertically moving pattern and a
  tolerance of a field line, the method's.
- **`rebate`'s `--pipe` dies of SIGPIPE on a closed stdout.** Measured while writing
  this one's check: `rbtest --pipe` into `head -c 100` exits **141**, not the 1 its
  CLAUDE.md states, because nothing ignores SIGPIPE, and its `verify.sh` does not
  test a closed stdout. Siblings copied from it likely share it. Not fixed here —
  another repo.

---

## Siblings

- **cadence** — the field machinery: fields by time, parity by row, never between
  rows, the weave a field late, Bob Linear.
- **afterglow** — a ring of timestamped recent frames.
- **rebate** and **pitch** — the harness, verify and CI shape, the negative controls,
  the `--pipe` contract.
- **slowscan** — `--offline`, `--allow-no-gl`, `check-shaders.sh` for the GL-less
  runner.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **readout** — the clock-unit voting.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.

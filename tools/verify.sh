#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler
#                 (tools/check-shaders.sh, which CI runs too), on the exact
#                 strings `sttest --dump-shaders` writes.
#   offline       the checks that need no GL, and their negative controls:
#                   --model     the plugin's temporal apertures and vertical
#                               filters against the stated geometry and
#                               kernels; 50 -> 60 repeats in six bit for bit;
#                               50 -> 59.94 creeps 1/200 exactly
#                   --clock     a six-day millisecond host clock makes the
#                               same fields as a fresh seconds one
#                   --names     nothing a host will silently truncate
#   physics       every rendering check, at TWO rasters: 320x180, which is
#                 what CI renders at, and 1280x720. Each is measured out of
#                 the picture:
#                   --weights   each output field's levels are its temporal
#                               weights; the six-field cycle; the creep
#                   --judder    the repeat pattern and the double image,
#                               whole-pixel and fractional, separately
#                   --lines     marked lines land where the geometry says;
#                               the impulse response and a zone plate's G(f)
#                               are the stated taps'
#                   --same      Same, Drop/Repeat, one tap: the fielded input,
#                               byte for byte
#                   --mc        motion compensation smooths a pan and tears
#                               only where an occlusion's blocks reach
#                   --resize    a resize mid-run keeps the field store
#                   --negative  every one of those FAILS on a perturbed model
#   pipe          the fleet's --pipe contract: whole frames only, a cue naming
#                 no control refused, and exit 1 -- not a silent 0, not a
#                 SIGPIPE 141 -- on a failed render or a closed stdout.
#   sweep         does every control change the picture.
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel 4 >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

STTEST="$BUILD/sttest"

step "shaders"
if out=$(tools/check-shaders.sh "$STTEST" 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 | sed 's/^ *//' )"
else
	fail "a shader does not compile"
	printf '%s\n' "$out"
fi

step "offline (no GL)"
for check in model clock names negative-offline; do
	if out=$("$STTEST" --$check 2>&1); then
		# The negative controls' verdict, not the OFFLINE banner after it.
		summary=$( printf '%s\n' "$out" | grep -E '^negative:' | tail -1 )
		[ -n "$summary" ] || summary=$( printf '%s\n' "$out" | grep -v '^$' | grep -v 'checks,' | tail -1 )
		pass "sttest --$check: $summary"
	else
		fail "sttest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in weights judder lines same mc resize negative; do
		if out=$("$STTEST" --$check --size $size 2>&1); then
			pass "sttest --$check: $( printf '%s\n' "$out" | grep -v '^$' | grep -v 'checks,' | tail -1 )"
		else
			fail "sttest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

got=$( "$STTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever sttest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$STTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

# A failed render stops the stream with exit 1 and nothing after it. The
# failure is injected by the harness (--fail-render-at), because the plugin
# only fails on input no ffmpeg would send.
got=$( "$STTEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

# A reader that goes away: forty frames is far more than a pipe buffer holds,
# so the writes after head leaves must fail. Exit 1, said on stderr -- not the
# 141 of a process SIGPIPE killed before it could say anything.
"$STTEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 100 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout: exit 1"
else
	fail "a closed stdout gave exit $status, not 1"
fi
rm -f "$raw" "$many" "$cues"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$STTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$STTEST" --bench --frames 60 2>&1 | sed -n '3,6p' | sed 's/^/   /'

BUNDLE="$BUILD/Standards.bundle"
BIN="$BUNDLE/Contents/MacOS/Standards"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.standards" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Standards.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Standards" "id:          ST01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))

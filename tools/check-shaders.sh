#!/usr/bin/env bash
#
# Every shader the plugin compiles, through a real GLSL compiler, before a host
# has to find out. Called by tools/verify.sh AND by CI, so the two cannot
# drift: a GitHub macOS runner cannot create an accelerated GL context, so in
# CI this is the only thing that looks at the GLSL at all. It is not a
# substitute for a driver -- only a real driver catches the class of bug where
# Apple's Metal-backed GL disagrees with the compiler, and that is checked on
# the dev Mac by the rendering checks, and nowhere else.
#
#   tools/check-shaders.sh [path/to/sttest]
#
# The text compiled is what `sttest --dump-shaders` writes: the exact strings
# the plugin hands the driver, not a transcription of them.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those
# are Vulkan rules and not GLSL ones, and without the flag every shader "fails"
# for reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#
set -uo pipefail
cd "$(dirname "$0")/.."

STTEST="${1:-build/sttest}"
EXPECTED=8

if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
	exit 0
fi
if [ ! -x "$STTEST" ]; then
	printf '   %s is not built\n' "$STTEST"
	exit 1
fi

dir="$( mktemp -d )"
"$STTEST" --dump-shaders "$dir" >/dev/null
n=0; bad=0
for shader in "$dir"/*.vert "$dir"/*.frag; do
	[ -e "$shader" ] || continue
	n=$(( n + 1 ))
	if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
		printf '   %s does not compile\n' "$( basename "$shader" )"
		sed "s|$dir/||; s|^|      |" "$dir/err"
		bad=$(( bad + 1 ))
	fi
done
rm -rf "$dir"

# The count is asserted, not counted up to: a check that silently looks at
# fewer shaders than exist is worse than no check.
if [ "$n" -ne "$EXPECTED" ]; then
	printf '   %d shaders dumped, expected %d -- the dump has gone stale\n' "$n" "$EXPECTED"
	exit 1
fi
if [ "$bad" -ne 0 ]; then
	exit 1
fi
printf '   %d shaders, all compile\n' "$n"
exit 0

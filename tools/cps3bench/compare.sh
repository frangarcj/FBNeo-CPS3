#!/bin/bash
# Run the CPS3 core with the SH2 interpreter and with the DRC, compare the
# framebuffer CRCs and report the speed of each.
#
#   compare.sh <rom.zip> [frames] [every]
#
# CORE and SYSDIR can be overridden from the environment.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
rom="$1"
frames="${2:-3000}"
every="${3:-300}"
core="${CORE:-$root/src/burner/libretro/fbneo_cps3_libretro.dylib}"
sysdir="${SYSDIR:-$(dirname "$rom")}"
out="${OUT:-$(mktemp -d)}"

make -s -C "$here" >/dev/null || exit 1

FBNEO_SH2_DRC=0 "$here/cps3bench" "$core" "$rom" -s "$sysdir" -n "$frames" -e "$every" -q > "$out/int.txt" 2> "$out/int.err"
echo "interpreter: exit $? $(tail -1 "$out/int.txt")"
"$here/cps3bench" "$core" "$rom" -s "$sysdir" -n "$frames" -e "$every" -q > "$out/drc.txt" 2> "$out/drc.err"
echo "drc:         exit $? $(tail -1 "$out/drc.txt")"

if diff <(grep '^frame' "$out/int.txt") <(grep '^frame' "$out/drc.txt") > "$out/frames.diff"; then
	echo "frames: identical"
else
	echo "frames: differ (first differences below, logs in $out)"
	head -6 "$out/frames.diff"
fi

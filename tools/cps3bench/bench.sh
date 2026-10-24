#!/bin/bash
# Speed of the CPS3 core: best of several runs, for the DRC and the interpreter.
#
#   bench.sh <rom.zip> [frames] [runs] [core options...]
#
# CORE and SYSDIR can be overridden from the environment.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
rom="$1"
frames="${2:-10000}"
runs="${3:-3}"
shift; shift; shift
opts=()
for o in "$@"; do opts+=(-o "$o"); done
core="${CORE:-$root/src/burner/libretro/fbneo_cps3_libretro.dylib}"
sysdir="${SYSDIR:-$(dirname "$rom")}"

make -s -C "$here" >/dev/null || exit 1

for mode in 1 0; do
	best=0
	for i in $(seq "$runs"); do
		fps=$(FBNEO_SH2_DRC=$mode "$here/cps3bench" "$core" "$rom" -s "$sysdir" -n "$frames" -e 0 -q ${opts[@]+"${opts[@]}"} 2>/dev/null \
			| sed -n 's/.*-> \([0-9.]*\) fps.*/\1/p')
		best=$(echo "$fps $best" | awk '{ print ($1 > $2) ? $1 : $2 }')
	done
	[ "$mode" = 1 ] && name=drc || name=interpreter
	echo "$name: $best fps (best of $runs x $frames frames)"
done

#!/bin/sh
# Make the PS Vita zip of the core: RetroArch built with the core, its core
# info and the licenses, laid out as on the Vita (unzip to the root of ux0:).
#
#   package.sh <RetroArch source dir, built> <output.zip>

set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
ra="$(cd "$1" && pwd)"
out="$2"
self="$ra/retroarch_vita.self"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
app="$tmp/app/RETROARCH"
mkdir -p "$app/info" "$tmp/licenses"

cp "$self" "$app/fbneo_cps3_libretro.self"
cp "$here/fbneo_cps3_libretro.info" "$app/info/"
source_url="${SOURCE_URL:-$(git -C "$root" remote get-url origin)}"
ra_version="${RA_VERSION:-$(git -C "$ra" describe --tags 2>/dev/null || echo unknown)}"
sed -e "s|@SOURCE@|$source_url|" -e "s|@RETROARCH@|$ra_version|" "$here/README.txt" > "$tmp/README.txt"
cp "$root/src/license.txt" "$tmp/licenses/FBNeo.txt"
cp "$root/src/cpu/sh2/drc/COPYING" "$tmp/licenses/PicoDrive-DRC.txt"
cp "$ra/COPYING" "$tmp/licenses/RetroArch.txt"

rm -f "$out"
(cd "$tmp" && zip -qr - .) > "$out"

#!/bin/bash
# Build the CPS3 core with CPS3_PROFILE=1, link it into RetroArch, run it on a
# PS Vita and print the profile lines it logs.
#
#   vita_profile.sh [lines] [extra make arguments for the core...]
#
# Needs VitaShell/vitacompanion FTP (port 1337) and vitacompanion commands
# (port 1338) on the Vita, and on it:
#   - ux0:data/retroarch/retroarch-salamander.cfg with
#       libretro_path = "app0:/fbneo_cps3_libretro.self"
#   - ux0:data/retroarch/autoload.txt with the path of the ROM to run
#   - retroarch.cfg with log_to_file = "true", libretro_log_level = "0"
# RetroArch is built with VITA_AUTOLOAD_CONTENT=1 (see platform_psp.c there).
#
# Environment: VITA_IP, RA_DIR (RetroArch source), BUILD_DIR (copy of src to
# build in, so the host objects are left alone).

set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lines="${1:-12}"
shift || true
vita="${VITA_IP:-192.168.0.25}"
ra="${RA_DIR:-$root/../cps3/RetroArch}"
build="${BUILD_DIR:-/tmp/fbneo_cps3_vita}"
export PATH="$VITASDK/bin:$PATH"

mkdir -p "$build"
rsync -a --exclude '*.o' --exclude '*.d' --exclude '*.dylib' --exclude '*.a' --exclude .git "$root/src" "$build/"
touch "$build/src/burn/drv/cps3/cps3run.cpp"	# profiling may have been off
make -s -C "$build/src/burner/libretro" SUBSET=cps3 platform=vita CPS3_PROFILE=1 -j6 "$@" > "$build/core.log" 2>&1 \
	|| { tail -20 "$build/core.log"; exit 1; }
cp "$build/src/burner/libretro/fbneo_cps3_libretro_vita.a" "$ra/libretro_vita.a"
rm -f "$ra/retroarch_vita.self" "$ra/retroarch_vita.elf"
make -s -C "$ra" -f Makefile.vita VITA_AUTOLOAD_CONTENT=1 -j6 > "$build/ra.log" 2>&1 \
	|| { tail -20 "$build/ra.log"; exit 1; }

echo "nosleep on" | nc -w 3 "$vita" 1338 > /dev/null	# don't let it sleep while we watch
echo destroy | nc -w 3 "$vita" 1338 > /dev/null
curl -s --max-time 600 -T "$ra/retroarch_vita.self" "ftp://$vita:1337/ux0:/app/RETROARCH/fbneo_cps3_libretro.self"

log="$build/retroarch.log"
# the first launch right after uploading sometimes hangs, so retry once
for attempt in 1 2; do
	sleep 3
	echo destroy | nc -w 3 "$vita" 1338 > /dev/null
	sleep 2
	echo "launch RETROARCH" | nc -w 3 "$vita" 1338 > /dev/null
	SECONDS=0
	until [ $SECONDS -gt 60 ]; do
		sleep 10
		curl -s --max-time 15 -o "$log" "ftp://$vita:1337/ux0:/data/retroarch/logs/retroarch.log" || continue
		grep -aq 'CPS3 profile' "$log" && break 2
	done
done

SECONDS=0
until [ $SECONDS -gt $((lines * 12 + 60)) ]; do
	curl -s --max-time 15 -o "$log" "ftp://$vita:1337/ux0:/data/retroarch/logs/retroarch.log" || continue
	[ "$(grep -ac 'CPS3 profile' "$log")" -ge "$lines" ] && break
	sleep 10
done
grep -a 'CPS3 profile' "$log" | sed 's/.*us per frame: //'

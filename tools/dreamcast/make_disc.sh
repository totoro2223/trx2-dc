#!/bin/bash
# Builds a self-booting CDI with TRX and the original TR2 data, for Flycast
# and GDEMU. The game data comes from the player's own copy and never goes
# into the repository.
#
#   tools/dreamcast/make_disc.sh <build-dir> <tr2-data-dir> [output.cdi]
#
# <tr2-data-dir> is the original DATA directory (*.TR2 and MAIN.SFX).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail

BUILD_DIR="$(cd "${1:?build dir}" && pwd)"
TR2_DATA="$(cd "${2:?TR2 DATA dir}" && pwd)"
OUT="${3:-$BUILD_DIR/TRX.cdi}"
ROOT="$(pwd)"
SHIP="$ROOT/data/trx/ship"
STAGE="$BUILD_DIR/disc"
TMP="$BUILD_DIR/disc-tmp"

rm -rf "$STAGE" "$TMP"
mkdir -p "$STAGE" "$TMP"

echo "== boot binary"
"${KOS_CC%gcc}objcopy" -R .stack -O binary "$BUILD_DIR/TRX.elf" "$TMP/TRX.bin"
/opt/toolchains/dc/kos/utils/scramble/scramble "$TMP/TRX.bin" "$STAGE/1ST_READ.BIN"
/opt/toolchains/dc/kos/utils/makeip/makeip -f \
  -g "TRX" -c "TRX PORT" -n "T-TRX00" \
  "$TMP/IP.BIN" > /dev/null

echo "== engine data"
cp -r "$SHIP/cfg" "$STAGE/cfg"
cp -r "$SHIP/modules" "$STAGE/modules"
cp -r "$SHIP/scripts" "$STAGE/scripts"
mkdir -p "$STAGE/games"
cp -r "$SHIP/games/tr2" "$STAGE/games/tr2"
# The GLSL shaders are of no use without OpenGL 3.
rm -rf "$STAGE/cfg/shaders"
# Injections too large for 16 MiB of RAM. The engine holds every injection of
# a level decompressed while the level loads, and leaves out with a warning
# any it cannot find.
#   lara_outfits.bin  every alternative outfit, 5.3 MiB decompressed
DC_EXCLUDED_INJECTIONS="lara_outfits.bin"
for inj in $DC_EXCLUDED_INJECTIONS; do
  rm -f "$STAGE/games/tr2/injections/$inj"
done
# Optional extras that cost memory in every level.
#   scripts/bignasty  the BFG9000 easter egg (its script and models)
rm -rf "$STAGE/scripts/bignasty"

echo "== TR2 data"
mkdir -p "$STAGE/games/tr2/levels"
for f in "$TR2_DATA"/*; do
  name="$(basename "$f")"
  lower="$(echo "$name" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in
    *.tr2) cp "$f" "$STAGE/games/tr2/levels/$lower" ;;
    main.sfx) cp "$f" "$STAGE/games/tr2/main.sfx" ;;
  esac
done

echo "== image"
mkisofs -quiet -C 0,11702 -V TRX -G "$TMP/IP.BIN" -r -J -l \
  -o "$TMP/TRX.iso" "$STAGE"
cdi4dc "$TMP/TRX.iso" "$OUT" > /dev/null
rm -rf "$TMP"
ls -la "$OUT"

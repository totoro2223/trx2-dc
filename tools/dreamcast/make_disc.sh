#!/bin/bash
# Builds a self-booting CDI with TRX and the original TR2 or TR3 data, for
# Flycast and GDEMU. The game data comes from the player's own copy and never
# goes into the repository.
#
#   tools/dreamcast/make_disc.sh <build-dir> <data-dir> [output.cdi] [level]
#   TRX_DC_GAME=tr3 tools/dreamcast/make_disc.sh <build-dir> <tr3-data-dir> ...
#
# <data-dir> is the original DATA directory (*.TR2 and MAIN.SFX); TR3's
# cutscenes are taken from the cuts directory beside it. TRX_DC_GAME picks
# the game, tr2 (the default) or tr3.
# [level] starts the disc straight in that level, past the level select
# (test discs); "1 8" also loads saved games 0 and 1 in turn, 8 times (see
# DC_Watchdog_TakeTestLoad).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail

BUILD_DIR="$(cd "${1:?build dir}" && pwd)"
GAME_DATA="$(cd "${2:?DATA dir}" && pwd)"
GAME="${TRX_DC_GAME:-tr2}"
case "$GAME" in
  tr2) ENGINE=2 ;;
  tr3) ENGINE=3 ;;
  *) echo "TRX_DC_GAME must be tr2 or tr3" >&2; exit 1 ;;
esac
OUT="${3:-$BUILD_DIR/TRX.cdi}"
START_LEVEL="${4:-}"
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

echo "== version"
# Every disc made gets the next number; the loading screen shows it.
COUNTER="$ROOT/build/dc-build-number"
BUILD_NUM=$(( $(cat "$COUNTER" 2>/dev/null || echo 0) + 1 ))
echo "$BUILD_NUM" > "$COUNTER"
echo "DC build $BUILD_NUM" > "$STAGE/VERSION.TXT"
cat "$STAGE/VERSION.TXT"
echo "$ENGINE" > "$STAGE/GAME.TXT"
echo "game $GAME"
if [ -n "$START_LEVEL" ]; then
  echo "$START_LEVEL" > "$STAGE/START.TXT"
  echo "starts in level $START_LEVEL"
fi

echo "== engine data"
cp -r "$SHIP/cfg" "$STAGE/cfg"
cp -r "$SHIP/modules" "$STAGE/modules"
cp -r "$SHIP/scripts" "$STAGE/scripts"
mkdir -p "$STAGE/games"
cp -r "$SHIP/games/$GAME" "$STAGE/games/$GAME"
# The GLSL shaders are of no use without OpenGL 3.
rm -rf "$STAGE/cfg/shaders"
# Injections too large for 16 MiB of RAM. The engine holds every injection of
# a level decompressed while the level loads, and leaves out with a warning
# any it cannot find.
#   lara_outfits.bin  every alternative outfit, 5.3 MiB decompressed
DC_EXCLUDED_INJECTIONS="lara_outfits.bin"
for inj in $DC_EXCLUDED_INJECTIONS; do
  rm -f "$STAGE/games/$GAME/injections/$inj"
done
# Optional extras that cost memory in every level.
#   scripts/bignasty  the BFG9000 easter egg (its script and models)
rm -rf "$STAGE/scripts/bignasty"

echo "== $GAME data"
mkdir -p "$STAGE/games/$GAME/levels"
for f in "$GAME_DATA"/* "$GAME_DATA"/../cuts/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lower="$(echo "$name" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in
    *.tr2) cp "$f" "$STAGE/games/$GAME/levels/$lower" ;;
    main.sfx) cp "$f" "$STAGE/games/$GAME/main.sfx" ;;
  esac
done

echo "== texture pages in VQ"
# Every page of every level and injection on the disc, which the game finds
# by its contents (see tools/dreamcast/texvq.c); made again only when the
# tool or the data change.
VQ="$BUILD_DIR/textures-$GAME.vq"
VQ_TOOL="$BUILD_DIR/texvq.exe"
if [ ! -f "$VQ_TOOL" ] || [ "$ROOT/tools/dreamcast/texvq.c" -nt "$VQ_TOOL" ]; then
  gcc -O2 -fopenmp -o "$VQ_TOOL" "$ROOT/tools/dreamcast/texvq.c" -lz
fi
VQ_INPUTS=("$STAGE"/games/$GAME/levels/*.tr2 "$STAGE"/games/$GAME/injections/*.bin)
if [ ! -f "$VQ" ] || [ "$VQ_TOOL" -nt "$VQ" ] \
  || [ -n "$(find "$GAME_DATA" "$SHIP/games/$GAME/injections" -newer "$VQ" -print -quit)" ]; then
  "$VQ_TOOL" "$VQ" "${VQ_INPUTS[@]}"
fi
cp "$VQ" "$STAGE/textures.vq"

echo "== image"
mkisofs -quiet -C 0,11702 -V TRX -G "$TMP/IP.BIN" -r -J -l \
  -o "$TMP/TRX.iso" "$STAGE"
cdi4dc "$TMP/TRX.iso" "$OUT" > /dev/null
rm -rf "$TMP"
ls -la "$OUT"

#!/bin/bash
# Builds a self-booting CDI that holds all three games (g1, g2 and g3 under the
# disc's root) and, from the remastered data, the expansion of each. The game
# the disc starts in is TR1; the main menu's Up and Down go to the game before
# and after (src/platform/dreamcast/dc_games.c), and the passport's New Game
# page offers the expansion.
#
#   tools/dreamcast/make_disc_all.sh <build-dir> [output.cdi]
#
# TRX_DC_REMASTER is the remastered collection's folder (the one that holds 1,
# 2 and 3); the default is where the other discs take theirs. With films the
# disc would not fit (~1.8 GB), so it has none unless TRX_DC_FMV says which
# (the games' films are kept in their own caches, see make_disc.sh).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail

BUILD_DIR="$(cd "${1:?build dir}" && pwd)"
OUT="${2:-$BUILD_DIR/TRX-all.cdi}"
REMASTER="${TRX_DC_REMASTER:-/h/GOG/Tomb Raider I-III Remastered}"
ROOT="$(pwd)"
STAGE="$BUILD_DIR/disc-all"
TMP="$BUILD_DIR/disc-tmp"

rm -rf "$STAGE" "$TMP"
mkdir -p "$STAGE" "$TMP"

# The copyright pictures of the original games, where they are (Steam).
STEAM="${TRX_DC_STEAM:-/g/SteamLibrary/steamapps/common}"
LEGAL1="/h/SteamLibrary/steamapps/common/Tomb Raider (I)/LARA_RTX/DATA/EIDOSPC.PCX"
LEGAL2="$STEAM/Tomb Raider (II)/data/legal.PCX"
LEGAL3="$STEAM/TombRaider (III)/pix/LEGAL.BMP"

for n in 1 2 3; do
  case "$n" in
    1) MOD=tr1-ub; LEGAL="$LEGAL1" ;;
    2) MOD=tr2-gm; LEGAL="$LEGAL2" ;;
    3) MOD=tr3-la; LEGAL="$LEGAL3" ;;
  esac
  echo "=== TR$n"
  TRX_DC_GAME="tr$n" TRX_DC_SOURCE=remastered TRX_DC_SUBDIR="g$n" \
    TRX_DC_FMV="${TRX_DC_FMV:-0}" TRX_DC_LEGAL="$LEGAL" \
    TRX_DC_MOD="$MOD" TRX_DC_MOD_OPTIONAL=1 \
    bash tools/dreamcast/make_disc.sh "$BUILD_DIR" "$REMASTER/$n/DATA"
done

echo "=== root"
"${KOS_CC%gcc}objcopy" -R .stack -O binary "$BUILD_DIR/TRX.elf" "$TMP/TRX.bin"
/opt/toolchains/dc/kos/utils/scramble/scramble "$TMP/TRX.bin" "$STAGE/1ST_READ.BIN"
/opt/toolchains/dc/kos/utils/makeip/makeip -f \
  -g "TRX" -c "TRX PORT" -n "T-TRX00" \
  "$TMP/IP.BIN" > /dev/null
echo "1 2 3" > "$STAGE/GAMES.TXT"
# A test disc that starts every game at its title (the level select needs the
# pad): TRX_DC_ALL_START=-1.
if [ -n "${TRX_DC_ALL_START:-}" ]; then
  for n in 1 2 3; do echo "$TRX_DC_ALL_START" > "$STAGE/g$n/START.TXT"; done
fi
# A test disc that changes the game from the title (TRX_DC_ALL_SWITCH="7 300":
# the code of executor.c's list and the frame; in TR1's folder only).
if [ -n "${TRX_DC_ALL_SWITCH:-}" ]; then
  echo "$TRX_DC_ALL_SWITCH" > "$STAGE/g1/SWITCH.TXT"
fi

echo "== image"
mkisofs -quiet -C 0,11702 -V TRX -G "$TMP/IP.BIN" -r -J -l \
  -o "$TMP/TRX.iso" "$STAGE"
cdi4dc "$TMP/TRX.iso" "$OUT" > /dev/null
rm -rf "$TMP"
ls -la "$OUT"

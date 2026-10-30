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
# TRX_DC_SOURCE says what copy the data comes from, for the music and the
# loading pictures beside it (the levels are the same in all):
#   remastered  Tomb Raider I-III Remastered (GOG or Steam): <n>/DATA, with
#               TRACKS/*.OGG and PIX/OG/*.DDS beside it
#   classic     the original games (Steam or GOG): music/*.mp3 or audio/*.ogg
#               (TR2), audio/cdaudio.wad and pix/*.BMP (TR3)
#   auto        (the default) remastered if TRACKS or PIX/OG is there
# Converting the music and pictures takes ffmpeg; without it the disc has
# neither.
# TRX_DC_BENCH=<n> runs the game's benchmark demo n (TR3: 0, London) at boot.
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
  tr1) ENGINE=1 ;;
  *) echo "TRX_DC_GAME must be tr1, tr2 or tr3" >&2; exit 1 ;;
esac
SOURCE="${TRX_DC_SOURCE:-auto}"
if [ "$SOURCE" = auto ]; then
  if [ -d "$GAME_DATA/../TRACKS" ] || [ -d "$GAME_DATA/../PIX/OG" ]; then
    SOURCE=remastered
  else
    SOURCE=classic
  fi
fi
case "$SOURCE" in
  remastered | classic) ;;
  *) echo "TRX_DC_SOURCE must be auto, remastered or classic" >&2; exit 1 ;;
esac
OUT="${3:-$BUILD_DIR/TRX.cdi}"
START_LEVEL="${4:-}"
ROOT="$(pwd)"
SHIP="$ROOT/data/trx/ship"
STAGE="$BUILD_DIR/disc"
TMP="$BUILD_DIR/disc-tmp"
# TRX_DC_SUBDIR=g1 stages this game as one folder of a disc that holds several
# (make_disc_all.sh): no boot binary and no image here, only the folder.
SUBDIR="${TRX_DC_SUBDIR:-}"
if [ -n "$SUBDIR" ]; then
  STAGE="$BUILD_DIR/disc-all/$SUBDIR"
  TMP="$BUILD_DIR/disc-tmp-$SUBDIR"
fi

rm -rf "$STAGE" "$TMP"
mkdir -p "$STAGE" "$TMP"

if [ -z "$SUBDIR" ]; then
  echo "== boot binary"
  "${KOS_CC%gcc}objcopy" -R .stack -O binary "$BUILD_DIR/TRX.elf" "$TMP/TRX.bin"
  /opt/toolchains/dc/kos/utils/scramble/scramble "$TMP/TRX.bin" "$STAGE/1ST_READ.BIN"
  /opt/toolchains/dc/kos/utils/makeip/makeip -f \
    -g "TRX" -c "TRX PORT" -n "T-TRX00" \
    "$TMP/IP.BIN" > /dev/null
fi

echo "== version"
# Every disc made gets the next number; the loading screen shows it.
COUNTER="$ROOT/build/dc-build-number"
BUILD_NUM=$(( $(cat "$COUNTER" 2>/dev/null || echo 0) + 1 ))
echo "$BUILD_NUM" > "$COUNTER"
echo "DC build $BUILD_NUM" > "$STAGE/VERSION.TXT"
cat "$STAGE/VERSION.TXT"
echo "$ENGINE" > "$STAGE/GAME.TXT"
echo "game $GAME, $SOURCE data"
if [ -n "${TRX_DC_DEMO:-}" ]; then
  # The disc starts straight in that demo of the title (test discs).
  echo "$TRX_DC_DEMO" > "$STAGE/DEMO.TXT"
  echo "starts in demo $TRX_DC_DEMO"
fi
if [ -n "${TRX_DC_BENCH:-}" ]; then
  # The disc runs the benchmark (that demo of the game) as soon as it boots.
  echo "$TRX_DC_BENCH" > "$STAGE/BENCH.TXT"
  echo "starts in benchmark $TRX_DC_BENCH"
fi
if [ -n "${TRX_DC_SAVE:-}" ]; then
  # The game is saved every that many frames, and loaded after every fifth.
  echo "$TRX_DC_SAVE" > "$STAGE/SAVE.TXT"
fi
if [ -n "${TRX_DC_EXIT:-}" ]; then
  # The game is left that many frames into the level (a test of the exit).
  echo "$TRX_DC_EXIT" > "$STAGE/EXIT.TXT"
fi
if [ -n "${TRX_DC_GIVE:-}" ]; then
  # Lara is handed that weapon (a LARA_GUN_TYPE) a moment into the level.
  echo "$TRX_DC_GIVE" > "$STAGE/GIVE.TXT"
fi
if [ -n "${TRX_DC_FIRE:-}" ]; then
  # The pistols smoke every n frames (a test of the smoke's cost).
  echo "$TRX_DC_FIRE" > "$STAGE/FIRE.TXT"
fi
if [ -n "$START_LEVEL" ]; then
  echo "$START_LEVEL" > "$STAGE/START.TXT"
  echo "starts in level $START_LEVEL"
fi

echo "== engine data"
# The engine's Lua bytecode, which the game puts in video memory.
cp "$(find "$BUILD_DIR" -name TRXLUA.BIN | head -1)" "$STAGE/TRXLUA.BIN"
cp -r "$SHIP/cfg" "$STAGE/cfg"
cp -r "$SHIP/modules" "$STAGE/modules"
cp -r "$SHIP/scripts" "$STAGE/scripts"
mkdir -p "$STAGE/games"
cp -r "$SHIP/games/$GAME" "$STAGE/games/$GAME"
# The GLSL shaders are of no use without OpenGL 3.
rm -rf "$STAGE/cfg/shaders"
if [ -n "${TRX_DC_LEVELS:-}" ]; then
  # A test disc that goes through the levels from the one it starts in:
  # "<last level> <frames in each>", e.g. "14 450" (dc_main.c, LEVELS.TXT).
  echo "$TRX_DC_LEVELS" > "$STAGE/LEVELS.TXT"
  echo "levels up to $TRX_DC_LEVELS"
fi
if [ -n "${TRX_DC_SWITCH:-}" ]; then
  # A test disc that changes the mode on a frame, as the menu does:
  # "<mode number> <frame>", e.g. "2 900" (dc_main.c, SWITCH.TXT).
  echo "$TRX_DC_SWITCH" > "$STAGE/SWITCH.TXT"
  echo "mode switch $TRX_DC_SWITCH"
fi
if [ -n "${TRX_DC_SHADOW:-}" ]; then
  # A test disc with that shadow type (the SHADOW_TYPE's number: 3 volume, 4 soft,
  # 5 soft high), whatever the mode picks.
  echo "$TRX_DC_SHADOW" > "$STAGE/SHADOW.TXT"
  echo "shadow type $TRX_DC_SHADOW"
fi
if [ -n "${TRX_DC_OBJECT_SHADOWS:-}" ]; then
  # A test disc with the object shadows on (1): with volume shadows, enemies,
  # objects and, under a flare, the walls near it cast volumes. Not in the menu.
  echo "$TRX_DC_OBJECT_SHADOWS" > "$STAGE/OBJSHADOW.TXT"
  echo "object shadows $TRX_DC_OBJECT_SHADOWS"
fi
if [ -n "${TRX_DC_CEL:-}" ]; then
  # A test disc with the cel shading and the room cel shading fixed: "<0|1> <0|1>".
  echo "$TRX_DC_CEL" > "$STAGE/CEL.TXT"
  echo "cel shading $TRX_DC_CEL"
fi
if [ -n "${TRX_DC_MODE:-}" ]; then
  # A test disc that starts in that mode (graphics, performance, original or
  # experimental-60), whatever the settings say (dc_main.c, MODE.TXT).
  case "$TRX_DC_MODE" in
    graphics) echo 0 > "$STAGE/MODE.TXT" ;;
    performance) echo 1 > "$STAGE/MODE.TXT" ;;
    original) echo 2 > "$STAGE/MODE.TXT" ;;
    experimental-60) echo 3 > "$STAGE/MODE.TXT" ;;
    *) echo "TRX_DC_MODE must be graphics, performance, original or experimental-60" >&2; exit 1 ;;
  esac
  echo "mode $TRX_DC_MODE"
fi
if [ -n "${TRX_DC_RESETOPT:-}" ]; then
  # A test disc that deletes the render options from the memory card as it
  # starts, so that it begins from the defaults (dc_main.c, RESETOPT.TXT).
  echo 1 > "$STAGE/RESETOPT.TXT"
  echo "render options reset"
fi
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
if [ -n "${TRX_DC_DEMO:-}" ]; then
  # A test disc: the title shows its next demo after a second, not thirty.
  sed -i 's|(delay_ok and delay or 0) \* trx.game.LOGIC_FPS|1 * trx.game.LOGIC_FPS|'     "$STAGE/modules/ui/ring/screen.lua"
fi

echo "== $GAME data"
mkdir -p "$STAGE/games/$GAME/levels"
for f in "$GAME_DATA"/* "$GAME_DATA"/../cuts/*; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lower="$(echo "$name" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in
    *.tr2 | *.phd)
      # TRX_DC_KEEP="wall title": a small test disc with only those levels.
      if [ -n "${TRX_DC_KEEP:-}" ] && [[ " $TRX_DC_KEEP " != *" ${lower%.*} "* ]]; then
        continue
      fi
      cp "$f" "$STAGE/games/$GAME/levels/$lower" ;;
    main.sfx) cp "$f" "$STAGE/games/$GAME/main.sfx" ;;
  esac
done

# main.sfx as the AICA's ADPCM, with an index of where every sample starts, so
# that a level reads only its own samples (level/pipeline.c, audio.c).
if [ -f "$STAGE/games/$GAME/main.sfx" ]; then
  SFX="$(cygpath -w "$STAGE/games/$GAME/main.sfx")"
  python3 "$ROOT/tools/dreamcast/sfx_encode.py" "$SFX" "$SFX.new"     && mv "$SFX.new" "$SFX" && mv "$SFX.new.idx" "$SFX.idx"
fi

# An expansion (TRX_DC_MOD=tr1-ub, tr2-gm or tr3-la) rides on its base game's
# disc: its gameflow and scripts, and its levels from the UB, GM or LA folder
# beside the base game's, which the engine runs by name (MOD.TXT).
MOD="${TRX_DC_MOD:-}"
if [ -n "$MOD" ]; then
  case "$MOD" in
    tr1-ub) MOD_BASE=tr1; MOD_DIR=UB ;;
    tr2-gm) MOD_BASE=tr2; MOD_DIR=GM ;;
    tr3-la) MOD_BASE=tr3; MOD_DIR=LA ;;
    *) echo "TRX_DC_MOD must be tr1-ub, tr2-gm or tr3-la" >&2; exit 1 ;;
  esac
  if [ "$MOD_BASE" != "$GAME" ] || [ ! -d "$GAME_DATA/$MOD_DIR" ]; then
    echo "$MOD needs TRX_DC_GAME=$MOD_BASE and $GAME_DATA/$MOD_DIR" >&2
    exit 1
  fi
  echo "== expansion $MOD"
  cp -r "$SHIP/games/$MOD" "$STAGE/games/$MOD"
  mkdir -p "$STAGE/games/$MOD/levels"
  for f in "$GAME_DATA/$MOD_DIR"/*; do
    [ -f "$f" ] || continue
    lower="$(basename "$f" | tr '[:upper:]' '[:lower:]')"
    case "$lower" in
      *.tr2 | *.phd) cp "$f" "$STAGE/games/$MOD/levels/$lower" ;;
    esac
  done
  # TRX_DC_MOD_OPTIONAL: the expansion is on the disc for the menu to start,
  # not what the disc starts in.
  [ -n "${TRX_DC_MOD_OPTIONAL:-}" ] || echo "$MOD" > "$STAGE/MOD.TXT"
  echo "$(ls "$STAGE/games/$MOD/levels" | wc -l) levels of $MOD"
fi

echo "== texture pages in VQ, and their relief"
# Every page of every level and injection on the disc, which the game finds
# by its contents, and the relief of the level pages the rooms use (see
# tools/dreamcast/texvq.c); made again only when the tool or the data change.
VQ="$BUILD_DIR/textures-${MOD:-$GAME}.vq"
BUMP="$BUILD_DIR/textures-${MOD:-$GAME}.bump"
VQ_TOOL="$BUILD_DIR/texvq.exe"
BUMP_HEADER="$ROOT/src/platform/dreamcast/render/bump_page.h"
if [ ! -f "$VQ_TOOL" ] || [ "$ROOT/tools/dreamcast/texvq.c" -nt "$VQ_TOOL" ] \
  || [ "$BUMP_HEADER" -nt "$VQ_TOOL" ]; then
  # SSE floats, as the Dreamcast's: x87's wider ones would round the relief
  # otherwise.
  gcc -O2 -fopenmp -msse2 -mfpmath=sse -o "$VQ_TOOL" \
    "$ROOT/tools/dreamcast/texvq.c" -lz -lm
fi
shopt -s nullglob
VQ_INPUTS=("$STAGE"/games/$GAME/levels/*.tr2 "$STAGE"/games/$GAME/levels/*.phd
  "$STAGE"/games/$GAME/injections/*.bin)
if [ -n "$MOD" ]; then
  VQ_INPUTS+=("$STAGE"/games/$MOD/levels/*.tr2 "$STAGE"/games/$MOD/levels/*.phd
    "$STAGE"/games/$MOD/injections/*.bin)
fi
shopt -u nullglob
if [ ! -f "$VQ" ] || [ ! -f "$BUMP" ] || [ "$VQ_TOOL" -nt "$VQ" ] \
  || [ -n "$(find "$GAME_DATA" "$SHIP/games/$GAME/injections" -newer "$VQ" -print -quit)" ]   || [ -n "$MOD" ]; then
  "$VQ_TOOL" -b "$BUMP" "$VQ" "${VQ_INPUTS[@]}"
fi
cp "$VQ" "$STAGE/textures.vq"
cp "$BUMP" "$STAGE/textures.bump"

echo "== loading pictures"
# The game's original 640x480 pictures (the middle of the remaster's 856x480
# ones; the classic TR3's pix as they are), shown behind the loading screen:
# 512x512 RGB565 as the PowerVR reads them, stretched back over the screen.
# Kept between discs; without ffmpeg the loading screen stays black. The
# classic TR2 has none.
PIX_CACHE="$BUILD_DIR/pix-$GAME-$SOURCE"
mkdir -p "$PIX_CACHE" "$STAGE/pix"
if [ "$SOURCE" = remastered ]; then
  PIX_FILES=("$GAME_DATA"/../PIX/OG/*.DDS)
  PIX_CROP="crop=640:480:(in_w-640)/2:0,"
else
  # TR3's pictures are BMPs beside the data; TR2's (title, credits) are
  # PCXs in the data itself.
  PIX_FILES=("$GAME_DATA"/../pix/*.BMP "$GAME_DATA"/../pix/*.bmp
    "$GAME_DATA"/title.PCX "$GAME_DATA"/TITLE.PCX "$GAME_DATA"/credit*.PCX
    "$GAME_DATA"/end.PCX "$GAME_DATA"/END.PCX
    "$GAME_DATA"/TITLEH.PCX "$GAME_DATA"/CRED?.PCX "$GAME_DATA"/INSTALL.PCX)
  PIX_CROP=""
fi
for f in "${PIX_FILES[@]}"; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lower="$(echo "${name%.*}" | tr '[:upper:]' '[:lower:]')"
  # The gameflow's name for the title picture.
  case "$lower" in
    title|titleuk) lower=title_eu ;;
    # TR1's title, credits (cred1 is credits_1) and install picture.
    titleh) lower=title ;;
    cred[1-3]) lower="credits_${lower#cred}" ;;
  esac
  out="$PIX_CACHE/$lower.565"
  if [ ! -f "$out" ] && command -v ffmpeg > /dev/null; then
    ffmpeg -hide_banner -loglevel error -y -i "$f" -frames:v 1 \
      -vf "${PIX_CROP}scale=512:512:flags=lanczos" \
      -f rawvideo -pix_fmt rgb565le "$out"
  fi
  [ -f "$out" ] && cp "$out" "$STAGE/pix/$lower.565"
done
# The copyright screen (the gameflow's legal_eu.webp) is not in the remaster's
# pictures: it is the classic game's legal.PCX (TR2, in its data directory) or
# pix/LEGAL.BMP (TR3). TRX_DC_LEGAL names one when the data comes from
# somewhere that has neither beside it.
if [ ! -f "$STAGE/pix/legal_eu.565" ]; then
  for f in "${TRX_DC_LEGAL:-}" "$GAME_DATA"/legal.PCX "$GAME_DATA"/legal.pcx     "$GAME_DATA"/../pix/LEGAL.BMP "$GAME_DATA"/../pix/legal.bmp; do
    [ -f "$f" ] || continue
    out="$PIX_CACHE/legal_eu.565"
    if [ ! -f "$out" ] && command -v ffmpeg > /dev/null; then
      ffmpeg -hide_banner -loglevel error -y -i "$f" -frames:v 1         -vf "scale=512:512:flags=lanczos" -f rawvideo -pix_fmt rgb565le "$out"
    fi
    [ -f "$out" ] && cp "$out" "$STAGE/pix/legal_eu.565" && echo "copyright screen from $f"
    break
  done
fi
# TR1's gameflow calls some pictures by other names than the remaster's files
# (peru is aztec.dds, greece greek.dds, atlantis atlan.dds), and its copyright
# screen (eidos.webp) is the remaster's PIX/LEGAL_1.DDS.
if [ "$GAME" = tr1 ]; then
  for pair in peru:aztec greece:greek atlantis:atlan title:title_eu; do
    [ -f "$STAGE/pix/${pair#*:}.565" ] && cp "$STAGE/pix/${pair#*:}.565" "$STAGE/pix/${pair%:*}.565"
  done
  # The original's EIDOSPC.PCX (TRX_DC_LEGAL) if given, else the remaster's.
  LEGAL1="${TRX_DC_LEGAL:-$GAME_DATA/../PIX/LEGAL_1.DDS}"
  if [ -f "$LEGAL1" ] && command -v ffmpeg > /dev/null; then
    out="$PIX_CACHE/eidos.565"
    LEGAL_CROP="$PIX_CROP"
    if [ -n "${TRX_DC_LEGAL:-}" ]; then
      LEGAL_CROP=""
      rm -f "$out"
    fi
    [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y -i "$LEGAL1" -frames:v 1       -vf "${LEGAL_CROP}scale=512:512:flags=lanczos" -f rawvideo -pix_fmt rgb565le "$out"
    [ -f "$out" ] && cp "$out" "$STAGE/pix/eidos.565"
  fi
fi
# The gameflow names each level's picture as india.webp and keeps the name
# only if such a file is installed: an empty one stands in for it.
mkdir -p "$STAGE/games/$GAME/images"
for f in "$STAGE"/pix/*.565; do
  [ -f "$f" ] && : > "$STAGE/games/$GAME/images/$(basename "$f" .565).webp"
done

echo "== music"
# The game's tracks as WAV files of stereo Yamaha ADPCM at 44.1 kHz, which the
# AICA plays itself (src/platform/dreamcast/audio.c): ~2.6 MB a minute. Kept
# between discs; without ffmpeg the game has no music.
# The files are named by their track number, as the game looks them up.
MUSIC_CACHE="$BUILD_DIR/music-$GAME-$SOURCE"
mkdir -p "$MUSIC_CACHE" "$STAGE/games/$GAME/music"
if [ "${TRX_DC_MUSIC:-1}" = 0 ]; then
  # TRX_DC_MUSIC=0: a disc without music.
  MUSIC_FILES=()
elif [ "$SOURCE" = remastered ]; then
  # The cutscenes' voices and some of the music are in the language's own
  # folder (TRACKS/EN: TR2's tracks 2 to 25, say), which the root lacks.
  MUSIC_FILES=("$GAME_DATA"/../TRACKS/*.OGG
    "$GAME_DATA"/../TRACKS/"${TRX_DC_LANG:-EN}"/*.OGG)
else
  MUSIC_FILES=("$GAME_DATA"/../music/*.mp3 "$GAME_DATA"/../music/*.ogg
    "$GAME_DATA"/../audio/*.ogg "$GAME_DATA"/../audio/*.mp3)
  WAD="$GAME_DATA/../audio/cdaudio.wad"
  if [ -f "$WAD" ] && command -v ffmpeg > /dev/null; then
    # TR3's tracks, each a whole WAV file inside the WAD: a table of 130
    # entries (name[260], size, offset) where entry n is track n, and the
    # file is a 90-byte header and size bytes from offset on.
    WAD_DIR="$MUSIC_CACHE/wad"
    mkdir -p "$WAD_DIR"
    python3 - "$(cygpath -w "$WAD")" "$(cygpath -w "$WAD_DIR")" << 'EOF'
import os, struct, sys
wad, out = sys.argv[1:3]
with open(wad, 'rb') as f:
    table = f.read(268 * 130)
    for n in range(1, 130):
        name, size, offset = struct.unpack_from('<260sII', table, n * 268)
        path = os.path.join(out, '%d.wav' % n)
        if size == 0 or not name.split(b'\0')[0].lower().endswith(b'.wav') \
                or os.path.exists(path):
            continue
        f.seek(offset)
        with open(path, 'wb') as o:
            o.write(f.read(90 + size))
EOF
    MUSIC_FILES+=("$WAD_DIR"/*.wav)
  fi
fi
for f in "${MUSIC_FILES[@]}"; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  # Only numbered tracks: the classic TR2's audio/cdaudio.mp3 is every
  # track in one, which the game cannot use.
  [[ "${name%.*}" =~ ^[0-9]+$ ]] || continue
  # TRX_DC_MUSIC_KEEP="29 37": only those tracks (small test discs).
  if [ -n "${TRX_DC_MUSIC_KEEP:-}" ] && [[ " $TRX_DC_MUSIC_KEEP " != *" $((10#${name%.*})) "* ]]; then
    continue
  fi
  out="$MUSIC_CACHE/${name%.*}.wav"
  if [ ! -f "$out" ] && command -v ffmpeg > /dev/null; then
    ffmpeg -hide_banner -loglevel error -y -i "$f" -ac 2 -ar 44100 \
      -c:a adpcm_yamaha "$out"
  fi
  [ -f "$out" ] && cp "$out" "$STAGE/games/$GAME/music/"
done
echo "$(ls "$STAGE/games/$GAME/music" | wc -l) tracks"

echo "== films"
# The game's films in the Dreamcast's own format (tools/dreamcast/fmvconv.c):
# each frame a VQ texture of 256x256 the PowerVR decodes itself, and the sound
# as the music is, Yamaha ADPCM at 44.1 kHz in a WAV beside it. Kept between
# discs. TRX_DC_FMV=0 makes a disc with no films; TRX_DC_FMV="ancient ms"
# only those (by name).
FMV_CACHE="$BUILD_DIR/fmv-$GAME-$SOURCE"
mkdir -p "$FMV_CACHE" "$STAGE/games/$GAME/fmv"
if [ "${TRX_DC_FMV:-all}" = 0 ] || ! command -v ffmpeg > /dev/null; then
  FMV_FILES=()
elif [ "$SOURCE" = remastered ]; then
  FMV_FILES=("$GAME_DATA"/../FMV/OG/*.FMV)
else
  FMV_FILES=("$GAME_DATA"/../fmv/*.rpl "$GAME_DATA"/../fmv/*.RPL)
fi
# Films the game's own folders lack (TR1's core and escape logos, TR2's logo)
# come from the directory TRX_DC_FMV_EXTRA names: any file FFmpeg reads, whose
# name (without extension) is the gameflow's. The game's own films win.
if [ -n "${TRX_DC_FMV_EXTRA:-}" ] && [ -d "$TRX_DC_FMV_EXTRA" ] && [ "${TRX_DC_FMV:-all}" != 0 ]; then
  FMV_FILES+=("$TRX_DC_FMV_EXTRA"/*)
fi
FMV_SEEN=" "
FMV_TOOL="$BUILD_DIR/fmvconv.exe"
if [ ${#FMV_FILES[@]} -gt 0 ] && { [ ! -f "$FMV_TOOL" ] || [ "$ROOT/tools/dreamcast/fmvconv.c" -nt "$FMV_TOOL" ]; }; then
  gcc -O2 -fopenmp -o "$FMV_TOOL" "$ROOT/tools/dreamcast/fmvconv.c"
fi
for f in "${FMV_FILES[@]}"; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lower="$(echo "${name%.*}" | tr '[:upper:]' '[:lower:]')"
  [[ "$FMV_SEEN" == *" $lower "* ]] && continue
  FMV_SEEN="$FMV_SEEN$lower "
  if [ "${TRX_DC_FMV:-all}" != all ] && [[ " $TRX_DC_FMV " != *" $lower "* ]]; then
    continue
  fi
  out="$FMV_CACHE/$lower.fmv"
  if [ ! -f "$out" ] || [ "$FMV_TOOL" -nt "$out" ]; then
    # width,height,frame rate of the film, which the player draws as it was.
    if command -v ffprobe > /dev/null; then
      IFS=',' read -r fw fh fr < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$f")
    else
      # Without ffprobe (the DreamSDK shell has only ffmpeg): read the
      # video stream's line, "..., 320x240, 30 tbr, ...".
      info="$(ffmpeg -hide_banner -i "$f" 2>&1 | grep -m1 'Video:' || true)"
      size="$(echo "$info" | grep -o ' [0-9]\{2,4\}x[0-9]\{2,4\}' | head -1 | tr -d ' ')"
      fw="${size%x*}"
      fh="${size#*x}"
      fr="$(echo "$info" | grep -o '[0-9.]* tbr' | head -1 | cut -d' ' -f1 | cut -d. -f1)/1"
    fi
    # Windows' ffprobe ends its lines with a carriage return.
    fr="${fr//[!0-9\/]/}"
    fps=$(( ${fr%/*} / ${fr#*/} ))
    # The sound first (Yamaha ADPCM, as the music), then the picture with it.
    SOUND=()
    if ffmpeg -hide_banner -loglevel error -y -i "$f" -vn -ac 2 -ar 44100 \
      -c:a adpcm_yamaha "$FMV_CACHE/$lower.wav"; then
      SOUND=("$FMV_CACHE/$lower.wav")
    fi
    ffmpeg -hide_banner -loglevel error -y -i "$f" \
      -vf "fps=$fps,scale=256:256:flags=lanczos" -f rawvideo -pix_fmt rgb24 - \
      | "$FMV_TOOL" "$out.tmp" "$fps" "$fw" "$fh" "${SOUND[@]}" 2> /dev/null
    mv "$out.tmp" "$out"
    rm -f "$FMV_CACHE/$lower.wav"
  fi
  [ -f "$out" ] && cp "$out" "$STAGE/games/$GAME/fmv/$lower.fmv"
done
echo "$(ls "$STAGE/games/$GAME/fmv" | grep -c fmv) films"

if [ -n "$SUBDIR" ]; then
  rm -rf "$TMP"
  echo "staged $STAGE"
  exit 0
fi
echo "== image"
bash "$(dirname "$0")/disc_sort.sh" "$STAGE" > "$TMP/sort.txt"
mkisofs -quiet -C 0,11702 -V TRX -G "$TMP/IP.BIN" -r -J -l \
  -sort "$TMP/sort.txt" -o "$TMP/TRX.iso" "$STAGE"
cdi4dc "$TMP/TRX.iso" "$OUT" > /dev/null
rm -rf "$TMP"
ls -la "$OUT"

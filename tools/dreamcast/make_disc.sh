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
# TRX_DC_FMV=0 leaves the films (fmv/) out; the first disc with them is slow.
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
  *) echo "TRX_DC_GAME must be tr2 or tr3" >&2; exit 1 ;;
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
if [ -n "${TRX_DC_MODE:-}" ]; then
  # A test disc that starts in that mode (graphics, performance or
  # experimental-60), whatever the settings say (dc_main.c, MODE.TXT).
  case "$TRX_DC_MODE" in
    graphics) echo 0 > "$STAGE/MODE.TXT" ;;
    performance) echo 1 > "$STAGE/MODE.TXT" ;;
    experimental-60) echo 2 > "$STAGE/MODE.TXT" ;;
    *) echo "TRX_DC_MODE must be graphics, performance or experimental-60" >&2; exit 1 ;;
  esac
  echo "mode $TRX_DC_MODE"
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

echo "== loading pictures"
# The game's original 640x480 pictures (the middle of the remaster's 856x480
# ones; the classic TR3's pix as they are), shown behind the loading screen:
# 512x512 RGB565 as the PowerVR reads them, stretched back over the screen.
# Kept between discs; without ffmpeg the loading screen stays black. The
# classic TR2 has only the title, legal and credit pictures.
PIX_CACHE="$BUILD_DIR/pix-$GAME-$SOURCE"
mkdir -p "$PIX_CACHE" "$STAGE/pix"
if [ "$SOURCE" = remastered ]; then
  PIX_FILES=("$GAME_DATA"/../PIX/OG/*.DDS)
  PIX_CROP="crop=640:480:(in_w-640)/2:0,"
else
  # TR3's pictures are BMPs beside the data; TR2's (title, legal, credits)
  # are PCXs in the data itself.
  PIX_FILES=("$GAME_DATA"/../pix/*.BMP "$GAME_DATA"/../pix/*.bmp
    "$GAME_DATA"/*.PCX "$GAME_DATA"/*.pcx)
  PIX_CROP=""
fi
for f in "${PIX_FILES[@]}"; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lower="$(echo "${name%.*}" | tr '[:upper:]' '[:lower:]')"
  # The gameflow's names for the title and the legal notice.
  case "$lower" in
    title|titleuk) lower=title_eu ;;
    legal) lower=legal_eu ;;
  esac
  out="$PIX_CACHE/$lower.565"
  if [ ! -f "$out" ] && command -v ffmpeg > /dev/null; then
    ffmpeg -hide_banner -loglevel error -y -i "$f" -frames:v 1 \
      -vf "${PIX_CROP}scale=512:512:flags=lanczos" \
      -f rawvideo -pix_fmt rgb565le "$out"
  fi
  [ -f "$out" ] && cp "$out" "$STAGE/pix/$lower.565"
done
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
  MUSIC_FILES=("$GAME_DATA"/../TRACKS/*.OGG)
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
  out="$MUSIC_CACHE/${name%.*}.wav"
  if [ ! -f "$out" ] && command -v ffmpeg > /dev/null; then
    ffmpeg -hide_banner -loglevel error -y -i "$f" -ac 2 -ar 44100 \
      -c:a adpcm_yamaha "$out"
  fi
  [ -f "$out" ] && cp "$out" "$STAGE/games/$GAME/music/"
done
echo "$(ls "$STAGE/games/$GAME/music" | wc -l) tracks"

echo "== films"
# The game's films (fmv/*.RPL, or the remaster's) as 256-wide streams of
# quantised frames at 15 a second (tools/dreamcast/fmvenc.c), with their sound
# as Yamaha ADPCM beside them (src/platform/dreamcast/video.c plays them).
# The game finds "LOGO.AVI" as logo.fmv. Kept between discs, but the first time
# takes minutes; TRX_DC_FMV=0 makes a disc without films, which the game then
# skips. Without ffmpeg there are none either.
FMV_CACHE="$BUILD_DIR/fmv-$GAME-$SOURCE"
mkdir -p "$FMV_CACHE" "$STAGE/fmv"
FMV_TOOL="$BUILD_DIR/fmvenc.exe"
if [ ! -f "$FMV_TOOL" ] || [ "$ROOT/tools/dreamcast/fmvenc.c" -nt "$FMV_TOOL" ]; then
  gcc -O2 -fopenmp -o "$FMV_TOOL" "$ROOT/tools/dreamcast/fmvenc.c"
fi
if [ "${TRX_DC_FMV:-1}" != 0 ] && command -v ffmpeg > /dev/null; then
  while IFS= read -r f; do
    name="$(basename "$f")"
    stem="${name%.*}"
    video="$FMV_CACHE/$stem.fmv"
    sound="$FMV_CACHE/$stem.wav"
    if [ ! -f "$video" ]; then
      size="$(ffmpeg -hide_banner -i "$f" 2>&1 | grep -m1 'Video:'         | grep -o ' [0-9]\{2,4\}x[0-9]\{2,4\}' | head -1 | tr -d ' ')"
      [ -n "$size" ] || continue
      fw="${size%x*}"
      fh="${size#*x}"
      h=$(( (256 * fh / fw + 1) / 2 * 2 ))
      if ffmpeg -hide_banner -loglevel error -i "$f" -an           -vf "fps=15,scale=256:$h:flags=lanczos" -f rawvideo -pix_fmt rgb24 -           | "$FMV_TOOL" "$video.part" 256 "$h" 15; then
        mv "$video.part" "$video"
      else
        rm -f "$video.part"
        echo "film $name skipped"
      fi
    fi
    if [ -f "$video" ] && [ ! -f "$sound" ]; then
      ffmpeg -hide_banner -loglevel error -y -i "$f" -vn -ac 2 -ar 22050         -c:a adpcm_yamaha "$sound" || rm -f "$sound"
    fi
    [ -f "$video" ] && cp "$video" "$STAGE/fmv/"
    [ -f "$sound" ] && cp "$sound" "$STAGE/fmv/"
  done < <(find "$GAME_DATA/.." -maxdepth 2 -ipath '*/fmv/*' -type f     \( -iname '*.rpl' -o -iname '*.avi' -o -iname '*.mp4' -o -iname '*.ogv'     -o -iname '*.mkv' -o -iname '*.webm' \) | sort)
fi
echo "$(ls "$STAGE/fmv" | grep -c '\.fmv$') films"

echo "== image"
mkisofs -quiet -C 0,11702 -V TRX -G "$TMP/IP.BIN" -r -J -l \
  -o "$TMP/TRX.iso" "$STAGE"
cdi4dc "$TMP/TRX.iso" "$OUT" > /dev/null
rm -rf "$TMP"
ls -la "$OUT"

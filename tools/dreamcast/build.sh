#!/bin/bash
# Configures and builds TRX for the Dreamcast. Run from the DreamSDK shell
# (or through tools/dreamcast/dcsh.ps1) at the repository root.
#
#   tools/dreamcast/build.sh [debug|debugoptimized|release] [deps-dir]
#
# deps-dir holds the libraries built by tools/dreamcast/build_deps.sh
# (default: ../deps).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail

BUILD_TYPE="${1:-debugoptimized}"
DEPS="$(cd "${2:-../deps}" && pwd)/dc"
ROOT="$(pwd)"
BUILD_DIR="$ROOT/build/dc-$BUILD_TYPE"
CROSS="$ROOT/build/dc-cross.ini"

win() { cygpath -m "$1"; }

# Native tools (meson, ninja, gcc) take Windows paths; the KOS environment is
# expressed in MSYS paths, so translate every -isystem/-L/-T argument.
translate() {
  local out=() arg
  for arg in "$@"; do
    case "$arg" in
      /*) out+=("$(win "$arg")") ;;
      -T/*) out+=("-T$(win "${arg#-T}")") ;;
      -L/*) out+=("-L$(win "${arg#-L}")") ;;
      -I/*) out+=("-I$(win "${arg#-I}")") ;;
      *) out+=("$arg") ;;
    esac
  done
  printf "'%s', " "${out[@]}"
}

# KOS_CFLAGS carries -O2 and -g; Meson adds its own optimisation flags.
CFLAGS_LIST=$(translate $(echo "$KOS_CFLAGS" | sed -e 's/ -O2 / /' -e 's/ -g$//' -e 's/-fomit-frame-pointer//') \
  -isystem "$DEPS/include" -isystem "$KOS_PORTS/include/lua" -isystem "$KOS_PORTS/include/zlib")
LDFLAGS_LIST=$(translate $(echo "$KOS_LDFLAGS" | sed -e 's/ -O2 / /' -e 's/ -g / /') \
  -L"$DEPS/lib")
LIBS_LIST=$(translate $KOS_LIBS)

mkdir -p "$ROOT/build"
cat > "$CROSS" <<EOF
[binaries]
c = '$(win "$KOS_CC")'
ar = '$(win "${KOS_CC%gcc}ar")'
strip = '$(win "${KOS_CC%gcc}strip")'
objcopy = '$(win "${KOS_CC%gcc}objcopy")'
python3 = 'python3'

[host_machine]
system = 'dreamcast'
cpu_family = 'sh4'
cpu = 'sh4'
endian = 'little'

[properties]
needs_exe_wrapper = true
dc_deps_dir = '$(win "$DEPS")'
dc_kos_ports_dir = '$(win "$KOS_PORTS")'

[built-in options]
c_args = [${CFLAGS_LIST}]
c_link_args = [${LDFLAGS_LIST}${LIBS_LIST}]

[project options]
staticdeps = true
EOF

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  meson setup "$BUILD_DIR" src --cross-file "$CROSS" --buildtype "$BUILD_TYPE" -Db_staticpic=false -Db_pie=false
fi

# minsize is -Os, which costs the game's logic and scripts' host code ~1 ms of
# 28 in the London benchmark against -O2, for 0.2 MB of code more.
if [ "$BUILD_TYPE" = minsize ] \
  && [ "$(meson configure "$BUILD_DIR" 2>/dev/null | awk '$1 == "optimization" { print $2 }')" != 2 ]; then
  meson configure "$BUILD_DIR" -Doptimization=2
fi

# The engine's scripts go in as bytecode when build_deps.sh made a luac.
LUAC="$DEPS/../host/luac.exe"
if [ -f "$LUAC" ]; then
  LUAC="$(win "$(cd "$(dirname "$LUAC")" && pwd)/luac.exe")"
  if ! grep -qF "$LUAC" "$BUILD_DIR/meson-info/intro-buildoptions.json"; then
    meson configure "$BUILD_DIR" -Dlua_compiler="$LUAC"
  fi
fi
# DC_PROBES=1 builds with the frame timers on (the benchmark reports need them).
want=false
[ "${DC_PROBES:-0}" = 1 ] && want=true
if [ "$(meson configure "$BUILD_DIR" 2>/dev/null | awk '$1 == "dc_probes" { print $2 }')" != "$want" ]; then
  meson configure "$BUILD_DIR" -Ddc_probes="$want"
fi
ninja -C "$BUILD_DIR" "${@:3}"

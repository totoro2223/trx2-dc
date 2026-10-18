#!/bin/bash
# Configures and builds TRX for the Dreamcast. Run from the DreamSDK shell
# (or through tools/dreamcast/dcsh.ps1) at the repository root.
#
#   tools/dreamcast/build.sh [debug|release] [deps-dir]
#
# deps-dir holds the libraries built by tools/dreamcast/build_deps.sh
# (default: ../deps).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail

BUILD_TYPE="${1:-debug}"
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
ninja -C "$BUILD_DIR" "${@:3}"

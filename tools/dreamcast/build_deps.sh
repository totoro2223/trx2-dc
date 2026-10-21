#!/bin/bash
# Builds the third-party libraries TRX needs that are not provided by
# kos-ports, installing them into $DEPS/dc/{include,lib}.
# Usage (from the DreamSDK shell): tools/dreamcast/build_deps.sh <deps-dir>
source /opt/toolchains/dc/kos/environ.sh
set -euo pipefail

DEPS="$(cd "${1:?deps dir}" && pwd)"
PREFIX="$DEPS/dc"
mkdir -p "$PREFIX/include" "$PREFIX/lib"

# uthash (header only)
cp "$DEPS"/uthash/*.h "$PREFIX/include/"

# pcre2 (8-bit, no JIT, no unicode tables beyond the defaults)
PCRE_SRC="$(ls -d "$DEPS"/pcre2-*/src | head -1)"
BUILD="$DEPS/build/pcre2"
rm -rf "$BUILD" && mkdir -p "$BUILD"
cp "$PCRE_SRC/config.h.generic" "$BUILD/config.h"
cp "$PCRE_SRC/pcre2.h.generic" "$BUILD/pcre2.h"
cp "$PCRE_SRC/pcre2_chartables.c.dist" "$BUILD/pcre2_chartables.c"
SRCS="auto_possess chkdint compile compile_cgroup compile_class config context
convert dfa_match error extuni find_bracket maketables match match_data
match_next newline ord2utf pattern_info script_run serialize string_utils
study substitute substring tables ucd valid_utf xclass"
OBJS=()
for s in $SRCS; do
  kos-cc -std=c11 -O2 -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC \
    -I"$BUILD" -I"$PCRE_SRC" -c "$PCRE_SRC/pcre2_$s.c" -o "$BUILD/pcre2_$s.o"
  OBJS+=("$BUILD/pcre2_$s.o")
done
kos-cc -std=c11 -O2 -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC \
  -I"$BUILD" -I"$PCRE_SRC" -c "$BUILD/pcre2_chartables.c" -o "$BUILD/pcre2_chartables.o"
OBJS+=("$BUILD/pcre2_chartables.o")
rm -f "$PREFIX/lib/libpcre2-8.a"
kos-ar rcs "$PREFIX/lib/libpcre2-8.a" "${OBJS[@]}"
cp "$BUILD/pcre2.h" "$PREFIX/include/"

# luac for this PC, configured as kos-ports configures the Dreamcast's Lua
# (LUA_32BITS is all it changes), so that the build can embed the engine's
# scripts as bytecode the console loads as it is.
LUA_TAR="$DEPS/dl/lua-5.5.0.tar.gz"
if [ -f "$LUA_TAR" ]; then
  LUA_BUILD="$DEPS/build/lua"
  rm -rf "$LUA_BUILD" && mkdir -p "$LUA_BUILD" "$DEPS/host"
  tar xzf "$LUA_TAR" -C "$LUA_BUILD"
  (cd "$LUA_BUILD/lua-5.5.0/src" \
    && gcc -std=gnu99 -O2 -DLUA_32BITS -o "$DEPS/host/luac.exe" \
      $(ls *.c | grep -v '^lua\.c$'))
  echo "luac built: $DEPS/host/luac.exe"
fi

echo "deps installed into $PREFIX"
ls -la "$PREFIX/lib"

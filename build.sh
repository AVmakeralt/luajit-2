#!/usr/bin/env bash
# build.sh — manual build for LuaVortex (when CMake is unavailable).
#
# Compiles the vendored VORTEX source tree and the LuaVortex frontend
# into a single `luavortex` executable.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
VORTEX="$ROOT/vendor/VORTEX"
BUILD="$ROOT/build"
SRC="$ROOT/src"

mkdir -p "$BUILD/obj" "$BUILD/obj/vortex"

# ---- Configuration ----
# VORTEX uses computed-goto (labels-as-values), a GCC extension.
CFLAGS="-std=gnu17 -O2 -g -fPIC -fno-strict-aliasing"
CFLAGS+=" -I$VORTEX/src -I$BUILD -I$SRC"
CFLAGS+=" -Wall -Wextra"
# Suppress noisy warnings in the vendored VORTEX.
CFLAGS+=" -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable"
CFLAGS+=" -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-result"
CFLAGS+=" -Wno-format-truncation -Wno-stringop-truncation -Wno-array-bounds"
CFLAGS+=" -Wno-maybe-uninitialized -Wno-discarded-qualifiers -Wno-implicit-fallthrough"
CFLAGS+=" -DVORTEX_ENABLE_ASSERTIONS=1 -DVORTEX_ENABLE_VERIFY=1 -DVORTEX_ENABLE_PROFILING=1"

# Generate vortex_config.h from the template using sed.
mkdir -p "$BUILD"
sed \
    -e 's/@VORTEX_CACHE_MAX_SIZE@/268435456/' \
    -e 's/@VORTEX_T1_THRESHOLD@/1000/' \
    -e 's/@VORTEX_T2_THRESHOLD@/10000/' \
    -e 's/@VORTEX_COMPILE_THREADS@/0/' \
    -e 's|#cmakedefine VORTEX_ENABLE_ASSERTIONS|#define VORTEX_ENABLE_ASSERTIONS 1|' \
    -e 's|#cmakedefine VORTEX_ENABLE_VERIFY|#define VORTEX_ENABLE_VERIFY 1|' \
    -e 's|#cmakedefine VORTEX_ENABLE_PROFILING|#define VORTEX_ENABLE_PROFILING 1|' \
    -e 's|#cmakedefine VORTEX_ENABLE_SOTA|#define VORTEX_ENABLE_SOTA 1|' \
    "$VORTEX/src/vortex_config.h.in" > "$BUILD/vortex_config.h"

# ---- Collect VORTEX sources ----
VORTEX_SRCS=$(find "$VORTEX/src" -name '*.c' -not -name 'main_new.c' | sort)
LUA_SRCS=$(find "$SRC" -name '*.c' | sort)

# ---- Compile VORTEX ----
echo "==> Compiling VORTEX..."
N=0
for src in $VORTEX_SRCS; do
    rel="${src#$VORTEX/src/}"
    obj="$BUILD/obj/vortex/$(echo "$rel" | sed 's|/|_|g; s|\.c$|.o|')"
    if [ "$src" -nt "$obj" ] || [ ! -f "$obj" ]; then
        gcc $CFLAGS -c "$src" -o "$obj" 2>&1 | head -5 || true
        if [ ! -f "$obj" ]; then
            echo "FAIL: $src"
            gcc $CFLAGS -c "$src" -o "$obj" 2>&1 | head -20
            exit 1
        fi
    fi
    N=$((N + 1))
done
echo "    compiled $N VORTEX objects"

# ---- Compile LuaVortex ----
echo "==> Compiling LuaVortex..."
for src in $LUA_SRCS; do
    name=$(basename "$src" .c)
    obj="$BUILD/obj/${name}.o"
    if [ "$src" -nt "$obj" ] || [ ! -f "$obj" ]; then
        gcc $CFLAGS -c "$src" -o "$obj"
    fi
done

# ---- Link ----
echo "==> Linking luavortex..."
VORTEX_OBJS=$(find "$BUILD/obj/vortex" -name '*.o' | sort)
LUA_OBJS=$(find "$BUILD/obj" -maxdepth 1 -name '*.o' | sort)
gcc -o "$BUILD/luavortex" $LUA_OBJS $VORTEX_OBJS -lm -lpthread -ldl
echo "==> Done: $BUILD/luavortex"
"$BUILD/luavortex" --version

#!/usr/bin/env bash
# Builds the HolyPHP compiler from the source under toolchain/, then compiles
# server.hphp into bin/server. Runs as a normal user; writes only inside this
# folder.
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
ROOT="$PWD/toolchain"
BUILD="$PWD/build"
CFLAGS=(-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" bin

# 1. Embed the runtime sources and bundled stdlib into the compiler binary.
#    The generated header is a build artifact, not checked in.
"$CC" -O2 -std=c11 "$ROOT/scripts/genembed.c" -o "$BUILD/genembed"
"$BUILD/genembed" "$ROOT/src/hphpc/runtime_embed.h" "$ROOT/src/hphpc/runtime"

# 2. Build the compiler.
for src in "$ROOT"/src/hphpc/*.c; do
  obj="$BUILD/obj/$(basename "${src%.c}").o"
  "$CC" "${CFLAGS[@]}" -I"$ROOT/src/hphpc" -c "$src" -o "$obj"
done
"$CC" -O2 "$BUILD"/obj/*.o -o "$BUILD/hphp" -lm -lpthread

# 3. Compile the challenge server. hphp emits C11, then links it against the
#    system SQLite installed by install.sh.
"$BUILD/hphp" build server.hphp -o bin/server

echo "built: $PWD/bin/server"

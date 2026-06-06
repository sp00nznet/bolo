#!/usr/bin/env bash
# Build the Bolo recomp with the MSYS2 mingw64 toolchain + SDL2.
# Verified working on Windows (gcc 15.2 / SDL2 from mingw64).
#
#   bash scripts/build.sh            # -> build/bolo.exe
#   SDL_VIDEODRIVER=dummy ./build/bolo.exe work/BOLO3_image.bin
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"
cd "$(dirname "$0")/.."

INC="-I src -I src/recomp -I src/recomp/gen"
SDLC="$(pkg-config --cflags sdl2)"
SDLL="$(pkg-config --libs sdl2)"
mkdir -p build/obj

SRCS=(
  src/recomp/gen/recomp_*.c
  src/recomp/cpu.c
  src/recomp/dos_compat.c
  src/recomp/hal/timer.c
  src/recomp/hal/input.c
  src/recomp/hal/video.c
  src/recomp/platform/sdl_platform.c
  src/icall.c
  src/main.c
)
# NOTE: src/recomp/startup.c is intentionally excluded -- it is the civ-specific
# bootstrap; src/main.c is our entry point.

echo "compiling..."
for f in ${SRCS[@]}; do
  o="build/obj/$(echo "$f" | sed 's#[/.]#_#g').o"
  gcc -O1 -c $INC $SDLC "$f" -o "$o"
done
echo "linking..."
gcc build/obj/*.o -o build/bolo.exe $SDLL
echo "built build/bolo.exe"

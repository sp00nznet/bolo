# Bolo Adventures III static recomp.
#
# 16-bit DOS game lifted to C against the pcrecomp recomp16 runtime (flat 1 MB
# memory + CPU-state model + DOS/BIOS shims backed by SDL2). Unlike 32-bit
# recomp targets this builds as a NATIVE 64/32-bit host binary -- the "16-bitness"
# is modelled in software (seg:off into mem[]), so no /DYNAMICBASE:NO trickery is
# needed. Build:
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64 -DSDL2_DIR=...
#   cmake --build build
#
# Regenerate the lifted C from the original game first:
#   python tools/unpklite.py original/BOLO3.EXE work/BOLO3_image.bin --mz work/BOLO3_unpacked.exe
#   python -m ...analyze... (see docs)   # -> work/bolo3.toml
#   python tools/lift_bolo.py            # -> src/recomp/gen/*.c
cmake_minimum_required(VERSION 3.20)
project(bolo C)

set(CMAKE_C_STANDARD 11)

if(MSVC)
    add_compile_options(/W1 /wd4244 /wd4101 /wd4102 /wd4267 /wd4018 /wd4146 /wd4013)
endif()

file(GLOB GENERATED_SOURCES "src/recomp/gen/recomp_*.c")

set(RUNTIME_SOURCES
    src/runtime/cpu.c
    src/runtime/dos_compat.c
    src/runtime/startup.c
    src/runtime/hal/input.c
    src/runtime/hal/timer.c
    src/runtime/hal/video.c
    src/runtime/platform/sdl_platform.c
)

add_executable(bolo
    src/main.c
    ${RUNTIME_SOURCES}
    ${GENERATED_SOURCES}
)

target_include_directories(bolo PRIVATE
    src
    src/runtime
    src/recomp/gen
)

# SDL2 for the EGA framebuffer / input / timing HAL.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_link_libraries(bolo PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    message(WARNING "SDL2 not found -- HAL (video/input/timer) will not link. "
                    "Install SDL2 and set SDL2_DIR.")
endif()

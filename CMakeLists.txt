# Navi2K Player - Navidrome/Subsonic client for Windows 2000 SP4 and XP SP3.
#
# Toolchain: 32-bit MinGW-w64 GCC, msvcrt runtime (NOT ucrt), win32 thread model.
# Win2000 has no UCRT and no winpthreads-friendly kernel APIs, so those variants won't run there.
#
#   cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Cross-compiling from Linux: add -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake

cmake_minimum_required(VERSION 3.13)
project(Navi2KPlayer C CXX RC)

if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Navi2K Player must be built with a 32-bit MinGW-w64 (i686) compiler.")
endif()

# ---------------------------------------------------------------------------
# CPU baseline / SSE2 policy
# ---------------------------------------------------------------------------
option(LEGACY_NO_SSE2 "Compile all code for CPUs without SSE2 (Pentium, P-II, P-III, Athlon XP)" ON)
option(LEGACY_SSE2_DISPATCH "Include one SSE2 PCM-conversion routine, used only if CPUID reports SSE2 at runtime" ON)
option(LEGACY_STRICT_CHECK "Fail the build if the post-build Win2000/SSE2 check finds problems" ON)
# i686 (CMOV) is the realistic floor: stock i686 MinGW-w64 runtimes (libgcc, libstdc++, CRT
# startup) already contain CMOV. i586 needs a toolchain whose runtime was itself built for i586;
# the post-build check scans for P6 instructions when i586 is selected.
set(LEGACY_MARCH "i686" CACHE STRING "GCC -march baseline: i686 = Pentium Pro/II/III, Athlon; i586 = Pentium/K6 (needs an i586-built runtime)")
set(LIBMAD_FPM "FPM_INTEL" CACHE STRING "libmad fixed-point backend: FPM_INTEL (i386 integer asm) or FPM_64BIT (portable C)")
set_property(CACHE LIBMAD_FPM PROPERTY STRINGS FPM_INTEL FPM_64BIT)

set(LEGACY_CPU_FLAGS -march=${LEGACY_MARCH} -mtune=${LEGACY_MARCH})
if(LEGACY_NO_SSE2)
    # -march=i586 already excludes MMX/SSE; the -mno-* flags make the intent explicit and
    # survive someone raising LEGACY_MARCH. x87 is the only FPU path.
    list(APPEND LEGACY_CPU_FLAGS -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mfpmath=387)
    set(LP_BUILD_NO_SSE2 1)
else()
    list(APPEND LEGACY_CPU_FLAGS -msse2 -mfpmath=sse)
    set(LP_BUILD_NO_SSE2 0)
endif()

if(LEGACY_MARCH STREQUAL "i586")
    set(LP_REQUIRE_CMOV 0)
    set(LEGACY_SCAN_CMOV ON)
else()
    set(LP_REQUIRE_CMOV 1)
    set(LEGACY_SCAN_CMOV OFF)
endif()

if(LEGACY_SSE2_DISPATCH)
    set(LP_SSE2_DISPATCH 1)
else()
    set(LP_SSE2_DISPATCH 0)
endif()

# Headers hide anything newer than Windows 2000, so XP/Vista APIs fail at compile time
# instead of failing to load on the target.
set(LEGACY_WIN_DEFS
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    WINVER=0x0500 _WIN32_WINNT=0x0500 _WIN32_IE=0x0500
    __USE_MINGW_ANSI_STDIO=0
    LP_BUILD_NO_SSE2=${LP_BUILD_NO_SSE2}
    LP_SSE2_DISPATCH=${LP_SSE2_DISPATCH}
    LP_REQUIRE_CMOV=${LP_REQUIRE_CMOV})

# ---------------------------------------------------------------------------
# Third-party sources (dropped into third_party/, see README.md)
# ---------------------------------------------------------------------------
set(MAD_DIR ${CMAKE_SOURCE_DIR}/third_party/libmad)
set(CJSON_DIR ${CMAKE_SOURCE_DIR}/third_party/cJSON)
if(NOT EXISTS ${MAD_DIR}/layer3.c)
    message(FATAL_ERROR "libmad 0.15.1b sources not found in ${MAD_DIR} (see README.md).")
endif()
if(NOT EXISTS ${CJSON_DIR}/cJSON.c)
    message(FATAL_ERROR "cJSON sources not found in ${CJSON_DIR} (see README.md).")
endif()

add_library(mad STATIC
    ${MAD_DIR}/bit.c ${MAD_DIR}/decoder.c ${MAD_DIR}/fixed.c ${MAD_DIR}/frame.c
    ${MAD_DIR}/huffman.c ${MAD_DIR}/layer12.c ${MAD_DIR}/layer3.c ${MAD_DIR}/stream.c
    ${MAD_DIR}/synth.c ${MAD_DIR}/timer.c ${MAD_DIR}/version.c)
target_include_directories(mad
    PUBLIC ${MAD_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libmad_config)
# SIZEOF_INT is public so fixed.h picks the same mad_fixed_t (int) in our code as in libmad.
target_compile_definitions(mad PUBLIC ${LIBMAD_FPM} SIZEOF_INT=4 PRIVATE HAVE_CONFIG_H)
# libmad is 2004-era C: build it as gnu89 and don't drown the log in its warnings.
target_compile_options(mad PRIVATE ${LEGACY_CPU_FLAGS} -O2 -std=gnu89 -w)

add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})
target_compile_options(cjson PRIVATE ${LEGACY_CPU_FLAGS} -O2)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
# Shared code for the GUI player and the console probe. An OBJECT library (not STATIC) so
# compat_win2000.cpp is always linked: its import-slot overrides must win over kernel32.
add_library(lpcore OBJECT
    src/cpu.cpp
    src/util.cpp
    src/http.cpp
    src/subsonic.cpp
    src/config.cpp
    src/pcm.cpp
    src/mad_decoder.cpp
    src/waveout.cpp
    src/player.cpp
    src/art.cpp
    src/stb_image_impl.c
    src/compat_win2000.cpp)
target_link_libraries(lpcore PUBLIC mad cjson)
target_include_directories(lpcore PUBLIC ${CMAKE_SOURCE_DIR}/third_party/stb)
# Third-party single-file decoder: optimise, keep its warnings out of the log.
set_source_files_properties(src/stb_image_impl.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

find_program(LEGACY_OBJDUMP NAMES ${CMAKE_OBJDUMP} i686-w64-mingw32-objdump objdump)
if(NOT LEGACY_OBJDUMP)
    message(WARNING "objdump not found: skipping the Win2000/SSE2 post-build check.")
endif()

function(legacy_target target)
    target_compile_definitions(${target} PRIVATE ${LEGACY_WIN_DEFS})
    # -fno-threadsafe-statics: libstdc++'s static-init guard (GCC 13+) uses Vista-only condition
    # variables. Function-local statics with dynamic initialisers must not be relied on from threads.
    target_compile_options(${target} PRIVATE
        ${LEGACY_CPU_FLAGS}
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++98 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -O2>)

    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        return()
    endif()
    target_link_libraries(${target} PRIVATE lpcore
        wininet winmm comctl32 advapi32 shell32 gdi32 user32 kernel32)
    target_link_options(${target} PRIVATE
        -static -static-libgcc -static-libstdc++
        # Modern linkers stamp 6.0 (Vista) into the PE header and Win2000 refuses to load it.
        -Wl,--major-subsystem-version,5,--minor-subsystem-version,0
        -Wl,--major-os-version,5,--minor-os-version,0)

    # Post-build: verify the .exe is actually Win2000-loadable and SSE2-free.
    if(LEGACY_OBJDUMP)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOBJDUMP=${LEGACY_OBJDUMP}
                -DEXE=$<TARGET_FILE:${target}>
                -DOUT_DIR=${CMAKE_BINARY_DIR}/compat-check/${target}
                -DSCAN_XMM=${LEGACY_NO_SSE2}
                -DSCAN_CMOV=${LEGACY_SCAN_CMOV}
                -DALLOW_XMM_IN=LpConvertPcmSse2
                -DSTRICT=${LEGACY_STRICT_CHECK}
                -P ${CMAKE_SOURCE_DIR}/cmake/check_binary.cmake
            VERBATIM)
    endif()
endfunction()

legacy_target(lpcore)

add_executable(Navi2KPlayer WIN32 src/main.cpp src/app.rc)
legacy_target(Navi2KPlayer)

# Console end-to-end check: connect, list, load an album, stream a track.
add_executable(navi2k_probe src/probe.cpp)
legacy_target(navi2k_probe)

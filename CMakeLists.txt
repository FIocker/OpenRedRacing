cmake_minimum_required(VERSION 3.21)

project(OpenBigRed C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Third-party sources: a folder you name, or downloaded at configure time ---------------
# SDL2, RmlUi and ENet are not in the repository. Each has a *_DIR cache variable: point it at an
# unpacked copy to build offline, or leave it empty and the first configure downloads the pinned
# release (hash-checked) into the build folder.
function(obr_fetch var url sha256 inner)
    if(NOT "${${var}}" STREQUAL "")
        return()
    endif()
    get_filename_component(archive_name "${url}" NAME)
    set(root "${CMAKE_BINARY_DIR}/_deps")
    set(archive "${root}/${archive_name}")
    if(NOT EXISTS "${root}/${inner}")
        message(STATUS "Downloading ${url}")
        file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} STATUS st)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Download failed: ${url} (${st}). Set ${var} to an unpacked copy.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}")
    endif()
    set(${var} "${root}/${inner}" CACHE PATH "" FORCE)
endfunction()

# --- SDL2 (windowing/input) ---
# Use the official pre-built MinGW devel package rather than building SDL2 from source: SDL2
# 2.30.x's own CMake build (via FetchContent) doesn't compile cleanly against this toolchain's
# GCC (its "dynapi" indirection layer hits "implicit declaration of function 'SDL_xxx_REAL'" -
# not resolved by pinning -std=gnu17 or disabling dynapi; root cause not fully chased down since
# the prebuilt package sidesteps it entirely and is the more normal way to consume SDL2 on
# Windows/MinGW anyway).
set(SDL2_PREBUILT_DIR "" CACHE PATH
    "Path to the official SDL2 MinGW devel package's x86_64-w64-mingw32 dir (empty: download)")
obr_fetch(SDL2_PREBUILT_DIR
    "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz"
    b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c
    "SDL2-2.30.9/x86_64-w64-mingw32")
list(APPEND CMAKE_PREFIX_PATH "${SDL2_PREBUILT_DIR}")
find_package(SDL2 REQUIRED)

# --- RmlUi (the menus' layout engine) ---
# Built FROM SOURCE rather than from the official release archives: those are MSVC builds and
# this project is MinGW/GCC, so they cannot be linked. RmlUi is plain CMake + C++17 and builds
# cleanly here.
#
# `cmake --build build --target rmlui_probe` then checks the whole thing still works with this
# toolchain - see src/tools/rmlui_probe.cpp.
#
# ⚠️ RMLUI_FONT_ENGINE=none, ON PURPOSE. It drops the FreeType dependency entirely; the text is
# drawn by our own font engine over the GAME's own fonts (SML_ALPH.SPR and the five WIBS
# sheets), which is both one less third-party library and the original's own typefaces.
set(RMLUI_DIR "" CACHE PATH "Path to the RmlUi 6.3 source tree (empty: download)")
obr_fetch(RMLUI_DIR "https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.zip"
    bfb340660e4bce38d4211cffc836cfbe1be676aa36ef9cc48e7df3855ee74fe3 "RmlUi-6.3")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # we static-link everything
set(RMLUI_FONT_ENGINE "none" CACHE STRING "" FORCE)
# RMLUI_BACKEND is only consulted to build the SAMPLES, and those are off - we bring our own
# SDL2 + GL 3.3. It is left at its default because the value is validated against a fixed list
# even when nothing uses it, and "none" is not on that list.
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${RMLUI_DIR}" "${CMAKE_BINARY_DIR}/rmlui" EXCLUDE_FROM_ALL)

# --- ENet (the network game's transport) ---
# Reliable UDP in about 3 000 lines of plain C, MIT licensed.
# Built here as a static library from its own sources rather than through its CMakeLists, which
# is five lines of the same thing plus options this project does not want. This is also why the
# project enables C: ENet is the only C in the build.
set(ENET_DIR "" CACHE PATH "Path to the ENet 1.3.18 source tree (empty: download)")
obr_fetch(ENET_DIR "https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.zip"
    32d6b15611c2667b88cd865e0cb48c3c011ddefcff6dfbdb266b8e0b04fffab3 "enet-1.3.18")
add_library(enet STATIC
    "${ENET_DIR}/callbacks.c" "${ENET_DIR}/compress.c" "${ENET_DIR}/host.c"
    "${ENET_DIR}/list.c" "${ENET_DIR}/packet.c" "${ENET_DIR}/peer.c"
    "${ENET_DIR}/protocol.c" "${ENET_DIR}/win32.c" "${ENET_DIR}/unix.c")
target_include_directories(enet PUBLIC "${ENET_DIR}/include")
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

# Where the game files are looked for last, after the folders next to the executable (see
# src/formats/game_files.h). Empty by default; a developer can bake in their own install.
set(OPENBIGRED_GAME_DIR "" CACHE PATH "Fallback folder holding the original game files")
if(OPENBIGRED_GAME_DIR)
    add_compile_definitions(OPENBIGRED_GAME_DIR="${OPENBIGRED_GAME_DIR}")
endif()

# `ctest --test-dir build`: the checks that answer yes or no (src/CMakeLists.txt, at the end).
enable_testing()
add_subdirectory(src)

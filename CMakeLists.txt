cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tell LVGL's builtin memory pool to live in PSRAM (.ext_ram.bss) instead of
# internal SRAM. The LVGL ESP-IDF component does not expose this as a Kconfig
# option, so it must be injected as a compile-time define. EXT_RAM_BSS_ATTR
# expands to __attribute__((section(".ext_ram.bss"))).
# Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y in sdkconfig.
#
# Must be called BEFORE project() -- component registration happens INSIDE
# project() and captures COMPILE_DEFINITIONS at that point. Setting it after
# is too late and the define silently does not propagate (verified by checking
# build/compile_commands.json for lv_mem_core_builtin.c).
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR" APPEND)

# ⛔ THE EMBEDDED VERSION FOLLOWED THE LAST CONFIGURE, NOT THE COMMIT.
#
# ESP-IDF computes PROJECT_VER from `git describe` inside project(), which runs
# at CMake CONFIGURE time. A plain `idf.py build` afterwards does not re-run
# it, so every build between two configures carries the FIRST one's version.
#
# Measured 2026-10-13: the dev bench reported v1.18.0 while running v1.18.0
# plus two commits. It had been flashed from a build made after the release's
# clean configure, and the binary's own app descriptor said v1.18.0. Nothing
# about that reads as wrong - which is the whole problem. Phase 9 of the
# release process exists so the bench runs the release and field reports can be
# reproduced on it; a bench that MISREPORTS its version defeats that silently,
# and `git describe --dirty` on the command line still looked right.
#
# These are the files `git describe` consults. Naming them as configure
# dependencies makes CMake re-configure itself on the first build after a
# commit or a tag, so the version is recomputed. The cost is one extra
# configure after a commit; a build with no new commit is unaffected.
#
# ⚠ A worktree keeps its own .git as a FILE pointing at the real
# directory, so these paths may not exist. EXISTS keeps that from breaking the
# build - it degrades to the old behaviour rather than failing.
foreach(_gitdep ".git/HEAD" ".git/index" ".git/refs/tags" ".git/packed-refs")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_gitdep}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/${_gitdep}")
    endif()
endforeach()

project(qmx_panadapter)

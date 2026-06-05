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

project(qmx_panadapter)

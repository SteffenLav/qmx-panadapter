# ESP-IDF component definition for ft8_lib (kgoba/ft8_lib)
# Vendored from https://github.com/kgoba/ft8_lib
# Build excludes host-only files: demo/, test/, utils/, common/wave.c, common/audio.c

idf_component_register(
    SRCS
        "ft8/constants.c"
        "ft8/crc.c"
        "ft8/decode.c"
        "ft8/encode.c"
        "ft8/ldpc.c"
        "ft8/message.c"
        "ft8/text.c"
        "fft/kiss_fft.c"
        "fft/kiss_fftr.c"
        "common/monitor.c"
    INCLUDE_DIRS
        "."
    REQUIRES
)

# Upstream ft8_lib targets host platforms where uint32_t == unsigned int and
# is built with looser warning flags. ESP-IDF default is -Werror with many
# diagnostic classes enabled. Demote upstream-only warning classes here so
# the vendored code builds without modifications. Format/const-variable
# checking is preserved for project code.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=unused-const-variable
)
cmake_minimum_required(VERSION 3.5)
project(ft8_test_harness)

# Find ft8_lib components
set(FT8_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/ft8_lib")

# Source files
set(SOURCES
    ft8_test_harness.c
    ft8_stage2.c
    ${FT8_LIB_DIR}/ft8/decode.c
    ${FT8_LIB_DIR}/ft8/encode.c
    ${FT8_LIB_DIR}/ft8/ldpc.c
    ${FT8_LIB_DIR}/ft8/text.c
    ${FT8_LIB_DIR}/ft8/message.c
    ${FT8_LIB_DIR}/ft8/crc.c
    ${FT8_LIB_DIR}/ft8/constants.c
    ${FT8_LIB_DIR}/common/monitor.c
    ${FT8_LIB_DIR}/fft/kiss_fft.c
    ${FT8_LIB_DIR}/fft/kiss_fftr.c
)

# Include directories
include_directories(
    ${FT8_LIB_DIR}
    ${FT8_LIB_DIR}/ft8
    ${FT8_LIB_DIR}/common
    ${FT8_LIB_DIR}/fft
)

# Create executable
add_executable(ft8_test_harness ${SOURCES})

# Link libraries
target_link_libraries(ft8_test_harness m)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")

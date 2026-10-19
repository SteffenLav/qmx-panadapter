#include "constants.h"
#include <stddef.h>   // NULL - ftx_protocol_js8_costas() has no table for FT8/FT4
#include "js8.h"   // the JS8 Costas tables, returned by ftx_protocol_js8_costas()

// Costas sync tone pattern
const uint8_t kFT8_Costas_pattern[7] = { 3, 1, 4, 0, 6, 5, 2 };
const uint8_t kFT4_Costas_pattern[4][4] = {
    { 0, 1, 3, 2 },
    { 1, 0, 2, 3 },
    { 2, 3, 1, 0 },
    { 3, 2, 0, 1 }
};

// Gray code map (FTx bits -> channel symbols)
const uint8_t kFT8_Gray_map[8] = { 0, 1, 3, 2, 5, 6, 4, 7 };
const uint8_t kFT4_Gray_map[4] = { 0, 1, 3, 2 };

const uint8_t kFT4_XOR_sequence[10] = {
    0x4Au, // 01001010
    0x5Eu, // 01011110
    0x89u, // 10001001
    0xB4u, // 10110100
    0xB0u, // 10110000
    0x8Au, // 10001010
    0x79u, // 01111001
    0x55u, // 01010101
    0xBEu, // 10111110
    0x28u, // 00101 [000]
};

// Parity generator matrix for (174,91) LDPC code, stored in bitpacked format (MSB first)
const uint8_t kFTX_LDPC_generator[FTX_LDPC_M][FTX_LDPC_K_BYTES] = {
    { 0x83, 0x29, 0xce, 0x11, 0xbf, 0x31, 0xea, 0xf5, 0x09, 0xf2, 0x7f, 0xc0 },
    { 0x76, 0x1c, 0x26, 0x4e, 0x25, 0xc2, 0x59, 0x33, 0x54, 0x93, 0x13, 0x20 },
    { 0xdc, 0x26, 0x59, 0x02, 0xfb, 0x27, 0x7c, 0x64, 0x10, 0xa1, 0xbd, 0xc0 },
    { 0x1b, 0x3f, 0x41, 0x78, 0x58, 0xcd, 0x2d, 0xd3, 0x3e, 0xc7, 0xf6, 0x20 },
    { 0x09, 0xfd, 0xa4, 0xfe, 0xe0, 0x41, 0x95, 0xfd, 0x03, 0x47, 0x83, 0xa0 },
    { 0x07, 0x7c, 0xcc, 0xc1, 0x1b, 0x88, 0x73, 0xed, 0x5c, 0x3d, 0x48, 0xa0 },
    { 0x29, 0xb6, 0x2a, 0xfe, 0x3c, 0xa0, 0x36, 0xf4, 0xfe, 0x1a, 0x9d, 0xa0 },
    { 0x60, 0x54, 0xfa, 0xf5, 0xf3, 0x5d, 0x96, 0xd3, 0xb0, 0xc8, 0xc3, 0xe0 },
    { 0xe2, 0x07, 0x98, 0xe4, 0x31, 0x0e, 0xed, 0x27, 0x88, 0x4a, 0xe9, 0x00 },
    { 0x77, 0x5c, 0x9c, 0x08, 0xe8, 0x0e, 0x26, 0xdd, 0xae, 0x56, 0x31, 0x80 },
    { 0xb0, 0xb8, 0x11, 0x02, 0x8c, 0x2b, 0xf9, 0x97, 0x21, 0x34, 0x87, 0xc0 },
    { 0x18, 0xa0, 0xc9, 0x23, 0x1f, 0xc6, 0x0a, 0xdf, 0x5c, 0x5e, 0xa3, 0x20 },
    { 0x76, 0x47, 0x1e, 0x83, 0x02, 0xa0, 0x72, 0x1e, 0x01, 0xb1, 0x2b, 0x80 },
    { 0xff, 0xbc, 0xcb, 0x80, 0xca, 0x83, 0x41, 0xfa, 0xfb, 0x47, 0xb2, 0xe0 },
    { 0x66, 0xa7, 0x2a, 0x15, 0x8f, 0x93, 0x25, 0xa2, 0xbf, 0x67, 0x17, 0x00 },
    { 0xc4, 0x24, 0x36, 0x89, 0xfe, 0x85, 0xb1, 0xc5, 0x13, 0x63, 0xa1, 0x80 },
    { 0x0d, 0xff, 0x73, 0x94, 0x14, 0xd1, 0xa1, 0xb3, 0x4b, 0x1c, 0x27, 0x00 },
    { 0x15, 0xb4, 0x88, 0x30, 0x63, 0x6c, 0x8b, 0x99, 0x89, 0x49, 0x72, 0xe0 },
    { 0x29, 0xa8, 0x9c, 0x0d, 0x3d, 0xe8, 0x1d, 0x66, 0x54, 0x89, 0xb0, 0xe0 },
    { 0x4f, 0x12, 0x6f, 0x37, 0xfa, 0x51, 0xcb, 0xe6, 0x1b, 0xd6, 0xb9, 0x40 },
    { 0x99, 0xc4, 0x72, 0x39, 0xd0, 0xd9, 0x7d, 0x3c, 0x84, 0xe0, 0x94, 0x00 },
    { 0x19, 0x19, 0xb7, 0x51, 0x19, 0x76, 0x56, 0x21, 0xbb, 0x4f, 0x1e, 0x80 },
    { 0x09, 0xdb, 0x12, 0xd7, 0x31, 0xfa, 0xee, 0x0b, 0x86, 0xdf, 0x6b, 0x80 },
    { 0x48, 0x8f, 0xc3, 0x3d, 0xf4, 0x3f, 0xbd, 0xee, 0xa4, 0xea, 0xfb, 0x40 },
    { 0x82, 0x74, 0x23, 0xee, 0x40, 0xb6, 0x75, 0xf7, 0x56, 0xeb, 0x5f, 0xe0 },
    { 0xab, 0xe1, 0x97, 0xc4, 0x84, 0xcb, 0x74, 0x75, 0x71, 0x44, 0xa9, 0xa0 },
    { 0x2b, 0x50, 0x0e, 0x4b, 0xc0, 0xec, 0x5a, 0x6d, 0x2b, 0xdb, 0xdd, 0x00 },
    { 0xc4, 0x74, 0xaa, 0x53, 0xd7, 0x02, 0x18, 0x76, 0x16, 0x69, 0x36, 0x00 },
    { 0x8e, 0xba, 0x1a, 0x13, 0xdb, 0x33, 0x90, 0xbd, 0x67, 0x18, 0xce, 0xc0 },
    { 0x75, 0x38, 0x44, 0x67, 0x3a, 0x27, 0x78, 0x2c, 0xc4, 0x20, 0x12, 0xe0 },
    { 0x06, 0xff, 0x83, 0xa1, 0x45, 0xc3, 0x70, 0x35, 0xa5, 0xc1, 0x26, 0x80 },
    { 0x3b, 0x37, 0x41, 0x78, 0x58, 0xcc, 0x2d, 0xd3, 0x3e, 0xc3, 0xf6, 0x20 },
    { 0x9a, 0x4a, 0x5a, 0x28, 0xee, 0x17, 0xca, 0x9c, 0x32, 0x48, 0x42, 0xc0 },
    { 0xbc, 0x29, 0xf4, 0x65, 0x30, 0x9c, 0x97, 0x7e, 0x89, 0x61, 0x0a, 0x40 },
    { 0x26, 0x63, 0xae, 0x6d, 0xdf, 0x8b, 0x5c, 0xe2, 0xbb, 0x29, 0x48, 0x80 },
    { 0x46, 0xf2, 0x31, 0xef, 0xe4, 0x57, 0x03, 0x4c, 0x18, 0x14, 0x41, 0x80 },
    { 0x3f, 0xb2, 0xce, 0x85, 0xab, 0xe9, 0xb0, 0xc7, 0x2e, 0x06, 0xfb, 0xe0 },
    { 0xde, 0x87, 0x48, 0x1f, 0x28, 0x2c, 0x15, 0x39, 0x71, 0xa0, 0xa2, 0xe0 },
    { 0xfc, 0xd7, 0xcc, 0xf2, 0x3c, 0x69, 0xfa, 0x99, 0xbb, 0xa1, 0x41, 0x20 },
    { 0xf0, 0x26, 0x14, 0x47, 0xe9, 0x49, 0x0c, 0xa8, 0xe4, 0x74, 0xce, 0xc0 },
    { 0x44, 0x10, 0x11, 0x58, 0x18, 0x19, 0x6f, 0x95, 0xcd, 0xd7, 0x01, 0x20 },
    { 0x08, 0x8f, 0xc3, 0x1d, 0xf4, 0xbf, 0xbd, 0xe2, 0xa4, 0xea, 0xfb, 0x40 },
    { 0xb8, 0xfe, 0xf1, 0xb6, 0x30, 0x77, 0x29, 0xfb, 0x0a, 0x07, 0x8c, 0x00 },
    { 0x5a, 0xfe, 0xa7, 0xac, 0xcc, 0xb7, 0x7b, 0xbc, 0x9d, 0x99, 0xa9, 0x00 },
    { 0x49, 0xa7, 0x01, 0x6a, 0xc6, 0x53, 0xf6, 0x5e, 0xcd, 0xc9, 0x07, 0x60 },
    { 0x19, 0x44, 0xd0, 0x85, 0xbe, 0x4e, 0x7d, 0xa8, 0xd6, 0xcc, 0x7d, 0x00 },
    { 0x25, 0x1f, 0x62, 0xad, 0xc4, 0x03, 0x2f, 0x0e, 0xe7, 0x14, 0x00, 0x20 },
    { 0x56, 0x47, 0x1f, 0x87, 0x02, 0xa0, 0x72, 0x1e, 0x00, 0xb1, 0x2b, 0x80 },
    { 0x2b, 0x8e, 0x49, 0x23, 0xf2, 0xdd, 0x51, 0xe2, 0xd5, 0x37, 0xfa, 0x00 },
    { 0x6b, 0x55, 0x0a, 0x40, 0xa6, 0x6f, 0x47, 0x55, 0xde, 0x95, 0xc2, 0x60 },
    { 0xa1, 0x8a, 0xd2, 0x8d, 0x4e, 0x27, 0xfe, 0x92, 0xa4, 0xf6, 0xc8, 0x40 },
    { 0x10, 0xc2, 0xe5, 0x86, 0x38, 0x8c, 0xb8, 0x2a, 0x3d, 0x80, 0x75, 0x80 },
    { 0xef, 0x34, 0xa4, 0x18, 0x17, 0xee, 0x02, 0x13, 0x3d, 0xb2, 0xeb, 0x00 },
    { 0x7e, 0x9c, 0x0c, 0x54, 0x32, 0x5a, 0x9c, 0x15, 0x83, 0x6e, 0x00, 0x00 },
    { 0x36, 0x93, 0xe5, 0x72, 0xd1, 0xfd, 0xe4, 0xcd, 0xf0, 0x79, 0xe8, 0x60 },
    { 0xbf, 0xb2, 0xce, 0xc5, 0xab, 0xe1, 0xb0, 0xc7, 0x2e, 0x07, 0xfb, 0xe0 },
    { 0x7e, 0xe1, 0x82, 0x30, 0xc5, 0x83, 0xcc, 0xcc, 0x57, 0xd4, 0xb0, 0x80 },
    { 0xa0, 0x66, 0xcb, 0x2f, 0xed, 0xaf, 0xc9, 0xf5, 0x26, 0x64, 0x12, 0x60 },
    { 0xbb, 0x23, 0x72, 0x5a, 0xbc, 0x47, 0xcc, 0x5f, 0x4c, 0xc4, 0xcd, 0x20 },
    { 0xde, 0xd9, 0xdb, 0xa3, 0xbe, 0xe4, 0x0c, 0x59, 0xb5, 0x60, 0x9b, 0x40 },
    { 0xd9, 0xa7, 0x01, 0x6a, 0xc6, 0x53, 0xe6, 0xde, 0xcd, 0xc9, 0x03, 0x60 },
    { 0x9a, 0xd4, 0x6a, 0xed, 0x5f, 0x70, 0x7f, 0x28, 0x0a, 0xb5, 0xfc, 0x40 },
    { 0xe5, 0x92, 0x1c, 0x77, 0x82, 0x25, 0x87, 0x31, 0x6d, 0x7d, 0x3c, 0x20 },
    { 0x4f, 0x14, 0xda, 0x82, 0x42, 0xa8, 0xb8, 0x6d, 0xca, 0x73, 0x35, 0x20 },
    { 0x8b, 0x8b, 0x50, 0x7a, 0xd4, 0x67, 0xd4, 0x44, 0x1d, 0xf7, 0x70, 0xe0 },
    { 0x22, 0x83, 0x1c, 0x9c, 0xf1, 0x16, 0x94, 0x67, 0xad, 0x04, 0xb6, 0x80 },
    { 0x21, 0x3b, 0x83, 0x8f, 0xe2, 0xae, 0x54, 0xc3, 0x8e, 0xe7, 0x18, 0x00 },
    { 0x5d, 0x92, 0x6b, 0x6d, 0xd7, 0x1f, 0x08, 0x51, 0x81, 0xa4, 0xe1, 0x20 },
    { 0x66, 0xab, 0x79, 0xd4, 0xb2, 0x9e, 0xe6, 0xe6, 0x95, 0x09, 0xe5, 0x60 },
    { 0x95, 0x81, 0x48, 0x68, 0x2d, 0x74, 0x8a, 0x38, 0xdd, 0x68, 0xba, 0xa0 },
    { 0xb8, 0xce, 0x02, 0x0c, 0xf0, 0x69, 0xc3, 0x2a, 0x72, 0x3a, 0xb1, 0x40 },
    { 0xf4, 0x33, 0x1d, 0x6d, 0x46, 0x16, 0x07, 0xe9, 0x57, 0x52, 0x74, 0x60 },
    { 0x6d, 0xa2, 0x3b, 0xa4, 0x24, 0xb9, 0x59, 0x61, 0x33, 0xcf, 0x9c, 0x80 },
    { 0xa6, 0x36, 0xbc, 0xbc, 0x7b, 0x30, 0xc5, 0xfb, 0xea, 0xe6, 0x7f, 0xe0 },
    { 0x5c, 0xb0, 0xd8, 0x6a, 0x07, 0xdf, 0x65, 0x4a, 0x90, 0x89, 0xa2, 0x00 },
    { 0xf1, 0x1f, 0x10, 0x68, 0x48, 0x78, 0x0f, 0xc9, 0xec, 0xdd, 0x80, 0xa0 },
    { 0x1f, 0xbb, 0x53, 0x64, 0xfb, 0x8d, 0x2c, 0x9d, 0x73, 0x0d, 0x5b, 0xa0 },
    { 0xfc, 0xb8, 0x6b, 0xc7, 0x0a, 0x50, 0xc9, 0xd0, 0x2a, 0x5d, 0x03, 0x40 },
    { 0xa5, 0x34, 0x43, 0x30, 0x29, 0xea, 0xc1, 0x5f, 0x32, 0x2e, 0x34, 0xc0 },
    { 0xc9, 0x89, 0xd9, 0xc7, 0xc3, 0xd3, 0xb8, 0xc5, 0x5d, 0x75, 0x13, 0x00 },
    { 0x7b, 0xb3, 0x8b, 0x2f, 0x01, 0x86, 0xd4, 0x66, 0x43, 0xae, 0x96, 0x20 },
    { 0x26, 0x44, 0xeb, 0xad, 0xeb, 0x44, 0xb9, 0x46, 0x7d, 0x1f, 0x42, 0xc0 },
    { 0x60, 0x8c, 0xc8, 0x57, 0x59, 0x4b, 0xfb, 0xb5, 0x5d, 0x69, 0x60, 0x00 }
};

// Each row describes one LDPC parity check.
// Each number is an index into the codeword (1-origin).
// The codeword bits mentioned in each row must XOR to zero.
const uint8_t kFTX_LDPC_Nm[FTX_LDPC_M][7] = {
    { 4, 31, 59, 91, 92, 96, 153 },
    { 5, 32, 60, 93, 115, 146, 0 },
    { 6, 24, 61, 94, 122, 151, 0 },
    { 7, 33, 62, 95, 96, 143, 0 },
    { 8, 25, 63, 83, 93, 96, 148 },
    { 6, 32, 64, 97, 126, 138, 0 },
    { 5, 34, 65, 78, 98, 107, 154 },
    { 9, 35, 66, 99, 139, 146, 0 },
    { 10, 36, 67, 100, 107, 126, 0 },
    { 11, 37, 67, 87, 101, 139, 158 },
    { 12, 38, 68, 102, 105, 155, 0 },
    { 13, 39, 69, 103, 149, 162, 0 },
    { 8, 40, 70, 82, 104, 114, 145 },
    { 14, 41, 71, 88, 102, 123, 156 },
    { 15, 42, 59, 106, 123, 159, 0 },
    { 1, 33, 72, 106, 107, 157, 0 },
    { 16, 43, 73, 108, 141, 160, 0 },
    { 17, 37, 74, 81, 109, 131, 154 },
    { 11, 44, 75, 110, 121, 166, 0 },
    { 45, 55, 64, 111, 130, 161, 173 },
    { 8, 46, 71, 112, 119, 166, 0 },
    { 18, 36, 76, 89, 113, 114, 143 },
    { 19, 38, 77, 104, 116, 163, 0 },
    { 20, 47, 70, 92, 138, 165, 0 },
    { 2, 48, 74, 113, 128, 160, 0 },
    { 21, 45, 78, 83, 117, 121, 151 },
    { 22, 47, 58, 118, 127, 164, 0 },
    { 16, 39, 62, 112, 134, 158, 0 },
    { 23, 43, 79, 120, 131, 145, 0 },
    { 19, 35, 59, 73, 110, 125, 161 },
    { 20, 36, 63, 94, 136, 161, 0 },
    { 14, 31, 79, 98, 132, 164, 0 },
    { 3, 44, 80, 124, 127, 169, 0 },
    { 19, 46, 81, 117, 135, 167, 0 },
    { 7, 49, 58, 90, 100, 105, 168 },
    { 12, 50, 61, 118, 119, 144, 0 },
    { 13, 51, 64, 114, 118, 157, 0 },
    { 24, 52, 76, 129, 148, 149, 0 },
    { 25, 53, 69, 90, 101, 130, 156 },
    { 20, 46, 65, 80, 120, 140, 170 },
    { 21, 54, 77, 100, 140, 171, 0 },
    { 35, 82, 133, 142, 171, 174, 0 },
    { 14, 30, 83, 113, 125, 170, 0 },
    { 4, 29, 68, 120, 134, 173, 0 },
    { 1, 4, 52, 57, 86, 136, 152 },
    { 26, 51, 56, 91, 122, 137, 168 },
    { 52, 84, 110, 115, 145, 168, 0 },
    { 7, 50, 81, 99, 132, 173, 0 },
    { 23, 55, 67, 95, 172, 174, 0 },
    { 26, 41, 77, 109, 141, 148, 0 },
    { 2, 27, 41, 61, 62, 115, 133 },
    { 27, 40, 56, 124, 125, 126, 0 },
    { 18, 49, 55, 124, 141, 167, 0 },
    { 6, 33, 85, 108, 116, 156, 0 },
    { 28, 48, 70, 85, 105, 129, 158 },
    { 9, 54, 63, 131, 147, 155, 0 },
    { 22, 53, 68, 109, 121, 174, 0 },
    { 3, 13, 48, 78, 95, 123, 0 },
    { 31, 69, 133, 150, 155, 169, 0 },
    { 12, 43, 66, 89, 97, 135, 159 },
    { 5, 39, 75, 102, 136, 167, 0 },
    { 2, 54, 86, 101, 135, 164, 0 },
    { 15, 56, 87, 108, 119, 171, 0 },
    { 10, 44, 82, 91, 111, 144, 149 },
    { 23, 34, 71, 94, 127, 153, 0 },
    { 11, 49, 88, 92, 142, 157, 0 },
    { 29, 34, 87, 97, 147, 162, 0 },
    { 30, 50, 60, 86, 137, 142, 162 },
    { 10, 53, 66, 84, 112, 128, 165 },
    { 22, 57, 85, 93, 140, 159, 0 },
    { 28, 32, 72, 103, 132, 166, 0 },
    { 28, 29, 84, 88, 117, 143, 150 },
    { 1, 26, 45, 80, 128, 147, 0 },
    { 17, 27, 89, 103, 116, 153, 0 },
    { 51, 57, 98, 163, 165, 172, 0 },
    { 21, 37, 73, 138, 152, 169, 0 },
    { 16, 47, 76, 130, 137, 154, 0 },
    { 3, 24, 30, 72, 104, 139, 0 },
    { 9, 40, 90, 106, 134, 151, 0 },
    { 15, 58, 60, 74, 111, 150, 163 },
    { 18, 42, 79, 144, 146, 152, 0 },
    { 25, 38, 65, 99, 122, 160, 0 },
    { 17, 42, 75, 129, 170, 172, 0 }
};

// Each row corresponds to a codeword bit.
// The numbers indicate which three LDPC parity checks (rows in Nm) refer to the codeword bit.
// 1-origin.
const uint8_t kFTX_LDPC_Mn[FTX_LDPC_N][3] = {
    { 16, 45, 73 },
    { 25, 51, 62 },
    { 33, 58, 78 },
    { 1, 44, 45 },
    { 2, 7, 61 },
    { 3, 6, 54 },
    { 4, 35, 48 },
    { 5, 13, 21 },
    { 8, 56, 79 },
    { 9, 64, 69 },
    { 10, 19, 66 },
    { 11, 36, 60 },
    { 12, 37, 58 },
    { 14, 32, 43 },
    { 15, 63, 80 },
    { 17, 28, 77 },
    { 18, 74, 83 },
    { 22, 53, 81 },
    { 23, 30, 34 },
    { 24, 31, 40 },
    { 26, 41, 76 },
    { 27, 57, 70 },
    { 29, 49, 65 },
    { 3, 38, 78 },
    { 5, 39, 82 },
    { 46, 50, 73 },
    { 51, 52, 74 },
    { 55, 71, 72 },
    { 44, 67, 72 },
    { 43, 68, 78 },
    { 1, 32, 59 },
    { 2, 6, 71 },
    { 4, 16, 54 },
    { 7, 65, 67 },
    { 8, 30, 42 },
    { 9, 22, 31 },
    { 10, 18, 76 },
    { 11, 23, 82 },
    { 12, 28, 61 },
    { 13, 52, 79 },
    { 14, 50, 51 },
    { 15, 81, 83 },
    { 17, 29, 60 },
    { 19, 33, 64 },
    { 20, 26, 73 },
    { 21, 34, 40 },
    { 24, 27, 77 },
    { 25, 55, 58 },
    { 35, 53, 66 },
    { 36, 48, 68 },
    { 37, 46, 75 },
    { 38, 45, 47 },
    { 39, 57, 69 },
    { 41, 56, 62 },
    { 20, 49, 53 },
    { 46, 52, 63 },
    { 45, 70, 75 },
    { 27, 35, 80 },
    { 1, 15, 30 },
    { 2, 68, 80 },
    { 3, 36, 51 },
    { 4, 28, 51 },
    { 5, 31, 56 },
    { 6, 20, 37 },
    { 7, 40, 82 },
    { 8, 60, 69 },
    { 9, 10, 49 },
    { 11, 44, 57 },
    { 12, 39, 59 },
    { 13, 24, 55 },
    { 14, 21, 65 },
    { 16, 71, 78 },
    { 17, 30, 76 },
    { 18, 25, 80 },
    { 19, 61, 83 },
    { 22, 38, 77 },
    { 23, 41, 50 },
    { 7, 26, 58 },
    { 29, 32, 81 },
    { 33, 40, 73 },
    { 18, 34, 48 },
    { 13, 42, 64 },
    { 5, 26, 43 },
    { 47, 69, 72 },
    { 54, 55, 70 },
    { 45, 62, 68 },
    { 10, 63, 67 },
    { 14, 66, 72 },
    { 22, 60, 74 },
    { 35, 39, 79 },
    { 1, 46, 64 },
    { 1, 24, 66 },
    { 2, 5, 70 },
    { 3, 31, 65 },
    { 4, 49, 58 },
    { 1, 4, 5 },
    { 6, 60, 67 },
    { 7, 32, 75 },
    { 8, 48, 82 },
    { 9, 35, 41 },
    { 10, 39, 62 },
    { 11, 14, 61 },
    { 12, 71, 74 },
    { 13, 23, 78 },
    { 11, 35, 55 },
    { 15, 16, 79 },
    { 7, 9, 16 },
    { 17, 54, 63 },
    { 18, 50, 57 },
    { 19, 30, 47 },
    { 20, 64, 80 },
    { 21, 28, 69 },
    { 22, 25, 43 },
    { 13, 22, 37 },
    { 2, 47, 51 },
    { 23, 54, 74 },
    { 26, 34, 72 },
    { 27, 36, 37 },
    { 21, 36, 63 },
    { 29, 40, 44 },
    { 19, 26, 57 },
    { 3, 46, 82 },
    { 14, 15, 58 },
    { 33, 52, 53 },
    { 30, 43, 52 },
    { 6, 9, 52 },
    { 27, 33, 65 },
    { 25, 69, 73 },
    { 38, 55, 83 },
    { 20, 39, 77 },
    { 18, 29, 56 },
    { 32, 48, 71 },
    { 42, 51, 59 },
    { 28, 44, 79 },
    { 34, 60, 62 },
    { 31, 45, 61 },
    { 46, 68, 77 },
    { 6, 24, 76 },
    { 8, 10, 78 },
    { 40, 41, 70 },
    { 17, 50, 53 },
    { 42, 66, 68 },
    { 4, 22, 72 },
    { 36, 64, 81 },
    { 13, 29, 47 },
    { 2, 8, 81 },
    { 56, 67, 73 },
    { 5, 38, 50 },
    { 12, 38, 64 },
    { 59, 72, 80 },
    { 3, 26, 79 },
    { 45, 76, 81 },
    { 1, 65, 74 },
    { 7, 18, 77 },
    { 11, 56, 59 },
    { 14, 39, 54 },
    { 16, 37, 66 },
    { 10, 28, 55 },
    { 15, 60, 70 },
    { 17, 25, 82 },
    { 20, 30, 31 },
    { 12, 67, 68 },
    { 23, 75, 80 },
    { 27, 32, 62 },
    { 24, 69, 75 },
    { 19, 21, 71 },
    { 34, 53, 61 },
    { 35, 46, 47 },
    { 33, 59, 76 },
    { 40, 43, 83 },
    { 41, 42, 63 },
    { 49, 75, 83 },
    { 20, 44, 48 },
    { 42, 49, 57 }
};

const uint8_t kFTX_LDPC_Num_rows[FTX_LDPC_M] = {
    7, 6, 6, 6, 7, 6, 7, 6, 6, 7, 6, 6, 7, 7, 6, 6,
    6, 7, 6, 7, 6, 7, 6, 6, 6, 7, 6, 6, 6, 7, 6, 6,
    6, 6, 7, 6, 6, 6, 7, 7, 6, 6, 6, 6, 7, 7, 6, 6,
    6, 6, 7, 6, 6, 6, 7, 6, 6, 6, 6, 7, 6, 6, 6, 7,
    6, 6, 6, 7, 7, 6, 6, 7, 6, 6, 6, 6, 6, 6, 6, 7,
    6, 6, 6
};

const ftx_ldpc_code_t kFTX_LDPC_code_174_91 = {
    .M = FTX_LDPC_M,
    .Nm = kFTX_LDPC_Nm,
    .Mn = kFTX_LDPC_Mn,
    .Num_rows = kFTX_LDPC_Num_rows,
};


/* ---- protocol facts, in one place - see the note in constants.h ---------- */

/* ⛔ EVERY SWITCH IN THIS SECTION LISTS EVERY ENUM VALUE AND HAS NO default.
 * That is deliberate: -Wswitch then makes the compiler name the file and line
 * of every fact a new protocol has not been given, instead of a default
 * quietly handing it FT8's. The trailing return is for a value cast in from
 * outside the enum, not for a missing case. */

int ftx_protocol_slot_ms(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return  7500;
    case FTX_PROTOCOL_FT8:       return 15000;
    /* JS8 Normal shares FT8's whole timing - 12000 Hz, 1920 samples per
     * symbol, 79 symbols, 15 s. Only the tables differ. */
    case FTX_PROTOCOL_JS8:       return 15000;
    case FTX_PROTOCOL_JS8_FAST:  return 10000;
    case FTX_PROTOCOL_JS8_TURBO: return  6000;
    /* 30 s, from commons.h's JS8E_TX_SECONDS - NOT js8e_params.f90's
     * NTXDUR=28, which is the decoder's analysis window. See constants.h. */
    case FTX_PROTOCOL_JS8_SLOW:  return 30000;
    }
    return 15000;
}

/* NSPS / 12000, in microseconds. Exact in every case: 1920, 1200, 600 and
 * 3840 all divide 12 evenly, so none of these is a rounded figure.
 * FT4's 48 ms is 576/12000 by the same arithmetic. */
int ftx_protocol_symbol_period_us(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return  48000;  /*  576 NSPS */
    case FTX_PROTOCOL_FT8:       return 160000;  /* 1920 NSPS */
    case FTX_PROTOCOL_JS8:       return 160000;  /* 1920 NSPS */
    case FTX_PROTOCOL_JS8_FAST:  return 100000;  /* 1200 NSPS */
    case FTX_PROTOCOL_JS8_TURBO: return  50000;  /*  600 NSPS */
    case FTX_PROTOCOL_JS8_SLOW:  return 320000;  /* 3840 NSPS */
    }
    return 160000;
}

/* 12000 / NSPS. The reciprocal of the symbol period, and in JS8 the tone
 * spacing and the baud rate are the same number. */
float ftx_protocol_tone_spacing_hz(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return 20.8333333f; /* 12000/576  */
    case FTX_PROTOCOL_FT8:       return  6.25f;      /* 12000/1920 */
    case FTX_PROTOCOL_JS8:       return  6.25f;      /* 12000/1920 */
    case FTX_PROTOCOL_JS8_FAST:  return 10.0f;       /* 12000/1200 */
    case FTX_PROTOCOL_JS8_TURBO: return 20.0f;       /* 12000/600  */
    case FTX_PROTOCOL_JS8_SLOW:  return  3.125f;     /* 12000/3840 */
    }
    return 6.25f;
}

int ftx_protocol_start_delay_ms(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return   0;
    case FTX_PROTOCOL_FT8:       return   0;
    case FTX_PROTOCOL_JS8:       return 500;
    case FTX_PROTOCOL_JS8_FAST:  return 200;
    case FTX_PROTOCOL_JS8_TURBO: return 100;
    case FTX_PROTOCOL_JS8_SLOW:  return 500;
    }
    return 0;
}

bool ftx_protocol_is_js8(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:
    case FTX_PROTOCOL_FT8:       return false;
    case FTX_PROTOCOL_JS8:
    case FTX_PROTOCOL_JS8_FAST:
    case FTX_PROTOCOL_JS8_TURBO:
    case FTX_PROTOCOL_JS8_SLOW:  return true;
    }
    return false;
}

int ftx_protocol_js8_submode(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:
    case FTX_PROTOCOL_FT8:       return -1;
    case FTX_PROTOCOL_JS8:       return 0;   /* A */
    case FTX_PROTOCOL_JS8_FAST:  return 1;   /* B */
    case FTX_PROTOCOL_JS8_TURBO: return 2;   /* C */
    case FTX_PROTOCOL_JS8_SLOW:  return 4;   /* E - the enum is NOT dense */
    }
    return -1;
}

const uint8_t (*ftx_protocol_js8_costas(ftx_protocol_t protocol))[7]
{
    const uint8_t (*costas)[7] = NULL;
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:
    case FTX_PROTOCOL_FT8:       costas = NULL;                 break;
    case FTX_PROTOCOL_JS8:       costas = kJS8_Costas_original; break;
    case FTX_PROTOCOL_JS8_FAST:
    case FTX_PROTOCOL_JS8_TURBO:
    case FTX_PROTOCOL_JS8_SLOW:  costas = kJS8_Costas_modified; break;
    }
    return costas;
}

int ftx_protocol_num_tones(ftx_protocol_t protocol)
{
    /* Every JS8 submode is 8-FSK; only FT4 is 4-FSK. */
    return (protocol == FTX_PROTOCOL_FT4) ? 4 : 8;
}

int ftx_protocol_num_symbols(ftx_protocol_t protocol)
{
    /* 21 sync + 58 data for FT8 and every JS8 submode; FT4 is its own shape.
     * The submodes change the symbol PERIOD, never the symbol COUNT - all four
     * JS8 physical layers carry the same 79. */
    return (protocol == FTX_PROTOCOL_FT4) ? 105 : 79;
}

/* ⛔ THE IDENTITY, NOT A LABEL - EVERY JS8 SUBMODE IS "JS8" HERE.
 *
 * This string is not decoration. It is the ADIF MODE field written into the
 * log (ft8_qso.c), the mode published to PSK Reporter (pskreporter.c, whose
 * buffer is char[6]), and what the web page tests with `proto === "JS8"` to
 * decide whether to show the whole JS8 pane.
 *
 * ⚠ IT RETURNED "JS8-FAST"/"JS8-TURBO"/"JS8-SLOW" FOR A FEW HOURS ON
 * 2026-10-14 AND EVERY ONE OF THOSE BROKE: the web JS8 pane vanished on any
 * submode but Normal, the ADIF log would have recorded a MODE no logger
 * accepts, and PSK Reporter would have been sent "JS8-T" - the first five
 * bytes of a truncated name - as a mode. Found by loading the page against
 * tools/webstub.py, which is the only check that runs the JavaScript;
 * check_web and check_webui both passed on it.
 *
 * The submode is a physical layer, not a different mode on the air: a JS8
 * contact is a JS8 contact whichever one it used. Where the submode genuinely
 * matters - logs, the drawer, the API's display field - use
 * ftx_protocol_label(). */
const char* ftx_protocol_name(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return "FT4";
    case FTX_PROTOCOL_FT8:       return "FT8";
    case FTX_PROTOCOL_JS8:
    case FTX_PROTOCOL_JS8_FAST:
    case FTX_PROTOCOL_JS8_TURBO:
    case FTX_PROTOCOL_JS8_SLOW:  return "JS8";
    }
    return "?";
}

/* The human label, which DOES name the submode. For log lines, the drawer and
 * the API's display field - never for ADIF, PSK Reporter or anything else that
 * compares it or puts it on the air. See ftx_protocol_name() above. */
const char* ftx_protocol_label(ftx_protocol_t protocol)
{
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:       return "FT4";
    case FTX_PROTOCOL_FT8:       return "FT8";
    case FTX_PROTOCOL_JS8:       return "JS8 Normal";
    case FTX_PROTOCOL_JS8_FAST:  return "JS8 Fast";
    case FTX_PROTOCOL_JS8_TURBO: return "JS8 Turbo";
    case FTX_PROTOCOL_JS8_SLOW:  return "JS8 Slow";
    }
    return "?";
}

bool ftx_protocol_is_implemented(ftx_protocol_t protocol)
{
    /* JS8 Normal is implemented as of J5: js8_decode_candidate() in decode.c
     * (its own post-LDPC half - 87 bits, CRC-12, colorder), js8_encode() for
     * the tones, js8_text.c for the message text both ways, and the QSO ladder
     * and UI reach it through ft8_op_mode_protocol().
     *
     * ⭐ RECEIVE IS PROVEN ON AIR (2026-10-08, 40 m 7.078): real JS8Call
     * traffic decoded - 9A3SWO's heartbeat six times, and a Directed SNR frame
     * carrying a portable callsign. That settles the Costas sync, LDPC(174,87),
     * colorder, the payload layout, BOTH callsign codecs, packGrid, packNum and
     * - the one link that had no arbiter at all - the CRC-12 engine, since
     * js8_check_payload_crc() recomputes and compares.
     *
     * ⛔ TRANSMIT IS NOT PROVEN. No JS8 frame of ours has been decoded by
     * anyone. The tone path is bit-exact against JS8Call's own genjs8.f90
     * (tools/js8_tone_arbiter.py, 64 of 64), so the odds are good, but "they
     * can decode us" has never been observed. Only a real JS8Call station
     * settles it - never a second Tab5, which would share our mistakes
     * exactly.
     *
     * ⭐ FAST, TURBO AND SLOW ARE IMPLEMENTED AS OF THIS CHANGE, AND NONE OF
     * THEM IS PROVEN. Implemented and proven are different words and this
     * function answers only the first: the codec, the three MODIFIED Costas
     * arrays, the per-submode NSPS/period/tone-spacing and the TX symbol loop
     * all exist and round-trip through the host harness
     * (test/js8_submode_harness.c, encode -> synthesise -> decode, every
     * submode). What has NOT happened is a real JS8Call station decoding one
     * of our Fast/Turbo/Slow frames, or us decoding one of theirs off air.
     * Until that, the mode is present and unverified - say so, and never
     * "not completed".
     *
     * ULTRA is absent by choice, not omission - see constants.h. */
    switch (protocol)
    {
    case FTX_PROTOCOL_FT4:
    case FTX_PROTOCOL_FT8:
    case FTX_PROTOCOL_JS8:
    case FTX_PROTOCOL_JS8_FAST:
    case FTX_PROTOCOL_JS8_TURBO:
    case FTX_PROTOCOL_JS8_SLOW:  return true;
    }
    return false;
}

// JS8 Normal codec primitives. See js8.h for the source references and for
// the three differences from FT8 that are easy to get wrong.

#include "js8.h"

#include <string.h>

void js8_encode174(const uint8_t payload[JS8_LDPC_K], uint8_t codeword[JS8_LDPC_N])
{
    // Parity bits first, payload second - then permuted by colorder. This is
    // encode174.f90's itmp(1:M)=pchecks / itmp(M+1:N)=message, followed by
    // codeword(colorder+1)=itmp.
    uint8_t itmp[JS8_LDPC_N];

    for (int i = 0; i < JS8_LDPC_M; ++i)
    {
        uint8_t sum = 0;
        for (int j = 0; j < JS8_LDPC_K; ++j)
        {
            // Generator rows are bitpacked MSB first.
            uint8_t g = (kJS8_LDPC_generator[i][j / 8] >> (7 - (j % 8))) & 1u;
            sum ^= (uint8_t)(payload[j] & g);
        }
        itmp[i] = sum;
    }
    memcpy(itmp + JS8_LDPC_M, payload, JS8_LDPC_K);

    for (int i = 0; i < JS8_LDPC_N; ++i)
    {
        codeword[kJS8_LDPC_colorder[i]] = itmp[i];
    }
}

uint16_t js8_crc12_raw(const uint8_t* data, int nbits, uint16_t poly)
{
    // MSB-first polynomial division, initial remainder 0, no reflection, no
    // final XOR - boost::augmented_crc<12, POLY> with its default parameters,
    // which is what lib/crc12.cpp calls. The augmenting zero bits are the
    // caller's business, as they are in JS8Call.
    uint16_t rem = 0;

    for (int i = 0; i < nbits; ++i)
    {
        uint16_t in = (uint16_t)((data[i / 8] >> (7 - (i % 8))) & 1u);
        uint16_t out = (uint16_t)((rem >> 11) & 1u);
        rem = (uint16_t)(((rem << 1) | in) & 0x0FFFu);
        if (out)
        {
            rem ^= poly;
        }
    }
    return rem;
}

// The 11-byte buffer the CRC is taken over: 72-bit frame, 3-bit transmission
// type, then zeros. genjs8.f90 masks byte 10 to its top 3 bits and clears
// byte 11, so bits 76..88 are zero - 13 augmenting zeros, not 12. That is
// what the reference does; it is not a transcription slip here.
static void js8_crc_buffer(const uint8_t frame[9], uint8_t itype, uint8_t buf[11])
{
    memcpy(buf, frame, 9);
    buf[9] = (uint8_t)((itype & 0x07u) << 5);
    buf[10] = 0;
}

static uint16_t js8_crc12_of(const uint8_t frame[9], uint8_t itype)
{
    uint8_t buf[11];
    js8_crc_buffer(frame, itype, buf);
    return (uint16_t)((js8_crc12_raw(buf, 88, JS8_CRC_POLY) ^ JS8_CRC_XOR) & 0x0FFFu);
}

void js8_pack_payload(const uint8_t frame[9], uint8_t itype, uint8_t payload[JS8_LDPC_K])
{
    for (int i = 0; i < JS8_FRAME_BITS; ++i)
    {
        payload[i] = (uint8_t)((frame[i / 8] >> (7 - (i % 8))) & 1u);
    }
    for (int i = 0; i < 3; ++i)
    {
        payload[JS8_FRAME_BITS + i] = (uint8_t)((itype >> (2 - i)) & 1u);
    }

    uint16_t crc = js8_crc12_of(frame, itype);
    for (int i = 0; i < JS8_CRC_BITS; ++i)
    {
        payload[JS8_MSG_BITS + i] = (uint8_t)((crc >> (11 - i)) & 1u);
    }
}

bool js8_check_payload_crc(const uint8_t payload[JS8_LDPC_K])
{
    uint8_t frame[9];
    uint8_t itype = 0;

    memset(frame, 0, sizeof(frame));
    for (int i = 0; i < JS8_FRAME_BITS; ++i)
    {
        frame[i / 8] |= (uint8_t)((payload[i] & 1u) << (7 - (i % 8)));
    }
    for (int i = 0; i < 3; ++i)
    {
        itype = (uint8_t)((itype << 1) | (payload[JS8_FRAME_BITS + i] & 1u));
    }

    uint16_t carried = 0;
    for (int i = 0; i < JS8_CRC_BITS; ++i)
    {
        carried = (uint16_t)((carried << 1) | (payload[JS8_MSG_BITS + i] & 1u));
    }

    return carried == js8_crc12_of(frame, itype);
}

void js8_tones_from_codeword(const uint8_t codeword[JS8_LDPC_N], ftx_protocol_t protocol,
                             uint8_t tones[JS8_NN])
{
    /* ⛔ The sync arrays are per SUBMODE, and the three blocks are not
     * necessarily the same array - see kJS8_Costas_modified. This used to read
     * kJS8_Costas_pattern[i] three times, which was Normal's and only
     * Normal's. */
    const uint8_t (*costas)[7] = ftx_protocol_js8_costas(protocol);

    // Message structure: S7 D29 S7 D29 S7 - identical to FT8's.
    for (int i = 0; i < 7; ++i)
    {
        tones[i]      = costas[0][i];
        tones[36 + i] = costas[1][i];
        tones[72 + i] = costas[2][i];
    }

    int k = 6; // genjs8.f90's k=7, one-based
    for (int j = 0; j < JS8_ND; ++j)
    {
        int i = 3 * j;
        ++k;
        if (j == 29)
            k += 7; // step over the middle sync block (its j.eq.30)
        tones[k] = (uint8_t)(codeword[i] * 4 + codeword[i + 1] * 2 + codeword[i + 2]);
    }
}

void js8_encode(const uint8_t frame[9], uint8_t itype, ftx_protocol_t protocol,
                uint8_t tones[JS8_NN])
{
    uint8_t payload[JS8_LDPC_K];
    uint8_t codeword[JS8_LDPC_N];

    /* Everything above the physical layer is shared by every submode: the
     * varicode frame, the 3-bit transmission type, CRC-12, LDPC(174,87) and
     * colorder are byte-identical. `protocol` reaches only as far as the sync
     * arrays, and the caller's NSPS/tone spacing do the rest. */
    js8_pack_payload(frame, itype, payload);
    js8_encode174(payload, codeword);
    js8_tones_from_codeword(codeword, protocol, tones);
}

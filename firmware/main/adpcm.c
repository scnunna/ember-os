// IMA-ADPCM block encoder for the Ember Link AUDIO stream (docs/EMBER_LINK.md). Must stay bit-identical to the
// reference in tools/ember_adpcm.py (checked on-device by the "adpcmtest" shell command against docs/link_vectors.json).
#include <stdint.h>
#include <string.h>
#include "adpcm.h"

static const int16_t STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int adpcm_encode_block(const int16_t *s, uint8_t seq, int *index, uint8_t *out)
{
    int pred = s[0], idx = *index;
    out[0] = seq;
    out[1] = (uint8_t)(pred & 0xFF);
    out[2] = (uint8_t)((pred >> 8) & 0xFF);
    out[3] = (uint8_t)idx;
    out[4] = 0;
    uint8_t *d = out + 5;
    for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
        int step = STEP[idx], diff = s[i] - pred, nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        if (diff >= step) { nib |= 4; diff -= step; }
        if (diff >= (step >> 1)) { nib |= 2; diff -= step >> 1; }
        if (diff >= (step >> 2)) nib |= 1;
        // decoder-side reconstruction (must match the reference exactly)
        int dq = step >> 3;
        if (nib & 1) dq += step >> 2;
        if (nib & 2) dq += step >> 1;
        if (nib & 4) dq += step;
        pred = clampi((nib & 8) ? pred - dq : pred + dq, -32768, 32767);
        idx = clampi(idx + IDX[nib], 0, 88);
        if (i & 1) d[i >> 1] |= (uint8_t)(nib << 4); else d[i >> 1] = (uint8_t)nib;
    }
    *index = idx;
    return ADPCM_BLOCK_BYTES;
}

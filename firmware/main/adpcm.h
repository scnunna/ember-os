#pragma once
#include <stdint.h>
#define ADPCM_BLOCK_SAMPLES 256
#define ADPCM_BLOCK_BYTES   (5 + ADPCM_BLOCK_SAMPLES / 2)    // 133
// Encodes 256 samples into one 133-byte Ember Link AUDIO block. *index carries over between blocks (start with 0).
int adpcm_encode_block(const int16_t *samples, uint8_t seq, int *index, uint8_t *out);

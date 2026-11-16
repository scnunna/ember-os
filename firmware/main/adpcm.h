#pragma once
#include <stdint.h>
#define ADPCM_BLOCK_SAMPLES 256
#define ADPCM_BLOCK_BYTES   (5 + ADPCM_BLOCK_SAMPLES / 2)    // 133
// Encodes 256 samples into one 133-byte Ember Link AUDIO block. *index carries over between blocks (start with 0).
int adpcm_encode_block(const int16_t *samples, uint8_t seq, int *index, uint8_t *out);
// Standard IMA-ADPCM WAV (format 0x11) block: 505 samples -> 256 bytes. Used for compressed meeting recordings.
#define IMA_WAV_BLOCK_SAMPLES 505
#define IMA_WAV_BLOCK_BYTES   256
int ima_wav_encode_block(const int16_t *samples, int nsamp, int *index, uint8_t *out);
void adpcm_decode_block(const uint8_t *in133, int16_t *out256);   // AUDIO block -> 256 samples

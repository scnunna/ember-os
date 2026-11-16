// Amber speaks: the phone turns Amber's reply into speech (offline TTS), encodes it as AUDIO-format IMA-ADPCM blocks
// and writes them to the SPK characteristic (Ember Link v1.5). This plays them on the watch speaker.
//  * a 2 s jitter buffer in PSRAM (the phone runs up to 1 s ahead), playback starts after ~200 ms of audio
//  * never while the mic is in use (meeting recording, Amber listening) -> say_skip
//  * a BOOT press stops it; it ends on say_end (after playing out) or after 3 s without audio
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "app.h"
#include "hw.h"
#include "ble.h"
#include "link.h"
#include "adpcm.h"
#include "diag.h"
#include "speak.h"

#define RING_BLOCKS 128
static StreamBufferHandle_t s_sb;
static volatile bool s_active, s_end, s_stop;
static char s_id[40];
static volatile uint32_t s_rx;

bool speak_active(void) { return s_active; }
void speak_stop(void) { if (s_active) s_stop = true; }

static void reply(const char *t, const char *why, int played, int lost)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    cJSON_AddStringToObject(o, "id", s_id);
    if (why) cJSON_AddStringToObject(o, "why", why);
    if (played >= 0) { cJSON_AddNumberToObject(o, "played", played); cJSON_AddNumberToObject(o, "lost", lost); }
    link_send_json(o);
}

// GATT write on SPK (host task): one 133-byte block
void speak_feed(const uint8_t *blk, int len)
{
    if (!s_active || len != ADPCM_BLOCK_BYTES || !s_sb) return;
    s_rx++;
    xStreamBufferSend(s_sb, blk, len, 0);          // full (phone too far ahead): the block is dropped and counted lost
}

static void player_task(void *a)
{
    EXT_RAM_BSS_ATTR static uint8_t blk[ADPCM_BLOCK_BYTES];
    EXT_RAM_BSS_ATTR static int16_t pcm[ADPCM_BLOCK_SAMPLES];
    int played = 0, lost = 0, expect = -1;
    int64_t t_last = esp_timer_get_time();
    // pre-buffer ~200 ms (12 blocks) so a late Bluetooth event does not cut words
    while (!s_stop && !s_end && xStreamBufferBytesAvailable(s_sb) < 12 * ADPCM_BLOCK_BYTES && esp_timer_get_time() - t_last < 3000000)
        vTaskDelay(pdMS_TO_TICKS(20));
    while (!s_stop) {
        size_t n = xStreamBufferReceive(s_sb, blk, ADPCM_BLOCK_BYTES, pdMS_TO_TICKS(100));
        if (n != ADPCM_BLOCK_BYTES) {
            if (s_end && xStreamBufferBytesAvailable(s_sb) == 0) break;           // played out
            if (esp_timer_get_time() - t_last > 3000000) break;                     // phone went quiet
            continue;
        }
        t_last = esp_timer_get_time();
        if (expect >= 0 && blk[0] != (uint8_t)expect) lost += (uint8_t)(blk[0] - expect);
        expect = (uint8_t)(blk[0] + 1);
        adpcm_decode_block(blk, pcm);
        hw_spk_write(pcm, ADPCM_BLOCK_SAMPLES);    // blocks ~16 ms: paces the loop at real time
        played++;
    }
    for (int i = 0; i < 8; i++) { memset(pcm, 0, sizeof(pcm)); hw_spk_write(pcm, ADPCM_BLOCK_SAMPLES); }   // no click at the end
    hw_spk_close();
    s_active = false;
    reply(s_stop ? "say_done" : "say_done", NULL, played, lost);
    diag_event("SAY end played=%d lost=%d stopped=%d", played, lost, s_stop);
    ble_link_slow_later(3000);
    vTaskDelete(NULL);
}

// CMD "say": open the speaker if allowed and tell the phone to start streaming
void speak_begin(const char *id, int n)
{
    if (s_active) { speak_stop(); for (int i = 0; i < 50 && s_active; i++) vTaskDelay(pdMS_TO_TICKS(20)); }
    snprintf(s_id, sizeof(s_id), "%s", id ? id : "");
    const char *why = NULL;
    if (!hw_pref_get("speak", 1)) why = "off";
    else if (rec_active()) why = "recording";
    else if (link_ptt_active() || hw_mic_owner(NULL, 0, NULL, 0, NULL, NULL)) why = "busy";
    if (!why && !hw_spk_open(16000, hw_pref_get("vol", 70))) why = "busy";
    if (why) { reply("say_skip", why, -1, 0); diag_event("SAY skip %s", why); return; }
    if (!s_sb) s_sb = xStreamBufferCreateWithCaps(RING_BLOCKS * ADPCM_BLOCK_BYTES, ADPCM_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    else xStreamBufferReset(s_sb);
    s_end = s_stop = false; s_rx = 0;
    s_active = true;
    ble_link_fast(true);
    xTaskCreate(player_task, "say", 3584, NULL, 5, NULL);
    reply("say_ready", NULL, -1, 0);
    diag_event("SAY begin n=%d", n);
}

void speak_end(void) { s_end = true; }

// Background recorder service to SD (or internal flash if no card):
//  * meeting recordings (16 kHz voice): IMA-ADPCM WAV (format 0x11, 4 bits/sample) = 4x smaller, ~8 KB/s
//  * music recordings (44.1 kHz): plain 16-bit PCM WAV (full quality for the guitar)
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app.h"
#include "diag.h"
#include "hw.h"
#include "adpcm.h"

static const char *TAG = "rec";
static volatile bool s_active, s_stop, s_done = true;
static volatile int s_level;
static volatile rec_mode_t s_mode;
static uint32_t s_t0, s_rate;
static char s_file[112];

#define SBUF (384 * 1024)          // PSRAM cushion between capture and storage writes
#define WBLK (16 * 1024)

static StreamBufferHandle_t s_sb;
static volatile bool s_cap_done;
static volatile uint32_t s_dropped, s_frames;
static int64_t s_cap_t0;

// ---- bookmarks: tap "Mark" (or the link) during a recording; saved as <file>.marks (one ms offset per line) ----
#define MAX_MARKS 64
static uint32_t s_marks[MAX_MARKS];
static int s_nmarks;
void rec_mark(void)
{
    if (!s_active || s_nmarks >= MAX_MARKS) return;
    s_marks[s_nmarks++] = (uint32_t)((esp_timer_get_time() - s_cap_t0) / 1000);
    hw_buzz(40);
}
static void save_marks(void)
{
    if (!s_nmarks) return;
    char p[128]; snprintf(p, sizeof(p), "%.*s.marks", (int)(strlen(s_file) - 4), s_file);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < s_nmarks; i++) fprintf(f, "%lu\n", (unsigned long)s_marks[i]);
    fclose(f);
}
int rec_marks_for(const char *wav_name, uint32_t *out, int max)
{
    char p[128]; snprintf(p, sizeof(p), "%s/rec/%.*s.marks", hw_storage_root(), (int)(strlen(wav_name) > 4 ? strlen(wav_name) - 4 : 0), wav_name);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0; unsigned long v;
    while (n < max && fscanf(f, "%lu", &v) == 1) out[n++] = v;
    fclose(f);
    return n;
}
void rec_stats(uint32_t *frames, uint32_t *dropped, uint32_t *rate) { *frames = s_frames; *dropped = s_dropped; *rate = s_rate; }

bool rec_active(void) { return s_active; }
rec_mode_t rec_mode(void) { return s_mode; }
int rec_level(void) { return s_level; }
const char *rec_last_file(void) { return s_file; }
uint32_t rec_seconds(void) { return s_active ? lv_tick_elaps(s_t0) / 1000 : 0; }

static void wav_header(FILE *f, uint32_t rate, uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);        // PCM
    s = 1; memcpy(h + 22, &s, 2);                 // mono
    memcpy(h + 24, &rate, 4);
    v = rate * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

// IMA-ADPCM WAV header (60 bytes): fmt (20 bytes, cbSize 2 + samplesPerBlock) + fact (sample count) + data
static void ima_header(FILE *f, uint32_t rate, uint32_t samples, uint32_t data_bytes)
{
    uint8_t h[60];
    uint32_t v; uint16_t s;
    memcpy(h, "RIFF", 4); v = 52 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 20; memcpy(h + 16, &v, 4);
    s = 0x11; memcpy(h + 20, &s, 2);                                   // IMA ADPCM
    s = 1; memcpy(h + 22, &s, 2);                                      // mono
    memcpy(h + 24, &rate, 4);
    v = rate * IMA_WAV_BLOCK_BYTES / IMA_WAV_BLOCK_SAMPLES; memcpy(h + 28, &v, 4);
    s = IMA_WAV_BLOCK_BYTES; memcpy(h + 32, &s, 2);                    // block align
    s = 4; memcpy(h + 34, &s, 2);                                      // bits per sample
    s = 2; memcpy(h + 36, &s, 2);                                      // cbSize
    s = IMA_WAV_BLOCK_SAMPLES; memcpy(h + 38, &s, 2);                  // samples per block
    memcpy(h + 40, "fact", 4); v = 4; memcpy(h + 44, &v, 4); memcpy(h + 48, &samples, 4);
    memcpy(h + 52, "data", 4); memcpy(h + 56, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 60, f);
    fseek(f, 0, SEEK_END);
}

// Writer: drains the stream buffer to the file in 16 KB blocks (SD / flash writes can stall 100+ ms).
static void writer_task(void *arg)
{
    uint8_t *blk = heap_caps_malloc(WBLK, MALLOC_CAP_SPIRAM);
    FILE *f = fopen(s_file, "wb");
    bool err = (!blk || !f);
    bool ima = (s_mode == REC_MEETING);
    uint32_t total = 0, last_patch = 0, samples = 0;
    // IMA: collect 505 samples (as bytes: the stream buffer can split a sample) -> one 256-byte block
    EXT_RAM_BSS_ATTR static uint8_t pend[IMA_WAV_BLOCK_SAMPLES * 2];
    uint8_t out[IMA_WAV_BLOCK_BYTES];
    int pn = 0, idx = 0;
    if (!err) { if (ima) ima_header(f, s_rate, 0, 0); else wav_header(f, s_rate, 0); }
    else ESP_LOGE(TAG, "open failed: %s", s_file);
    while (1) {
        size_t n = xStreamBufferReceive(s_sb, blk, WBLK, pdMS_TO_TICKS(100));
        bool last = (n == 0 && s_cap_done);
        if (n && !err && !ima) {
            if (fwrite(blk, 1, n, f) != n) { err = true; s_stop = true; ESP_LOGE(TAG, "write error (disk full?)"); }
            total += n;
        }
        if (!err && ima) {
            size_t i = 0;
            while (i < n || (last && pn > 0)) {
                if (i < n) { size_t take = sizeof(pend) - pn; if (take > n - i) take = n - i; memcpy(pend + pn, blk + i, take); pn += take; i += take; }
                if (pn < (int)sizeof(pend) && !last) break;
                if (pn < (int)sizeof(pend)) {                    // final partial block: pad by repeating the last sample
                    int have = pn / 2;
                    int16_t lastv = have ? ((int16_t *)pend)[have - 1] : 0;
                    for (int k = have; k < IMA_WAV_BLOCK_SAMPLES; k++) ((int16_t *)pend)[k] = lastv;
                    samples += have;
                } else samples += IMA_WAV_BLOCK_SAMPLES;
                ima_wav_encode_block((const int16_t *)pend, IMA_WAV_BLOCK_SAMPLES, &idx, out);
                pn = 0;
                if (fwrite(out, 1, sizeof(out), f) != sizeof(out)) { err = true; s_stop = true; ESP_LOGE(TAG, "write error (disk full?)"); break; }
                total += sizeof(out);
                if (last) break;
            }
        }
        if (!err && total - last_patch > (ima ? (uint32_t)s_rate / 2 : (uint32_t)s_rate * 2) * 20) {   // ~20 s: crash-safe header
            last_patch = total;
            if (ima) ima_header(f, s_rate, samples, total); else wav_header(f, s_rate, total);
            fflush(f);
        }
        if (last) break;
    }
    if (f) { if (ima) ima_header(f, s_rate, samples, total); else wav_header(f, s_rate, total); fclose(f); }
    diag_event("REC saved %s bytes=%lu dropped=%lu err=%d", s_file, (unsigned long)total, (unsigned long)s_dropped, err);
    save_marks();
    free(blk);
    int64_t ms = (esp_timer_get_time() - s_cap_t0) / 1000;
    ESP_LOGI(TAG, "saved %s (%lu bytes)  captured %lu frames in %lld ms = %.0f Hz (asked %lu)  dropped=%lu",
             s_file, (unsigned long)total, (unsigned long)s_frames, (long long)ms, ms ? s_frames * 1000.0 / ms : 0,
             (unsigned long)s_rate, (unsigned long)s_dropped);
    vStreamBufferDelete(s_sb);
    s_sb = NULL;
    s_active = false;
    s_done = true;
    vTaskDelete(NULL);
}

// Capture: reads the mic continuously (never blocks on storage) and feeds the stream buffer.
static volatile bool s_pause_req, s_paused;
static bool mic_open_for_rec(void)
{
    return hw_mic_open_as(s_rate, s_mode == REC_MUSIC ? 18.0f : 30.0f, "Recorder", s_mode == REC_MUSIC ? "music recording" : "meeting recording");
}
static void rec_task(void *arg)
{
    EXT_RAM_BSS_ATTR static int16_t pcm[1024];
    s_cap_t0 = esp_timer_get_time();
    while (!s_stop) {
        // pause: hand the mic to someone else (Amber) without closing the file; resume reopens it
        if (s_pause_req && !s_paused) { hw_mic_close(); s_level = 0; s_paused = true; }
        if (s_paused) {
            if (!s_pause_req && mic_open_for_rec()) { s_paused = false; rec_mark(); }   // a bookmark where it resumed
            else { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        }
        int n = hw_mic_read_mono(pcm, 1024);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        s_frames += n;
        double e = 0;
        for (int i = 0; i < n; i++) e += (double)pcm[i] * pcm[i];
        float db = 20.0f * log10f((float)sqrt(e / n) / 32768.0f + 1e-6f);
        int lv = (int)((db + 60.0f) * (100.0f / 60.0f));
        s_level = lv < 0 ? 0 : (lv > 100 ? 100 : lv);
        if (xStreamBufferSend(s_sb, pcm, n * 2, 0) != (size_t)(n * 2)) s_dropped += n;
    }
    if (!s_paused) hw_mic_close();
    s_paused = s_pause_req = false;
    s_level = 0;
    s_cap_done = true;
    vTaskDelete(NULL);
}

bool rec_pause(bool on)
{
    if (!s_active) return false;
    s_pause_req = on;
    for (int i = 0; i < 100 && s_paused != on; i++) vTaskDelay(pdMS_TO_TICKS(10));
    return s_paused == on;
}
bool rec_paused(void) { return s_active && s_paused; }

bool rec_start(rec_mode_t m)
{
    if (s_active || !s_done) return false;
    uint64_t tot, fre;
    hw_storage_info(&tot, &fre);
    if (fre < 3ULL * 1024 * 1024) { ESP_LOGE(TAG, "storage full"); return false; }
    s_rate = (m == REC_MUSIC) ? 44100 : 16000;
    s_mode = m;
    s_pause_req = s_paused = false;
    if (!mic_open_for_rec()) return false;

    char dir[40];
    snprintf(dir, sizeof(dir), "%s/rec", hw_storage_root());
    mkdir(dir, 0777);
    struct tm t;
    if (!hw_rtc_get(&t)) { memset(&t, 0, sizeof(t)); t.tm_year = 100; t.tm_mon = 0; t.tm_mday = 1; }
    snprintf(s_file, sizeof(s_file), "%s/%c_%04d%02d%02d_%02d%02d%02d.wav", dir, m == REC_MUSIC ? 'G' : 'M',
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    s_sb = xStreamBufferCreateWithCaps(SBUF, 1, MALLOC_CAP_SPIRAM);
    if (!s_sb) { hw_mic_close(); return false; }
    s_mode = m;
    s_stop = false; s_cap_done = false; s_done = false; s_active = true;
    s_frames = 0; s_dropped = 0; s_nmarks = 0;
    s_t0 = lv_tick_get();
    xTaskCreate(writer_task, "recw", 6144, NULL, 3, NULL);
    xTaskCreate(rec_task, "rec", 6144, NULL, 5, NULL);
    return true;
}

void rec_stop(void)
{
    if (!s_active) return;
    s_stop = true;
    for (int i = 0; i < 500 && !s_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

bool rec_toggle(rec_mode_t m)
{
    if (s_active) { rec_stop(); return s_done; }
    return rec_start(m);
}

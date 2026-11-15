// Guitar tuner: YIN pitch detection on the mic, nearest-string guidance.
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "hw.h"

#define RATE 16000
#define W 1024            // integration window
#define TAU_MAX 420       // lowest detectable ~38 Hz
#define TAU_MIN 36        // highest ~444 Hz
#define BUF (W + TAU_MAX + 1)

static const struct { const char *name; float hz; } STRINGS[6] = {
    {"E2  low E", 82.41f}, {"A2", 110.00f}, {"D3", 146.83f}, {"G3", 196.00f}, {"B3", 246.94f}, {"E4  high e", 329.63f}};
static const char *NOTES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static volatile bool s_run, s_exit;
static volatile float s_freq;          // 0 = nothing detected
static volatile int s_rms;
static TaskHandle_t s_task;
static lv_obj_t *s_note, *s_cents, *s_hz, *s_str, *s_bar, *s_hint;
static lv_timer_t *s_tm;
static int s_ref = 440;                // A4 reference (pref "aref"): 440 standard, 432 for the owner's solo practice
static lv_obj_t *s_ref_lbl;
static void ref_cb(lv_event_t *e)
{
    s_ref = s_ref == 440 ? 432 : 440;
    hw_pref_set("aref", s_ref);
    char b[16]; snprintf(b, sizeof(b), "A=%d", s_ref);
    lv_label_set_text(s_ref_lbl, b);
}

// returns frequency or 0
static float yin(const float *x, float *conf)
{
    EXT_RAM_BSS_ATTR static float d[TAU_MAX + 1];
    float run = 0;
    d[0] = 1;
    for (int tau = 1; tau <= TAU_MAX; tau++) {
        float s = 0;
        for (int j = 0; j < W; j++) { float t = x[j] - x[j + tau]; s += t * t; }
        d[tau] = s;
    }
    for (int tau = 1; tau <= TAU_MAX; tau++) {
        run += d[tau];
        d[tau] = run > 0 ? d[tau] * tau / run : 1;
    }
    int best = -1;
    for (int tau = TAU_MIN; tau < TAU_MAX; tau++) {
        if (d[tau] < 0.15f) {
            while (tau + 1 < TAU_MAX && d[tau + 1] < d[tau]) tau++;
            best = tau;
            break;
        }
    }
    if (best < 0) return 0;
    float a = d[best - 1], b = d[best], c = d[best + 1];
    float den = a - 2 * b + c;
    float shift = (fabsf(den) > 1e-9f) ? 0.5f * (a - c) / den : 0;
    *conf = 1.0f - b;
    return RATE / (best + shift);
}

static void tuner_task(void *arg)
{
    EXT_RAM_BSS_ATTR static float x[BUF];
    EXT_RAM_BSS_ATTR static int16_t pcm[512];
    EXT_RAM_BSS_ATTR static int16_t ring[BUF];
    int filled = 0;
    if (!hw_mic_open_as(RATE, 24.0f, "Tuner", "listening for notes")) { s_task = NULL; s_exit = true; vTaskDelete(NULL); }
    float hist[5] = {0};
    int hi = 0;
    while (s_run) {
        int n = hw_mic_read_mono(pcm, 512);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        memmove(ring, ring + n, (BUF - n) * sizeof(int16_t));
        memcpy(ring + BUF - n, pcm, n * sizeof(int16_t));
        if (filled < BUF) { filled += n; continue; }
        double e = 0;
        for (int i = BUF - W; i < BUF; i++) e += (double)ring[i] * ring[i];
        int rms = (int)sqrt(e / W);
        s_rms = rms;
        if (rms < 120) { s_freq = 0; continue; }
        for (int i = 0; i < BUF; i++) x[i] = ring[i] / 32768.0f;
        float conf = 0;
        float f = yin(x, &conf);
        if (f > 0 && conf > 0.6f) {
            hist[hi++ % 5] = f;
            float s[5]; memcpy(s, hist, sizeof(s));
            for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) if (s[j] < s[i]) { float t = s[i]; s[i] = s[j]; s[j] = t; }
            s_freq = s[2] > 0 ? s[2] : f;     // median of last 5
        } else s_freq = 0;
    }
    hw_mic_close();
    s_task = NULL;
    s_exit = true;
    vTaskDelete(NULL);
}

static void refresh(lv_timer_t *t)
{
    static float last_f; static uint32_t last_t;
    float f = s_freq;
    if (f > 0) { last_f = f; last_t = lv_tick_get(); }
    else if (lv_tick_elaps(last_t) > 900) last_f = 0;
    f = last_f;
    if (f <= 0) {
        lv_label_set_text(s_note, "--");
        lv_label_set_text(s_cents, "play a string");
        lv_label_set_text(s_hz, "");
        lv_label_set_text(s_str, "");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_DIM), LV_PART_INDICATOR);
        return;
    }
    float ref = (float)s_ref, k = ref / 440.0f;            // A4 = 440 or 432 Hz: every target scales with it
    float midi = 69.0f + 12.0f * log2f(f / ref);
    int n = (int)lroundf(midi);
    float cents = (midi - n) * 100.0f;
    // nearest guitar string
    int bi = 0; float bd = 1e9f;
    for (int i = 0; i < 6; i++) { float dd = fabsf(1200.0f * log2f(f / (STRINGS[i].hz * k))); if (dd < bd) { bd = dd; bi = i; } }
    float sc = 1200.0f * log2f(f / (STRINGS[bi].hz * k));
    // haptic: one buzz once a string has stayed within +-8 cents for 0.4 s (feel it while looking at the headstock)
    static int buzzed = -1; static uint32_t in_since;
    if (bd < 150 && fabsf(sc) <= 8) {
        if (!in_since) in_since = lv_tick_get() | 1;
        if (lv_tick_elaps(in_since) >= 400 && buzzed != bi) { hw_buzz(70); buzzed = bi; }
    } else { in_since = 0; if (fabsf(sc) > 20) buzzed = -1; }
    char b[48];
    snprintf(b, sizeof(b), "%s%d", NOTES[((n % 12) + 12) % 12], n / 12 - 1);
    lv_label_set_text(s_note, b);
    snprintf(b, sizeof(b), "%+.0f cents", cents);
    lv_label_set_text(s_cents, b);
    snprintf(b, sizeof(b), "%.1f Hz", f);
    lv_label_set_text(s_hz, b);
    if (bd < 150) {
        snprintf(b, sizeof(b), "%s  %s", STRINGS[bi].name, sc > 8 ? "too high  " LV_SYMBOL_DOWN : (sc < -8 ? "too low  " LV_SYMBOL_UP : "IN TUNE"));
        lv_label_set_text(s_str, b);
        int v = (int)sc; if (v > 50) v = 50; if (v < -50) v = -50;
        lv_bar_set_value(s_bar, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(fabsf(sc) <= 8 ? C_OK : (fabsf(sc) < 25 ? 0xFFC857 : C_WARN)), LV_PART_INDICATOR);
    } else lv_label_set_text(s_str, "");
}

static void start(lv_obj_t *scr)
{
    ui_title(scr, "Tuner");
    if (rec_active()) {
        ui_label(scr, &lv_font_montserrat_20, C_WARN, "Recording in progress.\nStop it to use the tuner.");
        lv_obj_t *l = lv_obj_get_child(scr, -1); lv_obj_center(l);
        s_tm = NULL;
        return;
    }
    s_ref = hw_pref_get("aref", 440) == 432 ? 432 : 440;
    char rb[16]; snprintf(rb, sizeof(rb), "A=%d", s_ref);
    lv_obj_t *rbtn = ui_btn(scr, rb, 90, 40, 0x333333, ref_cb, NULL);
    lv_obj_align(rbtn, LV_ALIGN_TOP_RIGHT, -24, 62);
    s_ref_lbl = lv_obj_get_child(rbtn, 0);
    s_note = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 100);
    s_cents = ui_label(scr, &lv_font_montserrat_28, C_ACCENT, "");
    lv_obj_align(s_cents, LV_ALIGN_TOP_MID, 0, 205);
    s_hz = ui_label(scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_hz, LV_ALIGN_TOP_MID, 0, 250);
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 340, 24);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 310);
    lv_bar_set_mode(s_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(s_bar, -50, 50);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x202020), 0);
    s_str = ui_label(scr, &lv_font_montserrat_28, C_TEXT, "");
    lv_obj_align(s_str, LV_ALIGN_TOP_MID, 0, 365);
    s_hint = ui_label(scr, &lv_font_montserrat_14, C_DIM, "pluck one string - buzz = in tune");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    s_run = true; s_exit = false;
    xTaskCreate(tuner_task, "tuner", 8192, NULL, 4, &s_task);
    s_tm = lv_timer_create(refresh, 80, NULL);
}

static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    if (s_task) {
        s_run = false;
        for (int i = 0; i < 100 && !s_exit; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_tuner_start(lv_obj_t *s) { start(s); }
void app_tuner_stop(void) { stop(); }

// Self-test helper: synthesise a plucked-string-like tone (fundamental + harmonics) and run YIN on it.
float tuner_selftest(float hz)
{
    EXT_RAM_BSS_ATTR static float x[BUF];
    for (int i = 0; i < BUF; i++) {
        float t = (float)i / RATE;
        x[i] = 0.30f * sinf(2 * 3.14159265f * hz * t) + 0.20f * sinf(2 * 3.14159265f * 2 * hz * t) + 0.12f * sinf(2 * 3.14159265f * 3 * hz * t);
    }
    float conf = 0;
    return yin(x, &conf);
}

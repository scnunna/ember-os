// Sound meter: level (dBFS) + live 24-band spectrum from the mic.
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_dsp.h"
#include "app.h"
#include "hw.h"

#define RATE 16000
#define N 512
#define BANDS 24

static volatile bool s_run, s_exit = true;
static volatile int s_band[BANDS];
static volatile float s_db = -90;
static lv_obj_t *s_db_lbl, *s_chart, *s_peak_lbl;
static lv_chart_series_t *s_ser;
static lv_timer_t *s_tm;
static float s_peak = -90;
static bool s_fft_ready;

static void snd_task(void *arg)
{
    EXT_RAM_BSS_ATTR static int16_t pcm[N];
    EXT_RAM_BSS_ATTR static float wind[N], fft[2 * N];
    if (!s_fft_ready) {
        s_fft_ready = (dsps_fft2r_init_fc32(NULL, N) == ESP_OK);
        dsps_wind_hann_f32(wind, N);
    }
    if (!hw_mic_open_as(RATE, 24.0f, "Sound", "measuring loudness")) { s_run = false; s_exit = true; vTaskDelete(NULL); }
    uint32_t mic = hw_mic_session();
    while (s_run) {
        if (hw_mic_read_mono(pcm, N) != N) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        double e = 0;
        for (int i = 0; i < N; i++) { e += (double)pcm[i] * pcm[i]; fft[2 * i] = pcm[i] / 32768.0f * wind[i]; fft[2 * i + 1] = 0; }
        s_db = 20.0f * log10f((float)sqrt(e / N) / 32768.0f + 1e-6f);
        if (s_fft_ready) {
            dsps_fft2r_fc32(fft, N);
            dsps_bit_rev_fc32(fft, N);
            // log-spaced bands 80 Hz .. 7.5 kHz  (bin width = 31.25 Hz)
            for (int b = 0; b < BANDS; b++) {
                float lo = 80.0f * powf(7500.0f / 80.0f, (float)b / BANDS);
                float hi = 80.0f * powf(7500.0f / 80.0f, (float)(b + 1) / BANDS);
                int k0 = (int)(lo / 31.25f), k1 = (int)(hi / 31.25f);
                if (k1 <= k0) k1 = k0 + 1;
                float m = 0;
                for (int k = k0; k < k1 && k < N / 2; k++) {
                    float p = fft[2 * k] * fft[2 * k] + fft[2 * k + 1] * fft[2 * k + 1];
                    if (p > m) m = p;
                }
                float db = 10.0f * log10f(m + 1e-12f);       // about -100 .. 0
                int v = (int)((db + 52.0f) * (100.0f / 80.0f));   // noise floor ~ -50 dB per bin, full-scale tone ~ +40
                s_band[b] = v < 0 ? 0 : (v > 100 ? 100 : v);
            }
        }
    }
    hw_mic_close_session(mic);
    s_exit = true;
    vTaskDelete(NULL);
}

static void refresh(lv_timer_t *t)
{
    char b[40];
    float db = s_db;
    if (db > s_peak) s_peak = db; else s_peak -= 0.15f;
    snprintf(b, sizeof(b), "%.0f dB", db);
    lv_label_set_text(s_db_lbl, b);
    lv_obj_set_style_text_color(s_db_lbl, lv_color_hex(db > -12 ? C_WARN : (db > -30 ? 0xFFC857 : C_OK)), 0);
    snprintf(b, sizeof(b), "peak %.0f dB   (0 dB = full scale)", s_peak);
    lv_label_set_text(s_peak_lbl, b);
    for (int i = 0; i < BANDS; i++) lv_chart_set_series_value_by_id(s_chart, s_ser, i, s_band[i]);
    lv_chart_refresh(s_chart);
}

static void start(lv_obj_t *scr)
{
    ui_title(scr, "Sound");
    if (rec_active()) {
        lv_obj_t *l = ui_label(scr, &lv_font_montserrat_20, C_WARN, "Recording in progress.\nStop it to use this.");
        lv_obj_center(l);
        s_tm = NULL;
        return;
    }
    s_db_lbl = ui_label(scr, &lv_font_montserrat_48, C_OK, "-- dB");
    lv_obj_align(s_db_lbl, LV_ALIGN_TOP_MID, 0, 90);
    s_peak_lbl = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_peak_lbl, LV_ALIGN_TOP_MID, 0, 160);
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 380, 230);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, BANDS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_column(s_chart, 2, 0);
    s_ser = lv_chart_add_series(s_chart, lv_color_hex(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_14, C_DIM, "80 Hz                        spectrum                        7.5 kHz");
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -24);
    s_run = true; s_exit = false;
    s_peak = -90;
    xTaskCreate(snd_task, "snd", 8192, NULL, 4, NULL);
    s_tm = lv_timer_create(refresh, 60, NULL);
}
static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    s_run = false;
    for (int i = 0; i < 100 && !s_exit; i++) vTaskDelay(pdMS_TO_TICKS(10));
}
void app_sound_start(lv_obj_t *s) { start(s); }
void app_sound_stop(void) { stop(); }

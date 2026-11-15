// Metronome: sample-accurate clicks through the speaker (timing comes from the I2S clock).
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "hw.h"
#include "practice.h"
#include "esp_timer.h"

#define RATE 16000
static volatile bool s_run, s_exit = true;
static volatile int s_bpm = 100, s_beat = -1;
static lv_obj_t *s_bpm_lbl, *s_go_lbl, *s_dots[4];
static lv_timer_t *s_tm;
static int s_shown = -2;
// practice extras: vibrating beat (silent practice with headphones/amp), speed trainer (+N BPM every 4 bars), session log
static volatile int s_vibe, s_speed, s_bpm_max;
static int64_t s_t_start;
static lv_obj_t *s_vibe_lbl, *s_speed_lbl;
static const int SPEED_STEPS[4] = {0, 2, 5, 10};

static void click(int16_t *b, int n, int hz)
{
    for (int i = 0; i < n; i++) {
        float env = expf(-i / (RATE * 0.012f));
        b[i] = (int16_t)(sinf(2 * 3.14159265f * hz * i / RATE) * 24000 * env);
    }
}

static void metro_task(void *arg)
{
    EXT_RAM_BSS_ATTR static int16_t hi[640], lo[640], chunk[320];
    click(hi, 640, 1900);
    click(lo, 640, 1200);
    if (!hw_spk_open(RATE, 85)) { s_run = false; s_exit = true; vTaskDelete(NULL); }
    int beat = 0, beats = 0;
    while (s_run) {
        s_beat = beat;
        if (s_vibe) hw_buzz(beat == 0 ? 45 : 22);
        if (s_speed && beats > 0 && beats % 16 == 0 && s_bpm < 240) s_bpm = s_bpm + s_speed > 240 ? 240 : s_bpm + s_speed;
        if (s_bpm > s_bpm_max) s_bpm_max = s_bpm;
        beats++;
        int period = RATE * 60 / s_bpm;
        int played = 0;
        const int16_t *c = beat == 0 ? hi : lo;
        while (played < period && s_run) {
            int n = period - played > 320 ? 320 : period - played;
            memset(chunk, 0, sizeof(chunk));
            if (played < 640) { int m = 640 - played; if (m > n) m = n; memcpy(chunk, c + played, m * 2); }
            hw_spk_write(chunk, n);
            played += n;
        }
        beat = (beat + 1) % 4;
    }
    memset(chunk, 0, sizeof(chunk));
    hw_spk_write(chunk, 320);
    hw_spk_close();
    s_beat = -1;
    s_exit = true;
    vTaskDelete(NULL);
}

static void refresh(lv_timer_t *t)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", s_bpm);
    lv_label_set_text(s_bpm_lbl, b);
    lv_label_set_text(s_go_lbl, s_run ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    int cur = s_run ? s_beat : -1;
    if (cur != s_shown) {
        s_shown = cur;
        for (int i = 0; i < 4; i++) {
            bool on = (i == cur);
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(on ? (i == 0 ? C_WARN : C_ACCENT) : 0x2A2A2A), 0);
        }
    }
}
static void bpm_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)ui_ud(e);
    int v = s_bpm + d;
    s_bpm = v < 30 ? 30 : (v > 240 ? 240 : v);
    hw_pref_set("bpm", s_bpm);
    refresh(NULL);
}
static void log_session(void)
{
    if (!s_t_start) return;
    int mins = (int)((esp_timer_get_time() - s_t_start) / 60000000LL);
    s_t_start = 0;
    if (mins >= 1) practice_log(s_speed ? "speed" : "metronome", mins, s_bpm_max, 0);
}
static const char *vibe_txt(void) { return s_vibe ? "Vibe ON" : "Vibe OFF"; }
static const char *speed_txt(void)
{
    static char b[20];
    if (s_speed) snprintf(b, sizeof(b), "Speed +%d", s_speed); else snprintf(b, sizeof(b), "Speed OFF");
    return b;
}
static void vibe_cb(lv_event_t *e) { s_vibe = !s_vibe; hw_pref_set("mvibe", s_vibe); lv_label_set_text(s_vibe_lbl, vibe_txt()); }
static void speed_cb(lv_event_t *e)
{
    int i = 0;
    while (i < 3 && SPEED_STEPS[i] != s_speed) i++;
    s_speed = SPEED_STEPS[(i + 1) % 4];
    hw_pref_set("mspeed", s_speed);
    lv_label_set_text(s_speed_lbl, speed_txt());
}
static void go_cb(lv_event_t *e)
{
    if (s_run) { s_run = false; log_session(); return; }
    if (!s_exit) return;
    s_t_start = esp_timer_get_time(); s_bpm_max = s_bpm;
    s_run = true; s_exit = false;
    xTaskCreate(metro_task, "metro", 4096, NULL, 4, NULL);
}

static void add_bpm_btn(lv_obj_t *scr, const char *txt, int d, lv_align_t al, int x)
{
    lv_obj_t *b = ui_btn(scr, txt, 84, 58, 0x333333, bpm_cb, (void *)(intptr_t)d);
    lv_obj_align(b, al, x, 290);
}

static void start(lv_obj_t *scr)
{
    s_bpm = hw_pref_get("bpm", 100);
    s_vibe = hw_pref_get("mvibe", 0);
    s_speed = hw_pref_get("mspeed", 0);
    s_shown = -2;
    ui_title(scr, "Metronome");
    lv_obj_t *vb = ui_btn(scr, vibe_txt(), 110, 38, 0x333333, vibe_cb, NULL);
    lv_obj_align(vb, LV_ALIGN_TOP_LEFT, 18, 64);
    s_vibe_lbl = lv_obj_get_child(vb, 0);
    lv_obj_set_style_text_font(s_vibe_lbl, &lv_font_montserrat_14, 0);
    lv_obj_t *sb = ui_btn(scr, speed_txt(), 110, 38, 0x333333, speed_cb, NULL);
    lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -18, 64);
    s_speed_lbl = lv_obj_get_child(sb, 0);
    lv_obj_set_style_text_font(s_speed_lbl, &lv_font_montserrat_14, 0);
    s_bpm_lbl = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "100");
    lv_obj_align(s_bpm_lbl, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *u = ui_label(scr, &lv_font_montserrat_20, C_DIM, "BPM");
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 172);
    for (int i = 0; i < 4; i++) {
        s_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(s_dots[i], 40, 40);
        lv_obj_set_style_radius(s_dots[i], 20, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s_dots[i], LV_ALIGN_TOP_MID, (i - 1) * 70 - 35, 225);
    }
    add_bpm_btn(scr, "-10", -10, LV_ALIGN_TOP_LEFT, 16);
    add_bpm_btn(scr, "-1", -1, LV_ALIGN_TOP_LEFT, 108);
    add_bpm_btn(scr, "+1", 1, LV_ALIGN_TOP_RIGHT, -108);
    add_bpm_btn(scr, "+10", 10, LV_ALIGN_TOP_RIGHT, -16);

    lv_obj_t *g = ui_btn(scr, "", 160, 90, C_ACCENT, go_cb, NULL);
    lv_obj_align(g, LV_ALIGN_BOTTOM_MID, 0, -36);
    s_go_lbl = lv_obj_get_child(g, 0);
    lv_obj_set_style_text_font(s_go_lbl, &lv_font_montserrat_48, 0);
    s_tm = lv_timer_create(refresh, 40, NULL);
    refresh(NULL);
}
static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    s_run = false;
    log_session();
    for (int i = 0; i < 100 && !s_exit; i++) vTaskDelay(pdMS_TO_TICKS(10));
}
void app_metro_start(lv_obj_t *s) { start(s); }
void app_metro_stop(void) { stop(); }

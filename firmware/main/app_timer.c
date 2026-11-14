// Timer + stopwatch. The countdown keeps running (and beeps) even if you leave the app.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "app.h"
#include "hw.h"

static int64_t s_end_ms;                    // countdown end (esp_timer ms)
static int64_t s_left_ms = 5 * 60 * 1000;   // when paused/idle
static bool s_cd_run, s_sw_run, s_alarm;
static int64_t s_sw_start, s_sw_acc;
static int s_mode;                          // 0 countdown, 1 stopwatch
static bool s_task_up;

static lv_obj_t *s_big, *s_go_lbl, *s_mode_lbl, *s_row_cd;
static lv_timer_t *s_tm;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void bg_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_cd_run && now_ms() >= s_end_ms) {
            s_cd_run = false; s_left_ms = 0; s_alarm = true;
            for (int i = 0; i < 4 && s_alarm; i++) { hw_beep(1500, 250, 90); vTaskDelay(pdMS_TO_TICKS(250)); }
            s_alarm = false;
        }
    }
}

static void fmt(char *b, size_t n, int64_t ms, bool tenths)
{
    if (ms < 0) ms = 0;
    int s = (int)(ms / 1000);
    if (tenths) snprintf(b, n, "%02d:%02d.%d", s / 60, s % 60, (int)(ms % 1000) / 100);
    else snprintf(b, n, "%02d:%02d", s / 60, s % 60);
}
static void refresh(lv_timer_t *t)
{
    char b[24];
    if (s_mode == 0) {
        int64_t left = s_cd_run ? s_end_ms - now_ms() : s_left_ms;
        fmt(b, sizeof(b), left, false);
        lv_label_set_text(s_go_lbl, s_cd_run ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_label_set_text(s_mode_lbl, "COUNTDOWN  -  tap for stopwatch");
        lv_obj_remove_flag(s_row_cd, LV_OBJ_FLAG_HIDDEN);
    } else {
        int64_t e = s_sw_acc + (s_sw_run ? now_ms() - s_sw_start : 0);
        fmt(b, sizeof(b), e, true);
        lv_label_set_text(s_go_lbl, s_sw_run ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_label_set_text(s_mode_lbl, "STOPWATCH  -  tap for countdown");
        lv_obj_add_flag(s_row_cd, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_big, b);
}
static void mode_cb(lv_event_t *e) { s_mode = !s_mode; refresh(NULL); }
static void go_cb(lv_event_t *e)
{
    s_alarm = false;
    if (s_mode == 0) {
        if (s_cd_run) { s_left_ms = s_end_ms - now_ms(); s_cd_run = false; }
        else if (s_left_ms > 0) { s_end_ms = now_ms() + s_left_ms; s_cd_run = true; }
    } else {
        if (s_sw_run) { s_sw_acc += now_ms() - s_sw_start; s_sw_run = false; }
        else { s_sw_start = now_ms(); s_sw_run = true; }
    }
    refresh(NULL);
}
static void reset_cb(lv_event_t *e)
{
    s_alarm = false;
    if (s_mode == 0) { s_cd_run = false; s_left_ms = 5 * 60 * 1000; }
    else { s_sw_run = false; s_sw_acc = 0; }
    refresh(NULL);
}
static void add_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)ui_ud(e);
    if (s_cd_run) { s_end_ms += m * 60000LL; if (s_end_ms < now_ms()) s_end_ms = now_ms(); }
    else { s_left_ms += m * 60000LL; if (s_left_ms < 0) s_left_ms = 0; }
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    if (!s_task_up) { s_task_up = true; xTaskCreate(bg_task, "timer_bg", 4096, NULL, 3, NULL); }
    ui_title(scr, "Timer");
    s_mode_lbl = ui_label(scr, &lv_font_montserrat_14, C_ACCENT, "");
    lv_obj_align(s_mode_lbl, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_add_flag(s_mode_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mode_lbl, 20);
    lv_obj_add_event_cb(s_mode_lbl, mode_cb, LV_EVENT_CLICKED, NULL);
    s_big = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "00:00");
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 130);

    s_row_cd = lv_obj_create(scr);
    lv_obj_set_size(s_row_cd, 410, 80);
    lv_obj_align(s_row_cd, LV_ALIGN_TOP_MID, 0, 220);
    lv_obj_set_style_bg_opa(s_row_cd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row_cd, 0, 0);
    lv_obj_remove_flag(s_row_cd, LV_OBJ_FLAG_SCROLLABLE);
    const char *lab[4] = {"-1m", "+1m", "+5m", "+10m"};
    int val[4] = {-1, 1, 5, 10};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_btn(s_row_cd, lab[i], 84, 56, 0x333333, add_cb, (void *)(intptr_t)val[i]);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 10 + i * 96, 0);
    }

    lv_obj_t *g = ui_btn(scr, "", 150, 90, C_ACCENT, go_cb, NULL);
    lv_obj_align(g, LV_ALIGN_BOTTOM_MID, -85, -60);
    s_go_lbl = lv_obj_get_child(g, 0);
    lv_obj_set_style_text_font(s_go_lbl, &lv_font_montserrat_48, 0);
    lv_obj_t *r = ui_btn(scr, "Reset", 150, 90, 0x444444, reset_cb, NULL);
    lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 85, -60);
    s_tm = lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }
void app_timer_start(lv_obj_t *s) { start(s); }
void app_timer_stop(void) { stop(); }

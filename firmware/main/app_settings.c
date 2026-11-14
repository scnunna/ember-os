// Settings: status, brightness, screen timeout, clock set.
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "app.h"
#include "hw.h"

static lv_obj_t *s_status, *s_time_lbl;
static lv_timer_t *s_tm;

static void refresh(lv_timer_t *t)
{
    batt_t b; hw_batt(&b);
    uint64_t tot, fre; hw_storage_info(&tot, &fre);
    char s[300];
    snprintf(s, sizeof(s),
             "Battery: %s     USB: %s\nStorage: %s, %llu MB free\nMotion sensor: %s     Steps: %lu\nEmber OS " EMBER_VERSION "  (" __DATE__ ")",
             b.present ? (b.charging ? "charging" : "on battery") : "none connected",
             b.vbus ? "yes" : "no",
             hw_storage_is_sd() ? "SD card" : "internal flash",
             (unsigned long long)(fre / 1048576),
             hw_imu_ok() ? "ok" : "NOT FOUND",
             (unsigned long)hw_steps_today());
    if (b.present) { char x[40]; snprintf(x, sizeof(x), "\nCharge: %d%%  (%d mV)", b.pct, b.mv); strncat(s, x, sizeof(s) - strlen(s) - 1); }
    lv_label_set_text(s_status, s);
    struct tm tm;
    if (hw_rtc_get(&tm)) { snprintf(s, sizeof(s), "%02d:%02d", tm.tm_hour, tm.tm_min); lv_label_set_text(s_time_lbl, s); }
}

static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    hw_pref_set("bright", v);
    bsp_display_brightness_set(v);
}
static void timeout_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)ui_ud(e);
    hw_pref_set("timeout", v);
    char m[40]; snprintf(m, sizeof(m), "Screen off after %ds", v);
    ui_msg(m);
}
static void lp_cb(lv_event_t *e)
{
    int v = !hw_pref_get("lp", 1);
    hw_pref_set("lp", v);
    ui_msg(v ? "Power saving ON (sleeps when dark, on battery)" : "Power saving OFF (battery drains faster)");
}
static void time_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)ui_ud(e);     // minutes to add
    struct tm t;
    if (!hw_rtc_get(&t)) return;
    int total = t.tm_hour * 60 + t.tm_min + d;
    total = ((total % 1440) + 1440) % 1440;
    t.tm_hour = total / 60; t.tm_min = total % 60; t.tm_sec = 0;
    hw_rtc_set(&t);
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    ui_title(scr, "Settings");
    s_status = ui_label(scr, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_size(s_status, 360, 92);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 25, 62);

    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_14, C_DIM, "Brightness");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 25, 214);
    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_size(sl, 360, 18);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 238);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, hw_pref_get("bright", 70), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    l = ui_label(scr, &lv_font_montserrat_14, C_DIM, "Screen off after");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 25, 268);
    const char *tl[4] = {"10s", "20s", "30s", "60s"};
    int tv[4] = {10, 20, 30, 60};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_btn(scr, tl[i], 78, 42, 0x333333, timeout_cb, (void *)(intptr_t)tv[i]);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 25 + i * 90, 290);
    }

    l = ui_label(scr, &lv_font_montserrat_14, C_DIM, "Clock  (phone / PC sync sets it)");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 25, 348);
    s_time_lbl = ui_label(scr, &lv_font_montserrat_28, C_TEXT, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 372);
    const char *cl[4] = {"-1h", "-1m", "+1m", "+1h"};
    int cv[4] = {-60, -1, 1, 60};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_btn(scr, cl[i], 84, 46, 0x333333, time_cb, (void *)(intptr_t)cv[i]);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 27 + i * 91, 420);
    }
    lv_obj_t *lp = ui_btn(scr, "Power saving: tap to toggle", 370, 36, 0x333333, lp_cb, NULL);
    lv_obj_align(lp, LV_ALIGN_TOP_MID, 0, 162);
    s_tm = lv_timer_create(refresh, 1000, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }
void app_settings_start(lv_obj_t *s) { start(s); }
void app_settings_stop(void) { stop(); }

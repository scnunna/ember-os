// Watch face / dashboard: time, date, steps ring, battery, recording status.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "hw.h"
#include "ble.h"
#include "wifi.h"

#define STEP_GOAL 8000

static lv_obj_t *s_ind, *s_bell, *s_time, *s_date, *s_arc, *s_steps, *s_batt, *s_rec, *s_hint, *s_store;
static uint32_t s_last_clock, s_last_batt;
static batt_t s_b;

void face_create(lv_obj_t *scr)
{
    s_ind = ui_label(scr, &lv_font_montserrat_20, C_ACCENT, "");
    lv_obj_align(s_ind, LV_ALIGN_TOP_RIGHT, -16, 10);
    s_bell = ui_label(scr, &lv_font_montserrat_20, 0xFFC857, "");
    lv_obj_align(s_bell, LV_ALIGN_TOP_LEFT, 16, 10);
    s_time = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "--:--");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 38);
    s_date = ui_label(scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 104);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 230, 230);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, 22);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, STEP_GOAL);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);

    s_steps = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "0");
    lv_obj_align(s_steps, LV_ALIGN_CENTER, 0, 12);
    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_14, C_DIM, "STEPS");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 52);

    s_batt = ui_label(scr, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_align(s_batt, LV_ALIGN_BOTTOM_LEFT, 28, -62);
    s_store = ui_label(scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_store, LV_ALIGN_BOTTOM_RIGHT, -28, -62);
    s_rec = ui_label(scr, &lv_font_montserrat_20, C_WARN, "");
    lv_obj_align(s_rec, LV_ALIGN_BOTTOM_MID, 0, -112);
    s_hint = ui_label(scr, &lv_font_montserrat_14, 0x555555, LV_SYMBOL_UP "  swipe up for apps");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void face_tick(void)
{
    if (!s_time) return;
    uint32_t now = lv_tick_get();
    if (lv_tick_elaps(s_last_clock) >= 1000 || s_last_clock == 0) {
        s_last_clock = now ? now : 1;
        static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        struct tm t;
        char b[40];
        if (hw_rtc_get(&t)) {
            snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
            lv_label_set_text(s_time, b);
            snprintf(b, sizeof(b), "%s  %s %d", wd[t.tm_wday % 7], mn[t.tm_mon % 12], t.tm_mday);
            lv_label_set_text(s_date, b);
        } else {
            lv_label_set_text(s_time, "--:--");
            lv_label_set_text(s_date, "set time in Settings");
        }
        uint32_t st = hw_steps_today();
        snprintf(b, sizeof(b), "%lu", (unsigned long)st);
        lv_label_set_text(s_steps, b);
        lv_arc_set_value(s_arc, st > STEP_GOAL ? STEP_GOAL : st);
        lv_obj_align(s_steps, LV_ALIGN_CENTER, 0, 12);

        char ind[24] = "";
        if (ble_connected()) strcat(ind, LV_SYMBOL_BLUETOOTH " ");
        if (wifi_ap_active()) strcat(ind, LV_SYMBOL_WIFI);
        lv_label_set_text(s_ind, ind);
        lv_obj_align(s_ind, LV_ALIGN_TOP_RIGHT, -16, 10);
        int un = ble_notif_unread();
        if (un > 0) { snprintf(b, sizeof(b), LV_SYMBOL_BELL " %d", un); lv_label_set_text(s_bell, b); } else lv_label_set_text(s_bell, "");

        if (rec_active()) {
            uint32_t s = rec_seconds();
            snprintf(b, sizeof(b), LV_SYMBOL_AUDIO "  REC %02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
            lv_label_set_text(s_rec, b);
        } else lv_label_set_text(s_rec, "");

        if (lv_tick_elaps(s_last_batt) > 15000 || s_last_batt == 0) {
            s_last_batt = now ? now : 1;
            hw_batt(&s_b);
            if (!s_b.present) snprintf(b, sizeof(b), s_b.vbus ? LV_SYMBOL_USB "  no battery" : "no battery");
            else snprintf(b, sizeof(b), "%s %d%%", s_b.charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, s_b.pct);
            lv_label_set_text(s_batt, b);
            lv_label_set_text(s_store, hw_storage_is_sd() ? LV_SYMBOL_SD_CARD " SD" : LV_SYMBOL_DRIVE " internal");
        }
    }
}

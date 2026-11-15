// Recorder app UI. The recording itself runs in rec.c and continues if you leave this screen.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "hw.h"

static lv_obj_t *s_btn, *s_btn_lbl, *s_time, *s_bar, *s_mode_lbl, *s_info, *s_file, *s_mark;
static void mark_cb(lv_event_t *e) { rec_mark(); ui_msg("Bookmarked"); }
static lv_timer_t *s_tm;
static int s_mode;     // 0 meeting 1 music


static void refresh(lv_timer_t *t)
{
    char b[96];
    bool a = rec_active();
    uint32_t s = rec_seconds();
    snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    lv_label_set_text(s_time, b);
    lv_bar_set_value(s_bar, a ? rec_level() : 0, LV_ANIM_OFF);
    lv_label_set_text(s_btn_lbl, a ? LV_SYMBOL_STOP : LV_SYMBOL_AUDIO);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(a ? 0x444444 : C_WARN), 0);
    if (a) lv_obj_remove_flag(s_mark, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
    int m = a ? (int)rec_mode() : s_mode;
    lv_label_set_text(s_mode_lbl, m ? "MUSIC   44.1 kHz" : "MEETING   16 kHz");
    uint64_t tot, fre;
    hw_storage_info(&tot, &fre);
    snprintf(b, sizeof(b), "%s  %llu MB free", hw_storage_is_sd() ? "SD card" : "Internal (no SD)", (unsigned long long)(fre / 1048576));
    lv_label_set_text(s_info, b);
    const char *f = rec_last_file();
    const char *slash = strrchr(f, '/');
    lv_label_set_text(s_file, f[0] ? (slash ? slash + 1 : f) : "no recordings yet");
}

static void rec_cb(lv_event_t *e)
{
    bool was = rec_active();
    if (!rec_toggle((rec_mode_t)s_mode)) ui_msg(was ? "Stop failed" : "Cannot start (mic busy or storage full)");
    refresh(NULL);
}
static void mode_cb(lv_event_t *e)
{
    if (rec_active()) { ui_msg("Stop recording first"); return; }
    s_mode = !s_mode;
    hw_pref_set("recmode", s_mode);
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    s_mode = hw_pref_get("recmode", 0);
    ui_title(scr, "Recorder");
    s_mode_lbl = ui_label(scr, &lv_font_montserrat_20, C_ACCENT, "");
    lv_obj_align(s_mode_lbl, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_add_flag(s_mode_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_lbl, mode_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *h = ui_label(scr, &lv_font_montserrat_14, C_DIM, "tap to switch Meeting / Music");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 106);

    s_time = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "00:00");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 150);
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 300, 14);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 225);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_OK), LV_PART_INDICATOR);

    s_btn = ui_btn(scr, "", 130, 130, C_WARN, rec_cb, NULL);
    lv_obj_set_style_radius(s_btn, 65, 0);
    lv_obj_align(s_btn, LV_ALIGN_TOP_MID, 0, 270);
    s_btn_lbl = lv_obj_get_child(s_btn, 0);
    lv_obj_set_style_text_font(s_btn_lbl, &lv_font_montserrat_48, 0);
    s_mark = ui_btn(scr, LV_SYMBOL_OK " Mark", 100, 60, 0xEDAE49, mark_cb, NULL);    // bookmark this moment (meeting notes)
    lv_obj_align(s_mark, LV_ALIGN_TOP_RIGHT, -18, 305);
    lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);

    s_file = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_file, LV_ALIGN_BOTTOM_MID, 0, -62);
    s_info = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_t *t = ui_label(scr, &lv_font_montserrat_14, 0x555555, "double-click BOOT = quick meeting record");
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_tm = lv_timer_create(refresh, 200, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }

void app_rec_start(lv_obj_t *s) { start(s); }
void app_rec_stop(void) { stop(); }

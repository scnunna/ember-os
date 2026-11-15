// Practice: guitar practice helpers that log to the shared practice log (synced to the phone).
//  * 1-minute chord changes: tap the big pad on every clean chord change; your count and best are kept.
//  * Session timer: start/stop a practice session; minutes are logged.
// Metronome sessions (>= 1 min) are logged by the Metronome app too.
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app.h"
#include "hw.h"
#include "practice.h"
#include "link.h"

static lv_obj_t *s_stats, *s_big, *s_sub, *s_pad, *s_pad_lbl, *s_chg_lbl, *s_ses_lbl;
static lv_timer_t *s_tm;
static enum { IDLE, CHANGES, SESSION } s_mode;
static int64_t s_t0;
static int s_count;

static void stats(void)
{
    char b[80];
    int st = practice_streak();
    snprintf(b, sizeof(b), "This week %d min   %s %d day%s", practice_week_minutes(), LV_SYMBOL_CHARGE, st, st == 1 ? "" : "s");
    lv_label_set_text(s_stats, b);
}

static void finish_changes(void)
{
    int best = hw_pref_get("chbest", 0);
    char b[64];
    if (s_count > best) { hw_pref_set("chbest", s_count); snprintf(b, sizeof(b), "%d changes - NEW BEST!", s_count); hw_buzz_pattern(3, 120, 100); }
    else { snprintf(b, sizeof(b), "%d changes  (best %d)", s_count, best); hw_buzz_pattern(2, 150, 120); }
    lv_label_set_text(s_sub, b);
    practice_log("chords", 1, 0, s_count);
    if (link_ready()) link_send_practice();
    s_mode = IDLE;
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    stats();
}

static void refresh(lv_timer_t *t)
{
    char b[32];
    int64_t el = (esp_timer_get_time() - s_t0) / 1000000;
    if (s_mode == CHANGES) {
        int left = 60 - (int)el;
        if (left <= 0) { finish_changes(); lv_label_set_text(s_big, "0"); }
        else { snprintf(b, sizeof(b), "%d", left); lv_label_set_text(s_big, b); }
        snprintf(b, sizeof(b), "%d changes", s_count);
        lv_label_set_text(s_pad_lbl, b);
    } else if (s_mode == SESSION) {
        snprintf(b, sizeof(b), "%02d:%02d", (int)(el / 60), (int)(el % 60));
        lv_label_set_text(s_big, b);
    }
    lv_label_set_text(s_ses_lbl, s_mode == SESSION ? LV_SYMBOL_STOP " Stop session" : LV_SYMBOL_PLAY " Session");
    lv_label_set_text(s_chg_lbl, s_mode == CHANGES ? LV_SYMBOL_STOP " Cancel" : "1-min changes");
}

static void chg_cb(lv_event_t *e)
{
    if (s_mode == SESSION) { ui_msg("Stop the session first"); return; }
    if (s_mode == CHANGES) {                                  // cancel: nothing logged
        s_mode = IDLE; lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_big, "--"); lv_label_set_text(s_sub, "cancelled");
        refresh(NULL); return;
    }
    s_mode = CHANGES; s_count = 0; s_t0 = esp_timer_get_time();
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_sub, "tap the pad on every clean change");
    hw_buzz(60);
    refresh(NULL);
}
static void pad_cb(lv_event_t *e)
{
    if (s_mode != CHANGES) return;
    s_count++;
    hw_buzz(15);
    refresh(NULL);
}
static void ses_cb(lv_event_t *e)
{
    if (s_mode == CHANGES) { ui_msg("Finish the changes first"); return; }
    if (s_mode == SESSION) {
        int mins = (int)((esp_timer_get_time() - s_t0) / 60000000LL);
        s_mode = IDLE;
        char b[48];
        if (mins >= 1) { practice_log("timer", mins, 0, 0); snprintf(b, sizeof(b), "logged %d min", mins); if (link_ready()) link_send_practice(); }
        else snprintf(b, sizeof(b), "under 1 min - not logged");
        lv_label_set_text(s_sub, b);
        stats();
    } else {
        s_mode = SESSION; s_t0 = esp_timer_get_time();
        lv_label_set_text(s_sub, "practising...");
    }
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    s_mode = IDLE;
    ui_title(scr, "Practice");
    s_stats = ui_label(scr, &lv_font_montserrat_14, C_ACCENT, "");
    lv_obj_align(s_stats, LV_ALIGN_TOP_MID, 0, 62);
    s_big = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 88);
    s_sub = ui_label(scr, &lv_font_montserrat_14, C_DIM, "pick a tool");
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_t *c = ui_btn(scr, "", 178, 64, 0x8E6CEF, chg_cb, NULL);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 18, 178);
    s_chg_lbl = lv_obj_get_child(c, 0);
    lv_obj_t *s = ui_btn(scr, "", 178, 64, 0x1F9D8A, ses_cb, NULL);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -18, 178);
    s_ses_lbl = lv_obj_get_child(s, 0);
    s_pad = ui_btn(scr, "", 374, 210, 0x3A2A55, pad_cb, NULL);
    lv_obj_align(s_pad, LV_ALIGN_TOP_MID, 0, 256);
    s_pad_lbl = lv_obj_get_child(s_pad, 0);
    lv_obj_set_style_text_font(s_pad_lbl, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    stats();
    s_tm = lv_timer_create(refresh, 250, NULL);
    refresh(NULL);
}
static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    if (s_mode == SESSION) {                                   // leaving the app ends and logs the session
        int mins = (int)((esp_timer_get_time() - s_t0) / 60000000LL);
        if (mins >= 1) practice_log("timer", mins, 0, 0);
    }
    s_mode = IDLE;
}
void app_practice_start(lv_obj_t *s) { start(s); }
void app_practice_stop(void) { stop(); }

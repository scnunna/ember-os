// Translate: push-to-talk conversation mode. HOLD "Me" or "Them" and speak; the watch streams the audio to the
// Ember Companion app, which recognises, translates (offline) and speaks it, and sends the text back here in
// large type so the other person can read it.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "app.h"
#include "hw.h"
#include "link.h"

static lv_obj_t *s_pair, *s_dst, *s_src, *s_status, *s_me_lbl, *s_them_lbl;
static lv_timer_t *s_tm;
static int s_gen = -1;

static void up(char *s) { for (; *s; s++) *s = toupper((unsigned char)*s); }

static void refresh(lv_timer_t *t)
{
    char me[8], them[8], src[200], dst[240]; int gen;
    link_tr_state(me, them, src, sizeof(src), dst, sizeof(dst), &gen);
    up(me); up(them);
    if (gen != s_gen) {
        s_gen = gen;
        char b[48];
        snprintf(b, sizeof(b), "%s  " LV_SYMBOL_LOOP "  %s", me, them);
        lv_label_set_text(s_pair, b);
        lv_label_set_text(s_dst, dst[0] ? dst : "Hold a button and speak.");
        lv_label_set_text(s_src, src);
        snprintf(b, sizeof(b), "Hold: Me\n%s", me); lv_label_set_text(s_me_lbl, b);
        snprintf(b, sizeof(b), "Hold: Them\n%s", them); lv_label_set_text(s_them_lbl, b);
    }
    if (link_ptt_active()) lv_label_set_text(s_status, LV_SYMBOL_AUDIO "  listening... release to translate");
    else if (!link_ready()) lv_label_set_text(s_status, "Open Ember Companion on your phone");
    else lv_label_set_text(s_status, "Phone connected - works offline");
}

static void ptt_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    bool me = (bool)(intptr_t)lv_event_get_user_data(e);
    if (c == LV_EVENT_PRESSED) {
        if (!link_ready()) { ui_msg("Phone not connected"); return; }
        if (!link_ptt_start(me)) { ui_msg("Mic busy (recording?) - try again"); return; }
        hw_buzz(30);
    } else if (link_ptt_active()) { link_ptt_stop(); hw_buzz(20); }
    refresh(NULL);
}

static lv_obj_t *hold_btn(lv_obj_t *scr, bool me, int x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 178, 110);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(me ? 0x2D6CDF : 0xD1495B), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);     // holding + talking must never swipe the app away
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -30);
    lv_obj_add_event_cb(b, ptt_cb, LV_EVENT_PRESSED, (void *)(intptr_t)me);
    lv_obj_add_event_cb(b, ptt_cb, LV_EVENT_RELEASED, (void *)(intptr_t)me);
    lv_obj_add_event_cb(b, ptt_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)me);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return l;
}

static void start(lv_obj_t *scr)
{
    s_gen = -1;
    ui_title(scr, "Translate");
    s_pair = ui_label(scr, &lv_font_montserrat_20, C_ACCENT, "");
    lv_obj_align(s_pair, LV_ALIGN_TOP_MID, 0, 62);
    s_dst = ui_label(scr, &lv_font_montserrat_28, C_TEXT, "");
    lv_obj_set_size(s_dst, 370, 150);
    lv_label_set_long_mode(s_dst, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_dst, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dst, LV_ALIGN_TOP_MID, 0, 96);
    s_src = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_size(s_src, 370, 52);
    lv_label_set_long_mode(s_src, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_src, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_src, LV_ALIGN_TOP_MID, 0, 252);
    s_status = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 314);
    s_me_lbl = hold_btn(scr, true, 18);
    s_them_lbl = hold_btn(scr, false, 214);
    s_tm = lv_timer_create(refresh, 200, NULL);
    refresh(NULL);
}
static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    link_ptt_stop();
}
void app_translate_start(lv_obj_t *s) { start(s); }
void app_translate_stop(void) { stop(); }

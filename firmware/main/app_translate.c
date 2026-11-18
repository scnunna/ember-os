// Translate: push-to-talk conversation mode. HOLD "Me" or "Them" and speak; the watch streams the audio to the
// Ember Companion app, which recognises, translates (offline) and speaks it, and sends the text back here in
// large type so the other person can read it.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "app.h"
#include "hw.h"
#include "link.h"

// same codes as the phone's Languages list (android/Models.kt)
static const char *const LC[] = {"ar","bn","cs","de","el","en","es","fa","fr","he","hi","hu","id","it","ja","ko","nl","pl","pt","ro","ru","sv","ta","te","th","tr","uk","ur","vi","zh"};
static const char LN[] = "Arabic\nBengali\nCzech\nGerman\nGreek\nEnglish\nSpanish\nPersian\nFrench\nHebrew\nHindi\nHungarian\nIndonesian\nItalian\nJapanese\nKorean\nDutch\nPolish\nPortuguese\nRomanian\nRussian\nSwedish\nTamil\nTelugu\nThai\nTurkish\nUkrainian\nUrdu\nVietnamese\nChinese";
#define NLC ((int)(sizeof(LC) / sizeof(LC[0])))
static int lc_index(const char *c) { for (int i = 0; i < NLC; i++) if (!strcasecmp(LC[i], c)) return i; return 0; }
static lv_obj_t *s_sheet, *s_r_me, *s_r_them;

static lv_obj_t *s_pair, *s_dst, *s_src, *s_status, *s_me_lbl, *s_them_lbl;
static lv_timer_t *s_tm;
static int s_gen = -1;

static void up(char *s) { for (; *s; s++) *s = toupper((unsigned char)*s); }

static void refresh(lv_timer_t *t);
static void refresh(lv_timer_t *t)
{
    char me[8], them[8], src[200], dst[240]; int gen;
    link_tr_state(me, them, src, sizeof(src), dst, sizeof(dst), &gen);
    up(me); up(them);
    if (gen != s_gen) {
        s_gen = gen;
        char b[48];
        snprintf(b, sizeof(b), "%s   " LV_SYMBOL_SHUFFLE "   %s", me, them);
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

// tap the pair = swap my language and theirs
static void swap_cb(lv_event_t *e)
{
    char me[8], them[8]; link_tr_state(me, them, NULL, 0, NULL, 0, NULL);
    link_tr_set(them, me);
    hw_buzz(20);
    refresh(NULL);
}
static void sheet_close(void) { if (s_sheet) { lv_obj_delete(s_sheet); s_sheet = NULL; } }
static void sheet_save(lv_event_t *e)
{
    int a = lv_roller_get_selected(s_r_me), b = lv_roller_get_selected(s_r_them);
    sheet_close();
    if (a == b) { ui_msg("Pick two different languages"); return; }
    link_tr_set(LC[a], LC[b]);
    refresh(NULL);
}
static void sheet_cancel(lv_event_t *e) { sheet_close(); }
static lv_obj_t *roller(lv_obj_t *p, const char *title, int x, int sel)
{
    lv_obj_t *t = ui_label(p, &lv_font_montserrat_20, C_DIM, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, x + 20, 64);
    lv_obj_t *r = lv_roller_create(p);
    lv_roller_set_options(r, LN, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, 180);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x111418), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2D6CDF), LV_PART_SELECTED);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);    // scrolling the wheel must not swipe the app away
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, x, 96);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    return r;
}
static void gear_cb(lv_event_t *e)
{
    if (s_sheet) return;
    char me[8], them[8]; link_tr_state(me, them, NULL, 0, NULL, 0, NULL);
    s_sheet = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_sheet, 410, 502);
    lv_obj_set_pos(s_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *h = ui_label(s_sheet, &lv_font_montserrat_20, C_ACCENT, LV_SYMBOL_SETTINGS "  Languages");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 26);
    s_r_me = roller(s_sheet, "I speak", 15, lc_index(me));
    s_r_them = roller(s_sheet, "They speak", 215, lc_index(them));
    lv_obj_t *sv = ui_btn(s_sheet, LV_SYMBOL_OK "  Save", 180, 70, 0x1F7A4D, sheet_save, NULL);
    lv_obj_align(sv, LV_ALIGN_BOTTOM_RIGHT, -20, -30);
    lv_obj_t *c = ui_btn(s_sheet, "Cancel", 180, 70, 0x3A3A3A, sheet_cancel, NULL);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 20, -30);
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
    // [ EN  <->  FR ] tap = swap      [gear] = pick languages
    lv_obj_t *pb = ui_btn(scr, "", 230, 50, 0x1E2633, swap_cb, NULL);
    lv_obj_align(pb, LV_ALIGN_TOP_MID, -20, 54);
    s_pair = lv_obj_get_child(pb, 0);
    lv_obj_set_style_text_color(s_pair, lv_color_hex(C_ACCENT), 0);
    lv_obj_t *g = ui_btn(scr, LV_SYMBOL_SETTINGS, 56, 50, 0x1E2633, gear_cb, NULL);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 130, 54);
    s_dst = ui_label(scr, &lv_font_montserrat_28, C_TEXT, "");
    lv_obj_set_size(s_dst, 370, 150);
    lv_label_set_long_mode(s_dst, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_dst, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dst, LV_ALIGN_TOP_MID, 0, 112);
    s_src = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_size(s_src, 370, 52);
    lv_label_set_long_mode(s_src, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_src, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_src, LV_ALIGN_TOP_MID, 0, 262);
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
    s_sheet = NULL;                               // deleted with the screen
    link_ptt_stop();
}
void app_translate_start(lv_obj_t *s) { start(s); }
void app_translate_stop(void) { stop(); }

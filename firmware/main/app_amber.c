// Amber: talk to your personal agent. Hold the button, speak, release: the phone turns it into text and sends it to
// Amber (Life OS on your tailnet). "You said" and Amber's replies are listed below, newest first.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "hw.h"
#include "link.h"

static lv_obj_t *s_scr, *s_status, *s_list;
static lv_timer_t *s_tmr;
static int s_gen = -1;

static void talk_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (link_ptt_start_ex("amber", NULL)) { hw_buzz(25); lv_label_set_text(s_status, LV_SYMBOL_AUDIO "  listening... release to send"); }
        else lv_label_set_text(s_status, link_ready() ? "Mic busy - stop the recording first" : "Phone not connected");
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && link_ptt_active()) {
        link_ptt_stop(); hw_buzz(20);
        lv_label_set_text(s_status, "Sent. Amber is thinking...");
    }
}

static void rebuild(void)
{
    lv_obj_clean(s_list);
    int n = link_amber_count();
    if (!n) {
        lv_obj_t *l = ui_label(s_list, &lv_font_montserrat_20, C_DIM, "Ask Amber to plan your day,\nmove a block, add a task\nor take a note.");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 330);
        return;
    }
    link_amber_t m;
    for (int i = 0; i < n && link_amber_get(i, &m); i++) {
        char b[330];
        snprintf(b, sizeof(b), "%s%s", m.mine ? "You: " : "Amber: ", m.text);
        lv_obj_t *l = ui_label(s_list, &lv_font_montserrat_20, m.mine ? C_DIM : C_TEXT, b);
        lv_obj_set_width(l, 330);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
}

static void poll(lv_timer_t *t)
{
    int g = link_amber_gen();
    if (g == s_gen) return;
    s_gen = g;
    link_amber_t m;
    if (link_amber_get(0, &m) && !m.mine && !link_ptt_active()) lv_label_set_text(s_status, "Hold to talk to Amber");
    rebuild();
}

void app_amber_start(lv_obj_t *scr)
{
    s_scr = scr;
    ui_title(scr, "Amber");
    lv_obj_t *t = ui_btn(scr, LV_SYMBOL_AUDIO "  Hold to talk", 330, 84, 0x7A4A12, NULL, NULL);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_add_event_cb(t, talk_ev, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(t, talk_ev, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(t, talk_ev, LV_EVENT_PRESS_LOST, NULL);
    s_status = ui_label(scr, &lv_font_montserrat_14, C_DIM, link_ready() ? "Hold to talk to Amber" : "Phone not connected");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 166);
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 360, 290);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 192);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    s_gen = -1;
    poll(NULL);
    s_tmr = lv_timer_create(poll, 500, NULL);
}

void app_amber_stop(void)
{
    if (link_ptt_active()) link_ptt_stop();
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    s_scr = s_status = s_list = NULL;
}

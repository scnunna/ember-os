// Security overlays drawn by the LVGL task, requested from other tasks without taking the display lock there
// (the BLE host task must never wait for the LVGL lock: LVGL code calls into NimBLE, so that could deadlock).
//  * ui_passkey(code): big 6-digit pairing code while a phone pairs (S1)
//  * ui_confirm(question, timeout): Allow / Deny prompt for risky phone requests (S3); default = deny
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "hw.h"
#include "link.h"

static volatile int s_pk_req = -2;           // -2 nothing pending, -1 hide, >= 0 show this code
static lv_obj_t *s_pk_box;
static SemaphoreHandle_t s_cf_done;
static volatile bool s_cf_req, s_cf_cancel, s_cf_result;
static char s_cf_text[160];
static lv_obj_t *s_cf_box;
// check-in prompt (sched.c): 0 on track, 1 off track, 2 snooze
static SemaphoreHandle_t s_ci_done;
static volatile bool s_ci_req, s_ci_cancel, s_ci_waiting;
static volatile int s_ci_result;
static char s_ci_head[48], s_ci_body[240], s_ci_ctx[96];
static lv_obj_t *s_ci_box, *s_ci_status;

void ui_passkey(int code) { s_pk_req = code < 0 ? -1 : code; }

static lv_obj_t *panel(void)
{
    lv_obj_t *b = lv_obj_create(lv_layer_top());
    lv_obj_set_size(b, 410, 502);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static void cf_answer(bool yes)
{
    s_cf_result = yes;
    if (s_cf_box) { lv_obj_delete(s_cf_box); s_cf_box = NULL; }
    xSemaphoreGive(s_cf_done);
}
static void allow_cb(lv_event_t *e) { cf_answer(true); }
static void deny_cb(lv_event_t *e) { cf_answer(false); }

static void ci_answer(int r)
{
    if (link_ptt_active()) link_ptt_stop();
    s_ci_result = r;
    if (s_ci_box) { lv_obj_delete(s_ci_box); s_ci_box = NULL; s_ci_status = NULL; }
    xSemaphoreGive(s_ci_done);
}
static void ci_on_cb(lv_event_t *e) { ci_answer(0); }
static void ci_snooze_cb(lv_event_t *e) { ci_answer(2); }
static void ci_skip_cb(lv_event_t *e) { ci_answer(1); }

// "Hold to tell Amber": audio streams to the phone while the finger is down (same path as Translate)
static void ci_talk_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (link_ptt_start_ex("amber", s_ci_ctx)) { hw_buzz(25); if (s_ci_status) lv_label_set_text(s_ci_status, LV_SYMBOL_AUDIO "  listening... release to send"); }
        else if (s_ci_status) lv_label_set_text(s_ci_status, "Phone not connected - tap a button");
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && link_ptt_active()) {
        link_ptt_stop(); hw_buzz(20);
        if (s_ci_status) lv_label_set_text(s_ci_status, LV_SYMBOL_OK "  sent to Amber");
    }
}
static lv_obj_t *talk_btn(lv_obj_t *p, int y)
{
    lv_obj_t *t = ui_btn(p, LV_SYMBOL_AUDIO "  Hold: tell Amber", 330, 68, 0x7A4A12, NULL, NULL);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(t, ci_talk_ev, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(t, ci_talk_ev, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(t, ci_talk_ev, LV_EVENT_PRESS_LOST, NULL);
    return t;
}
static void ci_off_cb(lv_event_t *e)             // off track -> ask what happened instead (voice), or skip
{
    lv_obj_clean(s_ci_box);
    lv_obj_t *h = ui_label(s_ci_box, &lv_font_montserrat_28, C_TEXT, "What did you do\ninstead?");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 80);
    s_ci_status = ui_label(s_ci_box, &lv_font_montserrat_20, C_DIM, "Amber will update your plan.");
    lv_obj_align(s_ci_status, LV_ALIGN_TOP_MID, 0, 190);
    talk_btn(s_ci_box, 250);
    lv_obj_t *d = ui_btn(s_ci_box, "Done", 200, 70, 0x3A3A3A, ci_skip_cb, NULL);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 350);
}

static void ci_show(void)
{
    s_ci_box = panel();
    lv_obj_t *h = ui_label(s_ci_box, &lv_font_montserrat_20, C_ACCENT, s_ci_head);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 34);
    // Amber's morning summary can be 200 characters: a smaller font keeps it inside the 120 px text area
    lv_obj_t *q = ui_label(s_ci_box, strlen(s_ci_body) > 110 ? &lv_font_montserrat_14 : &lv_font_montserrat_20, C_TEXT, s_ci_body);
    lv_obj_set_width(q, 350);
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(q, 120);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 66);
    s_ci_status = ui_label(s_ci_box, &lv_font_montserrat_14, C_DIM, "Kept up?");
    lv_obj_align(s_ci_status, LV_ALIGN_TOP_MID, 0, 192);
    lv_obj_t *on = ui_btn(s_ci_box, LV_SYMBOL_OK "  On track", 330, 70, 0x1F7A4D, ci_on_cb, NULL);
    lv_obj_align(on, LV_ALIGN_TOP_MID, 0, 216);
    lv_obj_t *off = ui_btn(s_ci_box, "Off track", 160, 66, 0x5A1E1E, ci_off_cb, NULL);
    lv_obj_align(off, LV_ALIGN_TOP_LEFT, 40, 298);
    lv_obj_t *sn = ui_btn(s_ci_box, "Snooze", 160, 66, 0x3A3A3A, ci_snooze_cb, NULL);
    lv_obj_align(sn, LV_ALIGN_TOP_RIGHT, -40, 298);
    talk_btn(s_ci_box, 376);
    hw_buzz_pattern(2, 90, 120);
}

static void tick(lv_timer_t *t)
{
    if (s_ci_req) {
        s_ci_req = false;
        launcher_wake();
        if (s_ci_box) { lv_obj_delete(s_ci_box); s_ci_box = NULL; }
        ci_show();
        lv_timer_reset(t);
    }
    if (s_ci_cancel) {
        s_ci_cancel = false;
        if (link_ptt_active()) link_ptt_stop();
        if (s_ci_box) { lv_obj_delete(s_ci_box); s_ci_box = NULL; s_ci_status = NULL; }
    }
    int pk = s_pk_req;
    if (pk != -2) {
        s_pk_req = -2;
        if (s_pk_box) { lv_obj_delete(s_pk_box); s_pk_box = NULL; }
        if (pk >= 0) {
            launcher_wake();
            s_pk_box = panel();
            lv_obj_t *h = ui_label(s_pk_box, &lv_font_montserrat_20, C_ACCENT, LV_SYMBOL_BLUETOOTH "  Pairing code");
            lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 110);
            char b[16]; snprintf(b, sizeof(b), "%03d %03d", pk / 1000, pk % 1000);
            lv_obj_t *c = ui_label(s_pk_box, &lv_font_montserrat_48, C_TEXT, b);
            lv_obj_align(c, LV_ALIGN_CENTER, 0, -10);
            lv_obj_t *n = ui_label(s_pk_box, &lv_font_montserrat_20, C_DIM, "Type this code on your phone.\nNever share it with anyone else.");
            lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -90);
            hw_buzz(80);
        }
    }
    if (s_cf_req) {
        s_cf_req = false;
        launcher_wake();
        s_cf_box = panel();
        lv_obj_t *h = ui_label(s_cf_box, &lv_font_montserrat_20, 0xFFC857, LV_SYMBOL_WARNING "  Allow this?");
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_t *q = ui_label(s_cf_box, &lv_font_montserrat_20, C_TEXT, s_cf_text);
        lv_obj_set_width(q, 360);
        lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 140);
        lv_obj_t *d = ui_btn(s_cf_box, "Deny", 160, 90, 0x5A1E1E, deny_cb, NULL);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 24, -50);
        lv_obj_t *a = ui_btn(s_cf_box, "Allow", 160, 90, 0x1F7A4D, allow_cb, NULL);
        lv_obj_align(a, LV_ALIGN_BOTTOM_RIGHT, -24, -50);
        hw_buzz_pattern(2, 60, 80);
    }
    if (s_cf_cancel) {                           // timed out on the other side: remove the prompt
        s_cf_cancel = false;
        if (s_cf_box) { lv_obj_delete(s_cf_box); s_cf_box = NULL; }
    }
}

bool ui_confirm(const char *question, int timeout_s)
{
    if (!s_cf_done) return false;
    xSemaphoreTake(s_cf_done, 0);                // clear a stale answer
    snprintf(s_cf_text, sizeof(s_cf_text), "%s", question);
    s_cf_result = false;
    s_cf_req = true;
    if (xSemaphoreTake(s_cf_done, pdMS_TO_TICKS(timeout_s * 1000)) != pdTRUE) { s_cf_cancel = true; return false; }
    return s_cf_result;
}

void ui_checkin_cancel(void)
{
    if (!s_ci_done || !s_ci_waiting) return;
    s_ci_result = -2;
    s_ci_cancel = true;                           // the overlay timer removes the box
    xSemaphoreGive(s_ci_done);
}

int ui_checkin(const char *head, const char *body, const char *amber_ctx, int timeout_s)
{
    if (!s_ci_done) return -1;
    xSemaphoreTake(s_ci_done, 0);
    snprintf(s_ci_head, sizeof(s_ci_head), "%s", head);
    snprintf(s_ci_body, sizeof(s_ci_body), "%s", body);
    snprintf(s_ci_ctx, sizeof(s_ci_ctx), "%s", amber_ctx ? amber_ctx : "");
    s_ci_result = -1;
    // a dark screen has no LVGL tick: light it from here so the overlay timer runs (same as wake_toast in link.c)
    bsp_display_lock(0);
    launcher_wake();
    bsp_display_unlock();
    s_ci_waiting = true;
    s_ci_req = true;
    bool got = xSemaphoreTake(s_ci_done, pdMS_TO_TICKS(timeout_s * 1000)) == pdTRUE;
    s_ci_waiting = false;
    if (!got) { s_ci_cancel = true; return -1; }
    return s_ci_result;
}

void overlay_start(void)                         // call once with the LVGL lock held
{
    s_cf_done = xSemaphoreCreateBinary();
    s_ci_done = xSemaphoreCreateBinary();
    lv_timer_create(tick, 1000, NULL);           // 1 wake-up per second at most: a pairing code / prompt shows within 1 s
}

// Security overlays drawn by the LVGL task, requested from other tasks without taking the display lock there
// (the BLE host task must never wait for the LVGL lock: LVGL code calls into NimBLE, so that could deadlock).
//  * ui_passkey(code): big 6-digit pairing code while a phone pairs (S1)
//  * ui_confirm(question, timeout): Allow / Deny prompt for risky phone requests (S3); default = deny
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app.h"
#include "hw.h"

static volatile int s_pk_req = -2;           // -2 nothing pending, -1 hide, >= 0 show this code
static lv_obj_t *s_pk_box;
static SemaphoreHandle_t s_cf_done;
static volatile bool s_cf_req, s_cf_cancel, s_cf_result;
static char s_cf_text[160];
static lv_obj_t *s_cf_box;

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

static void tick(lv_timer_t *t)
{
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

void overlay_start(void)                         // call once with the LVGL lock held
{
    s_cf_done = xSemaphoreCreateBinary();
    lv_timer_create(tick, 1000, NULL);           // 1 wake-up per second at most: a pairing code / prompt shows within 1 s
}

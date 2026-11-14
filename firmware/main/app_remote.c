// Remote: control your phone's music and find your phone, through the Chronos app link.
#include <stdio.h>
#include "app.h"
#include "ble.h"

static lv_obj_t *s_status;
static lv_timer_t *s_tm;
static bool s_find;

static void refresh(lv_timer_t *t)
{
    lv_label_set_text(s_status, ble_connected() ? LV_SYMBOL_BLUETOOTH " phone connected" : "not connected - see Phone app");
    lv_obj_set_style_text_color(s_status, lv_color_hex(ble_connected() ? C_OK : C_WARN), 0);
}
static void music_cb(lv_event_t *e)
{
    if (!ble_connected()) { ui_msg("Phone not connected"); return; }
    ble_music((uint16_t)(intptr_t)ui_ud(e));
}
static void find_cb(lv_event_t *e)
{
    if (!ble_connected()) { ui_msg("Phone not connected"); return; }
    s_find = !s_find;
    ble_find_phone(s_find);
    ui_msg(s_find ? "Phone is ringing - tap again to stop" : "Stopped");
}

static void start(lv_obj_t *scr)
{
    s_find = false;
    ui_title(scr, "Remote");
    s_status = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *b = ui_btn(scr, LV_SYMBOL_PLAY LV_SYMBOL_PAUSE, 150, 150, 0x5C6BC0, music_cb, (void *)(intptr_t)MUSIC_TOGGLE);
    lv_obj_set_style_radius(b, 75, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *p = ui_btn(scr, LV_SYMBOL_PREV, 100, 100, 0x3F4A94, music_cb, (void *)(intptr_t)MUSIC_PREV);
    lv_obj_set_style_radius(p, 50, 0);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 20, 135);
    lv_obj_t *n = ui_btn(scr, LV_SYMBOL_NEXT, 100, 100, 0x3F4A94, music_cb, (void *)(intptr_t)MUSIC_NEXT);
    lv_obj_set_style_radius(n, 50, 0);
    lv_obj_align(n, LV_ALIGN_TOP_RIGHT, -20, 135);

    lv_obj_t *vd = ui_btn(scr, LV_SYMBOL_VOLUME_MID " -", 170, 70, 0x333333, music_cb, (void *)(intptr_t)VOLUME_DOWN);
    lv_obj_align(vd, LV_ALIGN_TOP_LEFT, 20, 290);
    lv_obj_t *vu = ui_btn(scr, LV_SYMBOL_VOLUME_MAX " +", 170, 70, 0x333333, music_cb, (void *)(intptr_t)VOLUME_UP);
    lv_obj_align(vu, LV_ALIGN_TOP_RIGHT, -20, 290);
    lv_obj_t *f = ui_btn(scr, LV_SYMBOL_BELL "  Find my phone", 370, 70, 0xD1495B, find_cb, NULL);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 385);
    s_tm = lv_timer_create(refresh, 500, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }
void app_remote_start(lv_obj_t *s) { start(s); }
void app_remote_stop(void) { stop(); }

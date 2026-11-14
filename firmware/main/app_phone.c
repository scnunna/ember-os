// Phone app: Bluetooth link (Chronos, Android) + notifications + local Wi-Fi hotspot for files.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "hw.h"
#include "ble.h"
#include "wifi.h"

static lv_obj_t *s_ble_lbl, *s_ble_btn_lbl, *s_notifs, *s_wifi_btn_lbl, *s_wifi_info;
static lv_timer_t *s_tm;

static void refresh(lv_timer_t *t)
{
    char b[400];
    if (!ble_enabled()) snprintf(b, sizeof(b), "Bluetooth is off");
    else if (ble_connected()) {
        int pb = ble_phone_batt();
        if (pb >= 0) snprintf(b, sizeof(b), LV_SYMBOL_BLUETOOTH " Connected    phone battery %d%%", pb);
        else snprintf(b, sizeof(b), LV_SYMBOL_BLUETOOTH " Connected");
    } else snprintf(b, sizeof(b), "Waiting for Chronos app - pick %s", ble_name());
    lv_label_set_text(s_ble_lbl, b);
    lv_label_set_text(s_ble_btn_lbl, ble_enabled() ? "Bluetooth: ON" : "Bluetooth: OFF");

    b[0] = 0;
    int shown = 0;
    for (int i = 0; i < 3; i++) {
        ble_notif_t n;
        if (!ble_notif_get(i, &n)) break;
        char line[140];
        snprintf(line, sizeof(line), "%s%s  %s", i ? "\n" : "", n.time, n.text);
        strncat(b, line, sizeof(b) - strlen(b) - 1);
        shown++;
    }
    lv_label_set_text(s_notifs, shown ? b : "No notifications yet.\nThey appear here when your phone\nsends them through Chronos.");
    ble_notif_mark_seen();

    lv_label_set_text(s_wifi_btn_lbl, wifi_ap_active() ? "Wi-Fi hotspot: ON" : "Wi-Fi hotspot: OFF");
    if (wifi_ap_active())
        snprintf(b, sizeof(b), "Join Wi-Fi  %s\nPassword  %s\nthen open  http://192.168.4.1\n(%d device%s connected)", wifi_ap_ssid(), wifi_ap_pass(),
                 wifi_ap_clients(), wifi_ap_clients() == 1 ? "" : "s");
    else snprintf(b, sizeof(b), "Turn on to download recordings\nand set the clock from your phone.\nNo internet needed.");
    lv_label_set_text(s_wifi_info, b);
}

static void ble_cb(lv_event_t *e) { ble_set_enabled(!ble_enabled()); refresh(NULL); }
static void wifi_cb(lv_event_t *e)
{
    if (wifi_ap_active()) wifi_ap_stop(); else wifi_ap_start();
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    ui_title(scr, "Phone");
    s_ble_lbl = ui_label(scr, &lv_font_montserrat_14, C_ACCENT, "");
    lv_obj_set_width(s_ble_lbl, 370);
    lv_obj_align(s_ble_lbl, LV_ALIGN_TOP_LEFT, 20, 64);
    lv_obj_t *b = ui_btn(scr, "", 370, 48, 0x2D6CDF, ble_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 106);
    s_ble_btn_lbl = lv_obj_get_child(b, 0);

    lv_obj_t *h = ui_label(scr, &lv_font_montserrat_14, C_DIM, "NOTIFICATIONS");
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 20, 168);
    s_notifs = ui_label(scr, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(s_notifs, 370);
    lv_label_set_long_mode(s_notifs, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_notifs, 120);
    lv_obj_align(s_notifs, LV_ALIGN_TOP_LEFT, 20, 190);

    lv_obj_t *w = ui_btn(scr, "", 370, 48, 0x1F9D8A, wifi_cb, NULL);
    lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 318);
    s_wifi_btn_lbl = lv_obj_get_child(w, 0);
    s_wifi_info = ui_label(scr, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(s_wifi_info, 370);
    lv_obj_align(s_wifi_info, LV_ALIGN_TOP_LEFT, 20, 382);
    s_tm = lv_timer_create(refresh, 500, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }
void app_phone_start(lv_obj_t *s) { start(s); }
void app_phone_stop(void) { stop(); }

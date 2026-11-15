// Phone app: which phone/laptop is connected (with its name), remembered devices, notifications, Wi-Fi hotspot.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "hw.h"
#include "ble.h"
#include "wifi.h"
#include "link.h"
#include "devices.h"

static lv_obj_t *s_scr, *s_dev_lbl, *s_dev_sub, *s_ble_btn_lbl, *s_notifs, *s_wifi_btn_lbl, *s_wifi_info;
static lv_timer_t *s_tm;

// ---- small drawn "mobile" icon (the built-in symbol font has none) ----
static lv_obj_t *phone_icon(lv_obj_t *p, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, 16, 26);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *d = lv_obj_create(o);                  // home bar
    lv_obj_set_size(d, 6, 2);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *hold_btn(lv_obj_t *p, const char *txt, int w, int h, uint32_t color, lv_event_cb_t cb, void *ud)
{
    // press-and-HOLD buttons for things a stray tap (or a pocket) must never trigger
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_14, C_TEXT, txt);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED, ud);
    return b;
}
static void ago(uint32_t last_min, char *out, size_t n)
{
    struct tm t;
    if (!last_min || !hw_rtc_get(&t)) { snprintf(out, n, "never connected"); return; }
    int d = (int)((mktime(&t) - 946684800) / 60) - (int)last_min;
    if (d < 2) snprintf(out, n, "seen just now");
    else if (d < 120) snprintf(out, n, "seen %d min ago", d);
    else if (d < 48 * 60) snprintf(out, n, "seen %d h ago", d / 60);
    else snprintf(out, n, "seen %d days ago", d / 1440);
}

// ---- Ember Companion notifications: newest one with quick-reply buttons ----
static lv_obj_t *s_rep[4];
static int s_rep_id = -999;
static void reply_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)ui_ud(e);
    if (idx < 0) { link_notif_dismiss(s_rep_id); ui_msg("Dismissed"); }
    else { link_notif_reply(s_rep_id, idx); ui_msg("Reply sent"); }
    s_rep_id = -998;                                   // rebuild on next refresh
}
static void clear_reps(void) { for (int i = 0; i < 4; i++) if (s_rep[i]) { lv_obj_delete(s_rep[i]); s_rep[i] = NULL; } }
static bool link_notifs(char *b, size_t n)
{
    link_notif_t ln;
    if (!link_notif_get(0, &ln)) { if (s_rep_id != -999) { clear_reps(); s_rep_id = -999; } return false; }
    snprintf(b, n, "%s  %s: %s\n%s", ln.time, ln.app, ln.title, ln.body);
    if (ln.id != s_rep_id) {
        clear_reps();
        s_rep_id = ln.id;
        int k = ln.nrep + 1, w = (370 - (k - 1) * 8) / k;
        for (int i = 0; i < k; i++) {
            bool dis = (i == ln.nrep);
            s_rep[i] = ui_btn(s_scr, dis ? LV_SYMBOL_CLOSE : ln.replies[i], w, 40, dis ? 0x5A1E1E : 0x2D6CDF, reply_cb, (void *)(intptr_t)(dis ? -1 : i));
            lv_obj_set_style_text_font(lv_obj_get_child(s_rep[i], 0), &lv_font_montserrat_14, 0);
            lv_obj_align(s_rep[i], LV_ALIGN_TOP_LEFT, 20 + i * (w + 8), 270);
        }
    }
    link_notif_mark_seen();
    return true;
}

// ---- Devices panel: remembered phones/laptops; use one, rename it, forget it, add a new one ----
static lv_obj_t *s_panel, *s_kb, *s_ta;
static int s_sel = -1;
static void panel_build(void);
static void panel_close_cb(lv_event_t *e) { if (s_panel) { lv_obj_delete(s_panel); s_panel = NULL; s_kb = s_ta = NULL; } }
static void add_cb(lv_event_t *e)
{
    ble_pair_open();
    ui_msg("Adding a device for 2 minutes:\non it, pick EmberWatchOS\nand type the code shown here");
    panel_close_cb(e);
}
static void use_cb(lv_event_t *e)
{
    em_dev_t d;
    if (!devices_get(s_sel, &d)) return;
    devices_prefer(s_sel);
    char b[96]; snprintf(b, sizeof(b), "Waiting for %s\n(open Bluetooth or Ember Companion on it)", devices_label(&d));
    ui_msg(b);
    panel_close_cb(e);
}
static void forget_cb(lv_event_t *e)
{
    devices_forget(s_sel);
    s_sel = -1;
    ui_msg("Device forgotten (its keys are deleted)");
    panel_build();
}
static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) { devices_rename(s_sel, lv_textarea_get_text(s_ta)); ui_msg("Renamed"); }
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) panel_build();
}
static void rename_cb(lv_event_t *e)
{
    em_dev_t d;
    if (!devices_get(s_sel, &d)) return;
    lv_obj_clean(s_panel);
    lv_obj_t *t = ui_label(s_panel, &lv_font_montserrat_20, C_TEXT, "Name this device");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    s_ta = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 23);
    lv_textarea_set_text(s_ta, devices_label(&d));
    lv_obj_set_width(s_ta, 360);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 62);
    s_kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(s_kb, 410, 300);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
}
static void chronos_cb(lv_event_t *e)
{
    int v = !hw_pref_get("chronos", 0);
    hw_pref_set("chronos", v);
    ui_msg(v ? "Chronos ON after restart\n(it has no password - less safe)" : "Chronos OFF after restart");
    panel_build();
}
static void pick_cb(lv_event_t *e) { s_sel = (int)(intptr_t)ui_ud(e); panel_build(); }

static void panel_build(void)
{
    if (!s_panel) {
        s_panel = lv_obj_create(s_scr);
        lv_obj_set_size(s_panel, 410, 502);
        lv_obj_set_pos(s_panel, 0, 0);
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(C_BG), 0);
        lv_obj_set_style_border_width(s_panel, 0, 0);
        lv_obj_set_style_pad_all(s_panel, 0, 0);
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clean(s_panel);
    s_kb = s_ta = NULL;
    lv_obj_t *back = ui_btn(s_panel, LV_SYMBOL_LEFT " Phone", 120, 44, 0x333333, panel_close_cb, NULL);
    lv_obj_set_pos(back, 20, 16);
    lv_obj_t *t = ui_label(s_panel, &lv_font_montserrat_28, C_TEXT, "Devices");
    lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -24, 22);
    int n = devices_count(), cur = devices_current();
    if (!n) {
        lv_obj_t *l = ui_label(s_panel, &lv_font_montserrat_20, C_DIM, "No devices yet.\nHold \"Add device\" below,\nthen pick EmberWatchOS on your phone.");
        lv_obj_set_pos(l, 24, 90);
    }
    for (int i = 0; i < n; i++) {
        em_dev_t d; devices_get(i, &d);
        bool bonded = devices_bonded(i);
        char st[40];
        if (i == cur) snprintf(st, sizeof(st), link_ready() ? "connected - Ember Companion" : "connected");
        else if (!bonded) snprintf(st, sizeof(st), "needs pairing again");
        else ago(d.last_min, st, sizeof(st));
        char line[80]; snprintf(line, sizeof(line), "%s\n%s", devices_label(&d), st);
        lv_obj_t *b = ui_btn(s_panel, line, 370, 64, i == s_sel ? 0x2D4A6E : 0x1E2A36, pick_cb, (void *)(intptr_t)i);
        lv_obj_t *bl = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(bl, LV_ALIGN_LEFT_MID, 36, 0);
        lv_obj_t *ic = phone_icon(b, i == cur ? C_OK : C_DIM);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_pos(b, 20, 76 + i * 72);
    }
    int y = 76 + (n ? n : 2) * 72 + 6;
    if (s_sel >= 0 && s_sel < n) {                   // actions for the selected device
        lv_obj_t *u = ui_btn(s_panel, "Use", 110, 48, 0x1F7A4D, use_cb, NULL);
        lv_obj_set_pos(u, 20, y);
        lv_obj_t *r = ui_btn(s_panel, "Rename", 120, 48, 0x3A86FF, rename_cb, NULL);
        lv_obj_set_pos(r, 140, y);
        lv_obj_t *f = hold_btn(s_panel, "Hold: forget", 120, 48, 0x5A1E1E, forget_cb, NULL);
        lv_obj_set_pos(f, 270, y);
    }
    lv_obj_t *a = hold_btn(s_panel, "Hold: add device", 220, 48, 0x1F7A4D, add_cb, NULL);
    lv_obj_align(a, LV_ALIGN_BOTTOM_LEFT, 20, -24);
    lv_obj_t *c = ui_btn(s_panel, hw_pref_get("chronos", 0) ? "Chronos ON" : "Chronos OFF", 130, 48, 0x444444, chronos_cb, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(c, 0), &lv_font_montserrat_14, 0);
    lv_obj_align(c, LV_ALIGN_BOTTOM_RIGHT, -20, -24);
}
static void devices_cb(lv_event_t *e) { s_sel = devices_current(); panel_build(); }

static void refresh(lv_timer_t *t)
{
    char b[400], sub[96];
    int cur = devices_current();
    em_dev_t d;
    sub[0] = 0;
    if (!ble_enabled()) snprintf(b, sizeof(b), "Bluetooth is off");
    else if (ble_pair_window_open()) { snprintf(b, sizeof(b), "Adding a device (2 min)"); snprintf(sub, sizeof(sub), "on it pick %s, type the code shown here", ble_name()); }
    else if (cur >= 0 && devices_get(cur, &d)) {
        snprintf(b, sizeof(b), "%s", devices_label(&d));
        snprintf(sub, sizeof(sub), link_ready() ? "Connected - Ember Companion running" : "Connected - open Ember Companion for notifications");
    } else if (ble_connected()) snprintf(b, sizeof(b), "Connecting...");
    else if (devices_count() && devices_get(0, &d)) {
        int last = 0; em_dev_t x;
        for (int i = 1; i < devices_count(); i++) if (devices_get(i, &x) && x.last_min > d.last_min) { d = x; last = i; }
        (void)last;
        snprintf(b, sizeof(b), "%s", devices_label(&d));
        snprintf(sub, sizeof(sub), "not connected - tap Devices to switch");
    } else { snprintf(b, sizeof(b), "No device paired"); snprintf(sub, sizeof(sub), "Devices > hold Add device"); }
    lv_label_set_text(s_dev_lbl, b);
    lv_label_set_text(s_dev_sub, sub);
    lv_label_set_text(s_ble_btn_lbl, ble_enabled() ? "BT: ON" : "BT: OFF");

    if (link_notifs(b, sizeof(b))) { lv_obj_set_height(s_notifs, 74); lv_label_set_text(s_notifs, b); goto wifi; }
    lv_obj_set_height(s_notifs, 120);
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
    lv_label_set_text(s_notifs, shown ? b : "No notifications yet.\nThey appear here when Ember Companion\nforwards them from your phone.");
    ble_notif_mark_seen();
wifi:
    lv_label_set_text(s_wifi_btn_lbl, wifi_ap_active() ? "Wi-Fi hotspot: ON" : "Wi-Fi hotspot: OFF");
    if (wifi_ap_active())
        snprintf(b, sizeof(b), "Join Wi-Fi  %s   Password  %s\nopen  192.168.4.1/?k=%s\n(new password + key each time; %d connected)", wifi_ap_ssid(), wifi_ap_pass(),
                 wifi_ap_token(), wifi_ap_clients());
    else snprintf(b, sizeof(b), "Turn on to download recordings\nand set the clock from your phone.\nNo internet needed.");
    lv_label_set_text(s_wifi_info, b);
}

static void ble_cb(lv_event_t *e) { ble_set_enabled(!ble_enabled()); refresh(NULL); }
static void wifi_cb(lv_event_t *e)
{
    if (wifi_ap_active()) wifi_ap_stop();
    else {
        wifi_ap_start();
        if (!wifi_ap_active()) ui_msg("Wi-Fi needs more memory right now.\nRestart the watch (hold PWR) and try again.");
    }
    refresh(NULL);
}

static void start(lv_obj_t *scr)
{
    s_scr = scr; s_panel = NULL; s_sel = -1;
    ui_title(scr, "Phone");
    lv_obj_t *ic = phone_icon(scr, C_ACCENT);
    lv_obj_set_pos(ic, 20, 62);
    s_dev_lbl = ui_label(scr, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_width(s_dev_lbl, 340);
    lv_label_set_long_mode(s_dev_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_dev_lbl, 46, 58);
    s_dev_sub = ui_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_width(s_dev_sub, 340);
    lv_label_set_long_mode(s_dev_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_dev_sub, 46, 84);

    lv_obj_t *b = ui_btn(scr, "", 110, 48, 0x2D6CDF, ble_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 20, 110);
    s_ble_btn_lbl = lv_obj_get_child(b, 0);
    lv_obj_t *dv = ui_btn(scr, "Devices", 252, 48, 0x1F7A4D, devices_cb, NULL);
    lv_obj_align(dv, LV_ALIGN_TOP_LEFT, 138, 110);

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
static void stop(void)
{
    if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; }
    memset(s_rep, 0, sizeof(s_rep)); s_rep_id = -999;
    s_panel = s_kb = s_ta = NULL; s_scr = NULL;
}
void app_phone_start(lv_obj_t *s) { start(s); }
void app_phone_stop(void) { stop(); }

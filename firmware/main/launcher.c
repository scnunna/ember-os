// Launcher: watch face <-> app grid <-> apps. Swipes + BOOT button navigate.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "display/lv_display_private.h"   // scr_to_load: cancel an unfinished screen slide (see cancel_pending_open)
#include "app.h"
#include "diag.h"
#include "hw.h"

static lv_obj_t *s_face, *s_grid, *s_app;
static int s_cur = -1;
static enum { ST_FACE, ST_GRID, ST_APP } s_state = ST_FACE;
static bool s_on = true, s_dim = false;
static uint32_t s_last_act;
static lv_timer_t *s_idle_tmr;

// Built-in apps are 0..APP_COUNT-1, Ember scripted apps follow.
static int total_apps(void) { return APP_COUNT + script_count(); }
static void app_stop_idx(int i) { if (i >= APP_COUNT) script_stop(); else if (i >= 0 && APPS[i].stop) APPS[i].stop(); }

static int bright(void) { return hw_pref_get("bright", 70); }
static int timeout_s(void) { return hw_pref_get("timeout", 10); }   // was 20: screen time is the biggest drain

static void screen_set(bool on)
{
    bool was = s_on;
    s_on = on; s_dim = false;
    if (on && !was) power_update(true);          // panel out of sleep before the brightness command
    bsp_display_brightness_set(on ? bright() : 0);
    if (!on && was) power_update(false);
    if (s_idle_tmr) { if (on) lv_timer_resume(s_idle_tmr); else lv_timer_pause(s_idle_tmr); }
    script_pause(!on);
}
bool launcher_screen_on(void) { return s_on; }
static uint32_t s_raise_wakes;
uint32_t launcher_raise_wakes(void) { return s_raise_wakes; }
void launcher_raise(void)
{
    if (s_on) return;                            // only a dark screen: a raise while dimmed no longer keeps it alive
    s_raise_wakes++;
    bsp_display_lock(0);
    launcher_wake();
    bsp_display_unlock();
}

void launcher_wake(void)
{
    s_last_act = lv_tick_get();
    if (!s_on || s_dim) screen_set(true);
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

// ---- navigation ----
// Leaving an app whose opening slide hasn't finished: LVGL would silently ignore the new screen load (the target is
// still the "active" screen) and the app screen would finish sliding in, orphaned -> leak + wrong screen shown.
// Cancel that slide instead: the screen underneath stays, and the half-shown app screen is deleted right away.
static bool cancel_pending_open(lv_obj_t *app)
{
    lv_display_t *d = lv_display_get_default();
    if (!app || !d || d->scr_to_load != app) return false;
    // While the slide runs LVGL already has act_scr = app and prev_scr = the screen underneath; before the slide
    // starts, act_scr is still the screen underneath. Either way: make the screen underneath active again FIRST,
    // then delete the app (deleting it while it was act_scr left a dangling pointer -> endless layout loop).
    lv_obj_t *under = (d->act_scr == app) ? d->prev_scr : d->act_scr;
    lv_anim_delete(app, NULL);
    if (under) {
        lv_anim_delete(under, NULL);
        lv_obj_set_pos(under, 0, 0);
        lv_obj_remove_local_style_prop(under, LV_STYLE_OPA, 0);
    }
    d->act_scr = under;
    d->prev_scr = NULL; d->scr_to_load = NULL; d->del_prev = false; d->draw_prev_over_act = false;
    lv_obj_delete(app);                              // no longer referenced by the display: safe
    if (under) lv_obj_invalidate(under);
    return true;
}

void launcher_face(void)
{
    if (s_state == ST_APP) app_stop_idx(s_cur);
    bool del = (s_state == ST_APP);
    lv_obj_t *old = s_app;
    if (del && cancel_pending_open(old)) del = false;   // already gone; the grid is the active screen again
    s_state = ST_FACE; s_cur = -1; s_app = NULL;
    face_tick();
    // auto_del: LVGL deletes the old app screen exactly when ITS slide ends. The previous fixed 400 ms
    // lv_obj_delete_delayed could free a screen that was still sliding (rapid navigation) -> the animation
    // wrote to freed memory -> LoadProhibited crash + reboot (looked like an "install freeze").
    (void)old;
    lv_screen_load_anim(s_face, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, del);
}

void launcher_home(void)
{
    bool del = (s_state == ST_APP);
    if (del) app_stop_idx(s_cur);
    lv_obj_t *old = s_app;
    lv_scr_load_anim_t anim = (s_state == ST_FACE) ? LV_SCREEN_LOAD_ANIM_MOVE_TOP : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
    s_state = ST_GRID; s_cur = -1; s_app = NULL;
    if (del && cancel_pending_open(old)) return;      // the grid never stopped being the active screen
    lv_screen_load_anim(s_grid, anim, 220, 0, del);   // auto_del: see launcher_face
}

void launcher_back(void)
{
    if (s_state == ST_APP) launcher_home();
    else if (s_state == ST_GRID) launcher_face();
}

static unsigned s_gest_n; static int s_gest_dir;
static unsigned s_swallowed;
void launcher_gesture_dbg(unsigned *n, int *dir, unsigned *sw) { *n = s_gest_n; *dir = s_gest_dir; *sw = s_swallowed; }

static void gesture_cb(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    s_gest_n++; s_gest_dir = d;
    launcher_wake();
    if (s_state == ST_FACE && d == LV_DIR_TOP) launcher_home();
    else if (s_state == ST_GRID && (d == LV_DIR_RIGHT || d == LV_DIR_BOTTOM)) launcher_face();
    else if (s_state == ST_APP && d == LV_DIR_RIGHT) launcher_home();
}

void launcher_open(int idx)
{
    if (idx < 0 || idx >= total_apps() || s_state != ST_GRID) return;
    s_app = new_screen();
    lv_obj_add_event_cb(s_app, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_cur = idx;
    s_state = ST_APP;
    if (idx >= APP_COUNT) script_start(idx - APP_COUNT, s_app);
    else APPS[idx].start(s_app);
    lv_screen_load_anim(s_app, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static lv_point_t s_icon_start;
static void icon_press_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &s_icon_start);
}
static void icon_cb(lv_event_t *e)
{
    lv_point_t p = {0, 0};
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    int dx = p.x - s_icon_start.x, dy = p.y - s_icon_start.y;
    if (dx * dx + dy * dy > 22 * 22) return;         // it was a swipe
    launcher_open((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *s_grid_box;
static void add_icon(int i, const char *name, const char *sym, uint32_t color)
{
    const int cell = 130, x0 = (410 - 3 * cell) / 2;
    int cx = x0 + (i % 3) * cell, cy = 8 + (i / 3) * (cell + 8);
    lv_obj_t *ic = lv_obj_create(s_grid_box);
    lv_obj_set_size(ic, 88, 88);
    lv_obj_set_pos(ic, cx + (cell - 88) / 2, cy);
    lv_obj_set_style_radius(ic, 26, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_shadow_width(ic, 0, 0);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(ic, icon_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ic, icon_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *s = ui_label(ic, sym[0] ? &lv_font_montserrat_28 : &lv_font_montserrat_28, C_TEXT, sym[0] ? sym : "?");
    lv_obj_center(s);
    lv_obj_t *n = ui_label(s_grid_box, &lv_font_montserrat_14, C_TEXT, name);
    lv_obj_set_width(n, cell);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(n, cx, cy + 94);
}

static void fill_grid(void)
{
    lv_obj_clean(s_grid_box);
    for (int i = 0; i < APP_COUNT; i++) add_icon(i, APPS[i].name, APPS[i].sym, APPS[i].color);
    for (int j = 0; j < script_count(); j++) add_icon(APP_COUNT + j, script_name(j), script_icon(j), script_color(j));
}

static void build_grid(void)
{
    s_grid = new_screen();
    lv_obj_add_event_cb(s_grid, gesture_cb, LV_EVENT_GESTURE, NULL);
    // Icons live in a vertically scrolling box (more than 9 apps once scripts are installed).
    s_grid_box = lv_obj_create(s_grid);
    lv_obj_set_size(s_grid_box, 410, 502 - 48);
    lv_obj_set_pos(s_grid_box, 0, 48);
    lv_obj_set_style_bg_opa(s_grid_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid_box, 0, 0);
    lv_obj_set_style_pad_all(s_grid_box, 0, 0);
    lv_obj_set_style_pad_bottom(s_grid_box, 24, 0);
    lv_obj_set_scroll_dir(s_grid_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid_box, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(s_grid_box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    fill_grid();
}

// After an app was installed/removed (USB, Wi-Fi or Bluetooth). Caller holds the LVGL lock.
// The grid is NOT rebuilt immediately: rebuilding (lv_obj_clean + recreate) while a screen-slide animation is still
// running, several times in quick succession, sent LVGL's layout pass into an endless loop (task watchdog on
// taskLVGL). Requests are coalesced into one rebuild 500 ms later, after any animation and delayed delete are done.
static lv_timer_t *s_rebuild_tmr;
static void rebuild_cb(lv_timer_t *t)
{
    s_rebuild_tmr = NULL;                 // one-shot (repeat count 1): LVGL deletes the timer after this call
    if (lv_anim_count_running()) {        // still sliding: try again shortly
        s_rebuild_tmr = lv_timer_create(rebuild_cb, 200, NULL);
        lv_timer_set_repeat_count(s_rebuild_tmr, 1);
        return;
    }
    fill_grid();
}
void launcher_rebuild_grid(void)
{
    if (s_state == ST_APP) launcher_home();
    if (s_rebuild_tmr) { lv_timer_reset(s_rebuild_tmr); return; }
    s_rebuild_tmr = lv_timer_create(rebuild_cb, 500, NULL);
    lv_timer_set_repeat_count(s_rebuild_tmr, 1);
}

void launcher_open_script(const char *name)
{
    for (int j = 0; j < script_count(); j++)
        if (!strcmp(script_name(j), name)) {
            launcher_wake();
            if (s_state == ST_APP) launcher_home();
            if (s_state == ST_FACE) launcher_home();
            s_state = ST_GRID;
            launcher_open(APP_COUNT + j);
            return;
        }
}

// ---- idle: dim, then off; touch wakes ----
static void idle_cb(lv_timer_t *t)
{
    uint32_t inact = lv_display_get_inactive_time(NULL);
    if (inact < 400 && (!s_on || s_dim)) { launcher_wake(); return; }
    if (inact < 400) s_last_act = lv_tick_get();
    uint32_t idle = lv_tick_elaps(s_last_act) / 1000;
    int to = timeout_s();
    if (s_on && !s_dim && idle >= (uint32_t)to) { s_dim = true; bsp_display_brightness_set(bright() / 4); }
    else if (s_on && s_dim && idle >= (uint32_t)to + 4) { screen_set(false); }
    if (s_on) face_tick();
}

static bool s_swallow;
static struct { bool active, pressed; int x, y; } s_syn;      // synthetic finger for automated tests
void launcher_synth(bool active, bool pressed, int x, int y) { s_syn.active = active; s_syn.pressed = pressed; s_syn.x = x; s_syn.y = y; }
int launcher_state(void) { return s_state; }
int launcher_active_screen(void) { lv_obj_t *a = lv_screen_active(); return a == s_face ? 0 : (a == s_grid ? 1 : (a == s_app ? 2 : 9)); }
int launcher_cur(void) { return s_cur; }
bool launcher_dim(void) { return s_dim; }

// Our own touch reader. The stock one (esp_lvgl_port) wraps the controller read in ESP_ERROR_CHECK, so one NACK from
// the FT3168 (it naps between touches) aborts and reboots the whole watch. Here a failed read just means "no finger".
typedef struct { esp_lcd_touch_handle_t handle; lv_indev_t *indev; struct { float x, y; } scale; } port_touch_ctx_t;   // mirrors esp_lvgl_port's private struct
static unsigned s_touch_err, s_touch_max_ms;
unsigned touch_read_errors(void) { return s_touch_err; }
unsigned touch_read_max_ms(void) { return s_touch_max_ms; }

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static lv_point_t last_pt;
    static bool last_pressed;
    static int err_streak;
    data->state = LV_INDEV_STATE_RELEASED;
    port_touch_ctx_t *c = lv_indev_get_driver_data(indev);
    if (!c || !c->handle) return;
    int64_t t0 = esp_timer_get_time();
    esp_err_t re = esp_lcd_touch_read_data(c->handle);
    unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (ms > s_touch_max_ms) s_touch_max_ms = ms;
    if (re != ESP_OK) {
        s_touch_err++;
        // One-off NACK in the middle of a swipe: keep the finger where it was for up to 2 reads instead of cutting the swipe short.
        if (last_pressed && ++err_streak <= 2) { data->point = last_pt; data->state = LV_INDEV_STATE_PRESSED; }
        else last_pressed = false;
        return;
    }
    err_streak = 0;
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t n = 0;
    last_pressed = false;
    if (esp_lcd_touch_get_data(c->handle, pt, &n, 1) == ESP_OK && n > 0) {
        data->point.x = (int32_t)(c->scale.x * pt[0].x);
        data->point.y = (int32_t)(c->scale.y * pt[0].y);
        data->state = LV_INDEV_STATE_PRESSED;
        last_pt = data->point;
        last_pressed = true;
    }
}

static unsigned s_real_press_reads, s_real_reads, s_syn_reads, s_syn_passed;
void launcher_touch_stats(unsigned *reads, unsigned *presses) { *reads = s_real_reads; *presses = s_real_press_reads; }
void launcher_syn_stats(unsigned *reads, unsigned *passed) { *reads = s_syn_reads; *passed = s_syn_passed; }

static void read_wrap(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!s_syn.active) {                     // real controller (statistics count it only)
        touch_read(indev, data);
        s_real_reads++;
        if (data->state == LV_INDEV_STATE_PRESSED) s_real_press_reads++;
    } else {                                 // test hook: injected finger; never touch the (possibly napping) real controller
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = s_syn.x; data->point.y = s_syn.y;
        data->state = s_syn.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (!s_on || s_dim) { s_swallow = true; s_swallowed++; launcher_wake(); }
        else s_last_act = lv_tick_get();
    } else s_swallow = false;
    if (s_swallow) data->state = LV_INDEV_STATE_RELEASED;      // widgets never see the wake-up touch
    if (s_syn.active) { s_syn_reads++; if (data->state == LV_INDEV_STATE_PRESSED) s_syn_passed++; }
}

void launcher_start(void)
{
    bsp_display_lock(0);
    s_face = new_screen();
    lv_obj_add_event_cb(s_face, gesture_cb, LV_EVENT_GESTURE, NULL);
    face_create(s_face);
    build_grid();
    lv_screen_load(s_face);
    s_last_act = lv_tick_get();
    s_idle_tmr = lv_timer_create(idle_cb, 250, NULL);
    overlay_start();
    lv_indev_t *in = bsp_display_get_input_dev();
    if (in) lv_indev_set_read_cb(in, read_wrap);
    bsp_display_brightness_set(bright());      // under the LVGL lock: never race a screen flush on the SPI bus
    bsp_display_unlock();
}

// BOOT-button actions (called from buttons.c task)
void launcher_btn_short(void)
{
    bsp_display_lock(0);
    if (!s_on || s_dim) launcher_wake();
    else if (s_state == ST_FACE) screen_set(false);     // on the watch face, the button = screen off
    else if (s_state == ST_APP && s_cur >= APP_COUNT && script_button()) s_last_act = lv_tick_get();   // the script used it
    else { s_last_act = lv_tick_get(); launcher_back(); }
    bsp_display_unlock();
}
void launcher_btn_long(void)
{
    bsp_display_lock(0);
    if (s_on) screen_set(false); else launcher_wake();
    bsp_display_unlock();
}
void amber_hold(bool down);
void launcher_btn_hold(bool down)              // hold = talk to Amber while held
{
    diag_event("BTN hold %s", down ? "down" : "up");
    bsp_display_lock(0);
    launcher_wake();
    amber_hold(down);
    bsp_display_unlock();
}

// Opens app idx from anywhere (face, grid or another app; the other app is stopped first, freeing its mic).
void launcher_show_app(int idx)
{
    if (s_state == ST_APP && s_cur == idx) return;
    if (s_state != ST_GRID) launcher_home();
    s_state = ST_GRID;
    launcher_open(idx);
}

void launcher_btn_double(void)                 // double press = start/stop a meeting recording
{
    diag_event("BTN double (recording %s)", rec_active() ? "stop" : "start");
    bsp_display_lock(0);
    launcher_wake();
    bool was = rec_active();
    if (rec_toggle(REC_MEETING)) ui_msg(was ? "Recording saved" : "Recording started");
    else if (was) ui_msg("Stop failed");
    else {
        char who[16], what[40], m[96];
        uint64_t tot, fre; hw_storage_info(&tot, &fre);
        if (fre < 3ULL * 1024 * 1024)
            snprintf(m, sizeof(m), "Storage full (%.1f MB free). Save recordings to the phone: Companion > Meeting notes", fre / 1048576.0);
        else if (hw_mic_owner(who, sizeof(who), what, sizeof(what), NULL, NULL)) snprintf(m, sizeof(m), "Mic in use: %s (%s)", who, what);
        else snprintf(m, sizeof(m), "Could not start recording");
        ui_msg(m);
    }
    bsp_display_unlock();
}

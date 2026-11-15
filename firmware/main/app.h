#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- app registry: each app = start(screen) + stop() ----
typedef struct {
    const char *name;
    const char *sym;        // LV_SYMBOL_* shown on the grid icon
    uint32_t color;         // icon background 0xRRGGBB
    void (*start)(lv_obj_t *scr);
    void (*stop)(void);
} app_t;

extern const app_t APPS[];
extern const int APP_COUNT;

// ---- launcher ----
void launcher_start(void);
void launcher_open(int idx);
void launcher_home(void);        // app grid
void launcher_face(void);        // watch face
void launcher_back(void);        // one level up (app -> grid -> face)
void launcher_wake(void);        // light the screen / reset idle timer
void launcher_synth(bool active, bool pressed, int x, int y);   // test hook: fake finger
int  launcher_state(void);       // 0 face, 1 grid, 2 app
int  launcher_cur(void);
int  launcher_active_screen(void);   // which screen LVGL really shows: 0 face 1 grid 2 app 9 other
bool launcher_dim(void);
unsigned touch_read_errors(void);
void launcher_syn_stats(unsigned *reads, unsigned *passed);
void launcher_touch_stats(unsigned *reads, unsigned *presses);   // real-controller reads / reads that reported a finger
unsigned touch_read_max_ms(void);   // slowest touch-controller read so far   // failed (and ignored) touch-controller reads
void launcher_gesture_dbg(unsigned *n, int *dir, unsigned *swallowed);
bool launcher_screen_on(void);

// ---- power manager (power.c) ----
typedef struct { int64_t ms_active, ms_doze; uint32_t wakes, deep_count; bool sleep_allowed, screen_on; } power_stats_t;
void power_init(void);
void power_start(void);
void power_update(bool screen_on);
bool power_sleep_allowed(void);
void power_deep_sleep(uint32_t timer_s);
void power_stats(power_stats_t *s);
int  power_wake_cause(void);

// ---- Ember scripted apps (script.c) ----
#define EMBER_VERSION "1.1"
void script_init(void);                 // install sample apps on first boot, scan /flash/apps
int  script_count(void);
const char *script_name(int i);
const char *script_icon(int i);
uint32_t script_color(int i);
void script_start(int i, lv_obj_t *scr);
void script_stop(void);
bool script_button(void);               // BOOT short press inside a script app; true = consumed
void script_pause(bool paused);         // screen off/on: app timers stop while dark
void script_rescan(void);
bool script_put_begin(const char *name);   // upload: begin, data..., end (atomic rename)
bool script_put_data(const void *d, size_t n);
bool script_put_end(void);
const char *script_put_pending(void);   // name of the app being uploaded ("" if none)
void script_put_abort(void);
bool script_remove(const char *name);
void launcher_rebuild_grid(void);
void launcher_raise(void);              // wrist raised (from the IMU task): light the screen if it is dark
// ---- recorder extras ----
void rec_mark(void);                    // bookmark "this moment" in the current recording
void rec_stats(uint32_t *frames, uint32_t *dropped, uint32_t *rate);   // of the last/current recording
int  rec_marks_for(const char *wav_name, uint32_t *out, int max);      // bookmarks (ms) saved next to a recording
// ---- battery ----
void power_drain_text(char *out, size_t n);   // "On battery 2 h 10 min, used 7 % (3.2 %/h, ~31 h total)"
void launcher_open_script(const char *name);

// ---- buttons ----
void buttons_start(void);
void launcher_btn_short(void);
void launcher_btn_long(void);
void launcher_btn_double(void);
void launcher_btn_hold(bool down);
void launcher_show_app(int idx);   // open an app from anywhere

// ---- watch face widgets (face.c) ----
void face_create(lv_obj_t *scr);
void face_tick(void);

// ---- shared recorder service (rec.c) ----
typedef enum { REC_MEETING = 0, REC_MUSIC = 1 } rec_mode_t;
bool     rec_active(void);
uint32_t rec_seconds(void);
rec_mode_t rec_mode(void);
int      rec_level(void);        // 0..100 live input level
bool     rec_start(rec_mode_t m);
void     rec_stop(void);
bool     rec_pause(bool on);     // pause hands the mic to someone else; the file stays open, resume continues it
bool     rec_paused(void);
bool     rec_toggle(rec_mode_t m);
const char *rec_last_file(void);

// ---- shared UI helpers (ui.c) ----
#define C_BG     0x000000
#define C_TEXT   0xFFFFFF
#define C_DIM    0x8A8A8A
#define C_ACCENT 0x2EC4B6
#define C_WARN   0xFF4D4D
#define C_OK     0x4ADE80
lv_obj_t *ui_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt);
lv_obj_t *ui_title(lv_obj_t *scr, const char *txt);
lv_obj_t *ui_btn(lv_obj_t *p, const char *txt, int w, int h, uint32_t color, lv_event_cb_t cb, void *ud);
void ui_msg(const char *txt);
void overlay_start(void);            // starts the overlay timer (launcher_start)
void ui_passkey(int code);           // show the 6-digit pairing code (code < 0 hides it); any task
int  ui_checkin(const char *head, const char *body, const char *amber_ctx, int timeout_s);   // 0 on track, 1 off track, 2 snooze, -1 none; non-LVGL task
void ui_checkin_cancel(void);       // close an open check-in prompt; ui_checkin then returns -2
bool ui_confirm(const char *question, int timeout_s);   // Allow/Deny on the watch; call from a non-LVGL task
void *ui_ud(lv_event_t *e);          // user data of a ui_btn() callback
void cmd_start(void);                // USB serial dev shell
void usj_end_packet(void);           // make the PC see a reply whose length is a multiple of 64 bytes

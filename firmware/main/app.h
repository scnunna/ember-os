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
#define EMBER_VERSION "1.0"
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
bool script_remove(const char *name);
void launcher_rebuild_grid(void);
void launcher_open_script(const char *name);

// ---- buttons ----
void buttons_start(void);
void launcher_btn_short(void);
void launcher_btn_long(void);
void launcher_btn_double(void);

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
void *ui_ud(lv_event_t *e);          // user data of a ui_btn() callback
void cmd_start(void);                // USB serial dev shell

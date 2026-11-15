// Ember OS scripted apps: Lua 5.4 programs in /flash/apps/<name>.lua, pushed over USB (tools/ember.py) or the
// watch's own Wi-Fi hotspot. No reflashing needed.
//
// Power rules the OS enforces (apps cannot opt out):
//   * apps are event-driven: they run only inside start(), a button/tap callback or a w.every() timer
//   * timers are >= 100 ms and are paused automatically while the screen is dark
//   * a callback that runs longer than 250 ms is aborted (runaway loops can't drain the battery)
//   * no file, OS or network access; one small key/value store per app; Lua heap capped at 256 KB (PSRAM)
//
// Coordinates: the screen is 410 x 502. Every widget is placed by its CENTRE (cx, cy); cx = nil means centred.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "app.h"
#include "hw.h"

static const char *TAG = "ember";
#define APP_DIR   "/flash/apps"
#define MAX_APPS  24
#define MAX_OBJS  64
#define MAX_TMRS  8
#define HEAP_CAP  (256 * 1024)
#define CB_BUDGET_US 250000

typedef struct { char name[24]; const char *icon; uint32_t color; } script_info_t;
static script_info_t s_apps[MAX_APPS];
static int s_n;

static lua_State *L;
static lv_obj_t *s_scr, *s_err;
static lv_obj_t *s_objs[MAX_OBJS + 1];
static struct { lv_timer_t *t; int ref; } s_tmr[MAX_TMRS];
static int s_cur = -1;
static size_t s_heap;
static int64_t s_cb_t0;

// Numbers from apps: accept fractions (e.g. 205 - x * 128) and round, instead of erroring like luaL_checkinteger.
static int chk_num(lua_State *l, int i) { return (int)lroundf((float)luaL_checknumber(l, i)); }
static int opt_num(lua_State *l, int i, int def) { return lua_isnoneornil(l, i) ? def : chk_num(l, i); }

// ------------------------------------------------------------------ registry of apps on flash
static const struct { const char *k; const char *sym; } ICONS[] = {
    {"audio", LV_SYMBOL_AUDIO}, {"bell", LV_SYMBOL_BELL}, {"bluetooth", LV_SYMBOL_BLUETOOTH}, {"charge", LV_SYMBOL_CHARGE},
    {"eye", LV_SYMBOL_EYE_OPEN}, {"gps", LV_SYMBOL_GPS}, {"home", LV_SYMBOL_HOME}, {"image", LV_SYMBOL_IMAGE},
    {"list", LV_SYMBOL_LIST}, {"loop", LV_SYMBOL_LOOP}, {"play", LV_SYMBOL_PLAY}, {"plus", LV_SYMBOL_PLUS},
    {"refresh", LV_SYMBOL_REFRESH}, {"settings", LV_SYMBOL_SETTINGS}, {"shuffle", LV_SYMBOL_SHUFFLE}, {"tint", LV_SYMBOL_TINT},
    {"up", LV_SYMBOL_UP}, {"edit", LV_SYMBOL_EDIT}, {"file", LV_SYMBOL_FILE}, {"wifi", LV_SYMBOL_WIFI},
    {"battery", LV_SYMBOL_BATTERY_FULL}, {"warning", LV_SYMBOL_WARNING}, {"ok", LV_SYMBOL_OK}, {"power", LV_SYMBOL_POWER},
};

static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 20) return false;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void read_header(script_info_t *a, const char *path)
{
    a->icon = LV_SYMBOL_FILE; a->color = 0x3D405B;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[96];
    for (int i = 0; i < 6 && fgets(line, sizeof(line), f); i++) {
        char v[32];
        if (sscanf(line, "-- icon: %31s", v) == 1)
            for (unsigned k = 0; k < sizeof(ICONS) / sizeof(ICONS[0]); k++) if (!strcmp(ICONS[k].k, v)) a->icon = ICONS[k].sym;
        if (sscanf(line, "-- color: %31s", v) == 1) a->color = strtoul(v[0] == '#' ? v + 1 : v, NULL, 16);
    }
    fclose(f);
}

static int cmp_app(const void *a, const void *b) { return strcmp(((script_info_t *)a)->name, ((script_info_t *)b)->name); }

void script_rescan(void)
{
    s_n = 0;
    DIR *d = opendir(APP_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_n < MAX_APPS) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".lua")) continue;
        char nm[24]; snprintf(nm, sizeof(nm), "%.*s", (int)(l - 4), e->d_name);
        for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;     // FAT may report upper case
        if (!valid_name(nm)) continue;
        strcpy(s_apps[s_n].name, nm);
        char path[64]; snprintf(path, sizeof(path), APP_DIR "/%s", e->d_name);
        read_header(&s_apps[s_n], path);
        s_n++;
    }
    closedir(d);
    qsort(s_apps, s_n, sizeof(s_apps[0]), cmp_app);
}
int script_count(void) { return s_n; }
const char *script_name(int i) { return (i >= 0 && i < s_n) ? s_apps[i].name : ""; }
const char *script_icon(int i) { return (i >= 0 && i < s_n) ? s_apps[i].icon : ""; }
uint32_t script_color(int i) { return (i >= 0 && i < s_n) ? s_apps[i].color : 0; }

// ------------------------------------------------------------------ sample apps (installed once)
static const char COUNTER[] =
    "-- icon: plus\n-- color: E07A5F\n"
    "-- Tally counter: + / - buttons, the BOOT button resets it. Survives reboots.\n"
    "local n = w.load('n', 0)\nlocal lbl\n"
    "local function show() w.text(lbl, tostring(n)); w.save('n', n) end\n"
    "function start()\n  w.title('Counter')\n  lbl = w.label(tostring(n), nil, 200, 48)\n"
    "  w.button('-', 120, 360, 130, 90, 0x444444, function() if n > 0 then n = n - 1 end show() end)\n"
    "  w.button('+', 290, 360, 130, 90, 0xE07A5F, function() n = n + 1; show() end)\nend\n"
    "function on_button()\n  if n == 0 then return false end\n  n = 0; show(); return true\nend\n";
static const char LEVEL[] =
    "-- icon: tint\n-- color: 3A86FF\n"
    "-- Bubble level from the motion sensor, 10 updates per second (only while the screen is on).\n"
    "local dot, txt\n"
    "function start()\n  w.title('Level')\n  w.circle(205, 260, 300, 0x555555, true)\n  w.circle(205, 260, 70, 0x333333, true)\n"
    "  dot = w.circle(205, 260, 44, 0xEDAE49)\n  txt = w.label('', nil, 440, 20, 0x8A8A8A)\n  w.every(100, tick)\nend\n"
    "function tick()\n  local x, y = w.imu()\n  x = math.max(-1, math.min(1, x)); y = math.max(-1, math.min(1, y))\n"
    "  w.move(dot, 205 - x * 128, 260 + y * 128)\n"
    "  w.text(txt, string.format('%.1f  /  %.1f deg', math.deg(math.asin(x)), math.deg(math.asin(y))))\n"
    "  w.color(dot, (math.abs(x) < 0.03 and math.abs(y) < 0.03) and 0x4ADE80 or 0xEDAE49)\nend\n";
static const char DICE[] =
    "-- icon: shuffle\n-- color: 8E6CEF\n"
    "-- Dice: tap Roll or give the watch a shake.\n"
    "local face, info, rolls = nil, nil, 0\n"
    "local function roll()\n  rolls = rolls + 1\n  w.text(face, tostring(math.random(1, 6)))\n"
    "  w.text(info, 'roll #' .. rolls)\n  w.beep(880, 40)\nend\n"
    "function start()\n  w.title('Dice')\n  face = w.label('?', nil, 200, 48)\n  info = w.label('tap Roll or shake', nil, 270, 20, 0x8A8A8A)\n"
    "  w.button('Roll', nil, 390, 200, 80, 0x8E6CEF, roll)\n"
    "  w.every(150, function()\n    local x, y, z = w.imu()\n    if x * x + y * y + z * z > 4 then roll() end\n  end)\nend\n";

static void write_file(const char *name, const char *src)
{
    char p[64]; snprintf(p, sizeof(p), APP_DIR "/%s.lua", name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(src, f); fclose(f); }
}

void script_init(void)
{
    struct stat st;
    if (stat(APP_DIR, &st) != 0) {
        mkdir(APP_DIR, 0775);
        write_file("counter", COUNTER);
        write_file("level", LEVEL);
        write_file("dice", DICE);
        ESP_LOGI(TAG, "installed sample apps");
    }
    script_rescan();
    ESP_LOGI(TAG, "%d scripted apps", s_n);
}

// ------------------------------------------------------------------ runtime helpers
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    if (ptr == NULL) osize = 0;
    if (nsize == 0) { heap_caps_free(ptr); s_heap -= osize; return NULL; }
    if (s_heap - osize + nsize > HEAP_CAP) return NULL;          // Lua raises "not enough memory"
    void *np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
    if (np) s_heap = s_heap - osize + nsize;
    return np;
}

static void budget_hook(lua_State *l, lua_Debug *ar)
{
    if (esp_timer_get_time() - s_cb_t0 > CB_BUDGET_US) luaL_error(l, "app used the CPU for more than 250 ms (power limit)");
}

static void stop_timers(void)
{
    for (int i = 0; i < MAX_TMRS; i++)
        if (s_tmr[i].t) { lv_timer_delete(s_tmr[i].t); s_tmr[i].t = NULL; if (L) luaL_unref(L, LUA_REGISTRYINDEX, s_tmr[i].ref); }
}

static void show_error(const char *msg)
{
    ESP_LOGE(TAG, "%s: %s", script_name(s_cur), msg);
    printf("EMBER ERROR %s: %s\n", script_name(s_cur), msg);
    stop_timers();
    if (!s_scr) return;
    if (!s_err) {
        s_err = ui_label(s_scr, &lv_font_montserrat_14, C_WARN, "");
        lv_obj_set_width(s_err, 360);
        lv_label_set_long_mode(s_err, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_bg_color(s_err, lv_color_hex(0x200000), 0);
        lv_obj_set_style_bg_opa(s_err, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_err, 8, 0);
        lv_obj_align(s_err, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    lv_label_set_text(s_err, msg);
}

// Call the function on top of the stack with nargs arguments; returns true if it returned a truthy value.
static bool call(int nargs)
{
    s_cb_t0 = esp_timer_get_time();
    if (lua_pcall(L, nargs, 1, 0) != LUA_OK) {
        show_error(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    bool r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

static bool call_global(const char *fn)
{
    if (!L) return false;
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) { lua_pop(L, 1); return false; }
    return call(0);
}

static lv_obj_t *obj_arg(lua_State *l, int i)
{
    int h = (int)chk_num(l, i);
    if (h < 1 || h > MAX_OBJS || !s_objs[h]) luaL_error(l, "bad widget handle %d", h);
    return s_objs[h];
}
static int add_obj(lua_State *l, lv_obj_t *o)
{
    for (int h = 1; h <= MAX_OBJS; h++) if (!s_objs[h]) { s_objs[h] = o; lua_pushinteger(l, h); return 1; }
    lv_obj_delete(o);
    return luaL_error(l, "too many widgets (max %d)", MAX_OBJS);
}
static void place(lv_obj_t *o, lua_State *l, int ix, int iy)
{
    int cx = opt_num(l, ix, 205), cy = opt_num(l, iy, 251);
    lv_obj_align(o, LV_ALIGN_CENTER, cx - 205, cy - 251);
}
static const lv_font_t *font(int size)
{
    if (size >= 40) return &lv_font_montserrat_48;
    if (size >= 25) return &lv_font_montserrat_28;
    if (size >= 18) return &lv_font_montserrat_20;
    return &lv_font_montserrat_14;
}

// ------------------------------------------------------------------ the `w` API
static int w_title(lua_State *l) { ui_title(s_scr, luaL_checkstring(l, 1)); return 0; }

static int w_label(lua_State *l)
{
    lv_obj_t *o = ui_label(s_scr, font(opt_num(l, 4, 20)), opt_num(l, 5, C_TEXT), luaL_checkstring(l, 1));
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    place(o, l, 2, 3);
    return add_obj(l, o);
}

static void btn_cb(lv_event_t *e)
{
    int ref = (int)(intptr_t)ui_ud(e);      // ui_btn wraps user data in its own context (tap-vs-swipe filter)
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    call(0);
}
static int w_button(lua_State *l)
{
    const char *t = luaL_checkstring(l, 1);
    int bw = opt_num(l, 4, 160), bh = opt_num(l, 5, 70);
    uint32_t col = opt_num(l, 6, C_ACCENT);
    luaL_checktype(l, 7, LUA_TFUNCTION);
    lua_pushvalue(l, 7);
    int ref = luaL_ref(l, LUA_REGISTRYINDEX);
    lv_obj_t *b = ui_btn(s_scr, t, bw, bh, col, btn_cb, (void *)(intptr_t)ref);
    place(b, l, 2, 3);
    return add_obj(l, b);
}

static int w_circle(lua_State *l)
{
    int d = chk_num(l, 3);
    uint32_t col = opt_num(l, 4, C_ACCENT);
    bool ring = lua_toboolean(l, 5);
    lv_obj_t *o = lv_obj_create(s_scr);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, ring ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(o, ring ? 3 : 0, 0);
    place(o, l, 1, 2);
    return add_obj(l, o);
}

static int w_bar(lua_State *l)
{
    lv_obj_t *o = lv_bar_create(s_scr);
    lv_obj_set_size(o, opt_num(l, 3, 300), opt_num(l, 4, 20));
    lv_bar_set_range(o, 0, opt_num(l, 5, 100));
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    place(o, l, 1, 2);
    return add_obj(l, o);
}

static int w_value(lua_State *l)
{
    lv_obj_t *o = obj_arg(l, 1);
    if (lv_obj_check_type(o, &lv_bar_class)) lv_bar_set_value(o, chk_num(l, 2), LV_ANIM_OFF);
    return 0;
}
static int w_text(lua_State *l)
{
    lv_obj_t *o = obj_arg(l, 1);
    const char *t = luaL_tolstring(l, 2, NULL);
    if (lv_obj_check_type(o, &lv_label_class)) lv_label_set_text(o, t);
    else if (lv_obj_get_child_count(o) && lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class)) lv_label_set_text(lv_obj_get_child(o, 0), t);
    return 0;
}
static int w_color(lua_State *l)
{
    lv_obj_t *o = obj_arg(l, 1);
    lv_color_t c = lv_color_hex(chk_num(l, 2));
    if (lv_obj_check_type(o, &lv_label_class)) lv_obj_set_style_text_color(o, c, 0);
    else { lv_obj_set_style_bg_color(o, c, 0); lv_obj_set_style_border_color(o, c, 0); }
    return 0;
}
static int w_move(lua_State *l) { place(obj_arg(l, 1), l, 2, 3); return 0; }
static int w_show(lua_State *l)
{
    lv_obj_t *o = obj_arg(l, 1);
    if (lua_toboolean(l, 2)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

static void tmr_cb(lv_timer_t *t)
{
    int i = (int)(intptr_t)lv_timer_get_user_data(t);
    if (!L || s_tmr[i].t != t) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_tmr[i].ref);
    call(0);
}
static int w_every(lua_State *l)
{
    int ms = chk_num(l, 1);
    if (ms < 100) ms = 100;                          // power rule
    luaL_checktype(l, 2, LUA_TFUNCTION);
    for (int i = 0; i < MAX_TMRS; i++)
        if (!s_tmr[i].t) {
            lua_pushvalue(l, 2);
            s_tmr[i].ref = luaL_ref(l, LUA_REGISTRYINDEX);
            s_tmr[i].t = lv_timer_create(tmr_cb, ms, (void *)(intptr_t)i);
            lua_pushinteger(l, i + 1);
            return 1;
        }
    return luaL_error(l, "too many timers (max %d)", MAX_TMRS);
}
static int w_cancel(lua_State *l)
{
    int i = (int)chk_num(l, 1) - 1;
    if (i >= 0 && i < MAX_TMRS && s_tmr[i].t) { lv_timer_delete(s_tmr[i].t); s_tmr[i].t = NULL; luaL_unref(l, LUA_REGISTRYINDEX, s_tmr[i].ref); }
    return 0;
}

static int w_time(lua_State *l)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return 0;
    lua_pushinteger(l, t.tm_hour); lua_pushinteger(l, t.tm_min); lua_pushinteger(l, t.tm_sec); lua_pushinteger(l, t.tm_wday);
    return 4;
}
static int w_date(lua_State *l)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return 0;
    lua_pushinteger(l, t.tm_year + 1900); lua_pushinteger(l, t.tm_mon + 1); lua_pushinteger(l, t.tm_mday);
    return 3;
}
static int w_steps(lua_State *l) { lua_pushinteger(l, hw_steps_today()); return 1; }
static int w_battery(lua_State *l)
{
    batt_t b; hw_batt(&b);
    lua_pushinteger(l, b.pct); lua_pushinteger(l, b.mv); lua_pushboolean(l, b.charging); lua_pushboolean(l, b.vbus);
    return 4;
}
static int w_imu(lua_State *l)
{
    float x, y, z; hw_imu_get_g(&x, &y, &z);
    lua_pushnumber(l, x); lua_pushnumber(l, y); lua_pushnumber(l, z);
    return 3;
}
static int w_beep(lua_State *l)
{
    int hz = opt_num(l, 1, 1000), ms = opt_num(l, 2, 80);
    if (ms > 1000) ms = 1000;
    hw_beep(hz, ms, hw_pref_get("vol", 70));
    return 0;
}
static int w_buzz(lua_State *l)
{
    int ms = opt_num(l, 1, 60), count = opt_num(l, 2, 1), gap = opt_num(l, 3, 120);
    hw_buzz_pattern(count < 1 ? 1 : (count > 5 ? 5 : count), ms < 10 ? 10 : (ms > 800 ? 800 : ms), gap);
    return 0;
}
static int w_toast(lua_State *l) { ui_msg(luaL_checkstring(l, 1)); return 0; }
static int w_millis(lua_State *l) { lua_pushinteger(l, (lua_Integer)(esp_timer_get_time() / 1000)); return 1; }

// Per-app key/value store: /flash/apps/<name>.kv, one "key=n:123" or "key=s:text" per line.
static void kv_path(char *p, size_t n) { snprintf(p, n, APP_DIR "/%s.kv", script_name(s_cur)); }
static void kv_table(lua_State *l)
{
    if (lua_getfield(l, LUA_REGISTRYINDEX, "ember_kv") == LUA_TTABLE) return;
    lua_pop(l, 1);
    lua_newtable(l);
    char p[64]; kv_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (f) {
        char line[200];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq || strlen(eq) < 3 || eq[2] != ':') continue;
            *eq = 0;
            if (eq[1] == 'n') lua_pushnumber(l, strtof(eq + 3, NULL)); else lua_pushstring(l, eq + 3);
            lua_setfield(l, -2, line);
        }
        fclose(f);
    }
    lua_pushvalue(l, -1);
    lua_setfield(l, LUA_REGISTRYINDEX, "ember_kv");
}
static int w_load(lua_State *l)
{
    const char *k = luaL_checkstring(l, 1);
    kv_table(l);
    if (lua_getfield(l, -1, k) == LUA_TNIL) { lua_pop(l, 1); lua_pushvalue(l, 2); }
    else if (lua_type(l, -1) == LUA_TNUMBER) {                       // whole numbers come back as integers
        lua_Number v = lua_tonumber(l, -1);
        if (v == (lua_Integer)v) { lua_pop(l, 1); lua_pushinteger(l, (lua_Integer)v); }
    }
    return 1;
}
static int w_save(lua_State *l)
{
    const char *k = luaL_checkstring(l, 1);
    if (strlen(k) > 40 || strpbrk(k, "=\n")) return luaL_error(l, "bad key");
    kv_table(l);
    lua_pushvalue(l, 2);
    lua_setfield(l, -2, k);
    char p[64]; kv_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return 0;
    lua_pushnil(l);
    while (lua_next(l, -2)) {
        if (lua_type(l, -2) == LUA_TSTRING) {
            if (lua_type(l, -1) == LUA_TNUMBER) fprintf(f, "%s=n:%.9g\n", lua_tostring(l, -2), (double)lua_tonumber(l, -1));
            else if (lua_type(l, -1) == LUA_TSTRING) { const char *s = lua_tostring(l, -1); if (!strchr(s, '\n')) fprintf(f, "%s=s:%s\n", lua_tostring(l, -2), s); }
        }
        lua_pop(l, 1);
    }
    fclose(f);
    return 0;
}
static int w_print(lua_State *l)
{
    int n = lua_gettop(l);
    printf("EMBER %s:", script_name(s_cur));
    for (int i = 1; i <= n; i++) printf(" %s", luaL_tolstring(l, i, NULL)), lua_pop(l, 1);
    printf("\n");
    return 0;
}

static const luaL_Reg W[] = {
    {"title", w_title}, {"label", w_label}, {"button", w_button}, {"circle", w_circle}, {"bar", w_bar},
    {"value", w_value}, {"text", w_text}, {"color", w_color}, {"move", w_move}, {"show", w_show},
    {"every", w_every}, {"cancel", w_cancel}, {"time", w_time}, {"date", w_date}, {"steps", w_steps},
    {"battery", w_battery}, {"imu", w_imu}, {"beep", w_beep}, {"buzz", w_buzz}, {"toast", w_toast}, {"millis", w_millis},
    {"load", w_load}, {"save", w_save}, {NULL, NULL},
};

// ------------------------------------------------------------------ lifecycle (all under the LVGL lock)
void script_start(int i, lv_obj_t *scr)
{
    script_stop();
    s_cur = i; s_scr = scr; s_err = NULL; s_heap = 0;
    memset(s_objs, 0, sizeof(s_objs));
    L = lua_newstate(l_alloc, NULL);
    if (!L) { s_scr = scr; show_error("out of memory"); return; }
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}};
    for (unsigned k = 0; k < sizeof(libs) / sizeof(libs[0]); k++) { luaL_requiref(L, libs[k].name, libs[k].func, 1); lua_pop(L, 1); }
    lua_pushnil(L); lua_setglobal(L, "dofile");      // sandbox: no file access
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, w_print); lua_setglobal(L, "print");
    luaL_newlib(L, W);
    lua_setglobal(L, "w");
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 5000);

    char path[64]; snprintf(path, sizeof(path), APP_DIR "/%s.lua", script_name(i));
    s_cb_t0 = esp_timer_get_time();
    if (luaL_loadfile(L, path) != LUA_OK) { show_error(lua_tostring(L, -1)); lua_pop(L, 1); return; }
    if (!call(0) && s_err) return;
    if (!call_global("start") && s_err) return;
    if (!lv_obj_get_child_count(scr)) ui_title(scr, script_name(i));
    printf("EMBER started %s (lua heap %u B)\n", script_name(i), (unsigned)s_heap);
}

void script_stop(void)
{
    if (L) {
        call_global("on_stop");
        stop_timers();
        lua_close(L);
        L = NULL;
    }
    s_scr = NULL; s_err = NULL; s_cur = -1;
    memset(s_objs, 0, sizeof(s_objs));
}

bool script_button(void) { return L && call_global("on_button"); }

void script_pause(bool paused)
{
    for (int i = 0; i < MAX_TMRS; i++)
        if (s_tmr[i].t) { if (paused) lv_timer_pause(s_tmr[i].t); else lv_timer_resume(s_tmr[i].t); }
}

// ------------------------------------------------------------------ install / remove (USB shell + Wi-Fi upload)
static FILE *s_put;
static char s_put_name[24];
bool script_put_begin(const char *name)
{
    if (!valid_name(name)) return false;
    mkdir(APP_DIR, 0775);
    if (s_put) fclose(s_put);
    char p[64]; snprintf(p, sizeof(p), APP_DIR "/%s.tmp", name);
    s_put = fopen(p, "w");
    strcpy(s_put_name, name);
    return s_put != NULL;
}
bool script_put_data(const void *d, size_t n) { return s_put && fwrite(d, 1, n, s_put) == n; }
bool script_put_end(void)
{
    if (!s_put) return false;
    fclose(s_put); s_put = NULL;
    char tmp[64], dst[64];
    snprintf(tmp, sizeof(tmp), APP_DIR "/%s.tmp", s_put_name);
    snprintf(dst, sizeof(dst), APP_DIR "/%s.lua", s_put_name);
    bsp_display_lock(0);
    if (s_cur >= 0 && !strcmp(script_name(s_cur), s_put_name)) launcher_face();   // replacing the running app
    remove(dst);
    bool ok = rename(tmp, dst) == 0;
    script_rescan();
    launcher_rebuild_grid();
    bsp_display_unlock();
    return ok;
}
bool script_remove(const char *name)
{
    if (!valid_name(name)) return false;
    char p[64]; snprintf(p, sizeof(p), APP_DIR "/%s.lua", name);
    bsp_display_lock(0);
    if (s_cur >= 0 && !strcmp(script_name(s_cur), name)) launcher_face();
    bool ok = remove(p) == 0;
    snprintf(p, sizeof(p), APP_DIR "/%s.kv", name);
    remove(p);
    script_rescan();
    launcher_rebuild_grid();
    bsp_display_unlock();
    return ok;
}

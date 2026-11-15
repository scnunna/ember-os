// USB serial command shell for development / end-to-end tests (no effect unless the PC sends a command).
//   settime Y M D h m s   set the RTC (the flash scripts do this automatically)
//   layout                open every screen and report overlapping / off-screen widgets
//   nav N                 random navigation stress test (N steps)
//   status                clock, battery, heap
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "sched.h"
#include "hw.h"
#include "link.h"
#include "ble.h"
#include "devices.h"

static bool inter(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}
static bool contains(const lv_area_t *o, const lv_area_t *i)
{
    return o->x1 <= i->x1 && o->y1 <= i->y1 && o->x2 >= i->x2 && o->y2 >= i->y2;
}
static const char *desc(lv_obj_t *o, char *b, size_t n)
{
    const char *t = "";
    if (lv_obj_check_type(o, &lv_label_class)) t = lv_label_get_text(o);
    else if (lv_obj_get_child_count(o) && lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class)) t = lv_label_get_text(lv_obj_get_child(o, 0));
    snprintf(b, n, "\"%.24s\"", t);
    return b;
}

// Returns the number of problems found on the active screen. Must hold the LVGL lock.
static int check_screen(const char *name)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    int bad = 0;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *a = lv_obj_get_child(scr, i);
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t A; lv_obj_get_coords(a, &A);
        char da[40];
        if (lv_area_get_width(&A) <= 0 || lv_area_get_height(&A) <= 0) continue;     // empty label
        if (A.x1 < 0 || A.y1 < 0 || A.x2 > 409 || A.y2 > 501) {
            printf("LAYOUT %s: OFFSCREEN %s x=%d..%d y=%d..%d\n", name, desc(a, da, sizeof(da)), (int)A.x1, (int)A.x2, (int)A.y1, (int)A.y2);
            bad++;
        }
        for (uint32_t j = i + 1; j < n; j++) {
            lv_obj_t *b = lv_obj_get_child(scr, j);
            if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_area_t B; lv_obj_get_coords(b, &B);
            if (lv_area_get_width(&B) <= 0 || lv_area_get_height(&B) <= 0) continue;
            if (inter(&A, &B) && !contains(&A, &B) && !contains(&B, &A)) {
                char db[40];
                printf("LAYOUT %s: OVERLAP %s [%d,%d..%d,%d]  with  %s [%d,%d..%d,%d]\n", name, desc(a, da, sizeof(da)), (int)A.x1, (int)A.y1, (int)A.x2, (int)A.y2,
                       desc(b, db, sizeof(db)), (int)B.x1, (int)B.y1, (int)B.x2, (int)B.y2);
                bad++;
            }
        }
    }
    return bad;
}

static void cmd_layout(void)
{
    int total = 0;
    printf("LAYOUT start\n");
    bsp_display_lock(0); printf("LAYOUT got lock\n"); launcher_wake(); launcher_face(); bsp_display_unlock();
    printf("LAYOUT face loaded\n");
    vTaskDelay(pdMS_TO_TICKS(600));
    bsp_display_lock(0); total += check_screen("face"); launcher_home(); bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    bsp_display_lock(0); total += check_screen("grid"); bsp_display_unlock();
    for (int i = 0; i < APP_COUNT; i++) {
        printf("LAYOUT open %s\n", APPS[i].name);
        bsp_display_lock(0); launcher_open(i); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(700));
        bsp_display_lock(0); total += check_screen(APPS[i].name); launcher_home(); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("LAYOUT DONE problems=%d\n", total);
}

static void cmd_nav(int steps)
{
    size_t heap0 = esp_get_free_heap_size(), ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    for (int i = 0; i < steps; i++) {
        bsp_display_lock(0);
        launcher_wake();
        switch (esp_random() % 5) {
        case 0: launcher_home(); break;
        case 1: launcher_face(); break;
        case 2: launcher_back(); break;
        default: launcher_open(esp_random() % APP_COUNT); break;   // ignored unless on the grid
        }
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(20 + esp_random() % 260));
    }
    vTaskDelay(pdMS_TO_TICKS(800));
    printf("NAV DONE steps=%d heap %u->%u psram %u->%u\n", steps, (unsigned)heap0, (unsigned)esp_get_free_heap_size(), (unsigned)ps0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ---- synthetic touch: goes through the real LVGL input pipeline (read_cb -> press/release/gesture/click) ----
// The touch driver runs the indev in event mode (reads only on touch interrupts), so each fake sample must trigger a read.
static void feed(bool active, bool pressed, int x, int y)
{
    launcher_synth(active, pressed, x, y);
    lv_indev_t *in = bsp_display_get_input_dev();
    if (!bsp_display_lock(2000)) { printf("TOUCH step lvgl lock timeout in feed\n"); return; }
    if (in) lv_indev_read(in);
    bsp_display_unlock();
}
static void finger(int x1, int y1, int x2, int y2, int steps)
{
    printf("TOUCH step finger (%d,%d)->(%d,%d)\n", x1, y1, x2, y2);
    feed(true, true, x1, y1);
    vTaskDelay(pdMS_TO_TICKS(70));
    for (int i = 1; i <= steps; i++) {
        feed(true, true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        if (i < steps) vTaskDelay(pdMS_TO_TICKS(30));          // release right after the last move, like a real flick
    }
    feed(true, false, x2, y2);
    vTaskDelay(pdMS_TO_TICKS(120));
    feed(false, false, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(700));
}
static void tap(int x, int y) { finger(x, y, x, y, 1); }

static int rtc_minutes(void)
{
    struct tm t;
    return hw_rtc_get(&t) ? t.tm_hour * 60 + t.tm_min : -1;
}
static void wait_safe_second(void)      // avoid a natural minute rollover in the middle of a clock test
{
    struct tm t;
    while (hw_rtc_get(&t) && (t.tm_sec < 3 || t.tm_sec > 40)) vTaskDelay(pdMS_TO_TICKS(500));
}

static int s_pass, s_fail;
static void check(bool ok, const char *what)
{
    unsigned gn, sw, sr, sp; int gd;
    launcher_gesture_dbg(&gn, &gd, &sw);
    launcher_syn_stats(&sr, &sp);
    printf("TOUCH %s: %s   [ui state=%d shown=%d anims=%u app=%d screen_on=%d dim=%d touch_errs=%u gestures=%u lastdir=%d swallowed=%u max_read_ms=%u syn_reads=%u syn_pressed=%u]\n", ok ? "PASS" : "FAIL", what, launcher_state(), launcher_active_screen(), (unsigned)lv_anim_count_running(),
           launcher_cur(), launcher_screen_on(), launcher_dim(), touch_read_errors(), gn, gd, sw, touch_read_max_ms(), sr, sp);
    if (ok) s_pass++; else s_fail++;
}
static void go_face(void) { bsp_display_lock(0); launcher_wake(); launcher_face(); bsp_display_unlock(); vTaskDelay(pdMS_TO_TICKS(600)); }

static void cmd_touch(void)
{
    s_pass = s_fail = 0;
    go_face();
    finger(205, 440, 205, 150, 3);                                  // swipe up
    check(launcher_state() == 1, "swipe up on watch face opens the app grid");
    tap(335, 376);                                                  // Settings icon (3rd column, 3rd row)
    check(launcher_state() == 2 && launcher_cur() == 8, "tap on Settings icon opens Settings");

    wait_safe_second();
    int m0 = rtc_minutes();
    finger(140, 443, 180, 443, 4);                                  // 40 px drag that starts and ends INSIDE '-1m' (under LVGL's 50 px swipe threshold)
    int m1 = rtc_minutes();
    check(m0 >= 0 && m1 == m0, "swipe across '-1m' button does NOT change the clock");
    if (launcher_state() == 2) finger(60, 120, 300, 120, 3);        // a 55 px swipe is too short to navigate: leave via a proper swipe
    check(launcher_state() == 1, "swipe right leaves the app");

    tap(335, 376);
    wait_safe_second();
    m0 = rtc_minutes();
    tap(251, 443);                                                  // '+1m'
    m1 = rtc_minutes();
    printf("TOUCH info: clock before=%d after=%d (state=%d app=%d)\n", m0, m1, launcher_state(), launcher_cur());
    check(m1 == m0 + 1, "real tap on '+1m' adds exactly one minute");
    tap(160, 443);                                                  // '-1m' to restore
    check(rtc_minutes() == m0, "real tap on '-1m' restores it");
    finger(60, 120, 300, 120, 3);                                   // swipe right = back
    check(launcher_state() == 1, "swipe right inside an app returns to the grid");

    // dark-screen tap must only wake the screen
    tap(335, 376);                                                  // open Settings again
    wait_safe_second();
    m0 = rtc_minutes();
    bsp_display_lock(0); launcher_btn_long(); bsp_display_unlock();  // screen off
    vTaskDelay(pdMS_TO_TICKS(500));
    check(!launcher_screen_on(), "screen is off");
    tap(251, 443);                                                  // blind tap where '+1m' is
    m1 = rtc_minutes();
    check(launcher_screen_on() && m1 == m0, "tap on a dark screen wakes it but does NOT press the button underneath");
    tap(251, 443);
    check(rtc_minutes() == m0 + 1, "the next tap works normally");
    tap(160, 443);                                                  // restore clock
    check(rtc_minutes() == m0, "clock restored");
    go_face();
    printf("TOUCH DONE pass=%d fail=%d\n", s_pass, s_fail);
}

// ---- whole-app exercise: tap every button on the current app screen through the real LVGL input pipeline ----
typedef struct { int x, y; char txt[24]; } btn_spot_t;
static int collect_buttons(lv_obj_t *o, btn_spot_t *out, int n, int max)
{
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt && n < max; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(c, &lv_button_class)) {
            lv_area_t a; lv_obj_get_coords(c, &a);
            out[n].x = (a.x1 + a.x2) / 2; out[n].y = (a.y1 + a.y2) / 2;
            out[n].txt[0] = 0;
            if (lv_obj_get_child_count(c) && lv_obj_check_type(lv_obj_get_child(c, 0), &lv_label_class))
                snprintf(out[n].txt, sizeof(out[n].txt), "%s", lv_label_get_text(lv_obj_get_child(c, 0)));
            if (out[n].x >= 0 && out[n].x < 410 && out[n].y >= 0 && out[n].y < 502) n++;
        } else n = collect_buttons(c, out, n, max);
    }
    return n;
}
static void print_labels(lv_obj_t *o, int depth)          // every visible label, including inside buttons/containers
{
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char *t = lv_label_get_text(c);
            printf(" [");
            for (; *t; t++) putchar(*t == '\n' ? '|' : *t);      // keep one reply line per command
            printf("]");
        } else if (depth < 4) print_labels(c, depth + 1);
    }
}
// du [dir]: bytes per directory (recursive) and every file over 64 KB; ends with "CMD du <total>"
#include <dirent.h>
#include <sys/stat.h>
static uint64_t du_walk(const char *dir, int depth)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    char p[160];
    while ((e = readdir(d)) != NULL) {
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) { uint64_t sub = du_walk(p, depth + 1); printf("DU dir %s %llu\n", p, (unsigned long long)sub); total += sub; }
        } else {
            total += st.st_size;
            if (st.st_size > 65536) printf("DU file %s %ld\n", p, (long)st.st_size);
        }
    }
    closedir(d);
    return total;
}
static void cmd_du(const char *dir, int unused)
{
    uint64_t tot = 0, fre = 0;
    hw_storage_info(&tot, &fre);
    uint64_t used = du_walk(dir, 0);
    printf("CMD du %s files=%llu fs_total=%llu fs_free=%llu\n", dir, (unsigned long long)used, (unsigned long long)tot, (unsigned long long)fre);
}

static void cmd_labels(void)
{
    bsp_display_lock(0);
    printf("CMD labels:");
    print_labels(lv_screen_active(), 0);
    print_labels(lv_layer_top(), 0);             // overlays: pairing code, Allow/Deny prompts, toasts
    printf("\n");
    bsp_display_unlock();
}
static void hold(int x, int y, int ms)                       // press, keep the finger down, release (push-to-talk tests)
{
    feed(true, true, x, y);
    vTaskDelay(pdMS_TO_TICKS(ms));
    feed(true, false, x, y);
    vTaskDelay(pdMS_TO_TICKS(100));
    feed(false, false, 0, 0);
}
static void cmd_tapall(int times)
{
    static btn_spot_t b[24];
    bsp_display_lock(0);
    launcher_wake();
    int app = launcher_cur();
    int n = collect_buttons(lv_screen_active(), b, 0, 24);
    bsp_display_unlock();
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < times; k++) {
            bsp_display_lock(0); launcher_wake(); bsp_display_unlock();
            if (launcher_state() != 2 || launcher_cur() != app) { printf("TAPALL left the app after a tap, stopping\n"); goto done; }
            printf("TAPALL tap %d/%d [%s] at %d,%d\n", i + 1, n, b[i].txt, b[i].x, b[i].y);
            tap(b[i].x, b[i].y);
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
done:
    printf("CMD tapall done buttons=%d state=%d app=%d dma_free=%u\n", n, launcher_state(), launcher_cur(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

// A reply whose length is an exact multiple of 64 bytes fills the last USB packet completely; the PC then waits
// for "more" and the line only shows up with the next output. Once the FIFO has drained, flush again: on an empty
// FIFO that sends a zero-length packet = "end of transfer".
void usj_end_packet(void)
{
    fflush(stdout);
    for (int i = 0; i < 20 && !usb_serial_jtag_ll_txfifo_writable(); i++) vTaskDelay(1);
    vTaskDelay(pdMS_TO_TICKS(3));
    usb_serial_jtag_ll_txfifo_flush();
}

// Ember app management:  app put NAME | app hex HEX... | app end | app ls | app rm NAME | app run NAME
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'f') ? (c | 32) - 'a' + 10 : -1; }
static void cmd_app(const char *a)
{
    char nm[32];
    if (sscanf(a, "put %31s", nm) == 1) printf("CMD app put %s\n", script_put_begin(nm) ? "ok" : "FAIL (name: a-z 0-9 _ -, max 20)");
    else if (!strncmp(a, "hex ", 4)) {
        uint8_t buf[320]; size_t n = 0;
        for (const char *p = a + 4; p[0] && p[1] && n < sizeof(buf); p += 2) {
            int h = hexv(p[0]), lo = hexv(p[1]);
            if (h < 0 || lo < 0) { printf("CMD app hex BAD\n"); return; }
            buf[n++] = (uint8_t)(h << 4 | lo);
        }
        printf("CMD app hex %s %u\n", script_put_data(buf, n) ? "ok" : "FAIL", (unsigned)n);
    } else if (!strncmp(a, "end", 3)) printf("CMD app end %s\n", script_put_end() ? "ok" : "FAIL");
    else if (!strncmp(a, "ls", 2)) {
        printf("CMD app ls %d:", script_count());
        for (int i = 0; i < script_count(); i++) printf(" %s", script_name(i));
        printf("\n");
    } else if (sscanf(a, "rm %31s", nm) == 1) printf("CMD app rm %s\n", script_remove(nm) ? "ok" : "FAIL");
    else if (sscanf(a, "run %31s", nm) == 1) {
        bsp_display_lock(0); launcher_open_script(nm); bsp_display_unlock();
        printf("CMD app run state=%d cur=%d\n", launcher_state(), launcher_cur());
    } else printf("CMD app ?\n");
}

static void run_line(char *l)
{
    int Y, M, D, h, m, s, n;
    if (sscanf(l, "settime %d %d %d %d %d %d", &Y, &M, &D, &h, &m, &s) == 6) {
        struct tm t = {0};
        t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
        time_t e = mktime(&t);
        gmtime_r(&e, &t);
        hw_rtc_set(&t);
        printf("CMD time set %04d-%02d-%02d %02d:%02d:%02d\n", Y, M, D, h, m, s);
    } else if (sscanf(l, "open %d", &n) == 1) {                     // open app n, wait, go back to the grid
        bsp_display_lock(0); launcher_wake(); launcher_home(); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_display_lock(0); launcher_open(n); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(2500));
        bsp_display_lock(0); launcher_home(); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));
        printf("CMD open %d done\n", n);
    } else if (!strncmp(l, "layout", 6)) cmd_layout();
    else if (!strncmp(l, "touch", 5)) cmd_touch();
    else if (sscanf(l, "nav %d", &n) == 1) cmd_nav(n);
    else if (!strncmp(l, "diag", 4)) {
        bool lock_ok = bsp_display_lock(500);
        if (lock_ok) bsp_display_unlock();
        TaskHandle_t lv = xTaskGetHandle("taskLVGL");
        if (!lv) lv = xTaskGetHandle("LVGL task");
        unsigned tr, tp;
        launcher_touch_stats(&tr, &tp);
        printf("CMD diag lvgl_lock=%s lvgl_task=%s lvgl_stack_free=%u heap_integrity=%s touch_read_errors=%u real_touch_reads=%u real_touch_presses=%u\n",
               lock_ok ? "ok" : "BLOCKED", lv ? "found" : "missing", lv ? (unsigned)uxTaskGetStackHighWaterMark(lv) : 0,
               heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT", touch_read_errors(), tr, tp);
    } else if (!strncmp(l, "status", 6)) {
        struct tm t; batt_t b; hw_batt(&b);
        bool ok = hw_rtc_get(&t);
        printf("CMD status rtc=%s %04d-%02d-%02d %02d:%02d:%02d steps=%lu batt=%d%% vbus=%d dma_free=%u dma_largest=%u psram=%u\n", ok ? "ok" : "unset", t.tm_year + 1900,
               t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (unsigned long)hw_steps_today(), b.pct, b.vbus,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (sscanf(l, "hold %d %d %d", &h, &m, &n) == 3) { hold(h, m, n); printf("CMD hold done\n"); }
    else if (!strncmp(l, "face", 4)) { bsp_display_lock(0); launcher_wake(); launcher_face(); bsp_display_unlock(); vTaskDelay(pdMS_TO_TICKS(1300)); printf("CMD face state=%d\n", launcher_state()); }
    else if (sscanf(l, "tapall %d", &n) == 1) cmd_tapall(n);
    else if (!strncmp(l, "labels", 6)) cmd_labels();
    else if (sscanf(l, "tap %d %d", &h, &m) == 2) { tap(h, m); printf("CMD tap done state=%d app=%d\n", launcher_state(), launcher_cur()); }
    else if (sscanf(l, "launch %d", &n) == 1) {                  // open app n and STAY in it (built-ins 0..8, scripts after)
        bsp_display_lock(0); launcher_wake(); launcher_home(); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));
        bsp_display_lock(0); launcher_open(n); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(800));
        printf("CMD launch state=%d app=%d\n", launcher_state(), launcher_cur());
    } else if (!strncmp(l, "mem", 3)) {
        printf("CMD mem internal_free=%u dma_free=%u dma_largest=%u psram_free=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (!strncmp(l, "beep", 4)) {
        bool ok = hw_spk_open(16000, 40);
        if (ok) hw_spk_close();
        hw_beep(1200, 60, 40);
        printf("CMD beep speaker=%s\n", ok ? "ok" : "FAIL");
    } else if (sscanf(l, "rec %d", &n) == 1) {
        bool ok = rec_start(REC_MEETING);
        if (ok) {
            for (int i = 0; i < n; i++) { vTaskDelay(pdMS_TO_TICKS(1000)); if (i == 1 || i == n - 2) rec_mark(); }
            rec_stop();
        }
        uint32_t fr, dr, rt; rec_stats(&fr, &dr, &rt);
        const char *f = rec_last_file(), *sl = strrchr(f, '/');
        uint32_t mk[8]; int nm = ok ? rec_marks_for(sl ? sl + 1 : f, mk, 8) : 0;
        printf("CMD rec start=%s file=%s frames=%lu dropped=%lu rate=%lu marks=%d\n", ok ? "ok" : "FAIL", ok ? f : "-",
               (unsigned long)fr, (unsigned long)dr, (unsigned long)rt, nm);
    } else if (!strncmp(l, "link ", 5)) { link_inject(l + 5); printf("CMD link injected\n"); }   // act as the phone (tests)
    else if (!strncmp(l, "linkb ", 6)) { link_inject_untrusted(l + 6); printf("CMD linkb injected\n"); }  // as if over BLE (confirm tests)
    else if (!strncmp(l, "devices", 7)) devices_print();
    else if (!strncmp(l, "micstate", 8)) {
        char who[16] = "", what[40] = ""; uint32_t secs = 0, rate = 0;
        bool busy = hw_mic_owner(who, sizeof(who), what, sizeof(what), &secs, &rate);
        printf("CMD micstate busy=%d who=%s what=%s secs=%lu rate=%lu rec=%d paused=%d ptt=%d shared=%d\n", busy, who, what,
               (unsigned long)secs, (unsigned long)rate, rec_active(), rec_paused(), link_ptt_active(), link_ptt_shared());
    } else if (sscanf(l, "btn %d", &n) == 1) {                 // tests: 1 short, 2 double, 8 hold down, 9 hold released
        if (n == 2) launcher_btn_double(); else if (n == 8) launcher_btn_hold(true); else if (n == 9) launcher_btn_hold(false); else launcher_btn_short();
        printf("CMD btn %d state=%d app=%d\n", n, launcher_state(), launcher_cur());
    } else if (sscanf(l, "recstart %d", &n) == 1) { printf("CMD recstart %d ok=%d\n", n, rec_start(n ? REC_MUSIC : REC_MEETING)); }
    else if (!strncmp(l, "recstop", 7)) { rec_stop(); printf("CMD recstop file=%s\n", rec_last_file()); }
    else if (!strncmp(l, "taptest", 7)) {                      // a second listener gets a copy of the owner's audio
        static int16_t buf[512]; int got = 0; bool ok = hw_mic_tap_open(16000);
        int64_t t0 = esp_timer_get_time();
        while (ok && esp_timer_get_time() - t0 < 1000000) got += hw_mic_tap_read(buf, 512, 100);
        if (ok) hw_mic_tap_close();
        printf("CMD taptest open=%d frames=%d\n", ok, got);
    } else if (!strncmp(l, "recpause", 8)) { bool on = l[8] != '0'; printf("CMD recpause %d ok=%d\n", on, rec_pause(on)); }
    else if (!strncmp(l, "du", 2) && (l[2] == 0 || l[2] == ' ')) cmd_du(l[2] == ' ' ? l + 3 : "/flash", 0);
    else if (!strncmp(l, "pmu", 3)) { uint8_t r[4]; hw_pmu_regs(r); printf("CMD pmu r22=%02x r27=%02x r41=%02x r49=%02x\n", r[0], r[1], r[2], r[3]); }
    else if (!strncmp(l, "schedreset", 10)) { sched_reset(); sched_test_minute(-1); printf("CMD schedreset\n"); }
    else if (!strncmp(l, "sched", 5)) sched_print();
    else if (sscanf(l, "citest %d", &n) == 1) { sched_test_minute(n); vTaskDelay(pdMS_TO_TICKS(300)); printf("CMD citest %d\n", n); }
    else if (!strncmp(l, "secstate", 8)) printf("CMD secstate bonded=%d pairing_open=%d chronos=%d link_ready=%d\n", ble_bonded(), ble_pair_window_open(), hw_pref_get("chronos", 0), link_ready());
    else if (!strncmp(l, "adpcmtest", 9)) { static char hx[600]; link_adpcm_selftest(hx, sizeof(hx)); printf("CMD adpcm %s\n", hx); }
    else if (sscanf(l, "buzz %d", &n) == 1) { hw_buzz(n); printf("CMD buzz %d\n", n); }
    else if (!strncmp(l, "raisetest", 9)) printf("CMD raisetest fails=%d\n", hw_raise_selftest());
    else if (!strncmp(l, "imu", 3)) { float x, y, z; hw_imu_get_g(&x, &y, &z); printf("CMD imu x=%.2f y=%.2f z=%.2f still_ms=%lu\n", x, y, z, (unsigned long)hw_imu_still_ms()); }
    else if (!strncmp(l, "drain", 5)) { char b[120]; power_drain_text(b, sizeof(b)); printf("CMD drain %s\n", b); }
    else if (0) {
    } else if (!strncmp(l, "app ", 4)) cmd_app(l + 4);
    else if (!strncmp(l, "pwr", 3)) {
        power_stats_t p; power_stats(&p); batt_t b; hw_batt(&b);
        printf("CMD pwr screen=%s active_s=%lld doze_s=%lld wakes=%lu deep_sleeps=%lu sleep_allowed=%d still_ms=%lu batt_present=%d pct=%d mv=%d usb=%d charging=%d wake_cause=%d\n",
               p.screen_on ? "on" : "off", p.ms_active / 1000, p.ms_doze / 1000, (unsigned long)p.wakes, (unsigned long)p.deep_count, p.sleep_allowed,
               (unsigned long)hw_imu_still_ms(), b.present, b.pct, b.mv, b.vbus, b.charging, power_wake_cause());
    } else if (sscanf(l, "deepsleep %d", &n) == 1) {
        printf("CMD deepsleep %d s (USB drops and comes back)\n", n);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        power_deep_sleep(n);
    } else if (!strncmp(l, "screen off", 10)) { bsp_display_lock(0); if (launcher_screen_on()) launcher_btn_long(); bsp_display_unlock(); printf("CMD screen %d\n", launcher_screen_on()); }
    else if (!strncmp(l, "screen on", 9)) { bsp_display_lock(0); launcher_wake(); bsp_display_unlock(); printf("CMD screen %d\n", launcher_screen_on()); }
    else printf("CMD unknown: %s\n", l);
}

static void cmd_task(void *a)
{
    static char line[640];
    int len = 0;
    while (1) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (len) { line[len] = 0; run_line(line); len = 0; usj_end_packet(); }
        } else if (len < (int)sizeof(line) - 1) line[len++] = (char)c;
    }
}

void cmd_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) { ESP_LOGW("cmd", "usb driver install failed"); return; }
    xTaskCreate(cmd_task, "cmd", 6144, NULL, 2, NULL);
}

// Hardware layer: power chip, RTC, IMU + step counter, storage, audio, prefs.
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "hw.h"
#include "app.h"
#include "driver/gpio.h"

static const char *TAG = "hw";

// ---------------------------------------------------------------- I2C helpers
static i2c_master_dev_handle_t s_axp, s_rtc, s_imu;

static i2c_master_dev_handle_t dev_add(uint8_t addr)
{
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &h) != ESP_OK) ESP_LOGE(TAG, "add dev 0x%02X failed", addr);
    return h;
}
static esp_err_t rr(i2c_master_dev_handle_t h, uint8_t reg, uint8_t *out, size_t n)
{
    if (!h) return ESP_FAIL;
    return i2c_master_transmit_receive(h, &reg, 1, out, n, 100);
}
static esp_err_t wr(i2c_master_dev_handle_t h, uint8_t reg, const uint8_t *d, size_t n)
{
    if (!h || n > 16) return ESP_FAIL;
    uint8_t buf[17];
    buf[0] = reg;
    memcpy(buf + 1, d, n);
    return i2c_master_transmit(h, buf, n + 1, 100);
}
static void wr1(i2c_master_dev_handle_t h, uint8_t reg, uint8_t v) { wr(h, reg, &v, 1); }

// ---------------------------------------------------------------- prefs
static nvs_handle_t s_nvs;
int hw_pref_get(const char *key, int def)
{
    int32_t v;
    return nvs_get_i32(s_nvs, key, &v) == ESP_OK ? (int)v : def;
}
void hw_pref_set(const char *key, int val)
{
    nvs_set_i32(s_nvs, key, val);
    nvs_commit(s_nvs);
}
size_t hw_blob_get(const char *key, void *buf, size_t cap)
{
    size_t n = cap;
    return nvs_get_blob(s_nvs, key, buf, &n) == ESP_OK ? n : 0;
}
void hw_blob_set(const char *key, const void *buf, size_t n)
{
    nvs_set_blob(s_nvs, key, buf, n);
    nvs_commit(s_nvs);
}

// ---------------------------------------------------------------- power
void hw_batt(batt_t *b)
{
    uint8_t s1 = 0, s2 = 0, pct = 0, v[2] = {0};
    rr(s_axp, 0x00, &s1, 1);
    rr(s_axp, 0x01, &s2, 1);
    rr(s_axp, 0xA4, &pct, 1);
    rr(s_axp, 0x34, v, 2);
    b->vbus = (s1 >> 5) & 1;
    b->present = (s1 >> 3) & 1;
    b->charging = ((s2 >> 5) & 3) == 1;
    b->mv = ((v[0] & 0x3F) << 8) | v[1];
    b->pct = pct > 100 ? 100 : pct;
    if (!b->present) { b->pct = 0; b->mv = 0; }
}

static void pmu_init(void)
{
    s_axp = dev_add(0x34);
    uint8_t r;
    if (rr(s_axp, 0x30, &r, 1) == ESP_OK) wr1(s_axp, 0x30, r | 0x01);   // VBAT ADC on
    if (rr(s_axp, 0x18, &r, 1) == ESP_OK) wr1(s_axp, 0x18, r | 0x08);   // fuel gauge on
    if (rr(s_axp, 0x68, &r, 1) == ESP_OK) wr1(s_axp, 0x68, r | 0x01);   // battery detect on
    // Power button (PWRON): holding 6 s switches the device off (done by the AXP2101 itself, works even if the firmware
    // hangs); a short press is latched in IRQ status 1 (0x49) bit 3 and read by hw_pwrkey_short().
    if (rr(s_axp, 0x27, &r, 1) == ESP_OK) wr1(s_axp, 0x27, (r & ~0x0C) | (1 << 2));   // OFFLEVEL bits 3:2 = 01 -> 6 s
    if (rr(s_axp, 0x22, &r, 1) == ESP_OK) wr1(s_axp, 0x22, (r | 0x02) & ~0x04);       // long press = power off (not restart)
    if (rr(s_axp, 0x41, &r, 1) == ESP_OK) wr1(s_axp, 0x41, r | 0x0C);                 // latch short + long press
    wr1(s_axp, 0x49, 0x0C);                                                              // clear stale presses
}

// 1 = the power button was clicked since the last call (and the latch is cleared), 0 = no, -1 = PMU not readable
int hw_pwrkey_short(void)
{
    uint8_t st = 0;
    if (!s_axp || rr(s_axp, 0x49, &st, 1) != ESP_OK) return -1;
    if (!(st & 0x0C)) return 0;
    wr1(s_axp, 0x49, st & 0x0C);                // write-1-to-clear
    return (st & 0x08) ? 1 : 0;                 // bit 3 short press (bit 2 = long press, handled by the PMU)
}

void hw_pmu_regs(uint8_t out[4])               // 0x22, 0x27, 0x41, 0x49 (USB shell "pmu")
{
    const uint8_t regs[4] = {0x22, 0x27, 0x41, 0x49};
    for (int i = 0; i < 4; i++) { out[i] = 0xEE; if (s_axp) rr(s_axp, regs[i], &out[i], 1); }
}

// ---------------------------------------------------------------- RTC (PCF85063A)
static uint8_t bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t tobcd(int v) { return ((v / 10) << 4) | (v % 10); }

bool hw_rtc_valid(void)
{
    uint8_t s;
    return rr(s_rtc, 0x04, &s, 1) == ESP_OK && !(s & 0x80);
}
bool hw_rtc_get(struct tm *t)
{
    uint8_t d[7];
    if (rr(s_rtc, 0x04, d, 7) != ESP_OK || (d[0] & 0x80)) return false;
    memset(t, 0, sizeof(*t));
    t->tm_sec = bcd2(d[0] & 0x7F);
    t->tm_min = bcd2(d[1] & 0x7F);
    t->tm_hour = bcd2(d[2] & 0x3F);
    t->tm_mday = bcd2(d[3] & 0x3F);
    t->tm_wday = d[4] & 7;
    t->tm_mon = bcd2(d[5] & 0x1F) - 1;
    t->tm_year = bcd2(d[6]) + 100;
    return true;
}
void hw_rtc_set(const struct tm *t)
{
    uint8_t d[7] = {tobcd(t->tm_sec), tobcd(t->tm_min), tobcd(t->tm_hour), tobcd(t->tm_mday),
                    (uint8_t)(t->tm_wday & 7), tobcd(t->tm_mon + 1), tobcd(t->tm_year % 100)};
    wr(s_rtc, 0x04, d, 7);
}
static void rtc_init(void)
{
    s_rtc = dev_add(0x51);
    if (!hw_rtc_valid()) {
        // Never set: use firmware build time so the watch starts roughly right.
        static const char *mn = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char m[4] = {0};
        int d, y, hh, mm, ss;
        sscanf(__DATE__, "%3s %d %d", m, &d, &y);
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
        struct tm t = {0};
        t.tm_mon = (int)((strstr(mn, m) - mn) / 3);
        t.tm_mday = d; t.tm_year = y - 1900; t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
        time_t e = mktime(&t);
        gmtime_r(&e, &t);   // fills tm_wday
        hw_rtc_set(&t);
        ESP_LOGW(TAG, "RTC was unset; seeded from build time");
    }
}

// ---------------------------------------------------------------- IMU + steps
static bool s_imu_ok;
static volatile float s_gx, s_gy, s_gz;
static volatile uint32_t s_steps;
static struct { uint32_t date; uint32_t d[7]; } s_hist;  // d[0] = today
static uint32_t s_saved_steps;

static volatile uint32_t s_still_ms;
volatile bool s_raise_on = false;       // mirrors pref "raise" (default off since 2026-11-15)
bool hw_imu_ok(void) { return s_imu_ok; }
uint32_t hw_imu_still_ms(void) { return s_imu_ok ? s_still_ms : 0; }

// QMI8658 wake-on-motion (datasheet "WoM" via CTRL9 command 0x08). INT1 idles low and toggles high on motion.
bool hw_imu_wom_arm(void)
{
    if (!s_imu_ok) return false;
    wr1(s_imu, 0x08, 0x00);             // CTRL7: all sensors off while configuring
    wr1(s_imu, 0x03, 0x1E);             // CTRL2: accel +-4g, low-power 11 Hz ODR
    wr1(s_imu, 0x0B, 120);              // CAL1_L: threshold 120 mg
    wr1(s_imu, 0x0C, 0x80 | 0x02);      // CAL1_H: route to INT1, initial level 0, blank 2 samples
    wr1(s_imu, 0x0A, 0x08);             // CTRL9: CTRL_CMD_WRITE_WOM_SETTING
    uint8_t st = 0;
    for (int i = 0; i < 50 && !(st & 0x80); i++) { vTaskDelay(pdMS_TO_TICKS(2)); rr(s_imu, 0x2D, &st, 1); }
    wr1(s_imu, 0x0A, 0x00);             // CTRL9 ack
    wr1(s_imu, 0x02, 0x40 | 0x08);      // CTRL1: auto-increment + INT1 output enable
    wr1(s_imu, 0x08, 0x01);             // CTRL7: accel on
    uint8_t s1; rr(s_imu, 0x2F, &s1, 1); // STATUS1 read clears a stale event
    ESP_LOGI(TAG, "WoM armed (cmd done=%d)", (st & 0x80) != 0);
    return (st & 0x80) != 0;
}
void hw_imu_get_g(float *x, float *y, float *z) { *x = s_gx; *y = s_gy; *z = s_gz; }

// ---------------------------------------------------------------- haptics (motor driver on GPIO18, active high;
// pin found in Waveshare's factory firmware "VibrationTest"). Non-blocking: an esp_timer switches it off / steps patterns.
#define MOTOR_GPIO 18
static esp_timer_handle_t s_motor_t;
static volatile int s_pat_left, s_pat_on, s_pat_off;
static volatile bool s_motor_on;
static void motor_cb(void *a)
{
    if (s_motor_on) {
        gpio_set_level(MOTOR_GPIO, 0); s_motor_on = false;
        if (s_pat_left > 0) esp_timer_start_once(s_motor_t, (uint64_t)s_pat_off * 1000);
    } else if (s_pat_left > 0) {
        s_pat_left--;
        gpio_set_level(MOTOR_GPIO, 1); s_motor_on = true;
        esp_timer_start_once(s_motor_t, (uint64_t)s_pat_on * 1000);
    }
}
void hw_buzz_pattern(int count, int on_ms, int off_ms)
{
    if (!s_motor_t || count <= 0 || !hw_pref_get("haptic", 1)) return;
    if (on_ms > 2000) on_ms = 2000;
    esp_timer_stop(s_motor_t);
    s_pat_on = on_ms; s_pat_off = off_ms; s_pat_left = count - 1;
    gpio_set_level(MOTOR_GPIO, 1); s_motor_on = true;
    esp_timer_start_once(s_motor_t, (uint64_t)on_ms * 1000);
}
void hw_buzz(int ms) { hw_buzz_pattern(1, ms, 0); }
static void motor_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << MOTOR_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(MOTOR_GPIO, 0);
    esp_timer_create_args_t a = {.callback = motor_cb, .name = "motor"};
    esp_timer_create(&a, &s_motor_t);
}

// ---------------------------------------------------------------- raise-to-wake (pure detector + synthetic self-test)
// "Face up" = gravity mostly along the screen normal. On this board the screen-up reading is z = -1 g (measured on a
// desk), so the sign is a pref ("rsign", default -1) in case a strap mounts it differently.
// Trigger: the watch becomes face-up (tilt up to ~50 deg toward the eyes allowed) within 1 s after NOT being face-up,
// and then stays face-up AND STILL for 400 ms (each 20 ms sample moves < 0.12 g). An arm swinging while walking passes
// through face-up but never holds still, so it does not wake the screen (that was ~half the battery on 2026-11-15).
// Lying on a desk never re-triggers (needs a fresh not-face-up -> face-up transition).
static int s_rsign = -1;
bool hw_raise_feed(raise_t *r, float x, float y, float z, int64_t t)
{
    bool up = (s_rsign * z > 0.6f) && fabsf(x) < 0.6f && fabsf(y) < 0.8f;
    if (s_rsign * z < 0.2f) r->t_notup = t;                 // "far": screen vertical or facing away
    if (!up) { r->t_up_since = 0; r->armed = true; return false; }
    float moved = fabsf(x - r->px) + fabsf(y - r->py) + fabsf(z - r->pz);
    r->px = x; r->py = y; r->pz = z;
    if (r->t_up_since == 0) r->t_up_since = t;
    else if (moved > 0.12f) {                               // still swinging: restart the hold, but only within the raise window
        r->t_up_since = t;
        if (r->t_notup && t - r->t_notup > 1000) r->armed = false;   // too late for a raise now
    }
    // a real raise turns the screen from far to face-up in under a second and then holds still; a slow put-down does not
    if (r->armed && t - r->t_up_since >= 400 && r->t_notup && r->t_up_since - r->t_notup <= 1000) { r->armed = false; return true; }
    return false;
}
int hw_raise_selftest(void)
{
    int fails = 0, s = s_rsign;
    raise_t r;
    // 1) arm hanging (gravity along x) -> raise -> face up: must trigger exactly once
    memset(&r, 0, sizeof(r)); int hits = 0; int64_t t = 1000;
    for (int i = 0; i < 25; i++, t += 20) hits += hw_raise_feed(&r, -1.0f, 0.0f, 0.0f, t);
    for (int i = 0; i < 15; i++, t += 20) { float k = i / 15.0f; hits += hw_raise_feed(&r, -1.0f + k, 0.2f, s * k, t); }
    for (int i = 0; i < 40; i++, t += 20) hits += hw_raise_feed(&r, 0.05f, 0.35f, s * 0.92f, t);
    if (hits != 1) { fails++; printf("RAISE FAIL arm-up hits=%d\n", hits); }
    // 2) lying face-up on a desk for 10 s: never
    memset(&r, 0, sizeof(r)); hits = 0; t = 1000;
    for (int i = 0; i < 500; i++, t += 20) hits += hw_raise_feed(&r, 0.0f, 0.0f, s * 1.0f, t);
    if (hits != 0) { fails++; printf("RAISE FAIL desk hits=%d\n", hits); }
    // 3) face-down / sideways shaking: never
    memset(&r, 0, sizeof(r)); hits = 0; t = 1000;
    for (int i = 0; i < 300; i++, t += 20) hits += hw_raise_feed(&r, (i % 7) * 0.2f - 0.6f, 0.9f, -s * 0.4f, t);
    if (hits != 0) { fails++; printf("RAISE FAIL shake hits=%d\n", hits); }
    // 4) slow drift onto the desk over 5 s (put down): never
    memset(&r, 0, sizeof(r)); hits = 0; t = 1000;
    for (int i = 0; i < 250; i++, t += 20) { float k = i / 250.0f; hits += hw_raise_feed(&r, -1.0f + k, 0, s * k, t); }
    for (int i = 0; i < 100; i++, t += 20) hits += hw_raise_feed(&r, 0, 0, s * 1.0f, t);
    if (hits != 0) { fails++; printf("RAISE FAIL slow-putdown hits=%d\n", hits); }
    // 5) walking: the arm swings through face-up about once a second without stopping: never
    memset(&r, 0, sizeof(r)); hits = 0; t = 1000;
    for (int i = 0; i < 600; i++, t += 20) {
        float ph = (i % 50) / 50.0f;                        // 1 s swing period
        float k = ph < 0.5f ? ph * 2 : (1 - ph) * 2;        // 0 -> 1 -> 0
        hits += hw_raise_feed(&r, -1.0f + k * 1.05f, 0.1f, s * k * 0.95f, t);
    }
    if (hits != 0) { fails++; printf("RAISE FAIL walking-swing hits=%d\n", hits); }
    return fails;
}

static uint32_t today_key(void)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return 0;
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
static int days_between(uint32_t a, uint32_t b)  // yyyymmdd a -> b
{
    struct tm ta = {0}, tb = {0};
    ta.tm_year = a / 10000 - 1900; ta.tm_mon = (a / 100) % 100 - 1; ta.tm_mday = a % 100; ta.tm_hour = 12;
    tb.tm_year = b / 10000 - 1900; tb.tm_mon = (b / 100) % 100 - 1; tb.tm_mday = b % 100; tb.tm_hour = 12;
    return (int)((mktime(&tb) - mktime(&ta)) / 86400);
}
uint32_t hw_steps_today(void) { return s_steps; }
void hw_steps_history(uint32_t out[7])
{
    s_hist.d[0] = s_steps;
    memcpy(out, s_hist.d, sizeof(s_hist.d));
}
void hw_steps_save(void)
{
    s_hist.d[0] = s_steps;
    nvs_set_blob(s_nvs, "steps", &s_hist, sizeof(s_hist));
    nvs_commit(s_nvs);
    s_saved_steps = s_steps;
}
static void steps_rollover_check(void)
{
    uint32_t k = today_key();
    if (!k) return;
    if (s_hist.date == 0) { s_hist.date = k; return; }
    if (k != s_hist.date) {
        int n = days_between(s_hist.date, k);
        if (n < 0) { s_hist.date = k; return; }     // clock went backwards: just re-anchor
        s_hist.d[0] = s_steps;
        for (int i = 0; i < n && i < 7; i++) { memmove(&s_hist.d[1], &s_hist.d[0], 6 * sizeof(uint32_t)); s_hist.d[0] = 0; }
        s_hist.date = k;
        s_steps = 0;
        hw_steps_save();
    }
}

static void imu_task(void *arg)
{
    // Software pedometer: the chip's hardware pedometer is reported unusable on this board revision.
    float mean = 1.0f, lp = 0, prev = 0, prev2 = 0;
    int64_t last_peak = 0;
    int cand = 0;
    int64_t last_house = 0;
    const float THR = 0.10f;
    // Power: 50 Hz only while the wrist moves. After 10 s of stillness poll at 4 Hz (each poll = one light-sleep
    // wake-up instead of 50); any change > 0.06 g snaps back to 50 Hz. A step cluster needs 4 peaks, so nothing is lost.
    float sx = 0, sy = 0, sz = 0;
    int64_t still_since = esp_timer_get_time() / 1000;
    raise_t rz = {0};
    while (1) {
        int64_t t = esp_timer_get_time() / 1000;
        bool slow = (t - still_since) > 10000;
        vTaskDelay(pdMS_TO_TICKS(slow ? 250 : 20));
        uint8_t d[6];
        if (rr(s_imu, 0x35, d, 6) != ESP_OK) continue;
        int16_t ax = (int16_t)(d[1] << 8 | d[0]), ay = (int16_t)(d[3] << 8 | d[2]), az = (int16_t)(d[5] << 8 | d[4]);
        float gx = ax / 8192.0f, gy = ay / 8192.0f, gz = az / 8192.0f;   // +-4g
        s_gx = gx; s_gy = gy; s_gz = gz;
        if (fabsf(gx - sx) + fabsf(gy - sy) + fabsf(gz - sz) > 0.06f) { still_since = esp_timer_get_time() / 1000; sx = gx; sy = gy; sz = gz; }
        s_still_ms = (uint32_t)(esp_timer_get_time() / 1000 - still_since);
        if (s_raise_on && hw_raise_feed(&rz, gx, gy, gz, esp_timer_get_time() / 1000)) launcher_raise();
        if (slow) continue;                             // filter state only advances at its design rate (50 Hz)
        float mag = sqrtf(gx * gx + gy * gy + gz * gz);
        mean += 0.02f * (mag - mean);
        lp += 0.35f * ((mag - mean) - lp);
        int64_t now = esp_timer_get_time() / 1000;
        if (prev > prev2 && prev >= lp && prev > THR) {
            int64_t dt = now - last_peak;
            if (dt > 250) {
                if (dt < 2000) {
                    cand++;
                    if (cand == 4) s_steps += 4;
                    else if (cand > 4) s_steps++;
                } else cand = 1;
                last_peak = now;
            }
        }
        prev2 = prev; prev = lp;
        if (now - last_house > 30000) {
            last_house = now;
            steps_rollover_check();
            if (s_steps != s_saved_steps) hw_steps_save();
        }
    }
}

static void imu_init(void)
{
    s_imu = dev_add(0x6B);
    uint8_t who = 0;
    if (rr(s_imu, 0x00, &who, 1) != ESP_OK || who != 0x05) { ESP_LOGE(TAG, "QMI8658 not found"); return; }
    wr1(s_imu, 0x02, 0x40);     // CTRL1: address auto-increment
    wr1(s_imu, 0x03, 0x17);     // CTRL2: accel +-4g, 62.5 Hz
    wr1(s_imu, 0x08, 0x01);     // CTRL7: accel on
    s_imu_ok = true;

    size_t sz = sizeof(s_hist);
    if (nvs_get_blob(s_nvs, "steps", &s_hist, &sz) != ESP_OK) memset(&s_hist, 0, sizeof(s_hist));
    s_steps = s_hist.d[0];
    s_saved_steps = s_steps;
    steps_rollover_check();
    xTaskCreate(imu_task, "imu", 4096, NULL, 3, NULL);
}

// ---------------------------------------------------------------- storage
static bool s_sd;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_flash_mounted;

static void mount_flash(void)
{
    if (s_flash_mounted) return;
    esp_vfs_fat_mount_config_t cfg = {.format_if_mount_failed = true, .max_files = 8,   /* apps, kv, logs, uploads can overlap */ .allocation_unit_size = 16 * 1024};
    if (esp_vfs_fat_spiflash_mount_rw_wl("/flash", "storage", &cfg, &s_wl) == ESP_OK) s_flash_mounted = true;
    else ESP_LOGE(TAG, "flash FAT mount failed");
}
void hw_storage_remount(void)
{
    mount_flash();                 // always: apps, settings logs and the power log live here
    if (s_sd) return;
    s_sd = (bsp_sdcard_mount() == ESP_OK);
}
const char *hw_storage_root(void) { return s_sd ? BSP_SD_MOUNT_POINT : "/flash"; }
bool hw_storage_is_sd(void) { return s_sd; }
void hw_storage_info(uint64_t *total, uint64_t *free_bytes)
{
    *total = 0; *free_bytes = 0;
    esp_vfs_fat_info(hw_storage_root(), total, free_bytes);
}

// ---------------------------------------------------------------- audio
static esp_codec_dev_handle_t s_mic, s_spk;
static bool s_mic_open, s_spk_open;
static uint32_t s_mic_rate;

// ---- mic: single owner, but a 16 kHz owner can share its stream with one listener (the "tap") ----
static char s_mic_who[16], s_mic_what[40];
static uint32_t s_mic_gen;                      // session ticket: only the session that opened the mic may close it
static volatile bool s_park_hold;               // a paused recording will reopen the mic: don't park the I2S
void hw_audio_park_hold(bool hold) { s_park_hold = hold; }
uint32_t hw_mic_session(void) { return s_mic_open ? s_mic_gen : 0; }
void hw_mic_close(void);
void hw_mic_close_session(uint32_t gen) { if (gen && s_mic_open && gen == s_mic_gen) hw_mic_close(); }
static int64_t s_mic_since;
#include "freertos/stream_buffer.h"
static StreamBufferHandle_t s_tap;              // copy of every frame the owner reads (Amber while a meeting records)
static SemaphoreHandle_t s_tap_mtx;             // guards s_tap: the owner copies into it while the listener may delete it
bool hw_mic_open_as(uint32_t rate, float gain_db, const char *who, const char *what)
{
    if (s_mic_open) return false;          // mic is single-owner
    if (!s_mic) return false;              // set up once at boot (hw_audio_preinit); never lazily: see there
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 2, .channel_mask = 0, .sample_rate = rate};
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_in_gain(s_mic, gain_db);
    s_mic_open = true;
    s_mic_rate = rate;
    s_mic_gen++;
    snprintf(s_mic_who, sizeof(s_mic_who), "%s", who ? who : "an app");
    snprintf(s_mic_what, sizeof(s_mic_what), "%s", what ? what : "");
    s_mic_since = esp_timer_get_time();
    return true;
}
bool hw_mic_open(uint32_t rate, float gain_db) { return hw_mic_open_as(rate, gain_db, "an app", ""); }
bool hw_mic_owner(char *who, size_t wn, char *what, size_t tn, uint32_t *secs, uint32_t *rate)
{
    if (!s_mic_open) return false;
    if (who) snprintf(who, wn, "%s", s_mic_who);
    if (what) snprintf(what, tn, "%s", s_mic_what);
    if (secs) *secs = (uint32_t)((esp_timer_get_time() - s_mic_since) / 1000000);
    if (rate) *rate = s_mic_rate;
    return true;
}
int hw_mic_read_mono(int16_t *out, int frames)
{
    EXT_RAM_BSS_ATTR static int16_t tmp[1024 * 2];
    if (!s_mic_open) return 0;
    if (frames > 1024) frames = 1024;
    if (esp_codec_dev_read(s_mic, tmp, frames * 4) != ESP_CODEC_DEV_OK) return 0;
    for (int i = 0; i < frames; i++) out[i] = (int16_t)(((int)tmp[2 * i] + (int)tmp[2 * i + 1]) / 2);
    if (s_tap && s_tap_mtx && xSemaphoreTake(s_tap_mtx, 0) == pdTRUE) {   // never blocks the owner
        if (s_tap) xStreamBufferSend(s_tap, out, frames * 2, 0);           // a full tap just drops
        xSemaphoreGive(s_tap_mtx);
    }
    return frames;
}
bool hw_mic_tap_open(uint32_t rate)
{
    if (!s_mic_open || s_mic_rate != rate || s_tap) return false;
    if (!s_tap_mtx) s_tap_mtx = xSemaphoreCreateMutex();
    StreamBufferHandle_t t = xStreamBufferCreateWithCaps(32 * 1024, 2, MALLOC_CAP_SPIRAM);    // ~1 s of 16 kHz audio
    if (!t) return false;
    xSemaphoreTake(s_tap_mtx, portMAX_DELAY); s_tap = t; xSemaphoreGive(s_tap_mtx);
    return true;
}
int hw_mic_tap_read(int16_t *out, int frames, int timeout_ms)
{
    StreamBufferHandle_t t = s_tap;
    if (!t) return 0;
    return (int)(xStreamBufferReceive(t, out, frames * 2, pdMS_TO_TICKS(timeout_ms)) / 2);
}
void hw_mic_tap_close(void)
{
    if (!s_tap_mtx) return;
    // under the lock: the owner can no longer be inside a send when the buffer is freed (a 20 ms "grace" sleep was not
    // enough when the recorder task got pre-empted mid-send -> heap corruption -> tlsf_free assert, 2026-11-16)
    xSemaphoreTake(s_tap_mtx, portMAX_DELAY);
    StreamBufferHandle_t t = s_tap;
    s_tap = NULL;
    xSemaphoreGive(s_tap_mtx);
    if (t) vStreamBufferDeleteWithCaps(t);
}
void hw_mic_close(void)
{
    if (!s_mic_open) return;
    esp_codec_dev_close(s_mic);
    s_mic_open = false;
    // closing the mic leaves the paired TX channel enabled (and its power lock held = no light sleep): re-park,
    // except while a recording is only paused (it reopens at its own rate in a moment; parking in between at 16 kHz
    // tripped the task watchdog in "Pause, talk, resume")
    if (!s_spk_open && !s_park_hold) hw_audio_park();
    s_mic_who[0] = s_mic_what[0] = 0;
}
bool hw_spk_open(uint32_t rate, int vol)
{
    if (s_spk_open) return true;
    if (s_mic_open && s_mic_rate != rate) return false;
    if (!s_spk) return false;              // set up once at boot (hw_audio_preinit)
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = rate};
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_out_vol(s_spk, vol);
    s_spk_open = true;
    return true;
}
void hw_spk_write(const int16_t *mono, int frames)
{
    if (s_spk_open) esp_codec_dev_write(s_spk, (void *)mono, frames * 2);
}
void hw_spk_close(void)
{
    if (!s_spk_open) return;
    esp_codec_dev_close(s_spk);
    s_spk_open = false;
}
void hw_beep(int hz, int ms, int vol)
{
    const int rate = 16000;
    if (s_mic_open && s_mic_rate != rate) return;   // would retune the shared I2S clock mid-recording
    if (!hw_spk_open(rate, vol)) return;
    int n = rate * ms / 1000;
    EXT_RAM_BSS_ATTR static int16_t buf[1600];
    for (int off = 0; off < n; off += 1600) {
        int c = n - off > 1600 ? 1600 : n - off;
        for (int i = 0; i < c; i++) {
            float env = 1.0f;
            int g = off + i;
            if (g < 80) env = g / 80.0f;
            if (n - g < 160) env = (n - g) / 160.0f;
            buf[i] = (int16_t)(sinf(2 * 3.14159265f * hz * g / rate) * 14000 * env);
        }
        hw_spk_write(buf, c);
    }
    memset(buf, 0, sizeof(buf));
    hw_spk_write(buf, 800);
    hw_spk_close();
}

// The Waveshare BSP sets up I2S the first time a codec is requested and ABORTS the whole watch (ESP_ERROR_CHECK) if
// its DMA buffers can't be allocated. Doing that lazily (first beep/recording) crashed every sound-making app once
// Bluetooth + display had used the internal DMA memory. So: claim it here, at boot, while memory is plentiful.
void hw_audio_park(void);
static void hw_audio_preinit(void)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    // Codec setup leaves the I2S channels running (wastes power, blocks light sleep): park both (see hw_audio_park).
    hw_audio_park();
    ESP_LOGI(TAG, "audio ready: speaker=%d mic=%d (used %u B DMA RAM)", s_spk != NULL, s_mic != NULL,
             (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
}

// Parks both I2S channels so the I2S driver releases its power-management lock (a held i2s_driver lock blocks light
// sleep entirely: measured 2026-11-16, held 98 % of the time). The BSP enables TX and RX itself, behind
// esp_codec_dev's back; esp_codec_dev defers disabling one direction while it thinks the other is open. Opening both
// (so it knows both) and closing RX then TX leaves both disabled. No-op while audio is in use.
void hw_audio_park(void)
{
    if (s_mic_open || s_spk_open || !s_spk || !s_mic) return;
    esp_codec_dev_sample_info_t o = {.bits_per_sample = 16, .channel = 1, .sample_rate = 16000};
    esp_codec_dev_sample_info_t i = {.bits_per_sample = 16, .channel = 2, .sample_rate = 16000};
    bool so = esp_codec_dev_open(s_spk, &o) == ESP_CODEC_DEV_OK;
    bool mo = esp_codec_dev_open(s_mic, &i) == ESP_CODEC_DEV_OK;
    if (mo) esp_codec_dev_close(s_mic);
    if (so) esp_codec_dev_close(s_spk);
}

// ---------------------------------------------------------------- init
void hw_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    nvs_open("watch", NVS_READWRITE, &s_nvs);
    pmu_init();
    rtc_init();
    motor_init();
    s_rsign = hw_pref_get("rsign", -1) < 0 ? -1 : 1;
    s_raise_on = hw_pref_get("raise", 0);   // off (owner 2026-11-15: no wake on movement)
    imu_init();
    hw_audio_preinit();
    hw_storage_remount();
    ESP_LOGI(TAG, "hw ready: imu=%d sd=%d", s_imu_ok, s_sd);
}

// Ember OS power manager.
//
//   ACTIVE  screen on.  CPU 80..160 MHz (max held only while the screen is on), light sleep forbidden.
//   DOZE    screen off. Panel in SLEEP-IN, LVGL idle (no tick timer, refresh timer paused), button + touch are
//           interrupt-driven, IMU polls slowly when still. Automatic light sleep between wake-ups when on battery.
//   DEEP    long DOZE with no motion, on battery, nothing running -> ESP deep sleep (~10s of uA for the SoC).
//           Wakes on the BOOT button (ext0, GPIO0) or wrist motion (QMI8658 wake-on-motion -> INT1 -> GPIO21, ext1).
//           A motion wake only restarts step counting: the screen stays dark (launcher_start checks the wake cause).
//
// Light sleep drops the USB-serial link, so neither sleep is used while USB power is present (dev mode).
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "diag.h"
#include "sched.h"
#include "hw.h"
#include "ble.h"
#include "wifi.h"

static const char *TAG = "power";
static esp_pm_lock_handle_t s_lk_cpu, s_lk_nosleep;
static bool s_locks_held;
static bool s_screen_on = true;
static int64_t s_t_state, s_ms_active, s_ms_doze;
static int64_t s_doze_since;
static uint32_t s_wakes;
static RTC_DATA_ATTR uint32_t s_deep_count;
static int s_wake_cause;

// ---- panel access (esp_lvgl_port keeps the IO handle in the display's driver data) ----
typedef struct { int disp_type; esp_lcd_panel_io_handle_t io; esp_lcd_panel_handle_t panel; } port_disp_ctx_t;
static esp_lcd_panel_io_handle_t panel_io(void)
{
    lv_display_t *d = lv_display_get_default();
    port_disp_ctx_t *c = d ? lv_display_get_driver_data(d) : NULL;
    return c ? c->io : NULL;
}
static void panel_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_handle_t io = panel_io();
    if (io) esp_lcd_panel_io_tx_param(io, (0x02 << 24) | (cmd << 8), NULL, 0);   // QSPI write-command framing
}

// ---- policy ----
static volatile bool s_force_batt;              // USB shell "lpforce 1": behave as on battery while plugged (diagnosis)
void power_force_battery(bool on) { s_force_batt = on; }
static bool on_battery(void)
{
    if (s_force_batt) return true;
    batt_t b; hw_batt(&b);
    return b.present && !b.vbus;
}
bool power_sleep_allowed(void)
{
    return hw_pref_get("lp", 1) && on_battery() && !rec_active() && !wifi_ap_active();
}

static void account(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_screen_on) s_ms_active += now - s_t_state; else s_ms_doze += now - s_t_state;
    s_t_state = now;
}

static void locks(bool hold)
{
    if (hold == s_locks_held) return;
    if (hold) { esp_pm_lock_acquire(s_lk_cpu); esp_pm_lock_acquire(s_lk_nosleep); }
    else { esp_pm_lock_release(s_lk_cpu); esp_pm_lock_release(s_lk_nosleep); }
    s_locks_held = hold;
}

// Pins that must keep their level during automatic light sleep (CONFIG_PM_SLP_DISABLE_GPIO isolates the rest).
static void keep_pins_in_sleep(void)
{
    const int pins[] = {BSP_LCD_RST, BSP_LCD_TOUCH_RST, BSP_LCD_CS, BSP_POWER_AMP_IO, BSP_I2C_SCL, BSP_I2C_SDA, 0, BSP_LCD_TOUCH_INT, 21, 18 /* motor */};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) gpio_sleep_sel_dis(pins[i]);
}

void power_init(void)
{
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen", &s_lk_cpu);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "screen-ls", &s_lk_nosleep);
    esp_pm_config_t c = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
    esp_err_t e = esp_pm_configure(&c);
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_pm_configure failed: %d", e);
    keep_pins_in_sleep();
    locks(true);
    s_t_state = esp_timer_get_time() / 1000;
    s_wake_cause = esp_sleep_get_wakeup_cause();
    if (s_wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) ESP_LOGI(TAG, "woke from deep sleep #%lu, cause %d", (unsigned long)s_deep_count, s_wake_cause);
}

int power_wake_cause(void) { return s_wake_cause; }

// Called by the launcher (under the LVGL lock) whenever the screen turns on or off.
void power_update(bool screen_on)
{
    if (screen_on == s_screen_on) {             // re-evaluate only (e.g. USB unplugged)
        if (!screen_on) locks(!power_sleep_allowed());
        return;
    }
    account();
    s_screen_on = screen_on;
    if (screen_on) {
        s_wakes++;
        locks(true);
        gpio_wakeup_disable(BSP_LCD_TOUCH_INT);
        gpio_set_intr_type(BSP_LCD_TOUCH_INT, GPIO_INTR_NEGEDGE);
        panel_cmd(0x11);                          // SLPOUT
        vTaskDelay(pdMS_TO_TICKS(10));
        panel_cmd(0x29);                          // DISPON
        lv_obj_invalidate(lv_screen_active());
    } else {
        panel_cmd(0x28);                          // DISPOFF
        panel_cmd(0x10);                          // SLPIN: panel ~uA instead of ~mA at brightness 0
        s_doze_since = esp_timer_get_time() / 1000;
        // touch no longer wakes anything while dark (owner: only the BOOT button wakes the watch)
        locks(!power_sleep_allowed());
    }
}

// ---- deep sleep ----
void power_deep_sleep(uint32_t timer_s)
{
    ESP_LOGW(TAG, "entering deep sleep (timer %lus)", (unsigned long)timer_s);
    diag_event("DEEP SLEEP timer=%lus (still %lus)", (unsigned long)timer_s, (unsigned long)(hw_imu_still_ms() / 1000));
    diag_flush();
    hw_steps_save();
    bool wom = hw_imu_wom_arm();
    panel_cmd(0x28);
    panel_cmd(0x10);
    s_deep_count++;
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    rtc_gpio_pullup_en(GPIO_NUM_0);
    if (wom) {
        rtc_gpio_pulldown_en(GPIO_NUM_21);
        rtc_gpio_pullup_dis(GPIO_NUM_21);
        esp_sleep_enable_ext1_wakeup(1ULL << 21, ESP_EXT1_WAKEUP_ANY_HIGH);
    }
    if (timer_s) esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

// ---- stats + battery log ----
void power_stats(power_stats_t *s)
{
    account();
    s->ms_active = s_ms_active;
    s->ms_doze = s_ms_doze;
    s->wakes = s_wakes;
    s->deep_count = s_deep_count;
    s->sleep_allowed = power_sleep_allowed();
    s->screen_on = s_screen_on;
}

static void log_line(void)
{
    char path[48]; snprintf(path, sizeof(path), "%s/power.csv", "/flash");
    FILE *f = fopen(path, "a");
    if (!f) return;
    batt_t b; hw_batt(&b);
    struct tm t; char ts[24] = "?";
    if (hw_rtc_get(&t)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &t);
    power_stats_t s; power_stats(&s);
    if (ftell(f) == 0) fprintf(f, "time,pct,mv,usb,active_s,doze_s,wakes,deep\n");
    fprintf(f, "%s,%d,%d,%d,%lld,%lld,%lu,%lu\n", ts, b.pct, b.mv, b.vbus, s.ms_active / 1000, s.ms_doze / 1000, (unsigned long)s.wakes, (unsigned long)s.deep_count);
    fclose(f);
}

// ---- battery drain since unplugging (the real-world battery-life measurement) ----
// On the USB->battery transition the time (RTC, minutes since 2000) and charge % are stored in NVS, so the number
// survives deep sleep and reboots.
static int now_min(void)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return -1;
    return (int)((mktime(&t) - 946684800) / 60);
}
static void drain_track(void)
{
    static int last_vbus = -1;
    batt_t b; hw_batt(&b);
    if (!b.present) return;
    // NVS is written only on a USB state change (never periodically: flash wear)
    if (!b.vbus && (last_vbus == 1 || (last_vbus == -1 && hw_pref_get("unpl_t", -1) < 0))) {
        hw_pref_set("unpl_t", now_min()); hw_pref_set("unpl_p", b.pct);
    }
    if (b.vbus && last_vbus != 1 && hw_pref_get("unpl_t", -1) >= 0) hw_pref_set("unpl_t", -1);
    last_vbus = b.vbus;
}
void power_drain_text(char *out, size_t n)
{
    batt_t b; hw_batt(&b);
    int t0 = hw_pref_get("unpl_t", -1), p0 = hw_pref_get("unpl_p", 0), now = now_min();
    if (!b.present) { snprintf(out, n, "No battery"); return; }
    if (b.vbus || t0 < 0 || now < t0) { snprintf(out, n, "Charge %d%%  (on USB)", b.pct); return; }
    int mins = now - t0, used = p0 - b.pct;
    if (mins < 30 || used <= 0) { snprintf(out, n, "Charge %d%%  measuring use... (%d min)", b.pct, mins); return; }
    float per_h = used * 60.0f / mins;
    snprintf(out, n, "Charge %d%%  %.1f%%/h = ~%.0f h per charge", b.pct, per_h, 100.0f / per_h);
}

// Background supervisor: battery log every 15 min, re-evaluates sleep permission, decides on deep sleep.
static void power_task(void *a)
{
    int64_t last_log = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        int64_t now = esp_timer_get_time() / 1000;
        drain_track();
        if (now - last_log > 15 * 60 * 1000 || last_log == 0) { last_log = now; log_line(); }
        if (s_screen_on) continue;
        bsp_display_lock(0);
        power_update(false);                     // USB plugged/unplugged while dark
        bsp_display_unlock();
        int deep_min = hw_pref_get("deep", 30);   // 0 = never
        if (deep_min > 0 && power_sleep_allowed() && !ble_connected() && hw_imu_still_ms() > (uint32_t)deep_min * 60000 &&
            now - s_doze_since > (int64_t)deep_min * 60000)
            power_deep_sleep(sched_next_s());     // wake for the next Amber check-in
    }
}
void power_start(void) { xTaskCreate(power_task, "power", 4096, NULL, 2, NULL); }

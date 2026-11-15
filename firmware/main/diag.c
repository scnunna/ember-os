// On-watch diagnostics log for days away from the laptop: /flash/diag.log (rotates to diag.1.log at 256 KB).
//  * events from the firmware (boot + reset reason, Bluetooth connect/drop + reason, sleep, check-ins, Amber, buttons...)
//  * every ESP-IDF warning/error line (rate-limited: max 30 a minute, the rest are only counted)
//  * a snapshot every 5 minutes: battery, charging, steps, link, screen, memory
// Lines go to a RAM ring first and are appended to flash once a minute and right before deep sleep, so logging never
// blocks a task on storage. A "last event" breadcrumb survives a crash/watchdog reset and is reported at the next boot.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "app.h"
#include "hw.h"
#include "ble.h"
#include "link.h"
#include "diag.h"

#define RING 24576
#define FILE_MAX (256 * 1024)
#define F_LOG "/flash/diag.log"
#define F_OLD "/flash/diag.1.log"

EXT_RAM_BSS_ATTR static char s_ring[RING];
static size_t s_len;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;
static volatile int s_in_hook;
static uint32_t s_minute_lines, s_dropped_lines;
static int64_t s_minute_t0;
RTC_NOINIT_ATTR static char s_crumb[72];     // last event, survives a software/watchdog/panic reset
RTC_NOINIT_ATTR static uint32_t s_crumb_magic;

// Timestamps: the RTC is read once at boot; after that local time = that + uptime (esp_timer keeps counting through
// light sleep), so logging never touches the I2C bus and is safe from any task.
static time_t s_base_local = -1;
static int64_t s_base_us;
static void stamp(char *b, size_t n)
{
    int64_t up = esp_timer_get_time();
    if (s_base_local < 0) { snprintf(b, n, "up%llds", (long long)(up / 1000000)); return; }
    time_t t = s_base_local + (time_t)((up - s_base_us) / 1000000);
    struct tm tm; gmtime_r(&t, &tm);                 // local seconds stored as if UTC (the watch has no TZ)
    strftime(b, n, "%m-%d %H:%M:%S", &tm);
}

static void append(const char *line)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    size_t n = strlen(line);
    if (s_len + n + 1 > RING) { s_dropped_lines++; xSemaphoreGive(s_lock); return; }   // flush is late: drop, don't block
    memcpy(s_ring + s_len, line, n); s_len += n;
    if (n == 0 || line[n - 1] != '\n') s_ring[s_len++] = '\n';
    xSemaphoreGive(s_lock);
}

void diag_event(const char *fmt, ...)
{
    char ts[20], msg[140], line[170];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    stamp(ts, sizeof(ts));
    snprintf(line, sizeof(line), "%s %s", ts, msg);
    append(line);
    snprintf(s_crumb, sizeof(s_crumb), "%s", line); s_crumb_magic = 0xD1A6C0DE;
}

// ESP-IDF log hook: keep W/E lines (the console still gets everything)
static int hook(const char *fmt, va_list ap)
{
    va_list ap2; va_copy(ap2, ap);
    int r = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
    if (!s_in_hook && fmt && (fmt[0] == 'W' || fmt[0] == 'E' || (fmt[0] == '\033' && (strstr(fmt, "W (") || strstr(fmt, "E ("))))) {
        s_in_hook = 1;
        int64_t now = esp_timer_get_time();
        if (now - s_minute_t0 > 60000000LL) { s_minute_t0 = now; s_minute_lines = 0; }
        if (++s_minute_lines <= 30) {
            char m[120];
            vsnprintf(m, sizeof(m), fmt, ap2);
            char *e = m + strlen(m); while (e > m && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            char *p = strchr(m, 'W'); char *q = strchr(m, 'E');          // drop color codes
            char *start = (p && (!q || p < q)) ? p : (q ? q : m);
            diag_event("LOG %s", start);
        } else s_dropped_lines++;
        s_in_hook = 0;
    }
    va_end(ap2);
    return r;
}

void diag_flush(void)
{
    if (!s_lock) return;
    EXT_RAM_BSS_ATTR static char buf[RING];    // PSRAM: internal DMA RAM is what Wi-Fi needs
    size_t n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_len; memcpy(buf, s_ring, n); s_len = 0;
    xSemaphoreGive(s_lock);
    if (!n) return;
    s_in_hook = 1;                                   // file-system warnings while flushing must not re-enter
    FILE *f = fopen(F_LOG, "a");
    if (f) {
        fwrite(buf, 1, n, f);
        long size = ftell(f);
        fclose(f);
        if (size > FILE_MAX) { remove(F_OLD); rename(F_LOG, F_OLD); }
    }
    s_in_hook = 0;
}

static void snapshot(void)
{
    batt_t b; hw_batt(&b);
    power_stats_t p; power_stats(&p);
    diag_event("SNAP batt=%d%% mv=%d chg=%d usb=%d steps=%lu ble=%d link=%d screen=%d wakes=%lu deep=%lu dma=%u dma_min=%u psram=%u lost_lines=%lu",
               b.pct, b.mv, b.charging, b.vbus, (unsigned long)hw_steps_today(), ble_connected(), link_ready(), launcher_screen_on(),
               (unsigned long)p.wakes, (unsigned long)p.deep_count,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)s_dropped_lines);
}

static void diag_task(void *a)
{
    int n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (++n % 5 == 1) snapshot();
        diag_flush();
    }
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on"; case ESP_RST_SW: return "software"; case ESP_RST_PANIC: return "CRASH(panic)";
    case ESP_RST_INT_WDT: return "CRASH(int-watchdog)"; case ESP_RST_TASK_WDT: return "CRASH(task-watchdog)";
    case ESP_RST_WDT: return "CRASH(watchdog)"; case ESP_RST_DEEPSLEEP: return "deep-sleep-wake"; case ESP_RST_BROWNOUT: return "BROWNOUT(battery)";
    case ESP_RST_USB: return "usb"; default: return "other";
    }
}

void diag_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    struct tm t;
    if (hw_rtc_get(&t)) { s_base_local = mktime(&t); s_base_us = esp_timer_get_time(); }
    esp_reset_reason_t rr = esp_reset_reason();
    char prev[72] = "-";
    if (s_crumb_magic == 0xD1A6C0DE && rr != ESP_RST_POWERON) snprintf(prev, sizeof(prev), "%s", s_crumb);
    diag_event("BOOT fw=" EMBER_VERSION " reset=%s(%d) wake=%d last_before_reset=[%s]", reset_name(rr), (int)rr, power_wake_cause(), prev);
    s_orig = esp_log_set_vprintf(hook);
    snapshot();
    xTaskCreate(diag_task, "diag", 3072, NULL, 1, NULL);
}

// USB shell: last `kb` KB of the log (file + not-yet-flushed lines), between DIAG BEGIN / DIAG END
void diag_dump(int kb)
{
    diag_flush();
    FILE *f = fopen(F_LOG, "r");
    printf("DIAG BEGIN\n");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f), from = size - (long)kb * 1024;
        fseek(f, from > 0 ? from : 0, SEEK_SET);
        char line[220];
        while (fgets(line, sizeof(line), f)) { fputs(line, stdout); usj_end_packet(); }
        fclose(f);
    }
    printf("DIAG END\n");
}

void diag_clear(void) { diag_flush(); remove(F_LOG); remove(F_OLD); }

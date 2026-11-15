// Watch firmware entry point.
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "diag.h"
#include "hw.h"
#include "ble.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"

#ifdef WATCH_SELFTEST
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
float tuner_selftest(float hz);
#include "wifi.h"

static void check_wav(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) { ESP_LOGE("TEST", "FAIL: %s missing", path); return; }
    FILE *f = fopen(path, "rb");
    uint8_t h[44]; fread(h, 1, 44, f);
    uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24, dlen = h[40] | h[41] << 8 | h[42] << 16 | h[43] << 24;
    static int16_t b[1024]; double e = 0; long n = 0, clip = 0; int pk = 0;
    size_t r;
    while ((r = fread(b, 2, 1024, f)) > 0) for (size_t i = 0; i < r; i++) { e += (double)b[i] * b[i]; n++; if (abs(b[i]) > pk) pk = abs(b[i]); if (abs(b[i]) > 32000) clip++; }
    fclose(f);
    ESP_LOGI("TEST", "WAV %s: file=%ld B, hdr rate=%lu data=%lu, samples=%ld (%.2f s) rms=%.0f peak=%d clip=%ld  %s", path, (long)st.st_size,
             (unsigned long)rate, (unsigned long)dlen, n, (double)n / rate, sqrt(e / (n ? n : 1)), pk, clip,
             (dlen + 44 == (uint32_t)st.st_size && n > 0) ? "PASS" : "FAIL(size mismatch)");
}

static void rate_test(uint32_t rate)
{
    static int16_t b[1024];
    if (!hw_mic_open(rate, 24)) { ESP_LOGE("TEST", "mic open fail"); return; }
    hw_mic_read_mono(b, 1024);
    int64_t t0 = esp_timer_get_time(); long fr = 0;
    while (esp_timer_get_time() - t0 < 4000000) { int n = hw_mic_read_mono(b, 1024); fr += n; }
    int64_t dt = esp_timer_get_time() - t0;
    hw_mic_close();
    ESP_LOGI("TEST", "RATE asked %lu -> measured %.0f Hz (pure capture, no storage)", (unsigned long)rate, fr * 1e6 / dt);
}

static void selftest_task(void *a)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    rate_test(16000); rate_test(44100); rate_test(32000);
    uint64_t tot, fre; hw_storage_info(&tot, &fre);
    ESP_LOGI("TEST", "storage=%s total=%llu MB free=%llu MB", hw_storage_root(), tot >> 20, fre >> 20);
    const float hz[] = {82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f};
    for (int i = 0; i < 6; i++) { float f = tuner_selftest(hz[i]); ESP_LOGI("TEST", "YIN %.2f Hz -> %.2f Hz  err %.1f cents  %s", hz[i], f, f > 0 ? 1200 * log2f(f / hz[i]) : 999, (f > 0 && fabsf(1200 * log2f(f / hz[i])) < 5) ? "PASS" : "FAIL"); }
    for (int m = 0; m < 2; m++) {
        ESP_LOGI("TEST", "recording mode %d for 6 s (make some noise / talk)", m);
        if (!rec_start(m)) { ESP_LOGE("TEST", "rec_start FAILED"); continue; }
        vTaskDelay(pdMS_TO_TICKS(6000));
        rec_stop();
        check_wav(rec_last_file());
    }
    hw_beep(1000, 200, 80);
    ESP_LOGI("TEST", "beep done");
    for (int i = 0; i < 3; i++) { float x, y, z; hw_imu_get_g(&x, &y, &z); ESP_LOGI("TEST", "IMU g=(%.2f %.2f %.2f) |g|=%.2f", x, y, z, sqrtf(x * x + y * y + z * z)); vTaskDelay(pdMS_TO_TICKS(300)); }
    ESP_LOGI("TEST", "free heap=%lu psram=%lu", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    wifi_ap_start(); vTaskDelay(pdMS_TO_TICKS(4000)); ESP_LOGI("TEST", "WIFI AP active=%d ssid=%s", wifi_ap_active(), wifi_ap_ssid()); wifi_ap_stop();
    ESP_LOGI("TEST", "SELFTEST COMPLETE");
    vTaskDelete(NULL);
}
#endif



#ifdef WATCH_SNAP
void snap_start(void);
#endif

#define MEM(tag) ESP_LOGI("mem", "%-10s internal free=%6u largest=%6u  dma free=%6u largest=%6u", tag, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void app_main(void)
{
    MEM("start");
    bsp_i2c_init();
    hw_init();
    MEM("hw");
    power_init();
    diag_start();                                  // diagnostics log (after storage + RTC)
    MEM("power");
    bsp_display_cfg_t dcfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {.buff_dma = false, .buff_spiram = true},
    };
    dcfg.lvgl_port_cfg.task_max_sleep_ms = 2000;   // when nothing is scheduled, LVGL wakes at most every 2 s
    dcfg.lvgl_port_cfg.task_stack = 12288;          // default 7168 left only ~2.5 KB spare in the busiest screens
    lv_display_t *disp = bsp_display_start_with_config(&dcfg);
    MEM("display");
    // The BSP's draw buffer is not DMA-capable, so every flush needed a big temporary internal-RAM copy that
    // failed once Bluetooth/Wi-Fi had eaten memory -> whole strips of the screen never updated (ghost layers).
    // Give LVGL two small DMA-capable internal buffers, allocated now while memory is plentiful.
    {
        const size_t px = 410 * 20;     // 2 x 16 KB (was 2 x 32 KB): internal DMA RAM is the scarcest resource on this board
        const size_t bytes = ((px * 2) + 63) & ~(size_t)63;        // LVGL wants 64-byte aligned buffers
        void *b1 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        void *b2 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (disp && b1 && b2) {
            bsp_display_lock(0);
            lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
            bsp_display_unlock();
            ESP_LOGI("watch", "display: 2 x %u B DMA buffers", (unsigned)(px * 2));
        } else ESP_LOGE("watch", "display: DMA buffer alloc failed (%p %p)", b1, b2);
    }
    // Power: esp_lvgl_port drives LVGL's clock with a 5 ms periodic timer = 200 CPU wake-ups per second, which makes
    // light sleep useless. Read the clock from esp_timer instead and stop that timer for good (lvgl_port_stop also
    // disables LVGL timers, so they are re-enabled right away). LVGL then sleeps until its next real deadline.
    bsp_display_lock(0);
    lv_tick_set_cb(tick_ms);
    lvgl_port_stop();
    lv_timer_enable(true);
    bsp_display_unlock();
    MEM("dmabufs");
    script_init();
    MEM("scripts");
    launcher_start();
    MEM("launcher");
    buttons_start();
    ble_start();
    MEM("ble");
    cmd_start();
    power_start();
    MEM("up");
    ESP_LOGI("watch", "Ember OS " EMBER_VERSION " up (wake cause %d)", power_wake_cause());
#ifdef WATCH_SNAP
    snap_start();
#endif
#ifdef WATCH_SELFTEST
    xTaskCreate(selftest_task, "selftest", 8192, NULL, 2, NULL);
#endif
}

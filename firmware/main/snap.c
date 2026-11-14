// Debug-only (built with -DSNAP=1): dump screenshots of every screen over USB serial.
#ifdef WATCH_SNAP
#include <stdio.h>
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static void dump_tree(const char *name)
{
    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    printf("TREE %s scroll_x=%d scroll_y=%d w=%d h=%d\n", name, (int)lv_obj_get_scroll_x(scr), (int)lv_obj_get_scroll_y(scr),
           (int)lv_obj_get_width(scr), (int)lv_obj_get_height(scr));
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        lv_area_t a; lv_obj_get_coords(o, &a);
        printf("  child %u: x=%d..%d y=%d..%d\n", (unsigned)i, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2);
    }
    bsp_display_unlock();
}

static void snap(const char *name)
{
    dump_tree(name);
    bsp_display_lock(0);
    lv_draw_buf_t *d = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();
    if (!d) { printf("SNAP_FAIL %s\n", name); return; }
    // Decimate 2x (every other pixel) so the USB transfer takes ~10 s instead of ~45 s.
    uint32_t W = d->header.w, H = d->header.h, stride = d->header.stride;
    uint32_t w = W / 2, h = H / 2;
    uint16_t *small = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *src = (const uint16_t *)((const uint8_t *)d->data + (size_t)stride * y * 2);
        for (uint32_t x = 0; x < w; x++) small[y * w + x] = src[x * 2];
    }
    stride = w * 2;
    printf("\nSNAP_BEGIN %s %lu %lu %lu\n", name, (unsigned long)w, (unsigned long)h, (unsigned long)stride);
    size_t total = (size_t)stride * h, off = 0;
    static unsigned char out[1024];
    while (off < total) {
        size_t n = total - off > 600 ? 600 : total - off, ol = 0;
        mbedtls_base64_encode(out, sizeof(out), &ol, (const unsigned char *)small + off, n);
        out[ol] = 0;
        printf("@%05lu %s\n", (unsigned long)(off / 600), out);
        vTaskDelay(pdMS_TO_TICKS(4));
        off += n;
    }
    printf("SNAP_END\n");
    fflush(stdout);
    free(small);
    bsp_display_lock(0);
    lv_draw_buf_destroy(d);
    bsp_display_unlock();
}

static void snap_task(void *a)
{
    vTaskDelay(pdMS_TO_TICKS(2500));
    snap("face");
    bsp_display_lock(0); launcher_home(); bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(700));
    snap("grid");
    for (int i = 0; i < APP_COUNT; i++) {
        bsp_display_lock(0); launcher_open(i); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(1500));
        snap(APPS[i].name);
        bsp_display_lock(0); launcher_home(); bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    printf("SNAP_ALL_DONE\n");
    vTaskDelete(NULL);
}

void snap_start(void) { xTaskCreate(snap_task, "snap", 8192, NULL, 2, NULL); }
#endif

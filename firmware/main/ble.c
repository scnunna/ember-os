// BLE peripheral with the Nordic-UART-style service used by the Chronos Android app.
// Protocol (frame: 0xAB, len_hi, len_lo, 0xFF|0xFE, cmd, ...) taken from fbiego/chronos-esp32 (read-only reference).
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "hw.h"
#include "ble.h"

static const char *TAG = "ble";
static void send_battery(void);

// 6e400001-b5a3-f393-e0a9-e50e24dcca9e (little-endian byte order for NimBLE)
#define UUID_BYTES(n) {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, (n), 0x00, 0x40, 0x6e}
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(UUID_BYTES(0x01));
static const ble_uuid128_t UUID_RX = BLE_UUID128_INIT(UUID_BYTES(0x02));
static const ble_uuid128_t UUID_TX = BLE_UUID128_INIT(UUID_BYTES(0x03));

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE, s_tx_handle;
static bool s_inited, s_enabled, s_adv_wanted;
static uint8_t s_own_addr;
static char s_name[20] = "Watch";
static int s_phone_batt = -1;
static ble_notif_t s_notifs[BLE_NOTIF_MAX];
static int s_notif_total;
static QueueHandle_t s_txq;
typedef struct { uint8_t len; uint8_t d[24]; } txmsg_t;

const char *ble_name(void) { return s_name; }
bool ble_enabled(void) { return s_enabled; }
bool ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
int ble_phone_batt(void) { return s_phone_batt; }
int ble_notif_count(void) { return s_notif_total; }
static int s_seen;
int ble_notif_unread(void) { return s_notif_total - s_seen; }
void ble_notif_mark_seen(void) { s_seen = s_notif_total; }
int ble_notif_get(int back, ble_notif_t *out)
{
    if (back < 0 || back >= BLE_NOTIF_MAX || back >= s_notif_total) return 0;
    *out = s_notifs[(s_notif_total - 1 - back) % BLE_NOTIF_MAX];
    return 1;
}

// ---------------------------------------------------------------- TX (queued, paced like the reference lib)
static void tx_raw(const uint8_t *d, int n)
{
    if (!s_txq || n > 24) return;
    txmsg_t m = {.len = n};
    memcpy(m.d, d, n);
    xQueueSend(s_txq, &m, 0);
}
static void tx_task(void *arg)
{
    txmsg_t m;
    while (1) {
        if (xQueueReceive(s_txq, &m, pdMS_TO_TICKS(60000)) != pdTRUE) {      // idle minute: refresh phone-side stats
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) { ble_send_steps(hw_steps_today()); send_battery(); }
            continue;
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(m.d, m.len);
            if (om) ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static void send_battery(void);
static void send_info(void)
{
    const uint8_t info[] = {0xab, 0x00, 0x11, 0xff, 0x92, 0xc0, 1, 0, 0x00, 0xfb, 0x1e, 0x40, 0xc0, 0x0e, 0x32, 0x28, 0x00, 0xe2, 0x00, 0x80};
    tx_raw(info, sizeof(info));
}
static void send_battery(void)
{
    batt_t b; hw_batt(&b);
    uint8_t c[] = {0xAB, 0x00, 0x05, 0xFF, 0x91, 0x80, b.charging ? 1 : 0, (uint8_t)(b.present ? b.pct : 100)};
    tx_raw(c, sizeof(c));
}
void ble_send_steps(uint32_t steps)
{
    uint32_t cal = steps * 4 / 100;
    uint8_t c[] = {0xAB, 0x00, 0x0E, 0xFF, 0x51, 0x08, (uint8_t)(steps >> 16), (uint8_t)(steps >> 8), (uint8_t)steps,
                   (uint8_t)(cal >> 16), (uint8_t)(cal >> 8), (uint8_t)cal, 0, 0, 0, 0, 0};
    tx_raw(c, sizeof(c));
}
void ble_music(uint16_t cmd)
{
    uint8_t c[] = {0xAB, 0x00, 0x04, 0xFF, (uint8_t)(cmd >> 8), 0x80, (uint8_t)cmd};
    tx_raw(c, sizeof(c));
}
void ble_find_phone(bool on)
{
    uint8_t c[] = {0xAB, 0x00, 0x04, 0xFF, 0x7D, 0x80, on ? 1 : 0};
    tx_raw(c, sizeof(c));
}

// ---------------------------------------------------------------- RX frame assembly + commands
static uint8_t s_asm[400];
static int s_asm_len, s_asm_total;

static void sanitize(char *dst, size_t cap, const uint8_t *src, int n)
{
    int j = 0;
    for (int i = 0; i < n && j < (int)cap - 1; i++) dst[j++] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[j] = 0;
}

static void on_notification(const uint8_t *d, int len)
{
    if (len < 9) return;
    int icon = d[6], state = d[7];
    char txt[96];
    sanitize(txt, sizeof(txt), d + 8, len - 8);
    if (icon == 0x01 || icon == 0x02) {
        bsp_display_lock(0);
        launcher_wake();
        ui_msg(icon == 0x01 ? "Incoming call" : "Call ended");
        bsp_display_unlock();
        return;
    }
    if (state != 0x02) return;
    ble_notif_t *n = &s_notifs[s_notif_total % BLE_NOTIF_MAX];
    n->icon = icon;
    strncpy(n->text, txt, sizeof(n->text));
    struct tm t;
    if (hw_rtc_get(&t)) snprintf(n->time, sizeof(n->time), "%02d:%02d", t.tm_hour, t.tm_min); else n->time[0] = 0;
    s_notif_total++;
    bsp_display_lock(0);
    launcher_wake();
    ui_msg(txt);
    bsp_display_unlock();
}

static void handle_frame(const uint8_t *d, int len)
{
    if (len < 5) return;
    switch (d[4]) {
    case 0x92: send_info(); send_battery(); break;
    case 0x93:
        if (len >= 14) {
            struct tm t = {0};
            t.tm_year = (d[7] * 256 + d[8]) - 1900; t.tm_mon = d[9] - 1; t.tm_mday = d[10];
            t.tm_hour = d[11]; t.tm_min = d[12]; t.tm_sec = d[13];
            time_t e = mktime(&t);
            gmtime_r(&e, &t);
            hw_rtc_set(&t);
            ESP_LOGI(TAG, "time set from phone: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        }
        break;
    case 0x91:
        if (d[3] == 0xFE && len >= 8) s_phone_batt = d[7];
        break;
    case 0x72: on_notification(d, len); break;
    default: break;
    }
}

static void on_rx(const uint8_t *d, int n)
{
    if (n >= 5 && (d[0] == 0xAB || d[0] == 0xEA) && (d[3] == 0xFE || d[3] == 0xFF)) {
        s_asm_total = d[1] * 256 + d[2] + 3;
        if (s_asm_total > (int)sizeof(s_asm)) { s_asm_total = 0; return; }
        s_asm_len = 0;
        memcpy(s_asm, d, n);
        s_asm_len = n;
    } else if (s_asm_total > 0 && n > 1) {
        int c = n - 1;
        if (s_asm_len + c > (int)sizeof(s_asm)) c = sizeof(s_asm) - s_asm_len;
        memcpy(s_asm + s_asm_len, d + 1, c);
        s_asm_len += c;
    } else return;
    if (s_asm_total > 0 && s_asm_len >= s_asm_total) {
        handle_frame(s_asm, s_asm_len);
        s_asm_total = 0;
    }
}

// ---------------------------------------------------------------- GATT
static int rx_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint8_t buf[260];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) == 0) on_rx(buf, len);
    return 0;
}
static int tx_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg) { return 0; }

static const struct ble_gatt_svc_def GATT[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &UUID_SVC.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &UUID_RX.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = &UUID_TX.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
         {0}}},
    {0}};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    if (!s_enabled || !s_adv_wanted) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = BLE_GAP_ADV_ITVL_MS(1000), .itvl_max = BLE_GAP_ADV_ITVL_MS(1285)};
    int rc = ble_gap_adv_start(s_own_addr, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start rc=%d", rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; ESP_LOGI(TAG, "connected"); }
        else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_asm_total = 0;
        ESP_LOGI(TAG, "disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle && ev->subscribe.cur_notify) { send_info(); send_battery(); }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: advertise(); break;
    default: break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr);
    s_adv_wanted = true;
    advertise();
}
static void host_task(void *p)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_set_enabled(bool on)
{
    s_enabled = on;
    hw_pref_set("ble", on);
    if (!s_inited) return;
    if (on) advertise();
    else {
        ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "Watch-%02X%02X", mac[4], mac[5]);
    s_enabled = hw_pref_get("ble", 1);
    s_txq = xQueueCreate(16, sizeof(txmsg_t));
    xTaskCreate(tx_task, "ble_tx", 3072, NULL, 4, NULL);
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(GATT);
    ble_gatts_add_svcs(GATT);
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
    s_inited = true;
    ESP_LOGI(TAG, "started as %s (enabled=%d)", s_name, s_enabled);
}

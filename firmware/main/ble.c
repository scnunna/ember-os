// BLE peripheral with the Nordic-UART-style service used by the Chronos Android app.
// Protocol (frame: 0xAB, len_hi, len_lo, 0xFF|0xFE, cmd, ...) taken from fbiego/chronos-esp32 (read-only reference).
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
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
#include "diag.h"
#include "hw.h"
#include "ble.h"
#include "link.h"
#include "devices.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_store.h"
void ble_store_config_init(void);

static const char *TAG = "ble";
static void send_battery(void);

// 6e400001-b5a3-f393-e0a9-e50e24dcca9e (little-endian byte order for NimBLE)
#define UUID_BYTES(n) 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, (n), 0x00, 0x40, 0x6e   // no braces (see link.c)
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(UUID_BYTES(0x01));
static const ble_uuid128_t UUID_RX = BLE_UUID128_INIT(UUID_BYTES(0x02));
static const ble_uuid128_t UUID_TX = BLE_UUID128_INIT(UUID_BYTES(0x03));

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE, s_tx_handle;
static volatile bool s_radio_off;              // tests: no phone may connect (see ble_test_radio_off)
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
static void advertise(void);
// Battery: the radio wakes at every connection event. Idle: ask for 90-120 ms events and let the watch skip up to 4
// (slave latency), so it listens about every 0.5 s while still answering within one event when it has something to
// send. Fast (15-30 ms, no latency) only while audio streams to the phone. Android decides; the result is logged.
static volatile int s_itvl_x125;               // current interval in 1.25 ms units (0 = unknown)
static esp_timer_handle_t s_slow_tmr;
void ble_link_fast(bool fast)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_upd_params p = fast
        ? (struct ble_gap_upd_params){.itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 500}
        : (struct ble_gap_upd_params){.itvl_min = 72, .itvl_max = 96, .latency = 4, .supervision_timeout = 600};
    int rc = ble_gap_update_params(s_conn, &p);
    if (rc) ESP_LOGW(TAG, "conn update (%s) rc=%d", fast ? "fast" : "slow", rc);
}
int ble_link_interval_ms(void) { return s_itvl_x125 * 5 / 4; }
static void slow_cb(void *a) { ble_link_fast(false); }
void ble_link_slow_later(int ms)
{
    if (!s_slow_tmr) { esp_timer_create_args_t a = {.callback = slow_cb, .name = "bleslow"}; esp_timer_create(&a, &s_slow_tmr); }
    esp_timer_stop(s_slow_tmr);
    esp_timer_start_once(s_slow_tmr, (uint64_t)ms * 1000);
}

// Device tests: drop the phone and stop advertising, so nothing a test does can reach the real phone / Life OS.
void ble_test_radio_off(bool off)
{
    s_radio_off = off;
    if (off) { ble_gap_adv_stop(); if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    else advertise();
}
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
    if (link_ready()) {                     // Ember Companion connected: use the Ember Link message
        const char *c = cmd == MUSIC_NEXT ? "next" : cmd == MUSIC_PREV ? "prev" : cmd == VOLUME_UP ? "vol_up" : cmd == VOLUME_DOWN ? "vol_down" : "toggle";
        link_music(c);
    }
    uint8_t c[] = {0xAB, 0x00, 0x04, 0xFF, (uint8_t)(cmd >> 8), 0x80, (uint8_t)cmd};
    tx_raw(c, sizeof(c));
}
void ble_find_phone(bool on)
{
    if (link_ready()) link_find_phone(on);
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
        launcher_event();
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
    launcher_event();
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

// Ember Link: every write needs an encrypted, authenticated (passkey) link -> ATT error 0x05 makes Android pair.
// Subscriptions are also refused in link.c until the link is secure (NimBLE does not guard CCCD writes itself).
#define LINK_SVC_DEF {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &LINK_UUID_SVC.u,                                   \
     .characteristics = (struct ble_gatt_chr_def[]){                                                                 \
         {.uuid = &LINK_UUID_CMD.u, .access_cb = link_cmd_access,                                                   \
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN}, \
         {.uuid = &LINK_UUID_EVT.u, .access_cb = link_noop_access, .val_handle = &link_evt_handle, .flags = BLE_GATT_CHR_F_NOTIFY}, \
         {.uuid = &LINK_UUID_AUDIO.u, .access_cb = link_noop_access, .val_handle = &link_audio_handle, .flags = BLE_GATT_CHR_F_NOTIFY}, \
         {.uuid = &LINK_UUID_SPK.u, .access_cb = link_spk_access,                                                   \
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN}, \
         {0}}}
static const struct ble_gatt_svc_def GATT_WITH_CHRONOS[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,                       // Chronos (no authentication: off by default, S2)
     .uuid = &UUID_SVC.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &UUID_RX.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = &UUID_TX.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
         {0}}},
    LINK_SVC_DEF,
    {0}};
static const struct ble_gatt_svc_def GATT_LINK_ONLY[] = {LINK_SVC_DEF, {0}};
static bool s_chronos;

// ---------------------------------------------------------------- pairing (S1): passkey shown on the watch, one phone
static volatile int64_t s_pair_until;           // pairing window end (us); 0 = closed
static esp_timer_handle_t s_unsec_tmr;           // drops connections that never become secure (slot squatting)
bool ble_pair_window_open(void) { return esp_timer_get_time() < s_pair_until; }
void ble_pair_open(void)
{
    // up to DEV_MAX remembered devices (Phone app > Devices: forget / rename / use); the oldest is replaced when full
    s_pair_until = esp_timer_get_time() + 120LL * 1000000;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGW(TAG, "pairing window open for 120 s");
}
bool ble_bonded(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n);
    return n > 0;
}
static bool conn_secure(uint16_t c)
{
    struct ble_gap_conn_desc d;
    return c != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(c, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated && d.sec_state.bonded;
}
static void unsec_cb(void *a)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && !conn_secure(s_conn) && !s_chronos) {
        ESP_LOGW(TAG, "connection not secured in 30 s: dropping it");
        ble_gap_terminate(s_conn, BLE_ERR_AUTH_FAIL);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    if (s_radio_off) return;
    if (!s_enabled || !s_adv_wanted) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)(s_chronos ? &UUID_SVC : &LINK_UUID_SVC);
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
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle; link_on_connect(s_conn); ESP_LOGI(TAG, "connected"); diag_event("BLE connected");
            esp_timer_stop(s_unsec_tmr);
            esp_timer_start_once(s_unsec_tmr, 30LL * 1000000);
            // ask a bonded phone to switch on encryption right away (Security Request), so the link is ready
            // without waiting for the phone's first write; an unknown device gets refused at the passkey step
            if (ble_bonded()) ble_gap_security_initiate(s_conn);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_asm_total = 0;
        esp_timer_stop(s_unsec_tmr);
        devices_on_disconnect();
        ui_passkey(-1);
        link_on_disconnect();
        ESP_LOGI(TAG, "disconnected");
        diag_event("BLE disconnected reason=0x%x", ev->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle && ev->subscribe.cur_notify) { send_info(); send_battery(); }
        link_on_subscribe(ev->subscribe.attr_handle, ev->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
            s_itvl_x125 = d.conn_itvl;
            diag_event("BLE params itvl=%dms latency=%d timeout=%dms st=%d", d.conn_itvl * 5 / 4, d.conn_latency, d.supervision_timeout * 10, ev->conn_update.status);
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            if (!ble_pair_window_open()) {                    // nobody asked to pair: refuse (S1, one phone only)
                ESP_LOGW(TAG, "pairing attempt outside the pairing window: refused");
                ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
                break;
            }
            struct ble_sm_io pk = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            ui_passkey((int)pk.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        } else ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);   // only passkey-display is allowed
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // a known phone lost its keys: only re-pair inside the window, else keep the old bond and refuse
        if (!ble_pair_window_open()) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        bool sec = ev->enc_change.status == 0 && conn_secure(ev->enc_change.conn_handle);
        ESP_LOGI(TAG, "encryption change status=%d secure=%d", ev->enc_change.status, sec);
        diag_event("BLE secure=%d status=%d", sec, ev->enc_change.status);
        ui_passkey(-1);
        if (sec) {
            esp_timer_stop(s_unsec_tmr);
            if (ble_pair_window_open()) { s_pair_until = 0; hw_buzz_pattern(2, 80, 100); }   // paired: close the window
            devices_on_secure(ev->enc_change.conn_handle);    // remembered-devices list + reads its name
            link_on_secure(true);
            ble_link_slow_later(6000);                         // after the hello/plan exchange: low-power rhythm
        } else ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE: advertise(); break;
    default: break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr);
    ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK);
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
    snprintf(s_name, sizeof(s_name), "EmberWatchOS");      // owner's choice (was Watch-XXXX); the phone app accepts both
    (void)mac;
    s_enabled = hw_pref_get("ble", 1);
    s_txq = xQueueCreate(16, sizeof(txmsg_t));
    // stack in PSRAM: this task never writes flash (internal RAM is what the Wi-Fi hotspot needs)
    xTaskCreateWithCaps(tx_task, "ble_tx", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    link_start();
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    // S1: LE Secure Connections, bonding, MITM via passkey shown on the watch; keys stored in NVS
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    esp_timer_create_args_t ta = {.callback = unsec_cb, .name = "ble_unsec"};
    esp_timer_create(&ta, &s_unsec_tmr);
    devices_init();
    s_chronos = hw_pref_get("chronos", 0);            // S2: unauthenticated Chronos service off by default
    const struct ble_gatt_svc_def *gatt = s_chronos ? GATT_WITH_CHRONOS : GATT_LINK_ONLY;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt);
    ble_gatts_add_svcs(gatt);
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
    s_inited = true;
    ESP_LOGI(TAG, "started as %s (enabled=%d chronos=%d bonded=%d)", s_name, s_enabled, s_chronos, ble_bonded());
}

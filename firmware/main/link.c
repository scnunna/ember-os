// Ember Link v1 (docs/EMBER_LINK.md): JSON messages over the CMD/EVT characteristics, ADPCM audio on AUDIO.
// RX runs in the NimBLE host task only long enough to reassemble a message; everything else happens in link_task.
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "diag.h"
#include "hw.h"
#include "ble.h"
#include "wifi.h"
#include "link.h"
#include "adpcm.h"
#include "practice.h"
#include "devices.h"
#include "sched.h"

static const char *TAG = "link";
#define FW_VERSION "1.2"
#define MAX_MSG 4096

// eb3f00NN-6d62-4f53-8a1e-656d6265720a, little-endian for NimBLE
// NO braces: BLE_UUID128_INIT adds them. With braces only the first byte was kept (found by the real-radio test).
#define LUUID(n) 0x0a, 0x72, 0x65, 0x62, 0x6d, 0x65, 0x1e, 0x8a, 0x53, 0x4f, 0x62, 0x6d, (n), 0x00, 0x3f, 0xeb
const ble_uuid128_t LINK_UUID_SVC = BLE_UUID128_INIT(LUUID(0x01));
const ble_uuid128_t LINK_UUID_CMD = BLE_UUID128_INIT(LUUID(0x02));
const ble_uuid128_t LINK_UUID_EVT = BLE_UUID128_INIT(LUUID(0x03));
const ble_uuid128_t LINK_UUID_AUDIO = BLE_UUID128_INIT(LUUID(0x04));
uint16_t link_evt_handle, link_audio_handle;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_evt_sub_raw, s_audio_sub_raw, s_secure;   // raw CCCD state; only honoured once s_secure
#define s_evt_sub   (s_secure && s_evt_sub_raw)
#define s_audio_sub (s_secure && s_audio_sub_raw)
static QueueHandle_t s_rxq, s_txq;          // char* messages (heap); first byte: 'B' = Bluetooth, 'U' = USB (trusted)

bool link_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_evt_sub; }

// ------------------------------------------------------------------ RX (host task): reassemble FIRST..LAST
EXT_RAM_BSS_ATTR static char s_asm[MAX_MSG + 1];
static int s_asm_len = -1;                  // -1 = not inside a message

static void rx_packet(const uint8_t *d, int n)
{
    if (n < 1) return;
    uint8_t f = d[0];
    if (!s_secure) return;                   // defence in depth: GATT flags already require encryption
    if (f & 1) s_asm_len = 0;                // FIRST: start over
    if (s_asm_len < 0) return;               // continuation without a start: ignore
    int c = n - 1;
    if (s_asm_len + c > MAX_MSG) { s_asm_len = -1; ESP_LOGW(TAG, "message too long, dropped"); return; }
    memcpy(s_asm + s_asm_len, d + 1, c);
    s_asm_len += c;
    if (f & 2) {                             // LAST: hand over a copy
        s_asm[s_asm_len] = 0;
        char *m = malloc(s_asm_len + 2);
        // wait (briefly) rather than drop: the phone streams app_data chunks back to back
        if (m) { m[0] = 'B'; memcpy(m + 1, s_asm, s_asm_len + 1); if (xQueueSend(s_rxq, &m, pdMS_TO_TICKS(500)) != pdTRUE) { free(m); ESP_LOGE(TAG, "rx queue full, message dropped"); } }
        s_asm_len = -1;
    }
}

int link_cmd_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (!s_secure) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t buf[520];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) == 0) rx_packet(buf, len);
    return 0;
}
int link_noop_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg) { return 0; }

void link_inject_untrusted(const char *json)   // test hook: treated exactly like a message from the radio
{
    size_t n = strlen(json);
    char *m = malloc(n + 2);
    if (!m) return;
    m[0] = 'B';
    memcpy(m + 1, json, n + 1);
    if (xQueueSend(s_rxq, &m, pdMS_TO_TICKS(100)) != pdTRUE) free(m);
}
void link_inject(const char *json)
{
    size_t n = strlen(json);
    char *m = malloc(n + 2);
    if (!m) return;
    m[0] = 'U';                                 // USB dev shell = physical access: skips on-watch confirmation
    memcpy(m + 1, json, n + 1);
    if (xQueueSend(s_rxq, &m, pdMS_TO_TICKS(100)) != pdTRUE) free(m);
}

// ------------------------------------------------------------------ TX: queue of JSON strings, fragmented by MTU
static void send_obj(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return;
    printf("LINK tx %s\n", s);                  // USB log mirror: lets tests check replies without a phone
    usj_end_packet();
    if (!link_ready() || xQueueSend(s_txq, &s, 0) != pdTRUE) cJSON_free(s);
}
static bool notify_retry(uint16_t handle, const uint8_t *d, int n)
{
    for (int tries = 0; tries < 40; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
        if (om && ble_gatts_notify_custom(s_conn, handle, om) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(10));        // controller buffers full: wait a connection event
    }
    return false;
}
static void tx_task(void *a)
{
    char *s;
    uint8_t pkt[520];
    while (1) {
        if (xQueueReceive(s_txq, &s, portMAX_DELAY) != pdTRUE) continue;
        int len = strlen(s), mtu = s_conn != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn) : 23;
        int room = (mtu > 23 ? mtu : 23) - 3 - 1;
        if (room > (int)sizeof(pkt) - 1) room = sizeof(pkt) - 1;
        for (int off = 0; off < len || off == 0; off += room) {
            int c = len - off < room ? len - off : room;
            pkt[0] = (off == 0 ? 1 : 0) | (off + c >= len ? 2 : 0);
            memcpy(pkt + 1, s + off, c);
            if (!notify_retry(link_evt_handle, pkt, c + 1)) break;
            if (len == 0) break;
        }
        cJSON_free(s);
    }
}

// ------------------------------------------------------------------ connection hooks (host task)
void link_on_connect(uint16_t conn) { s_conn = conn; s_evt_sub_raw = s_audio_sub_raw = s_secure = false; s_asm_len = -1; }
void link_on_disconnect(void) { s_conn = BLE_HS_CONN_HANDLE_NONE; s_evt_sub_raw = s_audio_sub_raw = s_secure = false; link_ptt_stop(); }
static volatile bool s_hello_due;
static char s_wake;                             // queued as a pointer to wake link_task immediately (not a message)
static void hello_now(void)
{
    s_hello_due = true;
    char *w = &s_wake;
    xQueueSend(s_rxq, &w, 0);                   // without this, hello waited for the next message (up to 30 s)
}
void link_on_subscribe(uint16_t attr, bool notify)
{
    // a subscription on an insecure link is remembered but nothing is sent until the link is secure
    if (attr == link_evt_handle) { s_evt_sub_raw = notify; if (notify && s_secure) hello_now(); }
    if (attr == link_audio_handle) s_audio_sub_raw = notify;
    ESP_LOGI(TAG, "subscribe attr=%u notify=%d (evt=%u audio=%u) secure=%d", attr, notify, link_evt_handle, link_audio_handle, s_secure);
}
void link_on_secure(bool secure)
{
    ESP_LOGI(TAG, "secure=%d evt_sub=%d", secure, s_evt_sub_raw);
    s_secure = secure;
    if (secure && s_evt_sub_raw) hello_now();
}

// ------------------------------------------------------------------ notifications store
static link_notif_t s_notif[LINK_NOTIF_MAX];
static int s_nn, s_unread;
static SemaphoreHandle_t s_nlock;
int link_notif_count(void) { return s_nn; }
int link_notif_unread(void) { return s_unread; }
void link_notif_mark_seen(void) { s_unread = 0; }
bool link_notif_get(int i, link_notif_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_nlock, portMAX_DELAY);
    if (i >= 0 && i < s_nn) { *out = s_notif[i]; ok = true; }
    xSemaphoreGive(s_nlock);
    return ok;
}
static void notif_remove(int id)
{
    xSemaphoreTake(s_nlock, portMAX_DELAY);
    for (int i = 0; i < s_nn; i++)
        if (s_notif[i].id == id) { memmove(&s_notif[i], &s_notif[i + 1], (s_nn - i - 1) * sizeof(link_notif_t)); s_nn--; break; }
    xSemaphoreGive(s_nlock);
}
static void copy_str(char *dst, size_t n, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
    for (char *p = dst; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
}
static int get_int(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}
void link_notif_reply(int id, int idx)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "reply"); cJSON_AddNumberToObject(o, "id", id); cJSON_AddNumberToObject(o, "idx", idx);
    send_obj(o);
    notif_remove(id);
}
void link_notif_dismiss(int id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "dismiss"); cJSON_AddNumberToObject(o, "id", id);
    send_obj(o);
    notif_remove(id);
}

// ------------------------------------------------------------------ outgoing helpers
static uint32_t day_key(int days_back)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return 0;
    t.tm_mday -= days_back; t.tm_hour = 12;
    mktime(&t);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
void link_send_steps(void)
{
    uint32_t d[7]; hw_steps_history(d);
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "days");
    cJSON_AddStringToObject(o, "t", "steps");
    for (int i = 0; i < 7; i++) {
        uint32_t k = day_key(i);
        if (!k) break;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "date", k); cJSON_AddNumberToObject(e, "steps", d[i]);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(o);
}
void link_send_practice(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "practice");
    cJSON_AddItemToObject(o, "sessions", practice_json(30));
    send_obj(o);
}
void link_music(const char *cmd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "music_cmd"); cJSON_AddStringToObject(o, "cmd", cmd);
    send_obj(o);
}
void link_find_phone(bool on)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "find_phone"); cJSON_AddBoolToObject(o, "on", on);
    send_obj(o);
}
static const char *s_cur_t = "";            // type of the message being handled (for err.re)
static void send_simple(const char *t, const char *k, const char *v)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    if (k) cJSON_AddStringToObject(o, k, v);
    if (!strcmp(t, "err")) cJSON_AddStringToObject(o, "re", s_cur_t);
    send_obj(o);
}
static void send_hello(void)
{
    batt_t b; hw_batt(&b);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello"); cJSON_AddStringToObject(o, "fw", FW_VERSION); cJSON_AddStringToObject(o, "name", ble_name());
    cJSON_AddNumberToObject(o, "batt", b.present ? b.pct : -1); cJSON_AddNumberToObject(o, "mv", b.mv);
    cJSON_AddBoolToObject(o, "usb", b.vbus); cJSON_AddNumberToObject(o, "steps", hw_steps_today());
    cJSON_AddNumberToObject(o, "sd", sched_date()); cJSON_AddNumberToObject(o, "ciq", sched_queued());
    send_obj(o);
}

// queued check-in answers -> phone (re-sent until "ci_ack"); any task
static int64_t s_last_ci_flush;
static void flush_ci(void)
{
    s_last_ci_flush = esp_timer_get_time();
    if (!link_ready()) return;
    cJSON *arr = cJSON_CreateArray();
    sched_pending(arr, 12);                     // the TX queue holds 16; the rest goes after the next ack
    cJSON *it;
    while ((it = cJSON_DetachItemFromArray(arr, 0)) != NULL) send_obj(it);
    cJSON_Delete(arr);
}
static void send_recs(void)
{
    char dir[40]; snprintf(dir, sizeof(dir), "%s/rec", hw_storage_root());
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "files");
    cJSON_AddStringToObject(o, "t", "recs");
    // newest 40 (file names start with the recording time, so name order = time order); readdir order is arbitrary
    #define MAXR 200
    EXT_RAM_BSS_ATTR static char names[MAXR][32];      // 6.4 KB in PSRAM, not scarce internal RAM
    int cnt = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && cnt < MAXR) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l >= sizeof(names[0]) || strcasecmp(e->d_name + l - 4, ".wav")) continue;
        strcpy(names[cnt++], e->d_name);
    }
    if (d) closedir(d);
    qsort(names, cnt, sizeof(names[0]), (int (*)(const void *, const void *))strcmp);
    for (int k = cnt - 1, n = 0; k >= 0 && n < 40; k--) {
        char p[128]; struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, names[k]);
        if (stat(p, &st)) continue;
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "n", names[k]); cJSON_AddNumberToObject(f, "s", st.st_size);
        uint32_t mk[32]; int nm = rec_marks_for(names[k], mk, 32);
        cJSON *ma = cJSON_AddArrayToObject(f, "marks");
        for (int i = 0; i < nm; i++) cJSON_AddItemToArray(ma, cJSON_CreateNumber(mk[i]));
        cJSON_AddItemToArray(arr, f);
        n++;
    }
    if (cnt > 40) cJSON_AddNumberToObject(o, "more", cnt - 40);     // older ones exist but aren't listed
    send_obj(o);
}
static void send_apps(void)
{
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "list");
    cJSON_AddStringToObject(o, "t", "apps");
    for (int i = 0; i < script_count(); i++) cJSON_AddItemToArray(arr, cJSON_CreateString(script_name(i)));
    send_obj(o);
}

// ------------------------------------------------------------------ weather + translation state
static struct { char text[40]; int64_t at; } s_weather;
bool link_weather(char *out, size_t n)
{
    if (!s_weather.at || esp_timer_get_time() - s_weather.at > 3LL * 3600 * 1000000) return false;
    snprintf(out, n, "%s", s_weather.text);
    return true;
}
static struct { char me[8], them[8], src[200], dst[240]; int gen; } s_tr = {"en", "fr", "", "", 0};
void link_tr_state(char *me, char *them, char *src, size_t srcn, char *dst, size_t dstn, int *gen)
{
    if (me) strcpy(me, s_tr.me);
    if (them) strcpy(them, s_tr.them);
    if (src) snprintf(src, srcn, "%s", s_tr.src);
    if (dst) snprintf(dst, dstn, "%s", s_tr.dst);
    if (gen) *gen = s_tr.gen;
}

// ------------------------------------------------------------------ push-to-talk audio (mic -> ADPCM -> AUDIO)
static volatile bool s_ptt, s_ptt_task;
static char s_ptt_side[8] = "me";
static char s_ptt_ctx[128];
static volatile bool s_ptt_tap;                  // sharing a 16 kHz meeting recording's mic instead of owning it
static volatile int s_ptt_err;                   // why the last start failed: LINK_PTT_*
bool link_ptt_active(void) { return s_ptt; }
static void ptt_task(void *a)
{
    EXT_RAM_BSS_ATTR static int16_t pcm[ADPCM_BLOCK_SAMPLES];
#define RING_N 64                                    // ~1 s of audio waiting for the link
    EXT_RAM_BSS_ATTR static uint8_t ring[RING_N][ADPCM_BLOCK_BYTES];
    int rh = 0, rn = 0;
    int idx = 0, got = 0, seq_total = 0;
    uint8_t seq = 0;
    int64_t t0 = esp_timer_get_time();
    while (s_ptt && esp_timer_get_time() - t0 < 30000000LL) {           // 30 s safety cap
        int n = s_ptt_tap ? hw_mic_tap_read(pcm + got, ADPCM_BLOCK_SAMPLES - got, 50) : hw_mic_read_mono(pcm + got, ADPCM_BLOCK_SAMPLES - got);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        got += n;
        if (got < ADPCM_BLOCK_SAMPLES) continue;
        got = 0;
        adpcm_encode_block(pcm, seq++, &idx, ring[(rh + rn) % RING_N]);
        if (rn < RING_N) rn++; else { rh = (rh + 1) % RING_N; }    // ring full (>1 s behind): drop the oldest
        seq_total++;
        // send what the link takes; the rest waits (the link may still be switching to the fast rhythm)
        while (rn && s_conn != BLE_HS_CONN_HANDLE_NONE && s_audio_sub) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(ring[rh], ADPCM_BLOCK_BYTES);
            if (!om || ble_gatts_notify_custom(s_conn, link_audio_handle, om) != 0) break;
            rh = (rh + 1) % RING_N; rn--;
        }
    }
    for (int tries = 0; rn && tries < 100 && s_conn != BLE_HS_CONN_HANDLE_NONE && s_audio_sub; tries++) {   // flush the tail
        struct os_mbuf *om = ble_hs_mbuf_from_flat(ring[rh], ADPCM_BLOCK_BYTES);
        if (om && ble_gatts_notify_custom(s_conn, link_audio_handle, om) == 0) { rh = (rh + 1) % RING_N; rn--; }
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    ble_link_slow_later(3000);
    if (s_ptt_tap) hw_mic_tap_close(); else hw_mic_close();
    s_ptt_tap = false;
    s_ptt = false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ptt"); cJSON_AddStringToObject(o, "side", s_ptt_side); cJSON_AddBoolToObject(o, "on", false);
    cJSON_AddNumberToObject(o, "blocks", seq_total);          // how many AUDIO blocks were sent (phone: wait until seen)
    send_obj(o);
    diag_event("PTT end blocks=%d", seq_total);
    s_ptt_task = false;
    vTaskDelete(NULL);
}
bool link_ptt_start_ex(const char *side, const char *ctx_json)
{
    if (s_ptt || s_ptt_task) { s_ptt_err = LINK_PTT_MIC_BUSY; return false; }
    // the mic is checked first, so "mic in use" is reported (and can be solved) even without the phone
    bool amber = !strcmp(side, "amber");
    bool own = hw_mic_open_as(16000, 30.0f, amber ? "Amber" : "Translate", amber ? "listening to you" : "push-to-talk");
    bool tap = false;
    if (!own) {
        // a meeting recording (16 kHz) keeps going: Amber gets a copy of the same audio, and the moment is bookmarked
        if (amber && rec_active() && rec_mode() == REC_MEETING && !rec_paused() && hw_mic_tap_open(16000)) tap = true;
        else { s_ptt_err = LINK_PTT_MIC_BUSY; return false; }
    }
    if (!link_ready() || ble_att_mtu(s_conn) < ADPCM_BLOCK_BYTES + 3) {
        if (tap) hw_mic_tap_close(); else hw_mic_close();
        s_ptt_err = LINK_PTT_NO_PHONE; return false;
    }
    if (tap) rec_mark();
    diag_event("PTT start side=%s shared=%d", side, tap);
    s_ptt_tap = tap;
    s_ptt_err = 0;
    snprintf(s_ptt_side, sizeof(s_ptt_side), "%s", side);
    snprintf(s_ptt_ctx, sizeof(s_ptt_ctx), "%s", ctx_json ? ctx_json : "");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ptt"); cJSON_AddStringToObject(o, "side", s_ptt_side); cJSON_AddBoolToObject(o, "on", true);
    cJSON_AddNumberToObject(o, "rate", 16000); cJSON_AddStringToObject(o, "codec", "ima-adpcm");
    if (s_ptt_ctx[0]) { cJSON *c = cJSON_Parse(s_ptt_ctx); if (c) cJSON_AddItemToObject(o, "ctx", c); }
    send_obj(o);
    s_ptt = true; s_ptt_task = true;
    ble_link_fast(true);                             // audio needs short connection events
    xTaskCreate(ptt_task, "ptt", 4096, NULL, 5, NULL);
    return true;
}
bool link_ptt_start(bool me) { return link_ptt_start_ex(me ? "me" : "them", NULL); }
int link_ptt_error(void) { return s_ptt_err; }
bool link_ptt_shared(void) { return s_ptt && s_ptt_tap; }
const char *link_ptt_side(void) { return s_ptt_side; }
void link_ptt_stop(void) { s_ptt = false; }

int link_adpcm_selftest(char *hex, size_t n)
{
    static int16_t sig[2 * ADPCM_BLOCK_SAMPLES];
    for (int i = 0; i < 2 * ADPCM_BLOCK_SAMPLES; i++)
        sig[i] = (int16_t)lround(8000 * sin(2 * M_PI * 440 * i / 16000.0) + 4000 * sin(2 * M_PI * 1250 * i / 16000.0));
    uint8_t b1[ADPCM_BLOCK_BYTES], b2[ADPCM_BLOCK_BYTES];
    int idx = 0;
    adpcm_encode_block(sig, 0, &idx, b1);
    adpcm_encode_block(sig + ADPCM_BLOCK_SAMPLES, 1, &idx, b2);
    size_t o = 0;
    for (int i = 0; i < ADPCM_BLOCK_BYTES && o + 3 < n; i++) o += snprintf(hex + o, n - o, "%02x", b1[i]);
    if (o + 2 < n) hex[o++] = ' ';
    for (int i = 0; i < ADPCM_BLOCK_BYTES && o + 3 < n; i++) o += snprintf(hex + o, n - o, "%02x", b2[i]);
    hex[o < n ? o : n - 1] = 0;
    return 0;
}

// ------------------------------------------------------------------ Amber conversation (newest first, in PSRAM)
EXT_RAM_BSS_ATTR static link_amber_t s_amb[LINK_AMBER_MAX];
static int s_amb_n, s_amb_gen;
static void amber_push(const char *text, bool mine)
{
    xSemaphoreTake(s_nlock, portMAX_DELAY);
    memmove(&s_amb[1], &s_amb[0], sizeof(link_amber_t) * (LINK_AMBER_MAX - 1));
    snprintf(s_amb[0].text, sizeof(s_amb[0].text), "%s", text);
    s_amb[0].mine = mine;
    if (s_amb_n < LINK_AMBER_MAX) s_amb_n++;
    s_amb_gen++;
    xSemaphoreGive(s_nlock);
}
int link_amber_count(void) { return s_amb_n; }
int link_amber_gen(void) { return s_amb_gen; }
bool link_amber_get(int i, link_amber_t *out)
{
    if (i < 0 || i >= s_amb_n) return false;
    xSemaphoreTake(s_nlock, portMAX_DELAY);
    *out = s_amb[i];
    xSemaphoreGive(s_nlock);
    return true;
}

// ------------------------------------------------------------------ message handling (link task)
static void wake_toast(const char *msg, int buzz_n)
{
    if (buzz_n) hw_buzz_pattern(buzz_n, 80, 120);
    bsp_display_lock(0);
    launcher_wake();
    if (msg) ui_msg(msg);
    bsp_display_unlock();
}

static bool safe_id(const char *s)
{
    if (!s[0] || strlen(s) > 40) return false;
    for (const char *p = s; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '-' || *p == '.')) return false;
    return strstr(s, "..") == NULL;
}

// S3: risky requests from the phone need a tap on the watch (USB = physical access, trusted)
static bool allowed(bool trusted, const char *question)
{
    if (trusted) return true;
    if (ui_confirm(question, 30)) return true;
    send_simple("err", "msg", "denied on watch");
    return false;
}

static void handle(const char *json, bool trusted)
{
    cJSON *m = cJSON_Parse(json);
    const cJSON *tv = m ? cJSON_GetObjectItemCaseSensitive(m, "t") : NULL;
    if (!cJSON_IsString(tv)) { cJSON_Delete(m); return; }
    const char *t = tv->valuestring;
    s_cur_t = t;
    printf("LINK rx %s\n", t);

    if (!strcmp(t, "time")) {
        const cJSON *ep = cJSON_GetObjectItemCaseSensitive(m, "epoch");
        if (cJSON_IsNumber(ep)) {
            int tz = get_int(m, "tz", 0);
            if (tz >= -14 * 3600 && tz <= 14 * 3600 && hw_pref_get("tz", 0) != tz) hw_pref_set("tz", tz);   // check-in times are sent as UTC
            time_t e = (time_t)ep->valuedouble + tz;
            struct tm tm; gmtime_r(&e, &tm);
            hw_rtc_set(&tm);
        }
    } else if (!strcmp(t, "notif")) {
        link_notif_t n = {0};
        n.id = get_int(m, "id", 0);
        copy_str(n.app, sizeof(n.app), m, "app"); copy_str(n.title, sizeof(n.title), m, "title"); copy_str(n.body, sizeof(n.body), m, "body");
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "replies"), *it;
        cJSON_ArrayForEach(it, r) if (cJSON_IsString(it) && n.nrep < 3) snprintf(n.replies[n.nrep++], 24, "%s", it->valuestring);
        struct tm tm; if (hw_rtc_get(&tm)) snprintf(n.time, sizeof(n.time), "%02d:%02d", tm.tm_hour, tm.tm_min);
        notif_remove(n.id);
        xSemaphoreTake(s_nlock, portMAX_DELAY);
        memmove(&s_notif[1], &s_notif[0], (LINK_NOTIF_MAX - 1) * sizeof(link_notif_t));
        s_notif[0] = n;
        if (s_nn < LINK_NOTIF_MAX) s_nn++;
        s_unread++;
        xSemaphoreGive(s_nlock);
        char msg[96]; snprintf(msg, sizeof(msg), "%s: %s", n.app[0] ? n.app : "Phone", n.title[0] ? n.title : n.body);
        wake_toast(msg, 2);
    } else if (!strcmp(t, "notif_rm")) notif_remove(get_int(m, "id", -1));
    else if (!strcmp(t, "weather")) {
        char txt[24]; copy_str(txt, sizeof(txt), m, "text");
        snprintf(s_weather.text, sizeof(s_weather.text), "%d\xC2\xB0" "C  %s", get_int(m, "temp", 0), txt);
        s_weather.at = esp_timer_get_time();
    } else if (!strcmp(t, "steps_req")) link_send_steps();
    else if (!strcmp(t, "hello_req")) send_hello();
    else if (!strcmp(t, "phone_info")) { char nm[32]; copy_str(nm, sizeof(nm), m, "name"); devices_set_name_current(nm); }
    else if (!strcmp(t, "recs_req")) send_recs();
    else if (!strcmp(t, "wifi_req")) {
        if (!allowed(trusted, "Phone wants to turn on the Wi-Fi hotspot\n(to download recordings). Allow?")) goto done;
        wifi_ap_start();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "wifi");
        if (wifi_ap_active()) { cJSON_AddStringToObject(o, "ssid", wifi_ap_ssid()); cJSON_AddStringToObject(o, "pass", wifi_ap_pass()); cJSON_AddStringToObject(o, "ip", "192.168.4.1"); cJSON_AddStringToObject(o, "token", wifi_ap_token()); }
        else cJSON_AddStringToObject(o, "err", "hotspot could not start (low memory or busy)");
        send_obj(o);
    } else if (!strcmp(t, "wifi_off")) wifi_ap_stop();
    else if (!strcmp(t, "note")) {
        char id[48]; copy_str(id, sizeof(id), m, "id");
        if (!safe_id(id)) snprintf(id, sizeof(id), "note_%lld", (long long)(esp_timer_get_time() / 1000000));
        mkdir("/flash/notes", 0775);
        char p[96]; snprintf(p, sizeof(p), "/flash/notes/%.40s.json", id);
        FILE *f = fopen(p, "w");
        if (f) { fputs(json, f); fclose(f); }
        wake_toast("Meeting summary ready - see Notes", 2);
    } else if (!strcmp(t, "tr_cfg")) {
        copy_str(s_tr.me, sizeof(s_tr.me), m, "me"); copy_str(s_tr.them, sizeof(s_tr.them), m, "them");
        s_tr.gen++;
    } else if (!strcmp(t, "tr")) {
        copy_str(s_tr.src, sizeof(s_tr.src), m, "src"); copy_str(s_tr.dst, sizeof(s_tr.dst), m, "dst");
        s_tr.gen++;
        hw_buzz(50);
        wake_toast(NULL, 0);
    } else if (!strcmp(t, "app_ls")) send_apps();
    else if (!strcmp(t, "app_put")) {
        char nm[32]; copy_str(nm, sizeof(nm), m, "name");
        if (!script_put_begin(nm)) send_simple("err", "msg", "bad app name (a-z 0-9 _ -, max 20)");
    } else if (!strcmp(t, "app_data")) {
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(m, "b64");
        static uint8_t dec[2048];
        size_t ol = 0;
        if (!cJSON_IsString(b) || mbedtls_base64_decode(dec, sizeof(dec), &ol, (const unsigned char *)b->valuestring, strlen(b->valuestring)) || !script_put_data(dec, ol))
            send_simple("err", "msg", "app upload failed");
    } else if (!strcmp(t, "app_end")) {
        char q[96]; snprintf(q, sizeof(q), "Install app \"%s\" from your phone?", script_put_pending());
        if (!allowed(trusted, q)) { script_put_abort(); goto done; }
        if (script_put_end()) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "app_ok");
            send_obj(o);
        } else send_simple("err", "msg", "app install failed");
    } else if (!strcmp(t, "app_rm")) {
        char nm[32]; copy_str(nm, sizeof(nm), m, "name");
        char q[96]; snprintf(q, sizeof(q), "Remove app \"%s\"?", nm);
        if (!allowed(trusted, q)) goto done;
        script_remove(nm);
        send_apps();
    } else if (!strcmp(t, "sched")) {
        char er[32];
        if (sched_set(m, er, sizeof(er))) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "sched_ok"); cJSON_AddNumberToObject(o, "date", sched_date());
            cJSON_AddStringToObject(o, "gen", sched_gen()); cJSON_AddNumberToObject(o, "n", sched_count());
            send_obj(o);
        } else send_simple("err", "msg", er);
    } else if (!strcmp(t, "ci_ack")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(m, "upto");
        if (cJSON_IsNumber(u) && u->valuedouble >= 0) { sched_ack((uint32_t)u->valuedouble); if (sched_queued()) flush_ci(); }
    } else if (!strcmp(t, "ci_cfg")) sched_cfg(m);
    else if (!strcmp(t, "amber_heard") || !strcmp(t, "amber")) {
        // Amber answers in short bullet lines: keep '\n' (copy_str turns every control character into a space)
        char tx[304];
        const cJSON *tv2 = cJSON_GetObjectItemCaseSensitive(m, "text");
        snprintf(tx, sizeof(tx), "%s", cJSON_IsString(tv2) ? tv2->valuestring : "");
        for (char *p = tx; *p; p++) if ((unsigned char)*p < 32 && *p != '\n') *p = ' ';
        if (tx[0]) {
            bool heard = !strcmp(t, "amber_heard");
            amber_push(tx, heard);
            if (!heard && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "final"))) {
                char toast[80]; snprintf(toast, sizeof(toast), "Amber: %.66s", tx);
                wake_toast(toast, 1);
            }
        }
    }
    else if (!strcmp(t, "find_watch")) {
        wake_toast("Here I am!", 0);
        hw_buzz_pattern(3, 300, 200);
        for (int i = 0; i < 3; i++) hw_beep(1760, 180, 90);
    } else if (!strcmp(t, "practice_req")) link_send_practice();
    else if (!strcmp(t, "rec_del")) {                  // the phone has a verified copy: delete ours (size must match)
        char nm[64]; copy_str(nm, sizeof(nm), m, "n");
        int sz = get_int(m, "s", -1);
        size_t l = strlen(nm);
        if (safe_id(nm) && l > 4 && !strcasecmp(nm + l - 4, ".wav")) {
            char p[128]; struct stat st;
            snprintf(p, sizeof(p), "%s/rec/%s", hw_storage_root(), nm);
            if (stat(p, &st) == 0 && st.st_size == sz) {
                remove(p);
                snprintf(p, sizeof(p), "%s/rec/%.*s.marks", hw_storage_root(), (int)(l - 4), nm);
                remove(p);
            } else send_simple("err", "msg", "size mismatch or no such recording - kept");
        } else send_simple("err", "msg", "bad recording name");
        send_recs();
    }
done:
    s_cur_t = "";                               // t points into m, which is freed next
    cJSON_Delete(m);
}

static void link_task(void *a)
{
    int64_t last_steps = 0;
    while (1) {
        char *msg;
        if (xQueueReceive(s_rxq, &msg, pdMS_TO_TICKS(30000)) == pdTRUE && msg != &s_wake) { handle(msg + 1, msg[0] == 'U'); free(msg); }
        if (s_hello_due) {
            // A bonded phone's subscription is restored by NimBLE the instant encryption comes up - before the
            // phone app is actually listening. Wait a moment so the hello isn't lost (the phone can also send hello_req).
            vTaskDelay(pdMS_TO_TICKS(1200));
            s_hello_due = false;
            if (link_ready()) { send_hello(); link_send_steps(); last_steps = esp_timer_get_time(); flush_ci(); diag_event("LINK phone app ready"); }
        }
        if (link_ready() && sched_queued() && esp_timer_get_time() - s_last_ci_flush > 60LL * 1000000) flush_ci();
        if (link_ready() && esp_timer_get_time() - last_steps > 3600LL * 1000000) { last_steps = esp_timer_get_time(); link_send_steps(); }
    }
}

void link_start(void)
{
    s_rxq = xQueueCreate(32, sizeof(char *));
    s_txq = xQueueCreate(16, sizeof(char *));
    s_nlock = xSemaphoreCreateMutex();
    xTaskCreate(link_task, "link", 4096, NULL, 3, NULL);
    sched_start(flush_ci);
    xTaskCreate(tx_task, "link_tx", 3072, NULL, 4, NULL);
}

// Remembered devices: see devices.h. Each one is added only through the pairing window + 6-digit code (ble.c).
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "hw.h"
#include "devices.h"

static const char *TAG = "devices";
static struct { uint8_t n; em_dev_t d[DEV_MAX]; } s;
static int s_cur = -1, s_pref = -1;
static int64_t s_pref_until;
static SemaphoreHandle_t s_lock;
#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static uint32_t now_min(void)
{
    struct tm t;
    return hw_rtc_get(&t) ? (uint32_t)((mktime(&t) - 946684800) / 60) : 0;
}
static void save(void) { hw_blob_set("devs", &s, sizeof(s)); }

void devices_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (hw_blob_get("devs", &s, sizeof(s)) != sizeof(s) || s.n > DEV_MAX) memset(&s, 0, sizeof(s));
}
int devices_count(void) { return s.n; }
bool devices_get(int i, em_dev_t *out)
{
    LOCK(); bool ok = i >= 0 && i < s.n; if (ok) *out = s.d[i]; UNLOCK();
    return ok;
}
const char *devices_label(const em_dev_t *d) { return d->nick[0] ? d->nick : (d->name[0] ? d->name : "Unknown device"); }
int devices_current(void) { return s_cur; }

static ble_addr_t addr_of(const em_dev_t *d)
{
    ble_addr_t a = {.type = d->type};
    memcpy(a.val, d->addr, 6);
    return a;
}
bool devices_bonded(int i)
{
    em_dev_t d;
    if (!devices_get(i, &d)) return false;
    struct ble_store_key_sec k = {.peer_addr = addr_of(&d)};
    struct ble_store_value_sec v;
    return ble_store_read_peer_sec(&k, &v) == 0;
}
void devices_rename(int i, const char *nick)
{
    LOCK();
    if (i >= 0 && i < s.n) { snprintf(s.d[i].nick, sizeof(s.d[i].nick), "%s", nick); save(); }
    UNLOCK();
}
void devices_forget(int i)
{
    em_dev_t d;
    if (!devices_get(i, &d)) return;
    ble_addr_t a = addr_of(&d);
    ble_gap_unpair(&a);                              // deletes its keys (and drops it if connected)
    LOCK();
    memmove(&s.d[i], &s.d[i + 1], (s.n - i - 1) * sizeof(em_dev_t));
    s.n--;
    if (s_cur == i) s_cur = -1; else if (s_cur > i) s_cur--;
    s_pref = -1;
    save();
    UNLOCK();
}

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
void devices_prefer(int i)
{
    if (i < 0 || i >= s.n) return;
    s_pref = i;
    s_pref_until = esp_timer_get_time() + 120LL * 1000000;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_cur != i) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

// GAP Device Name (0x2A00) of the peer, read once the link is encrypted
static int name_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    int i = (int)(intptr_t)arg;
    if (err->status != 0 || !attr) return 0;
    char nm[32];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(attr->om, nm, sizeof(nm) - 1, &n) != 0) return 0;
    nm[n] = 0;
    for (char *p = nm; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    LOCK();
    if (i >= 0 && i < s.n && nm[0] && strcmp(s.d[i].name, nm)) { strcpy(s.d[i].name, nm); save(); }
    UNLOCK();
    ESP_LOGI(TAG, "peer name: %s", nm);
    return BLE_HS_EDONE;
}

void devices_on_secure(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    s_conn = conn;
    LOCK();
    int i = 0;
    while (i < s.n && !(s.d[i].type == desc.peer_id_addr.type && !memcmp(s.d[i].addr, desc.peer_id_addr.val, 6))) i++;
    if (i == s.n) {                                  // new device: add (replace the least recently seen if full)
        if (s.n == DEV_MAX) {
            int old = 0;
            for (int k = 1; k < s.n; k++) if (s.d[k].last_min < s.d[old].last_min) old = k;
            i = old;
        } else s.n++;
        memset(&s.d[i], 0, sizeof(em_dev_t));
        s.d[i].type = desc.peer_id_addr.type;
        memcpy(s.d[i].addr, desc.peer_id_addr.val, 6);
    }
    bool blocked = s_pref >= 0 && esp_timer_get_time() < s_pref_until && i != s_pref;
    if (!blocked) { s.d[i].last_min = now_min(); s_cur = i; if (i == s_pref) s_pref = -1; }
    save();
    UNLOCK();
    if (blocked) {                                   // "use this device" picked another one: let that one in
        ESP_LOGI(TAG, "device %d is not the chosen one: disconnecting", i);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    static const ble_uuid16_t DEV_NAME = BLE_UUID16_INIT(0x2A00);
    ble_gattc_read_by_uuid(conn, 1, 0xFFFF, &DEV_NAME.u, name_cb, (void *)(intptr_t)i);
}
void devices_on_disconnect(void) { s_cur = -1; s_conn = BLE_HS_CONN_HANDLE_NONE; }
void devices_set_name_current(const char *name)
{
    LOCK();
    if (s_cur >= 0 && name[0]) { snprintf(s.d[s_cur].name, sizeof(s.d[s_cur].name), "%s", name); save(); }
    UNLOCK();
}
void devices_print(void)
{
    printf("CMD devices %d:", s.n);
    for (int i = 0; i < s.n; i++) {
        em_dev_t d; devices_get(i, &d);
        printf(" [%d %s name=%s nick=%s bonded=%d current=%d last_min=%lu]", i, devices_label(&d), d.name, d.nick, devices_bonded(i),
               i == s_cur, (unsigned long)d.last_min);
    }
    printf("\n");
}

// Wi-Fi access point + HTTP file server (local only).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi.h"
#include "app.h"
#include "hw.h"

static const char *TAG = "wifi";
static bool s_active, s_inited;
static httpd_handle_t s_httpd;
static char s_ssid[24], s_pass[12];
static esp_netif_t *s_netif;
static volatile int64_t s_last_req;

const char *wifi_ap_ssid(void) { return s_ssid; }
const char *wifi_ap_pass(void) { return s_pass; }
bool wifi_ap_active(void) { return s_active; }
int wifi_ap_clients(void)
{
    wifi_sta_list_t l;
    return (s_active && esp_wifi_ap_get_sta_list(&l) == ESP_OK) ? l.num : 0;
}

// ---------------------------------------------------------------- web UI (single inline page)
static const char PAGE[] =
"<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Watch</title>"
"<style>body{font-family:system-ui;background:#111;color:#eee;margin:0;padding:16px;max-width:640px;margin:auto}"
"h1{font-size:22px}.c{background:#1c1c1c;border-radius:12px;padding:12px;margin:10px 0}"
"a,button{color:#2ec4b6;background:none;border:1px solid #2ec4b6;border-radius:8px;padding:6px 12px;text-decoration:none;font-size:15px}"
"button.d{color:#ff4d4d;border-color:#ff4d4d}.f{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid #2a2a2a}"
".s{color:#999;font-size:13px}</style></head><body><h1>Watch</h1>"
"<div class=c id=st>loading...</div>"
"<div class=c><button onclick=sync()>Set watch clock from this phone</button> <span class=s id=sy></span></div>"
"<div class=c><b>Recordings</b><div id=fl class=s>loading...</div></div>"
"<div class=c><b>Ember apps</b><div id=ap class=s>loading...</div>"
"<p class=s>Install a .lua app: <input type=file id=af accept='.lua'> <button onclick=up()>Install</button> <span id=au></span></p></div>"
"<script>"
"async function ap(){let r=await(await fetch('/api/apps')).json();let h='';"
"for(const n of r){h+='<div class=f><span>'+n+'</span><button class=d onclick=\"arm(\\''+n+'\\')\">Remove</button></div>'}"
"document.getElementById('ap').innerHTML=h||'none'}"
"async function arm(n){if(!confirm('Remove '+n+'?'))return;await fetch('/api/apprm?name='+n,{method:'POST'});ap()}"
"async function up(){let f=document.getElementById('af').files[0];if(!f)return;let n=f.name.replace(/\\.lua$/i,'').toLowerCase();"
"let r=await fetch('/api/app?name='+encodeURIComponent(n),{method:'POST',body:await f.text()});"
"document.getElementById('au').textContent=await r.text();ap()}"
"ap();"
"async function st(){let r=await(await fetch('/api/status')).json();"
"document.getElementById('st').innerHTML='Time '+r.time+'<br>Steps today <b>'+r.steps+'</b><br>Battery '+(r.batt<0?'none':r.batt+'%')+'<br>Storage '+r.store+' - '+r.free_mb+' MB free'}"
"async function fl(){let r=await(await fetch('/api/files')).json();let h='';"
"for(const f of r){h+='<div class=f><span>'+f.n+'<br><span class=s>'+(f.s/1048576).toFixed(1)+' MB</span></span><span>"
"<a href=\"/f/'+f.n+'\" download>Save</a> <button class=d onclick=\"del(\\''+f.n+'\\')\">Delete</button></span></div>'}"
"document.getElementById('fl').innerHTML=h||'none yet'}"
"async function del(n){if(!confirm('Delete '+n+'?'))return;await fetch('/api/del?name='+encodeURIComponent(n),{method:'POST'});fl()}"
"async function sync(){let d=new Date();let q='y='+d.getFullYear()+'&mo='+(d.getMonth()+1)+'&d='+d.getDate()+'&h='+d.getHours()+'&mi='+d.getMinutes()+'&s='+d.getSeconds();"
"await fetch('/api/time?'+q,{method:'POST'});document.getElementById('sy').textContent='done';st()}"
"st();fl();setInterval(st,5000)</script></body></html>";

static void touch_req(void) { s_last_req = esp_timer_get_time(); }

static esp_err_t h_root(httpd_req_t *r)
{
    touch_req();
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    touch_req();
    char b[256]; struct tm t; batt_t bt; uint64_t tot, fre;
    hw_batt(&bt); hw_storage_info(&tot, &fre);
    char ts[24] = "unset";
    if (hw_rtc_get(&t)) snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    snprintf(b, sizeof(b), "{\"time\":\"%s\",\"steps\":%lu,\"batt\":%d,\"store\":\"%s\",\"free_mb\":%llu}", ts,
             (unsigned long)hw_steps_today(), bt.present ? bt.pct : -1, hw_storage_is_sd() ? "SD card" : "internal", (unsigned long long)(fre >> 20));
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, b, HTTPD_RESP_USE_STRLEN);
}

static bool safe_name(const char *n)
{
    if (!n[0] || strlen(n) > 60) return false;
    for (const char *p = n; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '.' || *p == '-')) return false;
    return strstr(n, "..") == NULL;
}

static esp_err_t h_files(httpd_req_t *r)
{
    touch_req();
    httpd_resp_set_type(r, "application/json");
    char dir[48];
    snprintf(dir, sizeof(dir), "%s/rec", hw_storage_root());
    DIR *d = opendir(dir);
    httpd_resp_sendstr_chunk(r, "[");
    bool first = true;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!safe_name(e->d_name)) continue;
        char path[128]; struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        char b[128];
        snprintf(b, sizeof(b), "%s{\"n\":\"%s\",\"s\":%ld}", first ? "" : ",", e->d_name, (long)st.st_size);
        httpd_resp_sendstr_chunk(r, b);
        first = false;
    }
    if (d) closedir(d);
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_download(httpd_req_t *r)
{
    touch_req();
    const char *name = r->uri + 3;                      // after "/f/"
    if (!safe_name(name)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
    char path[128];
    snprintf(path, sizeof(path), "%s/rec/%s", hw_storage_root(), name);
    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_404(r);
    httpd_resp_set_type(r, "audio/wav");
    char *buf = malloc(8192);
    size_t n;
    esp_err_t res = ESP_OK;
    while ((n = fread(buf, 1, 8192, f)) > 0) {
        touch_req();
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { res = ESP_FAIL; break; }
    }
    free(buf);
    fclose(f);
    if (res == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    return res;
}

static bool qparam(const char *q, const char *key, char *out, size_t cap)
{
    return httpd_query_key_value(q, key, out, cap) == ESP_OK;
}

static esp_err_t h_del(httpd_req_t *r)
{
    touch_req();
    char q[128], name[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || !qparam(q, "name", name, sizeof(name)) || !safe_name(name))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
    char path[128];
    snprintf(path, sizeof(path), "%s/rec/%s", hw_storage_root(), name);
    remove(path);
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t h_time(httpd_req_t *r)
{
    touch_req();
    char q[128], v[8];
    int p[6] = {0};
    const char *k[6] = {"y", "mo", "d", "h", "mi", "s"};
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no query");
    for (int i = 0; i < 6; i++) { if (!qparam(q, k[i], v, sizeof(v))) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "missing"); p[i] = atoi(v); }
    if (p[0] < 2024 || p[0] > 2099 || p[1] < 1 || p[1] > 12 || p[2] < 1 || p[2] > 31 || p[3] > 23 || p[4] > 59 || p[5] > 59)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "range");
    struct tm t = {0};
    t.tm_year = p[0] - 1900; t.tm_mon = p[1] - 1; t.tm_mday = p[2]; t.tm_hour = p[3]; t.tm_min = p[4]; t.tm_sec = p[5];
    time_t e = mktime(&t);
    gmtime_r(&e, &t);
    hw_rtc_set(&t);
    return httpd_resp_sendstr(r, "ok");
}

// ---- Ember apps: list / upload (raw Lua body, max 64 KB) / remove ----
static esp_err_t h_apps(httpd_req_t *r)
{
    touch_req();
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr_chunk(r, "[");
    for (int i = 0; i < script_count(); i++) {
        char b[40]; snprintf(b, sizeof(b), "%s\"%s\"", i ? "," : "", script_name(i));
        httpd_resp_sendstr_chunk(r, b);
    }
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}
static esp_err_t h_app_put(httpd_req_t *r)
{
    touch_req();
    char q[96], name[32];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || !qparam(q, "name", name, sizeof(name)) || r->content_len > 65536 ||
        !script_put_begin(name))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "name must be a-z 0-9 _ - (max 20), file max 64 KB");
    char buf[1024];
    int left = r->content_len;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || !script_put_data(buf, n)) { script_put_end(); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); }
        left -= n;
    }
    return script_put_end() ? httpd_resp_sendstr(r, "installed") : httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "install failed");
}
static esp_err_t h_app_rm(httpd_req_t *r)
{
    touch_req();
    char q[96], name[32];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || !qparam(q, "name", name, sizeof(name)) || !script_remove(name))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no such app");
    return httpd_resp_sendstr(r, "removed");
}

static void start_httpd(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.max_uri_handlers = 12;
    c.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &c) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/files", .method = HTTP_GET, .handler = h_files},
        {.uri = "/api/del", .method = HTTP_POST, .handler = h_del},
        {.uri = "/api/time", .method = HTTP_POST, .handler = h_time},
        {.uri = "/f/*", .method = HTTP_GET, .handler = h_download},
        {.uri = "/api/apps", .method = HTTP_GET, .handler = h_apps},
        {.uri = "/api/app", .method = HTTP_POST, .handler = h_app_put},
        {.uri = "/api/apprm", .method = HTTP_POST, .handler = h_app_rm},
    };
    for (unsigned i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s_httpd, &u[i]);
}

// ---------------------------------------------------------------- AP lifecycle (auto-off after 10 idle minutes)
static void idle_task(void *a)
{
    while (s_active) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (wifi_ap_clients() > 0) touch_req();
        if (s_active && (esp_timer_get_time() - s_last_req) > 10LL * 60 * 1000000) { ESP_LOGI(TAG, "idle 10 min: AP off"); wifi_ap_stop(); }
    }
    vTaskDelete(NULL);
}

void wifi_ap_start(void)
{
    if (s_active) return;
    // Wi-Fi needs ~25-50 KB of internal RAM on top of Bluetooth. Running short used to crash inside the Wi-Fi blob
    // (beacon alloc fails -> NULL dereference). Refuse cleanly instead.
    size_t in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (in_free < 30000) { ESP_LOGE(TAG, "not starting hotspot: only %u B internal RAM free", (unsigned)in_free); return; }
    if (!s_inited) {
        esp_netif_init();
        esp_event_loop_create_default();
        s_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_inited = true;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "Watch-%02X%02X", mac[4], mac[5]);
    int pw = hw_pref_get("appw", 0);
    if (pw < 10000000) { pw = 10000000 + (int)(esp_random() % 89999999); hw_pref_set("appw", pw); }
    snprintf(s_pass, sizeof(s_pass), "%d", pw);
    wifi_config_t wc = {0};
    strcpy((char *)wc.ap.ssid, s_ssid);
    strcpy((char *)wc.ap.password, s_pass);
    wc.ap.ssid_len = strlen(s_ssid);
    wc.ap.channel = 6;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (esp_wifi_start() != ESP_OK) { ESP_LOGE(TAG, "wifi start failed"); return; }
    start_httpd();
    s_last_req = esp_timer_get_time();
    s_active = true;
    xTaskCreate(idle_task, "wifi_idle", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "AP up: %s / %s  ->  http://192.168.4.1", s_ssid, s_pass);
}

void wifi_ap_stop(void)
{
    if (!s_active) return;
    s_active = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
    ESP_LOGI(TAG, "AP down");
}

// Amber check-ins (docs/LIFE_MANAGER_PLAN.md rev 2, Ember Link v1.4):
//  * keeps the day's plan (time blocks + check-in slots from Life OS, sent by the phone as "sched") in NVS
//  * asks "Kept up?" at fixed slots (default 08:00 12:00 16:00 20:00 and 00:00 end of day) from the watch's own clock,
//    so it works without the phone
//  * queues answers (up to 48, in NVS) and sends them as "ci" until the phone acknowledges them ("ci_ack")
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app.h"
#include "hw.h"
#include "link.h"
#include "sched.h"

static const char *TAG = "sched";

typedef struct { char id[37]; char title[40]; int16_t s, e; uint8_t done; } blk_t;
typedef struct {
    uint32_t date; char gen[9]; uint8_t n, nslots;
    int16_t slots[SCHED_SLOTS];
    uint8_t asked;                       // bit k = slot k handled (asked, or logged as missed)
    char ans[SCHED_SLOTS];               // latest answer per slot: 0 o f z m
    blk_t b[SCHED_MAX];
} plan_t;
typedef struct { uint32_t seq, at, date; int16_t slot; char ans; uint8_t nids; char ids[SCHED_IDS][37]; } ci_t;
typedef struct { uint8_t n; ci_t q[SCHED_CIQ]; } ciq_t;

EXT_RAM_BSS_ATTR static plan_t s_plan, s_prev, s_new;   // s_new: scratch for sched_set (2 KB is too big for a task stack)  // s_prev = yesterday's plan, kept for its end-of-day slot
EXT_RAM_BSS_ATTR static ciq_t s_q;
static int s_snooze_slot = -1, s_snooze_min;
static bool s_snooze_prev;
static volatile int s_fake_min = -1;            // tests: pretend the clock reads this minute (1440+ = next day)
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static void (*s_send)(void);

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int cfg_on(void) { return hw_pref_get("ci_on", 1); }
static int cfg_snooze(void) { return hw_pref_get("ci_sn", 15); }
static const char *ans_name(char a) { return a == 'o' ? "on" : a == 'f' ? "off" : a == 'z' ? "snooze" : a == 'm' ? "missed" : ""; }
static const int16_t DEFAULT_SLOTS[] = {480, 720, 960, 1200, 1440};

static uint32_t next_date(uint32_t d)
{
    struct tm t = {.tm_year = d / 10000 - 1900, .tm_mon = (d / 100) % 100 - 1, .tm_mday = d % 100 + 1, .tm_hour = 12};
    mktime(&t);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

// local date + minute now (the RTC holds local time); false if the clock is unset
static bool now_local(uint32_t *date, int *min, time_t *utc)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return false;
    *date = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    if (s_fake_min >= 0) {                        // test clock: this minute of the RTC's day (1440+ = the next day)
        int fm = s_fake_min;
        if (fm >= 1440) *date = next_date(*date);
        *min = fm % 1440;
        if (utc) *utc = mktime(&t) - hw_pref_get("tz", 0);
        return true;
    }
    *min = t.tm_hour * 60 + t.tm_min;
    if (utc) *utc = mktime(&t) - hw_pref_get("tz", 0);   // the watch's TZ is UTC, so mktime gives "local seconds"
    return true;
}

// files on the internal flash FAT (NVS is only 24 KB); written to a temp file then renamed, so a power cut never
// leaves half a file
static void put_file(const char *path, const void *d, size_t n)
{
    char tmp[48]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", tmp); return; }
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    if (w != n) { remove(tmp); return; }
    remove(path);
    rename(tmp, path);
}
static bool get_file(const char *path, void *d, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t r = fread(d, 1, n, f);
    fclose(f);
    return r == n;
}
#define F_PLAN "/flash/sched.bin"
#define F_PREV "/flash/schedp.bin"
#define F_CIQ  "/flash/ciq.bin"
static void save(void) { put_file(F_PLAN, &s_plan, sizeof(s_plan)); }
static void save_prev(void) { put_file(F_PREV, &s_prev, sizeof(s_prev)); }
static void save_q(void) { put_file(F_CIQ, &s_q, sizeof(s_q)); }

// minutes since slot k of plan p at (date, min); <0 = not yet. 1440 belongs to the day after p.date at 00:00.
static int since(const plan_t *p, int k, uint32_t date, int min)
{
    int sm = p->slots[k];
    if (sm < 1440) return date == p->date ? min - sm : (date == next_date(p->date) ? min + 1440 - sm : 100000);
    return date == next_date(p->date) ? min : (date == p->date ? -1 : 100000);
}

// ------------------------------------------------------------------ from the phone
bool sched_set(const cJSON *m, char *err, size_t errn)
{
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "date");
    const cJSON *bl = cJSON_GetObjectItemCaseSensitive(m, "blocks");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(m, "gen");
    const cJSON *sl = cJSON_GetObjectItemCaseSensitive(m, "slots");
    if (!cJSON_IsNumber(d) || d->valuedouble < 20200101 || d->valuedouble > 29991231 || !cJSON_IsArray(bl)) {
        snprintf(err, errn, "bad sched"); return false;
    }
    static SemaphoreHandle_t busy;
    if (!busy) busy = xSemaphoreCreateMutex();
    xSemaphoreTake(busy, portMAX_DELAY);
    plan_t *pp = &s_new;
    memset(pp, 0, sizeof(*pp));
#define p (*pp)
    p.date = (uint32_t)d->valuedouble;
    if (cJSON_IsString(g)) snprintf(p.gen, sizeof(p.gen), "%s", g->valuestring);
    const cJSON *x;
    if (cJSON_IsArray(sl)) {
        cJSON_ArrayForEach(x, sl) {
            if (p.nslots >= SCHED_SLOTS || !cJSON_IsNumber(x)) continue;
            int v = (int)x->valuedouble;
            if (v >= 0 && v <= 1440 && (p.nslots == 0 || v > p.slots[p.nslots - 1])) p.slots[p.nslots++] = v;
        }
    } else {
        for (int i = 0; i < 5; i++) p.slots[p.nslots++] = DEFAULT_SLOTS[i];
    }
    cJSON_ArrayForEach(x, bl) {
        if (p.n >= SCHED_MAX) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(x, "id"), *ti = cJSON_GetObjectItemCaseSensitive(x, "title");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(x, "s"), *e = cJSON_GetObjectItemCaseSensitive(x, "e");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(s) || !cJSON_IsNumber(e)) continue;
        int si = (int)s->valuedouble, ei = (int)e->valuedouble;
        if (si < 0 || ei > 1440 || ei <= si) continue;
        blk_t *k = &p.b[p.n++];
        snprintf(k->id, sizeof(k->id), "%s", id->valuestring);
        const char *src = cJSON_IsString(ti) ? ti->valuestring : "";
        size_t L = strlen(src);                  // cut on a UTF-8 character boundary
        if (L > sizeof(k->title) - 1) { L = sizeof(k->title) - 1; while (L && ((unsigned char)src[L] & 0xC0) == 0x80) L--; }
        memcpy(k->title, src, L); k->title[L] = 0;
        for (char *c = k->title; *c; c++) if ((unsigned char)*c < 32) *c = ' ';
        k->s = si; k->e = ei;
        k->done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "done"));
    }
    lock();
    if (p.date == s_plan.date) {                 // same day: keep what was already asked/answered
        for (int k = 0; k < p.nslots; k++)
            for (int j = 0; j < s_plan.nslots; j++)
                if (p.slots[k] == s_plan.slots[j]) { if (s_plan.asked & (1 << j)) p.asked |= 1 << k; p.ans[k] = s_plan.ans[j]; }
    } else if (p.date > s_plan.date) {
        if (s_plan.date) { s_prev = s_plan; save_prev(); }   // its end-of-day slot may still be due
        // a new plan arriving in the middle of the day: slots already past are not asked late
        uint32_t nd; int nm;
        if (now_local(&nd, &nm, NULL))
            for (int k = 0; k < p.nslots; k++) if (since(&p, k, nd, nm) > 10) p.asked |= 1 << k;
    } else { unlock(); xSemaphoreGive(busy); snprintf(err, errn, "older plan"); return false; }
    s_plan = p;
    s_snooze_slot = -1;
    save();
    unlock();
    ESP_LOGI(TAG, "plan %lu gen=%s blocks=%d slots=%d", (unsigned long)p.date, p.gen, p.n, p.nslots);
#undef p
    xSemaphoreGive(busy);
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

void sched_cfg(const cJSON *m)
{
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(m, "on");
    const cJSON *sn = cJSON_GetObjectItemCaseSensitive(m, "snooze");
    if (cJSON_IsBool(on)) hw_pref_set("ci_on", cJSON_IsTrue(on));
    if (cJSON_IsNumber(sn)) { int v = (int)sn->valuedouble; if (v >= 5 && v <= 60) hw_pref_set("ci_sn", v); }
}

void sched_ack(uint32_t upto)
{
    lock();
    int k = 0;
    for (int i = 0; i < s_q.n; i++) if (s_q.q[i].seq > upto) s_q.q[k++] = s_q.q[i];
    bool changed = k != s_q.n;
    s_q.n = k;
    if (changed) save_q();
    unlock();
}

uint32_t sched_date(void) { return s_plan.date; }
int sched_queued(void) { return s_q.n; }
const char *sched_gen(void) { return s_plan.gen; }
int sched_count(void) { return s_plan.n; }

int sched_pending(cJSON *arr, int max)
{
    lock();
    int n = 0;
    for (int i = 0; i < s_q.n && n < max; i++, n++) {
        ci_t *c = &s_q.q[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "ci");
        cJSON_AddNumberToObject(o, "seq", c->seq);
        cJSON_AddNumberToObject(o, "date", c->date);
        cJSON_AddNumberToObject(o, "slot", c->slot);
        cJSON_AddNumberToObject(o, "at", c->at);
        cJSON_AddStringToObject(o, "ans", ans_name(c->ans));
        cJSON *ids = cJSON_AddArrayToObject(o, "ids");
        for (int j = 0; j < c->nids; j++) cJSON_AddItemToArray(ids, cJSON_CreateString(c->ids[j]));
        cJSON_AddItemToArray(arr, o);
    }
    unlock();
    return n;
}

// ------------------------------------------------------------------ the window of a slot
static int prev_slot_min(const plan_t *p, int k) { return k > 0 ? p->slots[k - 1] : 0; }

// blocks planned between the previous slot and this one (first slot of the day: the whole day ahead)
static int window(const plan_t *p, int k, int *idx, int max)
{
    int n = 0, a = prev_slot_min(p, k), b = p->slots[k];
    bool first = k == 0;
    for (int i = 0; i < p->n && n < max; i++) {
        if (first ? p->b[i].s >= b : (p->b[i].s < b && p->b[i].e > a)) idx[n++] = i;
    }
    return n;
}

static uint32_t record(plan_t *p, int k, char ans)
{
    time_t utc = 0; uint32_t d; int mi;
    now_local(&d, &mi, &utc);
    lock();
    if (ans != 'z' || !p->ans[k] || p->ans[k] == 'z') p->ans[k] = ans;      // a snooze never hides a final answer
    if (s_q.n >= SCHED_CIQ) { memmove(&s_q.q[0], &s_q.q[1], sizeof(ci_t) * (SCHED_CIQ - 1)); s_q.n--; }   // drop the oldest
    ci_t *c = &s_q.q[s_q.n++];
    memset(c, 0, sizeof(*c));
    uint32_t seq = (uint32_t)hw_pref_get("ciseq", 0) + 1;
    hw_pref_set("ciseq", (int)seq);
    c->seq = seq; c->at = (uint32_t)utc; c->date = p->date; c->slot = p->slots[k]; c->ans = ans;
    int idx[SCHED_IDS];
    int n = window(p, k, idx, SCHED_IDS);
    for (int j = 0; j < n; j++) snprintf(c->ids[j], sizeof(c->ids[j]), "%s", p->b[idx[j]].id);
    c->nids = n;
    printf("SCHED ci seq=%lu date=%lu slot=%d ans=%s ids=%d\n", (unsigned long)seq, (unsigned long)c->date, c->slot, ans_name(ans), n);
    if (p == &s_prev) save_prev(); else save();
    save_q();
    unlock();
    if (s_send) s_send();
    return seq;
}

static void fmt_hm(char *b, size_t n, int min) { snprintf(b, n, "%02d:%02d", (min / 60) % 24, min % 60); }

static void ask(plan_t *p, int k, bool is_prev, bool resnooze)
{
    char hm[8], head[48], body[200];
    int sm = p->slots[k];
    fmt_hm(hm, sizeof(hm), sm);
    if (sm == 1440) snprintf(head, sizeof(head), "End of day check-in");
    else snprintf(head, sizeof(head), "%s check-in", hm);
    int idx[SCHED_IDS];
    int n = window(p, k, idx, SCHED_IDS);
    int o = 0;
    if (k == 0) o = snprintf(body, sizeof(body), n ? "Today:" : "Nothing planned today.");
    else { char a[8]; fmt_hm(a, sizeof(a), prev_slot_min(p, k)); o = snprintf(body, sizeof(body), n ? "Planned since %s:" : "Nothing planned since %s.", a); }
    for (int j = 0; j < n && j < 3 && o < (int)sizeof(body) - 8; j++) {
        char st[8]; fmt_hm(st, sizeof(st), p->b[idx[j]].s);
        o += snprintf(body + o, sizeof(body) - o, "\n%s %s", st, p->b[idx[j]].title);
    }
    if (n > 3 && o < (int)sizeof(body) - 12) snprintf(body + o, sizeof(body) - o, "\n+%d more", n - 3);
    // context for "Hold to tell Amber" (the seq is the one this answer will get)
    char ctx[96];
    snprintf(ctx, sizeof(ctx), "{\"ci_seq\":%d,\"slot\":%d,\"date\":%lu}", hw_pref_get("ciseq", 0) + 1, sm, (unsigned long)p->date);
    printf("SCHED ask date=%lu slot=%d blocks=%d\n", (unsigned long)p->date, sm, n);
    int r = ui_checkin(head, body, ctx, 300);            // 0 on track, 1 off track, 2 snooze, -1 no answer
    char a = r == 0 ? 'o' : r == 1 ? 'f' : r == 2 ? 'z' : 'm';
    s_snooze_slot = -1;
    if (a == 'z' && resnooze) a = 'm';                    // snooze works once; a second one counts as no answer
    if (a == 'z') {
        uint32_t d; int mi;
        now_local(&d, &mi, NULL);
        s_snooze_slot = k; s_snooze_prev = is_prev;
        s_snooze_min = mi + (d != p->date && p->slots[k] != 1440 ? 1440 : 0) + cfg_snooze();
    }
    record(p, k, a);
}

// one step of the check-in clock
static void step(void)
{
    uint32_t date; int now;
    if (!cfg_on() || !now_local(&date, &now, NULL)) return;
    if (s_snooze_slot >= 0) {
        plan_t *p = s_snooze_prev ? &s_prev : &s_plan;
        int mins = now + (date != p->date && p->slots[s_snooze_slot] != 1440 ? 1440 : 0);
        if (mins >= s_snooze_min) { int k = s_snooze_slot; s_snooze_slot = -1; ask(p, k, s_snooze_prev, true); return; }
    }
    plan_t *plans[2] = {&s_prev, &s_plan};
    for (int pi = 0; pi < 2; pi++) {
        plan_t *p = plans[pi];
        if (!p->date) continue;
        for (int k = 0; k < p->nslots; k++) {
            if (p->asked & (1 << k)) continue;
            int d = since(p, k, date, now);
            if (d < 0 || d >= 100000) continue;
            lock(); p->asked |= 1 << k; if (p == &s_prev) save_prev(); else save(); unlock();
            if (d > 10) { record(p, k, 'm'); continue; }   // the watch slept through it: logged, not asked late
            ask(p, k, pi == 0, false);
            return;
        }
    }
}

// seconds until the next check-in (deep-sleep timer), 0 = none known
uint32_t sched_next_s(void)
{
    uint32_t date; int now;
    if (!cfg_on() || !now_local(&date, &now, NULL)) return 0;
    int best = -1;
    plan_t *plans[2] = {&s_prev, &s_plan};
    for (int pi = 0; pi < 2; pi++)
        for (int k = 0; k < plans[pi]->nslots; k++) {
            if (!plans[pi]->date || (plans[pi]->asked & (1 << k))) continue;
            int d = since(plans[pi], k, date, now);
            if (d < 0 && (best < 0 || -d < best)) best = -d;
        }
    return best < 0 ? 0 : (uint32_t)best * 60;
}

static void sched_task(void *a)
{
    while (1) {
        step();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));   // a test (citest) or a new plan wakes it at once
    }
}

void sched_test_minute(int min) { s_fake_min = min; if (s_task) xTaskNotifyGive(s_task); }

void sched_now_text(char *out, size_t n)
{
    out[0] = 0;
    uint32_t date; int now;
    if (!now_local(&date, &now, NULL) || date != s_plan.date) return;
    char t[8];
    for (int i = 0; i < s_plan.n; i++)
        if (now >= s_plan.b[i].s && now < s_plan.b[i].e) {
            fmt_hm(t, sizeof(t), s_plan.b[i].e);
            snprintf(out, n, "Now: %s  until %s", s_plan.b[i].title, t);
            return;
        }
    for (int i = 0; i < s_plan.n; i++)
        if (s_plan.b[i].s > now) {
            fmt_hm(t, sizeof(t), s_plan.b[i].s);
            snprintf(out, n, "Next: %s  %s", s_plan.b[i].title, t);
            return;
        }
}

bool sched_today(void)
{
    uint32_t date; int now;
    return now_local(&date, &now, NULL) && date == s_plan.date && s_plan.date;
}

bool sched_block(int i, sched_view_t *v)
{
    if (i < 0 || i >= s_plan.n || !sched_today()) return false;
    lock();
    snprintf(v->title, sizeof(v->title), "%s", s_plan.b[i].title);
    v->s = s_plan.b[i].s; v->e = s_plan.b[i].e; v->done = s_plan.b[i].done;
    unlock();
    return true;
}

bool sched_slot(int k, int *minute, char *ans)
{
    if (k < 0 || k >= s_plan.nslots || !s_plan.date) return false;
    *minute = s_plan.slots[k]; *ans = s_plan.ans[k];
    return true;
}

int sched_adherence(void)
{
    int on = 0, tot = 0;
    for (int k = 0; k < s_plan.nslots; k++) {
        char a = s_plan.ans[k];
        if (a == 'o') { on++; tot++; } else if (a == 'f' || a == 'm') tot++;
    }
    return tot ? on * 100 / tot : -1;
}

void sched_print(void)
{
    char sl[64] = ""; int o = 0;
    for (int k = 0; k < s_plan.nslots; k++) o += snprintf(sl + o, sizeof(sl) - o, "%s%d:%s", k ? "," : "", s_plan.slots[k], s_plan.ans[k] ? ans_name(s_plan.ans[k]) : (s_plan.asked & (1 << k)) ? "asked" : "-");
    printf("CMD sched date=%lu gen=%s n=%d slots=%s prev=%lu on=%d snooze=%d ciq=%d next_s=%lu adherence=%d\n",
           (unsigned long)s_plan.date, s_plan.gen, s_plan.n, sl, (unsigned long)s_prev.date, cfg_on(), cfg_snooze(), s_q.n,
           (unsigned long)sched_next_s(), sched_adherence());
    for (int i = 0; i < s_plan.n; i++)
        printf("SCHED blk %d %d-%d done=%d %s\n", i, s_plan.b[i].s, s_plan.b[i].e, s_plan.b[i].done, s_plan.b[i].title);
}

void sched_reset(void)
{
    lock();
    memset(&s_plan, 0, sizeof(s_plan)); memset(&s_prev, 0, sizeof(s_prev)); memset(&s_q, 0, sizeof(s_q));
    s_snooze_slot = -1;
    save(); save_prev(); save_q();
    unlock();
}

void sched_start(void (*on_new_answer)(void))
{
    s_send = on_new_answer;
    s_lock = xSemaphoreCreateMutex();
    if (!get_file(F_PLAN, &s_plan, sizeof(s_plan)) || s_plan.n > SCHED_MAX || s_plan.nslots > SCHED_SLOTS) memset(&s_plan, 0, sizeof(s_plan));
    if (!get_file(F_PREV, &s_prev, sizeof(s_prev)) || s_prev.n > SCHED_MAX || s_prev.nslots > SCHED_SLOTS) memset(&s_prev, 0, sizeof(s_prev));
    if (!get_file(F_CIQ, &s_q, sizeof(s_q)) || s_q.n > SCHED_CIQ) memset(&s_q, 0, sizeof(s_q));
    xTaskCreate(sched_task, "sched", 4096, NULL, 2, &s_task);
}

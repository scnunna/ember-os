// Guitar practice log shared by Metronome, Tuner and Practice apps; synced to the phone over Ember Link.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "hw.h"
#include "practice.h"

#define PFILE "/flash/practice.csv"
typedef struct { unsigned date; char start[6]; int min; char tool[12]; int bpm; int changes; } sess_t;

static int load(sess_t *out, int max)       // keeps the newest `max`
{
    FILE *f = fopen(PFILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        sess_t s = {0};
        if (sscanf(line, "%u,%5[^,],%d,%11[^,],%d,%d", &s.date, s.start, &s.min, s.tool, &s.bpm, &s.changes) != 6) continue;
        if (n == max) { memmove(out, out + 1, (max - 1) * sizeof(sess_t)); n--; }
        out[n++] = s;
    }
    fclose(f);
    return n;
}

void practice_log(const char *tool, int minutes, int bpm_max, int changes)
{
    struct tm t;
    if (!hw_rtc_get(&t) || (minutes <= 0 && changes <= 0)) return;
    FILE *f = fopen(PFILE, "a");
    if (!f) return;
    fprintf(f, "%04d%02d%02d,%02d:%02d,%d,%s,%d,%d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, minutes, tool, bpm_max, changes);
    fclose(f);
}

cJSON *practice_json(int max)
{
    static sess_t s[60];
    if (max > 60) max = 60;
    int n = load(s, max);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "date", s[i].date); cJSON_AddStringToObject(o, "start", s[i].start);
        cJSON_AddNumberToObject(o, "min", s[i].min); cJSON_AddStringToObject(o, "tool", s[i].tool);
        cJSON_AddNumberToObject(o, "bpm_max", s[i].bpm); cJSON_AddNumberToObject(o, "changes", s[i].changes);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static unsigned key_days_back(int d)
{
    struct tm t;
    if (!hw_rtc_get(&t)) return 0;
    t.tm_mday -= d; t.tm_hour = 12;
    mktime(&t);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
int practice_week_minutes(void)
{
    static sess_t s[60];
    int n = load(s, 60), sum = 0;
    unsigned from = key_days_back(6);
    for (int i = 0; i < n; i++) if (s[i].date >= from) sum += s[i].min;
    return sum;
}
int practice_streak(void)
{
    static sess_t s[60];
    int n = load(s, 60), streak = 0;
    for (int d = 0; d < 60; d++) {
        unsigned k = key_days_back(d);
        bool any = false;
        for (int i = 0; i < n; i++) if (s[i].date == k) { any = true; break; }
        if (!any) { if (d == 0) continue; break; }      // no practice yet today doesn't break the streak
        streak++;
    }
    return streak;
}

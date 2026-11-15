// Notes: meeting summaries + action items sent back by the Ember Companion app (Ember Link "note" messages,
// stored as /flash/notes/<recording>.json). Tap a note to read it; long-press "Delete" to remove it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "cJSON.h"
#include "app.h"
#include "hw.h"

#define NDIR "/flash/notes"
#define MAXN 20
static lv_obj_t *s_scr, *s_list, *s_detail;
static char s_names[MAXN][48];
static int s_n, s_open = -1;

static cJSON *load(int i)
{
    char p[96]; snprintf(p, sizeof(p), NDIR "/%s", s_names[i]);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    char *b = malloc(4097);
    size_t n = b ? fread(b, 1, 4096, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    cJSON *o = cJSON_Parse(b);
    free(b);
    return o;
}
static const char *str(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : def;
}

static void build_list(void);
static void close_cb(lv_event_t *e) { if (s_detail) { lv_obj_delete(s_detail); s_detail = NULL; } s_open = -1; }
static void del_cb(lv_event_t *e)
{
    if (s_open < 0) return;
    char p[96]; snprintf(p, sizeof(p), NDIR "/%s", s_names[s_open]);
    remove(p);
    close_cb(e);
    build_list();
    ui_msg("Note deleted");
}
static void open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)ui_ud(e);
    cJSON *o = load(i);
    if (!o) { ui_msg("Could not read this note"); return; }
    s_open = i;
    s_detail = lv_obj_create(s_scr);
    lv_obj_set_size(s_detail, 410, 502);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
    lv_obj_t *back = ui_btn(s_detail, LV_SYMBOL_LEFT " Notes", 130, 44, 0x333333, close_cb, NULL);
    lv_obj_set_pos(back, 20, 16);
    lv_obj_t *del = lv_button_create(s_detail);            // long-press only, so a stray tap never deletes
    lv_obj_set_size(del, 130, 44);
    lv_obj_set_pos(del, 260, 16);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x5A1E1E), 0);
    lv_obj_t *dl = ui_label(del, &lv_font_montserrat_14, C_TEXT, "hold to delete");
    lv_obj_center(dl);
    lv_obj_add_event_cb(del, del_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *t = ui_label(s_detail, &lv_font_montserrat_28, C_TEXT, str(o, "title", "Meeting"));
    lv_obj_set_width(t, 370); lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(t, 20, 74);
    lv_obj_t *sm = ui_label(s_detail, &lv_font_montserrat_20, 0xD8D8D8, str(o, "summary", "(no summary)"));
    lv_obj_set_width(sm, 370); lv_label_set_long_mode(sm, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align_to(sm, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(o, "actions"), *it;
    char ab[1200] = "";
    cJSON_ArrayForEach(it, acts) if (cJSON_IsString(it)) { strncat(ab, LV_SYMBOL_OK "  ", sizeof(ab) - strlen(ab) - 1); strncat(ab, it->valuestring, sizeof(ab) - strlen(ab) - 2); strcat(ab, "\n"); }
    if (ab[0]) {
        lv_obj_t *h = ui_label(s_detail, &lv_font_montserrat_14, C_ACCENT, "ACTION ITEMS");
        lv_obj_align_to(h, sm, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 18);
        lv_obj_t *a = ui_label(s_detail, &lv_font_montserrat_20, C_TEXT, ab);
        lv_obj_set_width(a, 370); lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align_to(a, h, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }
    cJSON_Delete(o);
}

static int cmp_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }
static void build_list(void)
{
    lv_obj_clean(s_list);
    s_n = 0;
    DIR *d = opendir(NDIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && s_n < MAXN) {
        size_t l = strlen(e->d_name);
        if (l > 5 && l < sizeof(s_names[0]) && !strcasecmp(e->d_name + l - 5, ".json")) strcpy(s_names[s_n++], e->d_name);
    }
    if (d) closedir(d);
    qsort(s_names, s_n, sizeof(s_names[0]), cmp_desc);     // recording names start with a timestamp: newest first
    if (!s_n) {
        lv_obj_t *l = ui_label(s_list, &lv_font_montserrat_20, C_DIM,
            "No notes yet.\n\n1. Record a meeting\n   (double-press BOOT)\n2. In Ember Companion:\n   Recordings > Transcribe\n3. Send to watch");
        lv_obj_set_pos(l, 16, 10);
        return;
    }
    for (int i = 0; i < s_n; i++) {
        cJSON *o = load(i);
        char lbl[80];
        snprintf(lbl, sizeof(lbl), "%.60s", o ? str(o, "title", s_names[i]) : s_names[i]);
        if (o) cJSON_Delete(o);
        lv_obj_t *b = ui_btn(s_list, lbl, 370, 64, 0x26323F, open_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(b, 0, i * 74);
        lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    }
}

static void start(lv_obj_t *scr)
{
    s_scr = scr; s_detail = NULL; s_open = -1;
    ui_title(scr, "Notes");
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 370, 420);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    build_list();
}
static void stop(void) { s_detail = NULL; s_scr = NULL; }
void app_notes_start(lv_obj_t *s) { start(s); }
void app_notes_stop(void) { stop(); }

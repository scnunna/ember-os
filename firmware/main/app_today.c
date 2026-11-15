// Today: the day's plan from Life OS (time blocks) and the check-ins so far, with today's adherence.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "sched.h"

static const char *ans_txt(char a) { return a == 'o' ? LV_SYMBOL_OK " on track" : a == 'f' ? LV_SYMBOL_CLOSE " off track" : a == 'z' ? "snoozed" : a == 'm' ? "missed" : "-"; }
static uint32_t ans_col(char a) { return a == 'o' ? C_OK : a == 'f' || a == 'm' ? C_WARN : C_DIM; }

void app_today_start(lv_obj_t *scr)
{
    ui_title(scr, "Today");
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, 370, 400);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    char b[96];
    if (!sched_today()) {
        lv_obj_t *l = ui_label(box, &lv_font_montserrat_20, C_DIM, "No plan for today yet.\nPlan your day in Life OS,\nor ask Amber.");
        lv_obj_set_width(l, 350);
        return;
    }
    int ad = sched_adherence();
    if (ad >= 0) snprintf(b, sizeof(b), "Kept up: %d%%", ad); else snprintf(b, sizeof(b), "No check-ins yet today");
    ui_label(box, &lv_font_montserrat_20, C_ACCENT, b);
    int minute; char a;
    for (int k = 0; sched_slot(k, &minute, &a); k++) {
        if (minute == 1440) snprintf(b, sizeof(b), "End of day   %s", ans_txt(a));
        else snprintf(b, sizeof(b), "%02d:00 check-in   %s", minute / 60, ans_txt(a));
        lv_obj_t *l = ui_label(box, &lv_font_montserrat_14, ans_col(a), b);
        lv_obj_set_width(l, 350);
    }
    sched_view_t v;
    int n = 0;
    for (int i = 0; sched_block(i, &v); i++, n++) {
        snprintf(b, sizeof(b), "%02d:%02d-%02d:%02d  %s%s", v.s / 60, v.s % 60, (v.e / 60) % 24, v.e % 60, v.done ? LV_SYMBOL_OK " " : "", v.title);
        lv_obj_t *l = ui_label(box, &lv_font_montserrat_20, v.done ? C_DIM : C_TEXT, b);
        lv_obj_set_width(l, 350);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    if (!n) ui_label(box, &lv_font_montserrat_20, C_DIM, "No time blocks today.");
}

void app_today_stop(void) {}

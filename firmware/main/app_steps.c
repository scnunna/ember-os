// Steps app: today's count, goal ring, distance, 7-day bars.
#include <stdio.h>
#include "app.h"
#include "hw.h"

#define GOAL 8000
static lv_obj_t *s_arc, *s_num, *s_info, *s_chart;
static lv_chart_series_t *s_ser;
static lv_timer_t *s_tm;

static void refresh(lv_timer_t *t)
{
    uint32_t h[7];
    hw_steps_history(h);
    char b[48];
    snprintf(b, sizeof(b), "%lu", (unsigned long)h[0]);
    lv_label_set_text(s_num, b);
    lv_obj_align_to(s_num, s_arc, LV_ALIGN_CENTER, 0, -14);
    lv_arc_set_value(s_arc, h[0] > GOAL ? GOAL : h[0]);
    snprintf(b, sizeof(b), "%.2f km    %lu kcal", h[0] * 0.00075f, (unsigned long)(h[0] * 0.04f));
    lv_label_set_text(s_info, b);
    uint32_t mx = 4000;
    for (int i = 0; i < 7; i++) if (h[i] > mx) mx = h[i];
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    for (int i = 0; i < 7; i++) lv_chart_set_series_value_by_id(s_chart, s_ser, i, h[6 - i]);
    lv_chart_refresh(s_chart);
}

static void start(lv_obj_t *scr)
{
    ui_title(scr, "Steps");
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 250, 250);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 70);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, GOAL);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    s_num = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "0");
    lv_obj_t *g = ui_label(scr, &lv_font_montserrat_14, C_DIM, "goal 8,000");
    lv_obj_align_to(g, s_arc, LV_ALIGN_CENTER, 0, 40);
    s_info = ui_label(scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 330);

    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 360, 110);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, 7);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_column(s_chart, 6, 0);
    s_ser = lv_chart_add_series(s_chart, lv_color_hex(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t *c = ui_label(scr, &lv_font_montserrat_14, C_DIM, "last 7 days");
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_tm = lv_timer_create(refresh, 1000, NULL);
    refresh(NULL);
}
static void stop(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } }

void app_steps_start(lv_obj_t *s) { start(s); }
void app_steps_stop(void) { stop(); }

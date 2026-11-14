#include "app.h"

lv_obj_t *ui_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t *ui_title(lv_obj_t *scr, const char *txt)
{
    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_28, C_TEXT, txt);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

typedef struct { lv_event_cb_t cb; void *ud; lv_point_t start; } btn_ctx_t;

void *ui_ud(lv_event_t *e) { return ((btn_ctx_t *)lv_event_get_user_data(e))->ud; }

// A press that travelled more than 22 px was a swipe, not a tap: never fire the button for it.
static void btn_ev(lv_event_t *e)
{
    btn_ctx_t *c = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = {0, 0};
    if (in) lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { c->start = p; return; }
    if (lv_event_get_code(e) == LV_EVENT_DELETE) { lv_free(c); return; }
    int dx = p.x - c->start.x, dy = p.y - c->start.y;
    if (dx * dx + dy * dy > 22 * 22) return;
    c->cb(e);
}

lv_obj_t *ui_btn(lv_obj_t *p, const char *txt, int w, int h, uint32_t color, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, h / 2 > 28 ? 28 : h / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_20, C_TEXT, txt);
    lv_obj_center(l);
    if (cb) {
        btn_ctx_t *c = lv_malloc(sizeof(*c));
        c->cb = cb; c->ud = ud; c->start.x = c->start.y = 0;
        lv_obj_add_event_cb(b, btn_ev, LV_EVENT_PRESSED, c);
        lv_obj_add_event_cb(b, btn_ev, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(b, btn_ev, LV_EVENT_DELETE, c);
    }
    return b;
}

static void msg_close(lv_timer_t *t)
{
    lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(t));   // timer auto-deletes (repeat_count = 1)
}

// Small toast on the top layer. Call with the LVGL lock held (inside callbacks it is).
void ui_msg(const char *txt)
{
    lv_obj_t *m = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 24, 0);
    lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_t *l = ui_label(m, &lv_font_montserrat_20, C_TEXT, txt);
    lv_obj_set_width(l, 300);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_timer_t *t = lv_timer_create(msg_close, 2200, m);
    lv_timer_set_repeat_count(t, 1);
}

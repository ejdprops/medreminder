#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_scroll.h"

#include "ui_theme.h"

#define BAR_W 8
#define BAR_PAD_V 4
#define THUMB_MIN_H 24

typedef struct {
    lv_obj_t *strip; /* touch area: the gutter, on top of the list */
    lv_obj_t *track;
    lv_obj_t *thumb;
} gutter_t;

static int32_t max_scroll(lv_obj_t *list)
{
    return lv_obj_get_scroll_top(list) + lv_obj_get_scroll_bottom(list);
}

/* Thumb size = visible share of the list, position = scroll position */
static void place_thumb(lv_obj_t *list, gutter_t *g)
{
    int32_t track_h = lv_obj_get_height(g->track);
    int32_t view_h = lv_obj_get_height(list);
    int32_t total = max_scroll(list);
    int32_t thumb_h = track_h * view_h / (view_h + total);
    if (thumb_h < THUMB_MIN_H) {
        thumb_h = THUMB_MIN_H;
    }
    lv_obj_set_height(g->thumb, thumb_h);
    lv_obj_set_y(g->thumb, total ? (track_h - thumb_h) * lv_obj_get_scroll_top(list) / total : 0);
}

static void list_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target_obj(e);
    gutter_t *g = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_free(g);
        return;
    }
    place_thumb(list, g);
}

/* Touch on the strip: scroll so the thumb's center follows the finger */
static void strip_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_user_data(e);
    gutter_t *g = lv_obj_get_user_data(list);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(g->track, &a);
    int32_t track_h = lv_area_get_height(&a);
    int32_t thumb_h = lv_obj_get_height(g->thumb);
    int32_t range = track_h - thumb_h;
    if (range <= 0) {
        return;
    }
    int32_t pos = p.y - a.y1 - thumb_h / 2;
    pos = pos < 0 ? 0 : (pos > range ? range : pos);
    lv_obj_scroll_to_y(list, max_scroll(list) * pos / range, LV_ANIM_OFF);
}

void ui_scroll_gutter(lv_obj_t *list)
{
    gutter_t *g = lv_malloc(sizeof(*g));
    LV_ASSERT_MALLOC(g);
    lv_obj_set_style_pad_right(list, UI_SCROLL_GUTTER, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF); /* our own bar instead */
    lv_obj_update_layout(list);

    g->strip = lv_obj_create(lv_obj_get_parent(list));
    lv_obj_remove_style_all(g->strip);
    lv_obj_remove_flag(g->strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(g->strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g->strip, UI_SCROLL_GUTTER, lv_obj_get_height(list));
    lv_obj_set_pos(g->strip, lv_obj_get_x(list) + lv_obj_get_width(list) - UI_SCROLL_GUTTER, lv_obj_get_y(list));
    lv_obj_add_event_cb(g->strip, strip_event_cb, LV_EVENT_PRESSED, list);
    lv_obj_add_event_cb(g->strip, strip_event_cb, LV_EVENT_PRESSING, list);

    g->track = lv_obj_create(g->strip);
    lv_obj_remove_style_all(g->track);
    lv_obj_remove_flag(g->track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g->track, BAR_W, lv_obj_get_height(list) - 2 * BAR_PAD_V);
    lv_obj_align(g->track, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g->track, BAR_W / 2, 0);
    lv_obj_set_style_bg_opa(g->track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g->track, lv_color_hex(UI_T->track), 0);

    g->thumb = lv_obj_create(g->track);
    lv_obj_remove_style_all(g->thumb);
    lv_obj_remove_flag(g->thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(g->thumb, BAR_W);
    lv_obj_set_style_radius(g->thumb, BAR_W / 2, 0);
    lv_obj_set_style_bg_opa(g->thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g->thumb, lv_color_hex(UI_T->indicator), 0);

    lv_obj_set_user_data(list, g);
    lv_obj_add_event_cb(list, list_event_cb, LV_EVENT_SCROLL, g);
    lv_obj_add_event_cb(list, list_event_cb, LV_EVENT_DELETE, g);
    ui_scroll_gutter_update(list);
}

void ui_scroll_gutter_update(lv_obj_t *list)
{
    gutter_t *g = lv_obj_get_user_data(list);
    if (!g) {
        return;
    }
    lv_obj_update_layout(list);
    lv_obj_set_flag(g->strip, LV_OBJ_FLAG_HIDDEN, max_scroll(list) <= 0); /* only when it overflows */
    place_thumb(list, g);
}

#endif

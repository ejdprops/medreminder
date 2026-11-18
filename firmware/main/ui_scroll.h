#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scrollbar gutter for a vertical list: a UI_SCROLL_GUTTER px strip on the right with a track and
 * thumb. Touching the strip jumps the list to that spot and drags it along (drag down = scroll
 * down); rows stop before the strip, so scrolling there never takes a dose. Call after the list
 * is sized and placed (the strip is a sibling on top of it), and ui_scroll_gutter_update() after
 * (re)filling it: the strip only shows when the content overflows. */
#define UI_SCROLL_GUTTER 32

void ui_scroll_gutter(lv_obj_t *list);
void ui_scroll_gutter_update(lv_obj_t *list);

#ifdef __cplusplus
}
#endif

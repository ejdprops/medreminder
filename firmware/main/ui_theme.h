#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* App-wide light/dark colors (Settings > Dark mode, saved in NVS). Dose colors don't change with
 * the theme; text drawn on a dose color picks dark or white by the color's brightness. */
typedef struct {
    uint32_t bg;          /* screen background */
    uint32_t text;        /* titles, values, selected ring */
    uint32_t text2;       /* button labels, icons */
    uint32_t muted;       /* secondary text */
    uint32_t caption;     /* small captions, version */
    uint32_t btn_bg, btn_pressed, btn_border;
    uint32_t card, card_border;
    uint32_t track, indicator, knob; /* sliders */
    uint32_t ok, warn, error;
    uint32_t del_border, del_text;
    uint32_t date, time;  /* home top line */
    uint32_t dev_border, dev_icon; /* dev reset button */
} ui_theme_t;

#define UI_T (ui_theme())

void ui_theme_init(void); /* load the saved mode; call before building any screen */
const ui_theme_t *ui_theme(void);
bool ui_theme_is_dark(void);
void ui_theme_set_dark(bool dark); /* saves it; the caller rebuilds the screens */

#ifdef __cplusplus
}
#endif

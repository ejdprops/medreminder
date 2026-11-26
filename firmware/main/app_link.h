#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* App link: the Med Dot app (apple/) reads and changes the main unit's settings over IP (UDP, see
 * app_link.c). Main unit with a display only; a no-op elsewhere. Start after esp_matter::start. */
void app_link_start(void);

/* LVGL context: the pairing dialog's answer (true = Pair, false = Cancel) */
void app_link_pair_answer(bool approve);

#ifdef __cplusplus
}
#endif

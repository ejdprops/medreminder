#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "doses.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single-wire UART link to a dashboard panel (see uart_link.c). Only boards that set
 * CONFIG_MEDREMINDER_UART_LINK_GPIO have one; a no-op elsewhere. */
void uart_link_start(void);

/* Doses task, every tick: what this device holds now (sent to the panel when its copy is old) */
void uart_link_publish(const doses_snapshot_t *snap);

#ifdef __cplusplus
}
#endif

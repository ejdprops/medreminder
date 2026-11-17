/* SPDX-License-Identifier: Apache-2.0 */
/* Board startup, buttons and storage. medreminder: RTC/IMU/SHTC3 and Wi-Fi removed. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "custom_io_expander_ch32v003.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Startup results: ESP_OK means ready; this is not a live health check. */
typedef struct {
    esp_err_t i2c, expander, power, display, touch;
    esp_err_t button, audio, sdcard;
} bsp_init_status_t;

/* Initialize buses, backlight-off, NVS, SD, display/touch, then other peripherals.
 * Optional failures are recorded in status. Create the UI before calling show(). */
esp_err_t bsp_init(void);
/* Read-only BSP-owned startup results. */
const bsp_init_status_t *bsp_get_init_status(void);
/* Initialize only I2C and CH32V003; pin configuration belongs to each peripheral. */
esp_err_t bsp_io_expander_init(void);
/* Average 8 ADC samples. Nominal 3.3 V reference, 3:1 divider; not a charge percentage. */
esp_err_t bsp_battery_get_voltage(float *voltage, uint16_t *raw);
/* Borrowed handles; NULL before initialization. */
esp_io_expander_handle_t bsp_get_io_expander_handle(void);
lv_display_t *bsp_display_get(void);
esp_lcd_touch_handle_t bsp_touch_get_handle(void);

/* This board has one BOOT button on GPIO28. */
typedef enum { BSP_BUTTON_ID_BOOT } bsp_button_id_t;
/* Start 20 ms sampling; count presses after three stable samples. */
esp_err_t bsp_user_button_init(void);
/* Read raw state into pressed; no debounce in this getter. */
esp_err_t bsp_button_get_pressed(bsp_button_id_t button, bool *pressed);
/* Global debounced press count; compare snapshots for per-APP counts. */
uint32_t bsp_user_button_get_press_count(void);

typedef struct { bool mounted;
    uint64_t capacity_bytes, total_bytes, free_bytes; /* Card capacity, FAT total/free space */
} bsp_sdcard_info_t;
/* Mount FAT at /sdcard over shared SPI; never auto-format. */
esp_err_t bsp_sdcard_mount(void);
/* Acquire before opening files; prevents unmount, not concurrent file access. */
esp_err_t bsp_sdcard_acquire(void);
/* Close files before release; release does not unmount. */
void bsp_sdcard_release(void);
/* Reject active leases; never free the SPI host shared with LCD. */
esp_err_t bsp_sdcard_unmount(void);
/* Query mounted card/FAT capacity; may block. Does not mount the card. */
esp_err_t bsp_sdcard_get_info(bsp_sdcard_info_t *info);

#ifdef __cplusplus
}
#endif

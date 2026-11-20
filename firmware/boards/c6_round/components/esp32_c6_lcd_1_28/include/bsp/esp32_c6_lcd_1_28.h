/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BSP for the Waveshare ESP32-C6-LCD-1.28 (round GC9A01 240x240, no touch).
 * Based on Waveshare's esp32_c6_touch_lcd_1_28 BSP (waveshareteam/ESP32-C6-LCD-1.28,
 * examples/ESP-IDF-v5.5.2/07_LVGL_Demo); medreminder: touch, SD card and SPIFFS removed (GPIO23 is
 * the charger status line on this board, not touch reset), API matched to the C5 BSP
 * (bool lock, bsp_display_show).
 */
#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"

/**************************************************************************************************
 *  BSP capabilities
 **************************************************************************************************/
#define BSP_CAPS_DISPLAY        1
#define BSP_CAPS_TOUCH          0
#define BSP_CAPS_BUTTONS        1
#define BSP_CAPS_AUDIO          0
#define BSP_CAPS_AUDIO_SPEAKER  0
#define BSP_CAPS_AUDIO_MIC      0
#define BSP_CAPS_SDCARD         0
#define BSP_CAPS_IMU            1
#define BSP_CAPS_RTC            1

/**************************************************************************************************
 *  Pinout
 **************************************************************************************************/
/* I2C: QMI8658 IMU (0x6B), PCF85063 RTC (0x51) */
#define BSP_I2C_SCL             (GPIO_NUM_8)
#define BSP_I2C_SDA             (GPIO_NUM_18)
#define BSP_IMU_INT1            (GPIO_NUM_5)
#define BSP_IMU_INT2            (GPIO_NUM_4)
#define BSP_RTC_INT             (GPIO_NUM_6)

/* Display (SPI2, shared with the unused TF card: CS GPIO15, MISO GPIO22) */
#define BSP_LCD_CS              (GPIO_NUM_10)
#define BSP_LCD_PCLK            (GPIO_NUM_11)
#define BSP_LCD_DC              (GPIO_NUM_19)
#define BSP_LCD_MOSI            (GPIO_NUM_21)
#define BSP_LCD_RST             (GPIO_NUM_20)
#define BSP_LCD_BACKLIGHT       (GPIO_NUM_7)
#define BSP_LCD_SPI_NUM         (SPI2_HOST)

#define BSP_LCD_H_RES           (240)
#define BSP_LCD_V_RES           (240)
#define BSP_LCD_ROTATION        ESP_LV_ADAPTER_ROTATE_0
#define BSP_LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)

/* Button */
#define BSP_BUTTON_BOOT         (GPIO_NUM_9)

/* Battery (200K/100K divider) */
#define BSP_BATTERY_ADC         (GPIO_NUM_0)

#define BSP_I2C_NUM             (I2C_NUM_0)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lv_adapter_config_t          lv_adapter_cfg;
    esp_lv_adapter_rotation_t        rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
} bsp_display_cfg_t;

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/* Display + LVGL. The backlight stays off until bsp_display_show() has flushed the first frame. */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
lv_display_t *bsp_display_get(void);
esp_err_t bsp_display_show(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

esp_err_t bsp_display_brightness_set(int percent);
int bsp_display_brightness_get(void);

#ifdef __cplusplus
}
#endif

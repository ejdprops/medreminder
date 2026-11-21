/* CrowPanel 7" HMI (Elecrow DIS08070H, board V3.0): display, touch, backlight.
 * Pins and timing from the V3.0 schematic (Eagle_SCH&PCB/V3.0 in Elecrow-RD/CrowPanel-7.0-HMI-
 * ESP32-Display-800x480) and Elecrow's V3.0 factory code (LovyanGFX config, PCA9557 sequence).
 *
 *   RGB565, 16 data lines (panel R3-R7/G2-G7/B3-B7): B 15 7 6 5 4, G 9 46 3 8 16 1, R 14 21 47 48 45;
 *   HSYNC 39, VSYNC 40, DE 41, PCLK 0 (so the BOOT button is unusable once the display runs).
 *   Backlight: GPIO2 -> MT9201 LED driver EN (PWM).
 *   I2C SDA 19 / SCL 20 (4.7K pull-ups): GT911 touch, PCA9557 expander (0x18).
 *   PCA9557 IO0 = touch reset, IO1 = touch INT (also to IO38 through R57, marked 10K/NC: the
 *   schematic says IO38 is free with the resistive touch option; see the link test).
 *   IO2 LCD reset / IO3 LCD power are behind 0R/NC resistors: left alone. */
#include "board.h"

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "board";

#define H_RES 800
#define V_RES 480
#define PCLK_HZ (15 * 1000 * 1000)
#define I2C_SDA 19
#define I2C_SCL 20
#define PCA9557_ADDR 0x18
#define PCA_OUTPUT 0x01
#define PCA_CONFIG 0x03 /* 1 = input */
#define BL_GPIO 2

static i2c_master_bus_handle_t s_i2c;

static esp_err_t pca_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, sizeof(b), 50);
}

/* GT911 reset with INT held low (I2C address 0x5D), as Elecrow's V3.0 code: both low, release reset
 * after 20 ms, INT back to input 100 ms later */
static void touch_reset(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PCA9557_ADDR, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t pca;
    if (i2c_master_bus_add_device(s_i2c, &cfg, &pca) != ESP_OK) {
        return;
    }
    if (pca_write(pca, PCA_OUTPUT, 0x00) != ESP_OK || pca_write(pca, PCA_CONFIG, 0xFC) != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 not answering: touch not reset");
    } else {
        vTaskDelay(pdMS_TO_TICKS(20));
        pca_write(pca, PCA_OUTPUT, 0x01);
        vTaskDelay(pdMS_TO_TICKS(100));
        pca_write(pca, PCA_CONFIG, 0xFE);
    }
    i2c_master_bus_rm_device(pca);
}

static esp_lcd_touch_handle_t touch_start(void)
{
    uint16_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(s_i2c, addr, 50) != ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (i2c_master_probe(s_i2c, addr, 50) != ESP_OK) {
            ESP_LOGE(TAG, "no GT911 at 0x5D or 0x14");
            return NULL;
        }
    }
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t touch = NULL;
    const esp_lcd_touch_config_t cfg = {
        .x_max = H_RES,
        .y_max = V_RES,
        .rst_gpio_num = GPIO_NUM_NC, /* on the PCA9557 */
        .int_gpio_num = GPIO_NUM_NC, /* polled */
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io) != ESP_OK || esp_lcd_touch_new_i2c_gt911(io, &cfg, &touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed");
        return NULL;
    }
    ESP_LOGI(TAG, "GT911 at 0x%02x", addr);
    return touch;
}

static esp_lcd_panel_handle_t panel_start(uint8_t num_fbs)
{
    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = PCLK_HZ,
            .h_res = H_RES,
            .v_res = V_RES,
            .hsync_pulse_width = 48,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 31,
            .vsync_back_porch = 13,
            .vsync_front_porch = 1,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = H_RES * 10, /* DMA reads internal RAM; refilled from PSRAM */
        .dma_burst_size = 64,
        .hsync_gpio_num = 39,
        .vsync_gpio_num = 40,
        .de_gpio_num = 41,
        .pclk_gpio_num = 0,
        .disp_gpio_num = -1,
        .data_gpio_nums = {15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45},
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

void board_backlight(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void)
{
    const ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                   .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    const ledc_channel_config_t c = {.gpio_num = BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                                     .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

lv_display_t *board_start(void)
{
    backlight_init(); /* dark until the first frame is up */
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
    touch_reset();

    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    acfg.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&acfg));
    uint8_t fbs = esp_lv_adapter_get_required_frame_buffer_count(ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB,
                                                                  ESP_LV_ADAPTER_ROTATE_0);
    esp_lcd_panel_handle_t panel = panel_start(fbs);
    esp_lv_adapter_display_config_t dcfg =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(panel, NULL, H_RES, V_RES, ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *disp = esp_lv_adapter_register_display(&dcfg);
    if (!disp) {
        ESP_LOGE(TAG, "display register failed");
        return NULL;
    }
    esp_lcd_touch_handle_t touch = touch_start();
    if (touch) {
        esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);
        esp_lv_adapter_register_touch(&tcfg);
    }
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI(TAG, "display %dx%d, %u frame buffers, pclk %d MHz", H_RES, V_RES, fbs, PCLK_HZ / 1000000);
    return disp;
}

bool board_lock(void)
{
    return esp_lv_adapter_lock(-1) == ESP_OK;
}

void board_unlock(void)
{
    esp_lv_adapter_unlock();
}

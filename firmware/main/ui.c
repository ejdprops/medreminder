#include "ui.h"

#include "audio.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>

static const char *TAG = "ui";

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#include "ui_home.h"

static void ui_init_task(void *arg)
{
    /* LVGL task stack in PSRAM: internal RAM is reserved for Matter and the SPI DMA buffers */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90, /* landscape 320x240 */
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    cfg.lv_adapter_cfg.stack_in_psram = true;
    if (!bsp_display_start_with_config(&cfg)) {
        ESP_LOGE(TAG, "display start failed");
    } else {
        if (bsp_display_lock(UINT32_MAX)) {
            ui_home_create(lv_screen_active());
            bsp_display_unlock();
        }
        esp_err_t err = bsp_display_show();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display show failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "UI started");
        }
        /* Audio shares the BSP's I2C bus and IO expander, which display start brought up */
        audio_start();
    }
    vTaskDeleteWithCaps(NULL);
}

void ui_start(void)
{
    /* bsp_display_show() renders the first frame on the calling task; main's 3.5KB stack is too
     * small for that. Do the startup on a one-shot task with its stack in PSRAM. */
    if (xTaskCreatePinnedToCoreWithCaps(ui_init_task, "ui_init", 8192, NULL, 5, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "ui_init task create failed");
    }
}

#else

void ui_start(void)
{
    ESP_LOGI(TAG, "no display on this board");
}

#endif

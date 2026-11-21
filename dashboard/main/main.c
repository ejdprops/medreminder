/* MedReminder dashboard: CrowPanel 7" next to the pills, fed by a XIAO C6 node over one wire
 * (link.c). The screen is ui.c; the panel hardware board.c; Wi-Fi (wifi.c) feeds the weather tile. */
#include <stdlib.h>
#include <time.h>

#include <esp_log.h>

#include "board.h"
#include "link.h"
#include "ui.h"
#include "weather.h"
#include "wifi.h"

static const char *TAG = "dash";

#define TZ "CST6CDT,M3.2.0,M11.1.0" /* the C5's CONFIG_MEDREMINDER_TZ */

void app_main(void)
{
    setenv("TZ", TZ, 1);
    tzset();
    link_start();
    if (!board_start()) {
        return;
    }
    if (board_lock()) {
        ui_start();
        board_unlock();
    }
    board_backlight(100);
    /* After the display: Wi-Fi only feeds the weather/calendar tiles */
    wifi_start();
    weather_start();
    ESP_LOGI(TAG, "started");
}

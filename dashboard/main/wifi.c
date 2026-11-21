/* Wi-Fi for the dashboard's internet tiles (weather, calendar). Doses never depend on it: they come
 * over the wire from the XIAO. Credentials from secrets.h (git-ignored, see secrets.h.example). */
#include "wifi.h"

#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "dash_wifi";

#define RETRY_MAX_S 60 /* reconnect backoff: 1, 2, 4 ... s, at most this */

static volatile bool s_connected;
static int s_backoff_s = 1;

bool wifi_configured(void)
{
#ifdef DASH_WIFI_SSID
    return DASH_WIFI_SSID[0] != 0;
#else
    return false;
#endif
}

bool wifi_connected(void)
{
    return s_connected;
}

#ifdef DASH_WIFI_SSID
static void reconnect_task(void *arg)
{
    int delay_s = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %d s", d->reason, s_backoff_s);
        /* Not from the event task: it must not block */
        xTaskCreate(reconnect_task, "wifi_retry", 2048, (void *)(intptr_t)s_backoff_s, 3, NULL);
        s_backoff_s = s_backoff_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : s_backoff_s * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_backoff_s = 1;
    }
}
#endif

void wifi_start(void)
{
    if (!wifi_configured()) {
        ESP_LOGW(TAG, "no secrets.h (or empty SSID): Wi-Fi off");
        return;
    }
#ifdef DASH_WIFI_SSID
    /* The Wi-Fi driver keeps PHY calibration in NVS (credentials stay in RAM, see below) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_log_level_set("wifi", ESP_LOG_WARN); /* the driver logs the network name */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, DASH_WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, DASH_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* secrets.h is the only source */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting");
#endif
}

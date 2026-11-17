#include "time_sync.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <esp_log.h>
#include <esp_netif_sntp.h>

static const char *TAG = "time_sync";

/* 2026-01-01T00:00:00Z: anything earlier is the unset boot clock */
#define VALID_AFTER 1767225600

static void synced_cb(struct timeval *tv)
{
    struct tm local;
    localtime_r(&tv->tv_sec, &local);
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "time synced: %s", buf);
}

void time_sync_start(void)
{
    setenv("TZ", CONFIG_MEDREMINDER_TZ, 1);
    tzset();

    /* lwIP SNTP retries until Thread is attached and the NAT64 DNS server is set, then
     * re-syncs every CONFIG_LWIP_SNTP_UPDATE_DELAY. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MEDREMINDER_NTP_SERVER);
    cfg.sync_cb = synced_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started (%s, TZ %s)", CONFIG_MEDREMINDER_NTP_SERVER, CONFIG_MEDREMINDER_TZ);
}

bool time_is_valid(void)
{
    return time(NULL) > VALID_AFTER;
}

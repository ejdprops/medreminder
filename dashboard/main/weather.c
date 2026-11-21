/* Weather tile data: Open-Meteo (api.open-meteo.com, free, no account or key) over HTTPS every
 * WEATHER_EVERY_S, sooner while it hasn't worked yet. Location from secrets.h. Fahrenheit. */
#include "weather.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "wifi.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "weather";

#define WEATHER_EVERY_S (15 * 60)
#define RETRY_S 60
#define WAIT_WIFI_S 2
#define BODY_MAX 4096

static SemaphoreHandle_t s_lock;
static weather_t s_weather;

void weather_get(weather_t *w)
{
    if (!s_lock) {
        memset(w, 0, sizeof(*w));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *w = s_weather;
    xSemaphoreGive(s_lock);
}

const char *weather_text(int code)
{
    /* WMO weather interpretation codes, as Open-Meteo documents them */
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Cloudy";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm, hail";
    default: return "";
    }
}

static int num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (int)(v->valuedouble + (v->valuedouble < 0 ? -0.5 : 0.5)) : 0;
}

/* First element of a daily array */
static int first(const cJSON *obj, const char *key)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, key);
    const cJSON *v = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, 0) : NULL;
    return cJSON_IsNumber(v) ? (int)(v->valuedouble + (v->valuedouble < 0 ? -0.5 : 0.5)) : 0;
}

static bool parse(const char *body, weather_t *w)
{
    cJSON *root = cJSON_Parse(body);
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *day = cJSON_GetObjectItemCaseSensitive(root, "daily");
    bool ok = cJSON_IsObject(cur) && cJSON_IsObject(day);
    if (ok) {
        w->temp_f = num(cur, "temperature_2m");
        w->humidity = num(cur, "relative_humidity_2m");
        w->code = num(cur, "weather_code");
        w->high_f = first(day, "temperature_2m_max");
        w->low_f = first(day, "temperature_2m_min");
        w->rain_pct = first(day, "precipitation_probability_max");
    }
    cJSON_Delete(root);
    return ok;
}

static bool fetch(char *body)
{
#ifdef DASH_LATITUDE
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&temperature_unit=fahrenheit&timezone=auto&forecast_days=1",
             DASH_LATITUDE, DASH_LONGITUDE);
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int len = 0, n;
        while (len < BODY_MAX - 1 && (n = esp_http_client_read(c, body + len, BODY_MAX - 1 - len)) > 0) {
            len += n;
        }
        body[len] = 0;
        ok = status == 200 && len > 0;
        if (!ok) {
            ESP_LOGW(TAG, "HTTP %d, %d bytes", status, len);
        }
    } else {
        ESP_LOGW(TAG, "can't reach api.open-meteo.com");
    }
    esp_http_client_cleanup(c);
    return ok;
#else
    (void)body;
    return false;
#endif
}

static void task(void *arg)
{
    (void)arg;
    static char body[BODY_MAX];
    while (true) {
        int wait_s = WAIT_WIFI_S;
        if (wifi_connected()) {
            wait_s = RETRY_S;
            weather_t w = {0};
            if (fetch(body) && parse(body, &w)) {
                w.valid = true;
                w.fetched = time(NULL);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_weather = w;
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "%d F, %s, high %d low %d, rain %d%%", w.temp_f, weather_text(w.code), w.high_f,
                         w.low_f, w.rain_pct);
                wait_s = WEATHER_EVERY_S;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

void weather_start(void)
{
    if (!wifi_configured()) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(task, "weather", 8192, NULL, 3, NULL);
}

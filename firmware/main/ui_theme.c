/* Light/dark theme colors, mode saved in NVS ("ui"/"dark"). Light is the default. */
#include "ui_theme.h"

#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "ui_theme";

static const ui_theme_t s_light = {
    .bg = 0xffffff, .text = 0x111827, .text2 = 0x374151, .muted = 0x4b5563, .caption = 0x6b7280,
    .btn_bg = 0xf3f4f6, .btn_pressed = 0xe5e7eb, .btn_border = 0xd1d5db,
    .card = 0xf9fafb, .card_border = 0xe5e7eb,
    .track = 0xe5e7eb, .indicator = 0x6b7280, .knob = 0x111827,
    .ok = 0x15803d, .warn = 0xb45309, .error = 0xdc2626,
    .del_border = 0xfca5a5, .del_text = 0xb91c1c,
    .date = 0x1e3a8a, .time = 0x166534,
};

static const ui_theme_t s_dark = {
    .bg = 0x0b0c0f, .text = 0xe5e7eb, .text2 = 0xd1d5db, .muted = 0x9ca3af, .caption = 0x6b7280,
    .btn_bg = 0x171920, .btn_pressed = 0x2a2e38, .btn_border = 0x2e333d,
    .card = 0x101217, .card_border = 0x1c1f26,
    .track = 0x23262e, .indicator = 0x9ca3af, .knob = 0xe5e7eb,
    .ok = 0x4ade80, .warn = 0xfbbf24, .error = 0xf87171,
    .del_border = 0x7f1d1d, .del_text = 0xfca5a5,
    .date = 0x93c5fd, .time = 0x86efac,
};

static bool s_dark_mode;

void ui_theme_init(void)
{
    nvs_handle_t nvs;
    uint8_t v = 0;
    if (nvs_open("ui", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "dark", &v);
        nvs_close(nvs);
    }
    s_dark_mode = v;
    ESP_LOGI(TAG, "%s mode", s_dark_mode ? "dark" : "light");
}

const ui_theme_t *ui_theme(void)
{
    return s_dark_mode ? &s_dark : &s_light;
}

bool ui_theme_is_dark(void)
{
    return s_dark_mode;
}

void ui_theme_set_dark(bool dark)
{
    s_dark_mode = dark;
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "dark", dark);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "%s mode", dark ? "dark" : "light");
}

#pragma once

#include <stdbool.h>

/* Wi-Fi station from secrets.h (DASH_WIFI_SSID/PASSWORD); reconnects by itself. Without secrets.h
 * (or with an empty SSID) it stays off. */
void wifi_start(void);
bool wifi_configured(void);
bool wifi_connected(void);

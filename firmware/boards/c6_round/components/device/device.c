// Waveshare ESP32-C6-LCD-1.28: pins for the esp-matter device HAL.
// Source: waveshareteam/ESP32-C6-LCD-1.28 HARDWARE_REFERENCE.md.
//   No physical LED: esp_matter_device.cmake selects the no-op hollow_led driver.
//   GPIO9 = BOOT button, active low.
#include <driver/gpio.h>
#include <device.h>

led_driver_config_t led_driver_get_config()
{
    led_driver_config_t config = {
        .gpio = GPIO_NUM_NC,
        .channel = 0,
    };
    return config;
}

button_gpio_config_t button_driver_get_config()
{
    button_gpio_config_t config = {
        .gpio_num = GPIO_NUM_9,
        .active_level = 0,
    };
    return config;
}

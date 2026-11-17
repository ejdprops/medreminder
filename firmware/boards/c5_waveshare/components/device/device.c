// Waveshare ESP32-C5-Touch-LCD-2.8: pins for the esp-matter device HAL.
// Source: waveshareteam/ESP32-C5-Touch-LCD-2.8 BSP header.
//   No physical LED: esp_matter_device.cmake selects the no-op hollow_led driver, so the
//            LED pin below is unused (GPIO26 is I2S DOUT to the codec; never drive it from RMT).
//   GPIO27 = I2S DIN (codec -> MCU). Never drive this pin.
//   GPIO28 = BOOT button, active low.
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
        .gpio_num = GPIO_NUM_28,
        .active_level = 0,
    };
    return config;
}

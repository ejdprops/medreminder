// Waveshare ESP32-C5-Touch-LCD-2.8: pins for the esp-matter device HAL.
// Source: waveshareteam/ESP32-C5-Touch-LCD-2.8 BSP header.
//   GPIO26 = I2S DOUT (MCU -> codec). No physical LED on this board; it is a
//            safe placeholder for the light example's WS2812 output.
//   GPIO27 = I2S DIN (codec -> MCU). Never drive this pin.
//   GPIO28 = BOOT button, active low.
#include <driver/gpio.h>
#include <device.h>

led_driver_config_t led_driver_get_config()
{
    led_driver_config_t config = {
        .gpio = GPIO_NUM_26,
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

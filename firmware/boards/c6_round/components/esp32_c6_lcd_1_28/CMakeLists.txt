idf_component_register(
    SRCS "esp32_c6_lcd_1_28.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_ledc esp_lcd esp_lvgl_adapter
)

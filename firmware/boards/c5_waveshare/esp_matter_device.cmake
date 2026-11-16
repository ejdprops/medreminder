# Waveshare ESP32-C5-Touch-LCD-2.8
if(NOT ("${IDF_TARGET}" STREQUAL "esp32c5"))
    message(FATAL_ERROR "Board c5_waveshare requires IDF_TARGET esp32c5")
endif()
set(device_type c5_waveshare)
set(led_type    ws2812)
set(button_type iot)

# Waveshare ESP32-C6-LCD-1.28 (round, no touch)
if(NOT ("${IDF_TARGET}" STREQUAL "esp32c6"))
    message(FATAL_ERROR "Board c6_round requires IDF_TARGET esp32c6")
endif()
set(device_type c6_round)
set(led_type    hollow_led)  # no LED on this board
set(button_type iot)

# Seeed XIAO ESP32-C6, headless node for the dashboard panel
if(NOT ("${IDF_TARGET}" STREQUAL "esp32c6"))
    message(FATAL_ERROR "Board xiao_c6_dash requires IDF_TARGET esp32c6")
endif()
set(device_type xiao_c6_dash)
set(led_type    hollow_led)  # the user LED (GPIO15) is left alone
set(button_type iot)

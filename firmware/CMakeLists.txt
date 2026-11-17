# medreminder firmware: ESP-IDF + esp-matter (Matter over Thread)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH is not set. Run getmatter first.")
endif()

# Board selection: idf.py -DBOARD=<name>, or BOARD env var. Default c5_waveshare.
if(NOT DEFINED BOARD)
    if(DEFINED ENV{BOARD})
        set(BOARD $ENV{BOARD})
    else()
        set(BOARD c5_waveshare)
    endif()
endif()
set(BOARD ${BOARD} CACHE STRING "Board folder name under boards/")
set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD})
if(NOT EXISTS ${BOARD_DIR}/esp_matter_device.cmake)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}': missing ${BOARD_DIR}/esp_matter_device.cmake")
endif()
message(STATUS "medreminder BOARD: ${BOARD}")
# esp-matter's led_driver and iot_button components read LED/button types from this path
set(ENV{ESP_MATTER_DEVICE_PATH} ${BOARD_DIR})

# Firmware version: bump both together for a release, then tag fw-v<PROJECT_VER> (CLAUDE.md).
# PROJECT_VER is Matter's SoftwareVersionString; PROJECT_VER_NUMBER is SoftwareVersion and must
# increase with every release (OTA compares it).
set(PROJECT_VER "0.4.0")
set(PROJECT_VER_NUMBER 4)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include(${BOARD_DIR}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/led_driver"
    "${ESP_MATTER_PATH}/device_hal/button_driver/iot_button"
    "${BOARD_DIR}/components")

project(medreminder)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

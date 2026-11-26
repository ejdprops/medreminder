# Shared with the node firmware (same layouts and look on both): doses.h, uart_link_proto.h, the dose
# palette (dose_colors.c) and the Inter fonts
set(FW ${CMAKE_CURRENT_LIST_DIR}/../../firmware/main)
file(GLOB FONTS ${FW}/fonts/*.c)
file(GLOB ICONS ${CMAKE_CURRENT_LIST_DIR}/icons/*.c ${CMAKE_CURRENT_LIST_DIR}/icons/mode/*.c)
idf_component_register(SRCS "main.c" "audio.c" "lv_mem_psram.c" "board.c" "link.c" "ui.c" "wifi.c" "weather.c" "calendar.c" "images/img_averak_logo_large.c" "images/img_averak_logo_corner.c" ${ICONS} "${FW}/dose_colors.c" ${FONTS}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${FW}" "..")  # .. holds secrets.h (git-ignored)
# lv_mem_psram.c is only referenced from LVGL: keep the linker from dropping it
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

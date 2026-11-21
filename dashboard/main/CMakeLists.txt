# Shared with the node firmware (same layouts and look on both): doses.h, uart_link_proto.h, the dose
# palette (dose_colors.c) and the Inter fonts
set(FW ${CMAKE_CURRENT_LIST_DIR}/../../firmware/main)
file(GLOB FONTS ${FW}/fonts/*.c)
idf_component_register(SRCS "main.c" "board.c" "link.c" "ui.c" "${FW}/dose_colors.c" ${FONTS}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${FW}")

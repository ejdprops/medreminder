# doses.h and uart_link_proto.h come from the node firmware (same layouts on both ends)
idf_component_register(SRCS "main.c" "board.c" "link.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../firmware/main")

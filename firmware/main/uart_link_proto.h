/* Single-wire UART link between a dashboard node (uart_link.c) and the dashboard panel
 * (dashboard/ in this repo): frame layout, shared by both ends. See uart_link.c for the protocol. */
#pragma once

#include <stdint.h>

#define LINK_BAUD 115200
#define LINK_SOF 0xA5
#define LINK_HDR_LEN 4 /* SOF, type, length u16 LE */
#define LINK_CRC_LEN 4 /* esp_rom_crc32_le over type, length and payload, LE */

enum { LINK_MSG_POLL = 0x01, LINK_MSG_TAKE = 0x02, LINK_MSG_REPLY = 0x81 };
enum { LINK_FLAG_MAIN_CONNECTED = 0x01 };

/* REPLY payload; the doses_snapshot_t (up to defs[count]) follows when the panel's CRC is old */
typedef struct __attribute__((packed)) {
    uint32_t time;  /* UTC seconds; 0 until the clock is set */
    uint8_t flags;  /* LINK_FLAG_* */
    uint8_t reserved[3];
    uint32_t crc;   /* of the doses state the node holds */
} link_status_t;

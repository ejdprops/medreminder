#pragma once

/* App link wire format, shared with the Med Dot app (apple/Shared/AppLink.swift) and
 * tools/app_link_client.py. Little-endian, packed.
 *
 * Every message: 'M' 'A' version type.
 *
 * Pairing (once per app install). The app keeps sending PAIR (every ~1 s) until it gets PAIR_OK or
 * PAIR_NO; the main unit answers each one with where it stands:
 *   PAIR      app -> unit  app_pub[65] (P-256, uncompressed), name_len u8, name (UTF-8, <= 32)
 *   PAIR_WAIT unit -> app  unit_pub[65]: both now derive the key and the 6-digit code; the unit shows
 *                          "Pair <name>? <code>" and waits for a tap
 *   PAIR_OK   unit -> app  unit_pub[65], client u8, mac[16] (HMAC of everything before, new key)
 *   PAIR_NO   unit -> app  unit_pub[65] (cancelled, timed out, or another pairing is under way:
 *                          unit_pub all zero then)
 *   key/code: shared = ECDH x coordinate; HKDF-SHA256(ikm shared, salt "meddot-pair-v1",
 *   info app_pub || unit_pub) -> 36 bytes: key = [0..32), code = u32 LE [32..36) % 1000000.
 *
 * Requests, signed with the key:
 *   REQ   app -> unit  client u8, counter u32, op u8, body..., mac[16]
 *   RESP  unit -> app  client u8, counter u32 (echoed), op u8, status u8, body..., mac[16]
 *   mac = HMAC-SHA256(key, everything before it) truncated to 16 bytes. The counter must grow
 *   (the unit keeps the last one per client; replays and bad MACs are dropped without an answer).
 *   An unknown client gets an unsigned RESP with status APP_ST_UNKNOWN (the app offers to pair again).
 */
#include <stdint.h>

#define APP_LINK_PORT 49232
#define APP_LINK_SERVICE "_meddot"   /* DNS-SD _meddot._udp, via the border routers (SRP) */
#define APP_LINK_VERSION 1
#define APP_PUB_LEN 65
#define APP_MAC_LEN 16
#define APP_NAME_MAX 32

enum {
    APP_MSG_PAIR = 1,
    APP_MSG_PAIR_WAIT = 2,
    APP_MSG_PAIR_OK = 3,
    APP_MSG_PAIR_NO = 4,
    APP_MSG_REQ = 5,
    APP_MSG_RESP = 6,
};

enum {
    APP_OP_GET = 1,    /* body: none -> app_settings_t */
    APP_OP_SET = 2,    /* body: app_settings_t (fields in mask) -> app_settings_t */
    APP_OP_FORGET = 3, /* the app unpairs itself -> no body */
    APP_OP_GET_DOSES = 4,   /* body: none -> doses list */
    APP_OP_SET_DOSE = 5,    /* body: app_dose_t (uid 0 = add) -> doses list, or REFUSED + message */
    APP_OP_DELETE_DOSE = 6, /* body: uid u8 -> doses list */
};

enum {
    APP_ST_OK = 0,
    APP_ST_UNKNOWN = 1, /* client not paired (forgotten, or the unit was reset) */
    APP_ST_BAD_VALUE = 2,
    APP_ST_BAD_OP = 3,
    APP_ST_REFUSED = 4, /* body: the reason, ASCII (the C5 editor's own messages, or "being edited on the C5") */
};

/* Shared settings: only what every device shares. Dark mode, brightness, volume and screen off stay
 * local to each device (the unit refuses a SET with other mask bits). */
#define APP_SET_OVERDUE 0x01

typedef struct __attribute__((packed)) {
    uint8_t version; /* 1 */
    uint8_t mask;    /* SET: which fields to change; GET reply: 0 */
    uint8_t overdue; /* minutes: 5 10 15 20 30 45 60 90 120 */
    uint8_t reserved;
} app_settings_t;

/* Dose schedule, as the C5 editor sees it. Doses list body: count u8, then count app_dose_t sorted by
 * time. SET_DOSE: uid of the dose to change (0 = add); the period follows the time; the unit checks
 * the rest as its editor does (name, one time per person, 16 doses max). */
typedef struct __attribute__((packed)) {
    uint8_t uid;
    uint16_t minute; /* of the day, multiple of 5 */
    uint8_t days;    /* 0x7f daily, or one weekday bit (bit 0 = Sunday) */
    uint8_t color;   /* palette index */
    char name[16];   /* NUL-padded, ASCII */
} app_dose_t;

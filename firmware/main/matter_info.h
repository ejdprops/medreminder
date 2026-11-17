#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matter facts for the UI. Safe to call from any task (takes the CHIP stack lock). */
bool matter_get_qr_code(char *buf, size_t len);     /* "MT:..." */
bool matter_get_manual_code(char *buf, size_t len); /* 11 digits, no dashes */
/* Number of commissioned fabrics; fills up to max vendor IDs (0x1349 Apple Home, 0x1384 Apple Keychain). */
int matter_fabric_vendors(uint16_t *vendor_ids, int max);

#ifdef __cplusplus
}
#endif

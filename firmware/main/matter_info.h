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

/* Pairing progress, from Matter's events (app_main): a commissioner (Home) is setting the device up;
 * and how many times commissioning has completed since boot (changes when one finishes). No lock. */
bool matter_pairing_active(void);
unsigned matter_pairing_completions(void);

/* Basic Information as Apple Home shows it (Manufacturer = vendor_name, Model = product_name) */
typedef struct {
    char vendor_name[33];
    char product_name[33];
    uint16_t vendor_id;
    uint16_t product_id;
} matter_device_info_t;
bool matter_get_device_info(matter_device_info_t *info);

#ifdef __cplusplus
}

#include <platform/CHIPDeviceEvent.h>
/* app_main's Matter event callback passes every event here */
void matter_info_on_event(const chip::DeviceLayer::ChipDeviceEvent *event);
#endif

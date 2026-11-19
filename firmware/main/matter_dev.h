#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "doses.h"

#ifdef __cplusplus
#include <esp_matter.h>

/* Create the bridge's aggregator endpoint (before esp_matter::start) */
void matter_dev_create(esp_matter::node_t *node);

/* After esp_matter::start: restore the bridged endpoints and start following matter_dev_publish */
void matter_dev_start(void);

/* From the app's attribute callback (CHIP task, PRE_UPDATE): a write from Home to a dose's On/Off
 * becomes a take/undo request for the UI task. Always ESP_OK (the UI task corrects refused
 * writes). */
esp_err_t matter_dev_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                      esp_matter_attr_val_t *val, void *priv_data);

extern "C" {
#endif

#define MATTER_DOSE_LABEL_MAX 32
#define MATTER_DOSE_SERIAL_MAX 31

/* One dose as Home sees it: a bridged On/Off "outlet", on = taken today */
typedef struct {
    uint8_t uid; /* dose_def_t.uid: the dose's endpoint follows it through edits and re-sorting */
    bool on;
    char label[MATTER_DOSE_LABEL_MAX + 1];
    char serial[MATTER_DOSE_SERIAL_MAX + 1]; /* "K-0900", weekly "K-0900-TUE" */
} matter_dose_t;

/* From the UI task (doses.c): the doses Home should show and whether any is overdue. Cheap when
 * nothing changed; endpoint changes happen on the matter_dev task. */
void matter_dev_publish(const matter_dose_t *doses, int count, bool overdue);

/* From the UI task: next take (on) / undo (off) request from Home; false when there is none */
bool matter_dev_next_request(uint8_t *uid, bool *on);

/* Identify from Home (any endpoint): play the alert chime */
void matter_dev_identify(uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif

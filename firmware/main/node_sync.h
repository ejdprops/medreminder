#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "doses.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Node sync over the Thread mesh (UDP; see node_sync.c). Start after esp_matter::start. */
void node_sync_start(void);

/* UI task, every tick: what this device holds now. The main unit sends it to nodes; a node reports
 * which state it has (its CRC) back to the main unit. Cheap when nothing changed. */
void node_sync_publish(const doses_snapshot_t *snap);

/* Node, UI task: true and a copy of the main unit's state when a new one arrived */
bool node_sync_take_state(doses_snapshot_t *snap);

/* Main unit: nodes heard from recently (known) and how many of them hold the current state */
void node_sync_status(int *synced, int *known);

/* Node, UI task: ask the main unit to take the dose with this uid (retried until its new state
 * arrives). Main unit, UI task: the next take request from a node, if any. */
void node_sync_request_take(uint8_t uid);
bool node_sync_next_take(uint8_t *uid);

/* Node: the main unit was heard from recently */
bool node_sync_connected(void);

#ifdef __cplusplus
}
#endif

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

/* What a node can ask the main unit for */
typedef enum { NODE_REQ_TAKE, NODE_REQ_UNDO, NODE_REQ_SNOOZE } node_req_t;

/* Node: ask the main unit to take / undo the dose with this uid, or snooze the overdue chime (uid
 * unused); retried until the main unit's new state arrives. Main unit, UI task: the next request
 * from a node, if any. */
void node_sync_request(uint8_t uid, node_req_t req);
bool node_sync_next_request(uint8_t *uid, node_req_t *req);

/* Node: the main unit was heard from recently */
bool node_sync_connected(void);

#ifdef __cplusplus
}
#endif

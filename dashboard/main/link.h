#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "doses.h"
#include "uart_link_proto.h"

/* The node's side as last heard over the one-wire link (link.c) */
typedef struct {
    bool up;                 /* a reply within the last second */
    link_status_t status;    /* from the last reply */
    bool have_doses;
    doses_snapshot_t doses;  /* the node's dose state (the C5's, via Thread) */
    uint32_t ok, failed;     /* polls */
} link_view_t;

void link_start(void);
/* A copy of the latest; true if the doses changed since the last call */
bool link_get(link_view_t *v);
/* Take (take false: undo) the dose with this uid (the node sends it on to the C5) */
void link_take(uint8_t uid, bool take);
/* Snooze the overdue chime (the node sends it on to the C5; the new state shows it) */
void link_snooze(void);

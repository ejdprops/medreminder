#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the local timezone and start SNTP (over Thread via NAT64). Call after Matter/Thread start. */
void time_sync_start(void);
/* True once the clock holds a real date (SNTP synced at least once since boot). */
bool time_is_valid(void);

#ifdef __cplusplus
}
#endif

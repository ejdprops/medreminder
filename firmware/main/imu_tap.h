#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Double tap on the case (QMI8658 tap engine, see imu_tap.c). Nodes only; a no-op elsewhere.
 * Start from the BSP init task (after the display: BSP init isn't thread-safe). */
void imu_tap_start(void);

/* UI task: true once per double tap since the last call */
bool imu_tap_take_double(void);

#ifdef __cplusplus
}
#endif

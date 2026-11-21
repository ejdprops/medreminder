#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shake the node (QMI8658 accelerometer, see imu_shake.c). Nodes only; a no-op elsewhere.
 * Start from the BSP init task (after the display: BSP init isn't thread-safe). */
void imu_shake_start(void);

/* UI task: true once per shake since the last call */
bool imu_shake_take(void);

#ifdef __cplusplus
}
#endif

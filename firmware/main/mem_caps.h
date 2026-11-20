#pragma once

#include <esp_heap_caps.h>
#include <sdkconfig.h>

/* Where task stacks and large buffers go: PSRAM when the board has it (internal RAM is kept for
 * Matter, DMA and ISRs), else internal RAM (C6 boards have no PSRAM). */
#if CONFIG_SPIRAM
#define MEM_CAPS_BULK (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define MEM_CAPS_BULK (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

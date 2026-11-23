/* LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC): PSRAM first. With plain malloc, every LVGL
 * object went to internal RAM (malloc keeps blocks under SPIRAM_MALLOC_ALWAYSINTERNAL there); two
 * screens with a calendar each left Wi-Fi and TLS without memory and the PHY aborted. Sending all
 * malloc to PSRAM instead is not an option: the LVGL adapter's panel context must be internal. */
#include <esp_heap_caps.h>
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, CAPS_PSRAM, MALLOC_CAP_DEFAULT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, CAPS_PSRAM, MALLOC_CAP_DEFAULT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif

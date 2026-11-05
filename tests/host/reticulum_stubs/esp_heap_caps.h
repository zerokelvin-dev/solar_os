#pragma once

/* tests/host/esp_heap_caps.h plus the heap statistics microReticulum's
 * Memory overlay reports. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM UINT32_C(1)
#define MALLOC_CAP_8BIT UINT32_C(2)
#define MALLOC_CAP_INTERNAL UINT32_C(4)
#define MALLOC_CAP_DEFAULT UINT32_C(8)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

static inline size_t heap_caps_get_total_size(uint32_t caps)
{
    (void)caps;
    return 512U * 1024U;
}

static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 256U * 1024U;
}

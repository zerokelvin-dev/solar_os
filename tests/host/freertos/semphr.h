#pragma once

#include "FreeRTOS.h"

typedef int StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

#define portMAX_DELAY 0xffffffffU

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *storage)
{
    return storage;
}

/* Drivers that allocate their own mutex get one shared sentinel here: the
 * host tests are single-threaded, so the handle only has to be non-NULL. */
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t storage;
    return &storage;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}

static inline int xSemaphoreTake(SemaphoreHandle_t semaphore,
                                 unsigned timeout)
{
    (void)semaphore;
    (void)timeout;
    return 1;
}

static inline int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return 1;
}

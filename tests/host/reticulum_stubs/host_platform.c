/* The POSIX-thread FreeRTOS of reticulum_stubs/freertos, the SolarOS task
 * calls the Reticulum job makes, and strlcpy. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_platform.h"
#include "solar_os_task.h"

struct host_task {
    pthread_t thread;
    TaskFunction_t function;
    void *parameters;
};

static __thread struct host_task *current_task;
static pthread_mutex_t deletes_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned deletes_of_other_tasks;

static uint64_t host_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

TickType_t xTaskGetTickCount(void)
{
    static uint64_t start;
    if (start == 0U) {
        start = host_us();
    }
    return (TickType_t)((host_us() - start) / 100U);
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec wait = {
        .tv_sec = (time_t)(ticks / 10000U),
        .tv_nsec = (long)(ticks % 10000U) * 100000L,
    };
    if (ticks == 0U) {
        wait.tv_nsec = 1000L;
    }
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {
    }
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096U;
}

static SemaphoreHandle_t semaphore_init(StaticSemaphore_t *storage, int count)
{
    pthread_mutex_init(&storage->mutex, NULL);
    pthread_cond_init(&storage->cond, NULL);
    storage->count = count;
    return storage;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return semaphore_init(storage, 0);
}

/* Not recursive, as a FreeRTOS mutex is not; priority inheritance is moot. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return semaphore_init(storage, 1);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return semaphore_init(malloc(sizeof(StaticSemaphore_t)), 1);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    pthread_mutex_lock(&semaphore->mutex);
    const TickType_t start = xTaskGetTickCount();
    while (semaphore->count == 0) {
        if (ticks != portMAX_DELAY && xTaskGetTickCount() - start >= ticks) {
            pthread_mutex_unlock(&semaphore->mutex);
            return pdFALSE;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&semaphore->cond, &semaphore->mutex, &until);
    }
    semaphore->count--;
    pthread_mutex_unlock(&semaphore->mutex);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    pthread_mutex_lock(&semaphore->mutex);
    semaphore->count = 1;
    pthread_cond_signal(&semaphore->cond);
    pthread_mutex_unlock(&semaphore->mutex);
    return pdTRUE;
}

static void *task_entry(void *arg)
{
    current_task = arg;
    current_task->function(current_task->parameters);
    return NULL;
}

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)core_id;
    (void)role;
    struct host_task *created = calloc(1, sizeof(*created));
    created->function = task;
    created->parameters = parameters;
    *handle = created;
    pthread_create(&created->thread, NULL, task_entry, created);
    pthread_detach(created->thread);
    return pdPASS;
}

/* A task can end itself. Deleting another is counted, not done: a thread
 * cannot be killed safely, and the job must never need it. */
void solar_os_task_delete_internal(TaskHandle_t task)
{
    if (task == NULL || task == current_task) {
        struct host_task *self = current_task;
        current_task = NULL;
        free(self);
        pthread_exit(NULL);
    }
    pthread_mutex_lock(&deletes_lock);
    deletes_of_other_tasks++;
    pthread_mutex_unlock(&deletes_lock);
}

unsigned host_task_deletes_of_other_tasks(void)
{
    pthread_mutex_lock(&deletes_lock);
    const unsigned count = deletes_of_other_tasks;
    pthread_mutex_unlock(&deletes_lock);
    return count;
}

bool host_task_is_current(void)
{
    return current_task != NULL;
}

/* As solar_os_task.c: poll until done or out of time. */
bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms)
{
    if (task == NULL) {
        return true;
    }
    const TickType_t start = xTaskGetTickCount();
    while (task_done == NULL || !*task_done) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return true;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

#include "solar_os_reticulum_job.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_reticulum.h"
#include "solar_os_task.h"

#define RETICULUM_LOOP_DELAY_MS 5U
#define RETICULUM_STOP_WAIT_MS 4000U

static const char *TAG = "reticulum";

typedef struct {
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
} reticulum_job_state_t;

static reticulum_job_state_t reticulum_job;

static bool parse_args(int argc,
                       char **argv,
                       const char **host,
                       uint16_t *port)
{
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_reticulum_job.name) == 0) {
        first = 1;
    }
    const int count = argc - first;
    if (argv == NULL || count < 1 || count > 2) {
        return false;
    }
    *host = argv[first];
    *port = SOLAR_OS_RETICULUM_DEFAULT_PORT;
    if (count == 2) {
        char *end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(argv[first + 1], &end, 10);
        if (errno != 0 || end == argv[first + 1] || *end != '\0' ||
            parsed == 0UL || parsed > UINT16_MAX) {
            return false;
        }
        *port = (uint16_t)parsed;
    }
    return true;
}

static void reticulum_task(void *arg)
{
    (void)arg;
    while (!reticulum_job.stop_requested) {
        solar_os_reticulum_loop_once();
        solar_os_reticulum_note_stack_watermark(
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
            sizeof(StackType_t));
        vTaskDelay(pdMS_TO_TICKS(RETICULUM_LOOP_DELAY_MS));
    }
    reticulum_job.task_done = true;
    reticulum_job.task = NULL;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t reticulum_start(solar_os_context_t *ctx,
                                 int argc,
                                 char **argv)
{
    (void)ctx;
    const char *host = NULL;
    uint16_t port = 0;
    if (!parse_args(argc, argv, &host, &port)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_reticulum_start(host, port);
    if (error != ESP_OK) {
        return error;
    }

    memset(&reticulum_job, 0, sizeof(reticulum_job));
    if (solar_os_task_create_pinned_internal(
            reticulum_task,
            "reticulum",
            SOLAR_OS_RETICULUM_WORKER_STACK,
            NULL,
            tskIDLE_PRIORITY + 2,
            &reticulum_job.task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        (void)solar_os_reticulum_stop();
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource(
        solar_os_reticulum_job.name,
        SOLAR_OS_JOB_RESOURCE_CUSTOM,
        host,
        "Reticulum TCP server");
    SOLAR_OS_LOGI(TAG, "job started");
    return ESP_OK;
}

static void reticulum_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    reticulum_job.stop_requested = true;
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(RETICULUM_STOP_WAIT_MS);
    while (!reticulum_job.task_done && reticulum_job.task != NULL &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(1);
    }
    if (reticulum_job.task != NULL) {
        solar_os_task_delete_internal(reticulum_job.task);
        reticulum_job.task = NULL;
    }
    (void)solar_os_reticulum_stop();
    SOLAR_OS_LOGI(TAG, "job stopped");
}

const solar_os_job_t solar_os_reticulum_job = {
    .name = "reticulum",
    .summary = "Reticulum network stack over TCP",
    .start = reticulum_start,
    .stop = reticulum_stop,
    .worker_stack_bytes = SOLAR_OS_RETICULUM_WORKER_STACK,
};

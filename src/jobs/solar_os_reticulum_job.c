#include "solar_os_reticulum_job.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
    /* The start the worker runs, and its result, while the caller waits. */
    const solar_os_reticulum_config_t *config;
    esp_err_t start_result;
} reticulum_job_state_t;

static reticulum_job_state_t reticulum_job;
static StaticSemaphore_t started_storage;
static SemaphoreHandle_t started;

/*
 * job start reticulum <host> [port]
 * job start reticulum lora <radio> <profile>
 * job start reticulum <host> [port] lora <radio> <profile>
 */
static bool parse_args(int argc,
                       char **argv,
                       solar_os_reticulum_config_t *config)
{
    int index = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_reticulum_job.name) == 0) {
        index = 1;
    }
    if (argv == NULL || argc - index < 1) {
        return false;
    }
    memset(config, 0, sizeof(*config));
    if (strcmp(argv[index], "lora") != 0) {
        config->host = argv[index++];
        config->port = SOLAR_OS_RETICULUM_DEFAULT_PORT;
        if (index < argc && strcmp(argv[index], "lora") != 0) {
            char *end = NULL;
            errno = 0;
            const unsigned long parsed = strtoul(argv[index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                parsed == 0UL || parsed > UINT16_MAX) {
                return false;
            }
            config->port = (uint16_t)parsed;
            index++;
        }
    }
    if (index < argc) {
        if (strcmp(argv[index], "lora") != 0 || argc - index != 3) {
            return false;
        }
        config->radio = argv[index + 1];
        config->profile = argv[index + 2];
        index += 3;
    }
    return index == argc;
}

/*
 * The worker starts the service, runs it, and stops it, all on its own
 * stack: starting and stopping need more than a small caller (a scheduled
 * script) has spare.
 */
static void reticulum_task(void *arg)
{
    (void)arg;
    reticulum_job.start_result = solar_os_reticulum_start(reticulum_job.config);
    const bool running = reticulum_job.start_result == ESP_OK;
    reticulum_job.config = NULL; /* the caller's, and it stops waiting now */
    xSemaphoreGive(started);
    while (running && !reticulum_job.stop_requested) {
        solar_os_reticulum_loop_once();
        solar_os_reticulum_note_stack_watermark(
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
            sizeof(StackType_t));
        vTaskDelay(pdMS_TO_TICKS(RETICULUM_LOOP_DELAY_MS));
    }
    (void)solar_os_reticulum_stop();
    reticulum_job.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t reticulum_start(solar_os_context_t *ctx,
                                 int argc,
                                 char **argv)
{
    (void)ctx;
    solar_os_reticulum_config_t config;
    if (!parse_args(argc, argv, &config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reticulum_job.task != NULL && !reticulum_job.task_done) {
        /* A worker from the last run has not finished yet. */
        return ESP_ERR_INVALID_STATE;
    }
    if (started == NULL) {
        started = xSemaphoreCreateBinaryStatic(&started_storage);
    }

    memset(&reticulum_job, 0, sizeof(reticulum_job));
    reticulum_job.config = &config;
    if (solar_os_task_create_pinned_internal(
            reticulum_task,
            "reticulum",
            SOLAR_OS_RETICULUM_WORKER_STACK,
            NULL,
            tskIDLE_PRIORITY + 2,
            &reticulum_job.task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        reticulum_job.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Not bounded: the start itself is (it never waits on the network), and
     * config lives on this stack until the worker is done with it. */
    (void)xSemaphoreTake(started, portMAX_DELAY);
    const esp_err_t error = reticulum_job.start_result;
    if (error != ESP_OK) {
        (void)solar_os_task_wait_done(reticulum_job.task,
                                      &reticulum_job.task_done,
                                      RETICULUM_STOP_WAIT_MS);
        reticulum_job.task = NULL;
        return error;
    }
    if (config.host != NULL) {
        (void)solar_os_jobs_note_resource(
            solar_os_reticulum_job.name,
            SOLAR_OS_JOB_RESOURCE_CUSTOM,
            config.host,
            "Reticulum TCP server");
    }
    if (config.radio != NULL) {
        (void)solar_os_jobs_note_resource(
            solar_os_reticulum_job.name,
            SOLAR_OS_JOB_RESOURCE_CUSTOM,
            config.radio,
            "Reticulum packet radio");
    }
    SOLAR_OS_LOGI(TAG, "job started");
    return ESP_OK;
}

static void reticulum_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    reticulum_job.stop_requested = true;
    /* The worker stops the service itself once its current loop pass ends (a
     * DNS lookup or a LoRa send can take seconds), so it is never deleted
     * from here. */
    if (!solar_os_task_wait_done(reticulum_job.task,
                                 &reticulum_job.task_done,
                                 RETICULUM_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms; "
                      "it stops after its current pass",
                      (unsigned)RETICULUM_STOP_WAIT_MS);
        return;
    }
    reticulum_job.task = NULL;
    SOLAR_OS_LOGI(TAG, "job stopped");
}

static void reticulum_error_detail(char *buffer, size_t buffer_len)
{
    solar_os_reticulum_start_error(buffer, buffer_len);
}

const solar_os_job_t solar_os_reticulum_job = {
    .name = "reticulum",
    .summary = "Reticulum network stack over TCP and LoRa",
    .start = reticulum_start,
    .stop = reticulum_stop,
    .error_detail = reticulum_error_detail,
    .worker_stack_bytes = SOLAR_OS_RETICULUM_WORKER_STACK,
};

/*
 * The Reticulum job against a scripted service, on real threads: a start
 * that fails and is retried, and a stop while the worker is inside a slow
 * pass (a DNS lookup or a LoRa send, holding the service's lock).
 */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "../../src/jobs/solar_os_reticulum_job.c"
#include "reticulum_stubs/host_platform.h"

/* The service's own lock, as solar_os_reticulum.cpp holds it for a pass. */
static pthread_mutex_t service_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int service_running;
static atomic_int starts, stops, passes;
/* Stops or starts made by anything other than the worker. */
static atomic_int calls_off_worker;
static esp_err_t next_start_result;
/* While set, a pass holds the lock and waits for it to clear. */
static atomic_int hold_pass;
static atomic_int pass_holding;


esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag,
                             const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
    return ESP_OK;
}

esp_err_t solar_os_jobs_note_resource(const char *name,
                                      solar_os_job_resource_type_t type,
                                      const char *resource, const char *detail)
{
    (void)name;
    (void)type;
    (void)resource;
    (void)detail;
    return ESP_OK;
}

esp_err_t solar_os_reticulum_start(const solar_os_reticulum_config_t *config)
{
    assert(config != NULL && config->radio != NULL);
    if (!host_task_is_current()) {
        calls_off_worker++;
    }
    pthread_mutex_lock(&service_lock);
    starts++;
    const esp_err_t result = next_start_result;
    if (result == ESP_OK) {
        service_running = 1;
    }
    pthread_mutex_unlock(&service_lock);
    return result;
}

esp_err_t solar_os_reticulum_stop(void)
{
    if (!host_task_is_current()) {
        /* It would wait on a pass holding the lock: fail, don't hang. */
        calls_off_worker++;
        return ESP_ERR_INVALID_STATE;
    }
    pthread_mutex_lock(&service_lock);
    if (service_running) {
        stops++;
    }
    service_running = 0;
    pthread_mutex_unlock(&service_lock);
    return ESP_OK;
}

void solar_os_reticulum_loop_once(void)
{
    pthread_mutex_lock(&service_lock);
    assert(service_running);
    passes++;
    while (hold_pass) {
        pass_holding = 1;
        vTaskDelay(1);
    }
    pass_holding = 0;
    pthread_mutex_unlock(&service_lock);
}

void solar_os_reticulum_note_stack_watermark(uint32_t bytes) { (void)bytes; }
void solar_os_reticulum_start_error(char *buffer, size_t buffer_len)
{
    strlcpy(buffer, "radio radio0 is in use by meshtastic", buffer_len);
}

static char *lora_args[] = {"reticulum", "lora", "radio0", "rns-sf10-cr8"};

static esp_err_t job_start(void)
{
    return solar_os_reticulum_job.start(NULL, 4, lora_args);
}

static void wait_for(atomic_int *value, int at_least)
{
    for (int tries = 0; *value < at_least; tries++) {
        assert(tries < 100000);
        vTaskDelay(1);
    }
}

static void reset(void)
{
    starts = stops = passes = calls_off_worker = 0;
    next_start_result = ESP_OK;
    hold_pass = 0;
}

/* A refused start leaves no worker behind, and the next start runs. */
static void test_failed_start_is_retried(void)
{
    reset();
    next_start_result = ESP_ERR_INVALID_STATE;
    assert(job_start() == ESP_ERR_INVALID_STATE);
    assert(starts == 1 && passes == 0 && !service_running);
    assert(reticulum_job.task == NULL);

    next_start_result = ESP_OK;
    assert(job_start() == ESP_OK);
    wait_for(&passes, 2);
    assert(starts == 2 && service_running);

    solar_os_reticulum_job.stop(NULL);
    assert(!service_running && stops == 1);
    assert(calls_off_worker == 0);
    assert(host_task_deletes_of_other_tasks() == 0U);
}

/* A stop during a pass that outlasts the stop's wait: the worker is never
 * deleted under the lock, nothing else touches the service, a start in the
 * meantime is refused, and the worker stops the service once the pass
 * ends. */
static void test_stop_during_slow_pass(void)
{
    reset();
    assert(job_start() == ESP_OK);
    hold_pass = 1;
    wait_for(&pass_holding, 1);

    solar_os_reticulum_job.stop(NULL); /* waits RETICULUM_STOP_WAIT_MS */
    assert(host_task_deletes_of_other_tasks() == 0U);
    assert(calls_off_worker == 0);
    assert(service_running && pass_holding);
    assert(job_start() == ESP_ERR_INVALID_STATE);
    assert(starts == 1);

    hold_pass = 0;
    for (int tries = 0; service_running; tries++) {
        assert(tries < 100000);
        vTaskDelay(1);
    }
    assert(stops == 1);
    for (int tries = 0; !reticulum_job.task_done; tries++) {
        assert(tries < 100000);
        vTaskDelay(1);
    }

    assert(job_start() == ESP_OK);
    const int before = passes;
    wait_for(&passes, before + 1);
    solar_os_reticulum_job.stop(NULL);
    assert(stops == 2 && !service_running);
    assert(calls_off_worker == 0);
    assert(host_task_deletes_of_other_tasks() == 0U);
}

int main(void)
{
    test_failed_start_is_retried();
    test_stop_during_slow_pass();
    puts("reticulum job tests passed");
    return 0;
}

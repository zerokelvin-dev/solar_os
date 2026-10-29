#include "solar_os_lxmf.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_saved_config.h"

static SemaphoreHandle_t settings_lock;
static StaticSemaphore_t settings_lock_storage;
static solar_os_saved_config_t settings_file;
static solar_os_lxmf_settings_t settings;
static bool settings_loaded;

static bool settings_valid(const void *data, size_t length)
{
    const solar_os_lxmf_settings_t *candidate = data;
    char canonical[SOLAR_OS_LXMF_HASH_HEX_LEN];
    return length == sizeof(*candidate) && candidate->version == 1U &&
           candidate->sync_interval_min <= SOLAR_OS_LXMF_SYNC_INTERVAL_MAX &&
           (candidate->propagation_node[0] == '\0' ||
            (solar_os_lxmf_normalize_hash(candidate->propagation_node, canonical,
                                          sizeof(canonical)) &&
             strcmp(canonical, candidate->propagation_node) == 0));
}

/* Callers hold settings_lock. */
static void load_locked(void)
{
    if (settings_loaded) {
        return;
    }
    solar_os_lxmf_settings_t loaded = {0};
    size_t length = 0U;
    if (solar_os_saved_config_load(&settings_file, "lxmf", "lxmf", "settings", &loaded,
                                   sizeof(loaded), &length, settings_valid) == ESP_OK) {
        settings = loaded;
    } else {
        settings = (solar_os_lxmf_settings_t){
            .version = 1U,
            .sync_interval_min = SOLAR_OS_LXMF_SYNC_INTERVAL_DEFAULT,
        };
    }
    settings_loaded = true;
}

static void lock_settings(void)
{
    if (settings_lock == NULL) {
        settings_lock = xSemaphoreCreateMutexStatic(&settings_lock_storage);
    }
    xSemaphoreTake(settings_lock, portMAX_DELAY);
}

void solar_os_lxmf_settings_get(solar_os_lxmf_settings_t *out)
{
    lock_settings();
    load_locked();
    *out = settings;
    xSemaphoreGive(settings_lock);
}

esp_err_t solar_os_lxmf_settings_set(const solar_os_lxmf_settings_t *next)
{
    if (next == NULL || !settings_valid(next, sizeof(*next))) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_settings();
    const esp_err_t error = solar_os_saved_config_save(&settings_file, "lxmf", next, sizeof(*next));
    if (error == ESP_OK) {
        settings = *next;
        settings_loaded = true;
    }
    xSemaphoreGive(settings_lock);
    return error;
}

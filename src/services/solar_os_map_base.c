#include "solar_os_map_base.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "solar_os_task.h"

#include "solar_os_http_client.h"
#include "solar_os_log.h"
#include "solar_os_map_geojson.h"
#include "solar_os_map_http.h"
#include "solar_os_map_layers.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

static const char *TAG = "map_base";

/*
 * The same files the desktop script packs, taken from the same place, so
 * there is one source of a SolarOS world map rather than two that drift.
 */
#define MAP_BASE_SOURCE \
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/" \
    "master/geojson/"

/*
 * Order is drawing order: land, then the lakes that sit on it, then the
 * borders between countries, then the ones inside them. Province lines are
 * the United States alone at 1:110m, so they are taken from the one scale
 * in between; everything else is the coarsest scale, which is all a screen
 * this size can show of the whole world.
 */
static const struct {
    const char *name;
    const char *file;
    solar_os_map_class_t klass;
} map_base_layers[] = {
    {"land", "ne_110m_land.geojson", SOLAR_OS_MAP_CLASS_LAND},
    {"lakes", "ne_110m_lakes.geojson", SOLAR_OS_MAP_CLASS_WATER},
    {"borders", "ne_110m_admin_0_boundary_lines_land.geojson",
     SOLAR_OS_MAP_CLASS_BOUNDARY},
    {"regions", "ne_50m_admin_1_states_provinces_lines.geojson",
     SOLAR_OS_MAP_CLASS_REGION},
};

#define MAP_BASE_COUNT \
    (sizeof(map_base_layers) / sizeof(map_base_layers[0]))
#define MAP_BASE_TIMEOUT_MS 120000U
#define MAP_BASE_MAX_BYTES (4U * 1024U * 1024U)
#define MAP_BASE_STACK 12288
#define MAP_BASE_PRIORITY 4

static bool map_base_running;
static char map_base_message[64] = "";
static uint32_t map_base_generation;

uint32_t solar_os_map_base_generation(void)
{
    return map_base_generation;
}

bool solar_os_map_base_busy(void)
{
    return map_base_running;
}

const char *solar_os_map_base_status(void)
{
    return map_base_message;
}

size_t solar_os_map_base_kept(void)
{
    size_t kept = 0U;
    for (size_t index = 0U; index < MAP_BASE_COUNT; index++) {
        if (solar_os_map_layer_is_kept(map_base_layers[index].name)) {
            kept++;
        }
    }
    return kept;
}

esp_err_t solar_os_map_base_load(void)
{
    esp_err_t first = ESP_ERR_NOT_FOUND;
    for (size_t index = 0U; index < MAP_BASE_COUNT; index++) {
        const char *name = map_base_layers[index].name;
        if (!solar_os_map_layer_is_kept(name) ||
            solar_os_map_layer_is_loaded(name)) {
            continue;
        }
        const esp_err_t error = solar_os_map_layer_load_named(name);
        if (error == ESP_OK && solar_os_map_layer_count() > 0U) {
            /* The world stays whatever else has to go. */
            solar_os_map_layer_pin(solar_os_map_layer_count() - 1U);
        }
        if (first == ESP_ERR_NOT_FOUND || error != ESP_OK) {
            first = error;
        }
    }
    return first == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : first;
}

/* One layer: asked for, packed and kept, by the one thing that does that. */
static esp_err_t map_base_one(size_t index)
{
    const char *name = map_base_layers[index].name;
    char url[256];
    (void)snprintf(url, sizeof(url), "%s%s", MAP_BASE_SOURCE,
                   map_base_layers[index].file);
    char progress[sizeof(map_base_message)];
    const solar_os_map_download_t what = {
        .name = name,
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .limit = MAP_BASE_MAX_BYTES,
        .timeout_ms = MAP_BASE_TIMEOUT_MS,
        /* The source answers from a redirect. */
        .follow_redirects = true,
        .klass = map_base_layers[index].klass,
        .pack = solar_os_map_geojson_read,
        .progress = progress,
        .progress_capacity = sizeof(progress),
    };
    (void)snprintf(map_base_message, sizeof(map_base_message),
                   "getting %s from natural earth", name);
    const esp_err_t error = solar_os_map_download(&what);
    if (error == ESP_ERR_INVALID_STATE) {
        strlcpy(map_base_message, "no storage for maps",
                sizeof(map_base_message));
    } else if (error != ESP_OK) {
        (void)snprintf(map_base_message, sizeof(map_base_message),
                       "could not get %s", name);
    }
    return error;
}

static void map_base_task(void *argument)
{
    (void)argument;
    size_t done = 0U;
    size_t failed = 0U;
    for (size_t index = 0U; index < MAP_BASE_COUNT; index++) {
        if (solar_os_map_layer_is_kept(map_base_layers[index].name)) {
            done++;
            continue;
        }
        if (map_base_one(index) == ESP_OK) {
            done++;
        } else {
            failed++;
        }
    }
    if (failed == 0U) {
        (void)snprintf(map_base_message, sizeof(map_base_message),
                       "world ready, %u layers", (unsigned)done);
    } else {
        /* Whatever arrived is kept, so coming back later finishes the rest. */
        (void)snprintf(map_base_message, sizeof(map_base_message),
                       "%u of %u layers, try again later", (unsigned)done,
                       (unsigned)MAP_BASE_COUNT);
    }
    SOLAR_OS_LOGI(TAG, "%s", map_base_message);
    map_base_generation++;
    map_base_running = false;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_map_base_fetch(void)
{
    if (map_base_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!solar_os_storage_is_mounted()) {
        strlcpy(map_base_message, "no storage for maps",
                sizeof(map_base_message));
        return ESP_ERR_INVALID_STATE;
    }
    if (solar_os_map_base_kept() == MAP_BASE_COUNT) {
        strlcpy(map_base_message, "world already kept",
                sizeof(map_base_message));
        return ESP_OK;
    }
    map_base_running = true;
    (void)snprintf(map_base_message, sizeof(map_base_message),
                   "getting the world map, %u of %u layers to go",
                   (unsigned)(MAP_BASE_COUNT - solar_os_map_base_kept()),
                   (unsigned)MAP_BASE_COUNT);
    /* Through the system's own task creation, so a download counts against
     * task admission and shows up in `mem policy` like every other task. */
    if (solar_os_task_create_pinned_internal(
            map_base_task, "map_base", MAP_BASE_STACK, NULL,
            MAP_BASE_PRIORITY, NULL, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        map_base_running = false;
        strlcpy(map_base_message, "no room to start",
                sizeof(map_base_message));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

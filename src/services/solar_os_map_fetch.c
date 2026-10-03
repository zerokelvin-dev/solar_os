#include "solar_os_map_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "solar_os_task.h"

#include "solar_os_http_client.h"
#include "solar_os_log.h"
#include "solar_os_map_geo.h"
#include "solar_os_map_http.h"
#include "solar_os_map_layers.h"
#include "solar_os_map_osm.h"
#include "solar_os_memory.h"

static const char *TAG = "map_fetch";

#define MAP_FETCH_ENDPOINT "https://overpass-api.de/api/interpreter"
#define MAP_FETCH_TIMEOUT_MS 180000U
/*
 * The ways worth carrying: water, the rivers that drain it, the railways,
 * and the roads down to tertiary. Residential streets are left out, not
 * because they do not fit but because they cover a third of the screen
 * where these cover a tenth.
 */
#define MAP_FETCH_QUERY_FMT                                                   \
    "[out:json][timeout:170];\n(\n"                                           \
    "  way[\"natural\"=\"water\"](%s);\n  rel[\"natural\"=\"water\"](%s);\n"  \
    "  way[\"waterway\"=\"river\"](%s);\n"                                    \
    "  way[\"railway\"=\"rail\"](%s);\n"                                      \
    "  way[\"highway\"~\"^(motorway|trunk|primary|secondary|tertiary)$\"]"    \
    "(%s);\n);\nout geom;\n"

/* TLS wants room: the same work through the curl application is given
 * twelve kilobytes, and less than that is not worth finding out about. */
#define MAP_FETCH_STACK 12288
#define MAP_FETCH_PRIORITY 4

static bool map_fetch_running;
static char map_fetch_message[64] = "";
static uint32_t map_fetch_generation;

/* Where the work happens is a task of its own, so the press that started it
 * can return and the map can keep drawing. */
typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
} map_fetch_job_t;

uint32_t solar_os_map_fetch_generation(void)
{
    return map_fetch_generation;
}

bool solar_os_map_fetch_busy(void)
{
    return map_fetch_running;
}

const char *solar_os_map_fetch_status(void)
{
    return map_fetch_message;
}

static esp_err_t map_fetch_run(int32_t latitude_e7, int32_t longitude_e7)
{
    char name[SOLAR_OS_MAP_CELL_NAME_MAX];
    solar_os_map_cell_name(latitude_e7, longitude_e7, name, sizeof(name));
    int32_t south = 0;
    int32_t west = 0;
    solar_os_map_cell_corner(latitude_e7, longitude_e7, &south, &west);

    /* Overpass takes a box as south,west,north,east in degrees. */
    char box[64];
    (void)snprintf(box, sizeof(box), "%d.%07d,%d.%07d,%d.%07d,%d.%07d",
                   south / 10000000, abs(south % 10000000),
                   west / 10000000, abs(west % 10000000),
                   (south + SOLAR_OS_MAP_CELL_E7) / 10000000,
                   abs((south + SOLAR_OS_MAP_CELL_E7) % 10000000),
                   (west + SOLAR_OS_MAP_CELL_E7) / 10000000,
                   abs((west + SOLAR_OS_MAP_CELL_E7) % 10000000));
    char query[1024];
    (void)snprintf(query, sizeof(query), MAP_FETCH_QUERY_FMT, box, box, box,
                   box, box);

    char progress[sizeof(map_fetch_message)];
    const solar_os_map_download_t what = {
        .name = name,
        .url = MAP_FETCH_ENDPOINT,
        .method = SOLAR_OS_HTTP_METHOD_POST,
        .body = query,
        .body_len = strlen(query),
        .limit = SOLAR_OS_MAP_FETCH_MAX_BYTES,
        .timeout_ms = MAP_FETCH_TIMEOUT_MS,
        .klass = SOLAR_OS_MAP_CLASS_ROAD,
        .pack = solar_os_map_osm_read,
        .progress = progress,
        .progress_capacity = sizeof(progress),
    };
    SOLAR_OS_LOGI(TAG, "asking overpass for %s (%s)", name, box);

    /*
     * The progress line is written where the work can update it and only
     * copied out between phases, so a reader never sees half a message.
     */
    (void)snprintf(map_fetch_message, sizeof(map_fetch_message),
                   "asking openstreetmap for %s", name);
    const esp_err_t error = solar_os_map_download(&what);

    /* Three things a reader can act on. The rest went to the log. */
    switch (error) {
    case ESP_OK:
        (void)snprintf(map_fetch_message, sizeof(map_fetch_message),
                       "%s fetched", name);
        break;
    case ESP_ERR_NOT_FOUND:
        strlcpy(map_fetch_message, "nothing mapped here",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_TIMEOUT:
        strlcpy(map_fetch_message, "openstreetmap busy, try later",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_INVALID_SIZE:
        strlcpy(map_fetch_message, "too much data for one cell",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_INVALID_STATE:
        strlcpy(map_fetch_message, "no storage for maps",
                sizeof(map_fetch_message));
        break;
    default:
        (void)snprintf(map_fetch_message, sizeof(map_fetch_message),
                       "could not fetch %s", name);
        break;
    }
    return error;
}

static void map_fetch_task(void *argument)
{
    map_fetch_job_t *job = argument;
    (void)map_fetch_run(job->latitude_e7, job->longitude_e7);
    solar_os_memory_free(job);
    map_fetch_generation++;
    map_fetch_running = false;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_map_fetch_cell(int32_t latitude_e7, int32_t longitude_e7)
{
    if (map_fetch_running) {
        return ESP_ERR_INVALID_STATE;
    }
    map_fetch_job_t *job =
        solar_os_memory_calloc(1U, sizeof(*job),
                               SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
                               "map.fetch.job");
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }
    job->latitude_e7 = latitude_e7;
    job->longitude_e7 = longitude_e7;
    map_fetch_running = true;
    char name[SOLAR_OS_MAP_CELL_NAME_MAX];
    solar_os_map_cell_name(latitude_e7, longitude_e7, name, sizeof(name));
    (void)snprintf(map_fetch_message, sizeof(map_fetch_message),
                   "asking openstreetmap for %s", name);
    /* Through the system's own task creation, so a download counts against
     * task admission and shows up in `mem policy` like every other task. */
    if (solar_os_task_create_pinned_internal(
            map_fetch_task, "map_fetch", MAP_FETCH_STACK, job,
            MAP_FETCH_PRIORITY, NULL, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        solar_os_memory_free(job);
        map_fetch_running = false;
        strlcpy(map_fetch_message, "no room to start", sizeof(map_fetch_message));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

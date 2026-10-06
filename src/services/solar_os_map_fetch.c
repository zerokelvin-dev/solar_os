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
#include "solar_os_map_project.h"
#include "solar_os_memory.h"

static const char *TAG = "map_fetch";

#define MAP_FETCH_ENDPOINT "https://overpass-api.de/api/interpreter"
/* How long the server may work on a query. */
#define MAP_FETCH_TIMEOUT_S 60U
/* How long the device waits for the whole answer, which arrives far
 * slower than the server takes to make it: about a megabyte of a dense
 * cell takes over a minute. */
#define MAP_FETCH_DEADLINE_MS 180000U
/*
 * What each tier asks for, the box filled in for every clause. The base
 * carries the water, the rivers that drain it, the railways and the roads
 * down to secondary; the other two are one kind of road each.
 */
/*
 * What each tier asks for, how large its cell is, and how far each class may
 * be simplified. The base carries the water, the rivers that drain it, the
 * railways and the roads down to secondary, and so needs its tags; the other
 * two are one kind of road each, so their tags are left on the server. A
 * finer tier is kept in smaller cells, about a screen wide where it starts
 * to be drawn, so loading one is never a city's worth at once.
 *
 * Every answer is cut to its cell by the server. For the base, which carries
 * lakes, the answer ends with which water areas hold each corner, south-west
 * first and round anticlockwise, so a shore cut by the border can be closed
 * along it.
 */
typedef struct {
    const char *suffix;
    int32_t cell_e7;
    solar_os_map_class_t klass;
    const char *clauses;
    bool skeleton;
    bool corners;
} map_fetch_tier_t;

static const map_fetch_tier_t map_fetch_tiers[SOLAR_OS_MAP_TIER_COUNT] = {
    [SOLAR_OS_MAP_TIER_BASE] = {
        .suffix = "",
        .cell_e7 = SOLAR_OS_MAP_CELL_E7,
        .klass = SOLAR_OS_MAP_CLASS_ROAD,
        .clauses =
            "  way[\"natural\"=\"water\"](%s);\n  rel[\"natural\"=\"water\"](%s);\n"
            "  way[\"waterway\"=\"river\"](%s);\n"
            "  way[\"railway\"=\"rail\"](%s);\n"
            "  way[\"highway\"~\"^(motorway|trunk|primary|secondary)$\"](%s);\n",
        .corners = true,
    },
    [SOLAR_OS_MAP_TIER_MINOR] = {
        .suffix = ".minor",
        .cell_e7 = SOLAR_OS_MAP_CELL_E7,
        .klass = SOLAR_OS_MAP_CLASS_ROAD_MINOR,
        .clauses = "  way[\"highway\"=\"tertiary\"](%s);\n",
        .skeleton = true,
    },
    [SOLAR_OS_MAP_TIER_STREETS] = {
        .suffix = ".streets",
        .cell_e7 = SOLAR_OS_MAP_CELL_E7 / 5,
        .klass = SOLAR_OS_MAP_CLASS_STREET,
        .clauses =
            "  way[\"highway\"~\"^(residential|unclassified|living_street)$\"](%s);\n",
        .skeleton = true,
    },
};

/* Metres a vertex may be off the simplified line, by class. */
static const uint16_t map_fetch_tolerance_m[SOLAR_OS_MAP_CLASS_COUNT] = {
    [SOLAR_OS_MAP_CLASS_WATER] = 10U,
    [SOLAR_OS_MAP_CLASS_WATER_EDGE] = 10U,
    [SOLAR_OS_MAP_CLASS_RAIL] = 15U,
    [SOLAR_OS_MAP_CLASS_HIGHWAY] = 10U,
    [SOLAR_OS_MAP_CLASS_ROAD] = 10U,
    [SOLAR_OS_MAP_CLASS_ROAD_MINOR] = 10U,
    [SOLAR_OS_MAP_CLASS_STREET] = 5U,
};

uint32_t solar_os_map_tier_max_mpp(solar_os_map_tier_t tier)
{
    switch (tier) {
    case SOLAR_OS_MAP_TIER_MINOR:
        return SOLAR_OS_MAP_ROAD_MINOR_MAX_MPP;
    case SOLAR_OS_MAP_TIER_STREETS:
        return SOLAR_OS_MAP_STREET_MAX_MPP;
    default:
        return UINT32_MAX;
    }
}

int32_t solar_os_map_tier_cell_e7(solar_os_map_tier_t tier)
{
    return tier < SOLAR_OS_MAP_TIER_COUNT ? map_fetch_tiers[tier].cell_e7
                                          : SOLAR_OS_MAP_CELL_E7;
}

void solar_os_map_tier_name(int32_t latitude_e7,
                            int32_t longitude_e7,
                            solar_os_map_tier_t tier,
                            char *out,
                            size_t out_len)
{
    char cell[SOLAR_OS_MAP_CELL_NAME_MAX];
    solar_os_map_cell_name_sized(latitude_e7, longitude_e7,
                                 solar_os_map_tier_cell_e7(tier), cell,
                                 sizeof(cell));
    (void)snprintf(out, out_len, "%s%s", cell,
                   tier < SOLAR_OS_MAP_TIER_COUNT ? map_fetch_tiers[tier].suffix : "");
}

/* Degrees as Overpass wants them, with the sign kept for -0.5. */
static void map_fetch_degrees(int32_t e7, char *out, size_t len)
{
    const uint32_t magnitude = e7 < 0 ? (uint32_t)-(int64_t)e7 : (uint32_t)e7;
    (void)snprintf(out, len, "%s%lu.%07lu", e7 < 0 ? "-" : "",
                   (unsigned long)(magnitude / 10000000U),
                   (unsigned long)(magnitude % 10000000U));
}

static esp_err_t map_fetch_pack(solar_os_inflate_t *reader,
                                solar_os_map_class_t klass,
                                const void *context,
                                uint8_t **out,
                                size_t *out_size)
{
    return solar_os_map_osm_read_cell(reader, klass, context, out, out_size);
}

/* TLS wants room: the same work through the curl application is given
 * twelve kilobytes, and less than that is not worth finding out about. */
#define MAP_FETCH_STACK 12288
#define MAP_FETCH_PRIORITY 4

static bool map_fetch_running;
static char map_fetch_message[64] = "";
/* What is being fetched and how far it has got, read while it runs. */
static char map_fetch_cell_name[SOLAR_OS_MAP_CELL_NAME_MAX];
static solar_os_map_tier_t map_fetch_tier;
static volatile solar_os_map_phase_t map_fetch_phase;
static volatile size_t map_fetch_received;
static uint32_t map_fetch_generation;

/* Where the work happens is a task of its own, so the press that started it
 * can return and the map can keep drawing. */
typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    solar_os_map_tier_t tier;
} map_fetch_job_t;

uint32_t solar_os_map_fetch_generation(void)
{
    return map_fetch_generation;
}

bool solar_os_map_fetch_busy(void)
{
    return map_fetch_running;
}

/* What a reader calls each tier. */
static const char *const map_fetch_tier_words[SOLAR_OS_MAP_TIER_COUNT] = {
    [SOLAR_OS_MAP_TIER_BASE] = "map",
    [SOLAR_OS_MAP_TIER_MINOR] = "roads",
    [SOLAR_OS_MAP_TIER_STREETS] = "streets",
};

const char *solar_os_map_fetch_status(void)
{
    if (!map_fetch_running) {
        return map_fetch_message;
    }
    static const char *const verbs[] = {
        [SOLAR_OS_MAP_PHASE_REQUESTING] = "requesting",
        [SOLAR_OS_MAP_PHASE_DOWNLOADING] = "downloading",
        [SOLAR_OS_MAP_PHASE_PARSING] = "parsing",
        [SOLAR_OS_MAP_PHASE_WRITING] = "writing",
    };
    static char line[sizeof(map_fetch_message)];
    const solar_os_map_phase_t phase = map_fetch_phase;
    const int used = snprintf(line, sizeof(line), "%s %s %s", verbs[phase],
                              map_fetch_cell_name,
                              map_fetch_tier_words[map_fetch_tier]);
    /* The answer comes without a length, so how much has arrived is what
     * there is to show, and a count that stops moving is a stall. */
    if (phase == SOLAR_OS_MAP_PHASE_DOWNLOADING && used > 0 &&
        (size_t)used < sizeof(line)) {
        (void)snprintf(&line[used], sizeof(line) - (size_t)used, " %u KB",
                       (unsigned)(map_fetch_received / 1024U));
    }
    return line;
}

static esp_err_t map_fetch_run(int32_t latitude_e7,
                               int32_t longitude_e7,
                               solar_os_map_tier_t tier)
{
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    solar_os_map_tier_name(latitude_e7, longitude_e7, tier, name, sizeof(name));
    const map_fetch_tier_t *spec = &map_fetch_tiers[tier];
    solar_os_map_osm_cell_t cell = {0};
    solar_os_map_cell_corner_sized(latitude_e7, longitude_e7, spec->cell_e7,
                                   &cell.south, &cell.west);
    cell.north = cell.south + spec->cell_e7;
    cell.east = cell.west + spec->cell_e7;
    memcpy(cell.tolerance_m, map_fetch_tolerance_m, sizeof(cell.tolerance_m));

    /* Overpass takes a box as south,west,north,east in degrees. */
    char south[16];
    char west[16];
    char north[16];
    char east[16];
    map_fetch_degrees(cell.south, south, sizeof(south));
    map_fetch_degrees(cell.west, west, sizeof(west));
    map_fetch_degrees(cell.north, north, sizeof(north));
    map_fetch_degrees(cell.east, east, sizeof(east));
    char box[64];
    (void)snprintf(box, sizeof(box), "%s,%s,%s,%s", south, west, north, east);
    char clauses[768];
    /* Surplus arguments are ignored, so every tier is handed the box as many
     * times as the base, which wants it most. */
    (void)snprintf(clauses, sizeof(clauses), spec->clauses, box, box, box, box,
                   box);
    char corners[512] = "";
    if (spec->corners) {
        (void)snprintf(corners, sizeof(corners),
                       "is_in(%s,%s)->.c;area.c[\"natural\"=\"water\"];out ids;"
                       "make corner;out;\n"
                       "is_in(%s,%s)->.c;area.c[\"natural\"=\"water\"];out ids;"
                       "make corner;out;\n"
                       "is_in(%s,%s)->.c;area.c[\"natural\"=\"water\"];out ids;"
                       "make corner;out;\n"
                       "is_in(%s,%s)->.c;area.c[\"natural\"=\"water\"];out ids;"
                       "make corner;out;\n",
                       south, west, south, east, north, east, north, west);
    }
    char query[1600];
    (void)snprintf(query, sizeof(query),
                   "[out:json][timeout:%u];\n(\n%s);\nout %sgeom(%s);\n%s",
                   MAP_FETCH_TIMEOUT_S, clauses, spec->skeleton ? "skel " : "",
                   box, corners);

    const solar_os_map_download_t what = {
        .name = name,
        .url = MAP_FETCH_ENDPOINT,
        .method = SOLAR_OS_HTTP_METHOD_POST,
        .body = query,
        .body_len = strlen(query),
        .limit = SOLAR_OS_MAP_FETCH_MAX_BYTES,
        .timeout_ms = MAP_FETCH_DEADLINE_MS,
        .klass = spec->klass,
        .pack = map_fetch_pack,
        .pack_context = &cell,
        .phase = &map_fetch_phase,
        .received = &map_fetch_received,
    };
    SOLAR_OS_LOGI(TAG, "asking overpass for %s (%s)", name, box);

    const esp_err_t error = solar_os_map_download(&what);

    /* Three things a reader can act on. The rest went to the log. */
    switch (error) {
    case ESP_OK:
        (void)snprintf(map_fetch_message, sizeof(map_fetch_message),
                       "fetched %s %s", map_fetch_cell_name,
                       map_fetch_tier_words[tier]);
        break;
    case ESP_ERR_NOT_FOUND:
        strlcpy(map_fetch_message, "nothing mapped here",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_NOT_ALLOWED:
        strlcpy(map_fetch_message, "openstreetmap rate limit, wait a minute",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_INVALID_RESPONSE:
        strlcpy(map_fetch_message, "openstreetmap busy, try again",
                sizeof(map_fetch_message));
        break;
    case ESP_ERR_TIMEOUT:
        strlcpy(map_fetch_message, "openstreetmap timed out, try again",
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
    (void)map_fetch_run(job->latitude_e7, job->longitude_e7, job->tier);
    solar_os_memory_free(job);
    map_fetch_generation++;
    map_fetch_running = false;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_map_fetch_cell(int32_t latitude_e7,
                                  int32_t longitude_e7,
                                  solar_os_map_tier_t tier)
{
    if (tier >= SOLAR_OS_MAP_TIER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
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
    job->tier = tier;
    solar_os_map_cell_name_sized(latitude_e7, longitude_e7,
                                 solar_os_map_tier_cell_e7(tier),
                                 map_fetch_cell_name,
                                 sizeof(map_fetch_cell_name));
    map_fetch_tier = tier;
    map_fetch_phase = SOLAR_OS_MAP_PHASE_REQUESTING;
    map_fetch_received = 0U;
    map_fetch_running = true;
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

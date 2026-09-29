#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_map_geo.h"

#define SOLAR_OS_MAP_CAPACITY 96U
#define SOLAR_OS_MAP_SOURCE_MAX 16U
#define SOLAR_OS_MAP_KEY_MAX 40U
#define SOLAR_OS_MAP_LABEL_MAX 32U
#define SOLAR_OS_MAP_DETAIL_MAX 64U
#define SOLAR_OS_MAP_PATH_CAPACITY 48U

typedef enum {
    SOLAR_OS_MAP_KIND_SELF = 0,
    SOLAR_OS_MAP_KIND_NODE = 1,
    SOLAR_OS_MAP_KIND_WAYPOINT = 2,
} solar_os_map_kind_t;

/*
 * A producer owns the (source, key) pair: publishing it again moves the point
 * rather than adding another. Points live in RAM only; producers republish
 * whenever they hear a position.
 */
typedef struct {
    const char *source;
    const char *key;
    const char *label;
    const char *detail;
    solar_os_map_kind_t kind;
    int32_t latitude_e7;
    int32_t longitude_e7;
    /* Wall-clock time of the position, or 0 when the producer has none. */
    uint64_t timestamp_ms;
} solar_os_map_publish_t;

typedef struct {
    uint32_t id;
    solar_os_map_kind_t kind;
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint64_t timestamp_ms;
    uint32_t updated_ms;
    char source[SOLAR_OS_MAP_SOURCE_MAX];
    char key[SOLAR_OS_MAP_KEY_MAX];
    char label[SOLAR_OS_MAP_LABEL_MAX];
    char detail[SOLAR_OS_MAP_DETAIL_MAX];
} solar_os_map_point_t;

/*
 * A path is a line between two points, held as references rather than as
 * coordinates so that it follows its endpoints as they move. A path whose
 * endpoint is removed or evicted is removed with it: a line to somewhere
 * the map no longer knows is worse than no line.
 */
typedef struct {
    const char *source;
    const char *key;
    const char *label;
    uint32_t from_id;
    uint32_t to_id;
} solar_os_map_path_publish_t;

typedef struct {
    uint32_t id;
    uint32_t from_id;
    uint32_t to_id;
    uint32_t updated_ms;
    char source[SOLAR_OS_MAP_SOURCE_MAX];
    char key[SOLAR_OS_MAP_KEY_MAX];
    char label[SOLAR_OS_MAP_LABEL_MAX];
} solar_os_map_path_t;

typedef struct {
    bool initialized;
    size_t capacity;
    size_t count;
    size_t path_capacity;
    size_t path_count;
    uint32_t evicted;
    /* Changes on every publish, removal and clear. */
    uint32_t generation;
} solar_os_map_status_t;

esp_err_t solar_os_map_init(void);
esp_err_t solar_os_map_publish(const solar_os_map_publish_t *point, uint32_t *id);
esp_err_t solar_os_map_get(uint32_t id, solar_os_map_point_t *point);
esp_err_t solar_os_map_remove(uint32_t id);
esp_err_t solar_os_map_remove_source(const char *source, size_t *removed);
esp_err_t solar_os_map_clear(void);
/* Newest first. Returns the number copied; total counts every stored point. */
size_t solar_os_map_snapshot(solar_os_map_point_t *points,
                             size_t max_points,
                             size_t *total);
esp_err_t solar_os_map_get_status(solar_os_map_status_t *status);

esp_err_t solar_os_map_path_publish(const solar_os_map_path_publish_t *path,
                                    uint32_t *id);
esp_err_t solar_os_map_path_remove(uint32_t id);
esp_err_t solar_os_map_path_remove_source(const char *source, size_t *removed);
size_t solar_os_map_path_snapshot(solar_os_map_path_t *paths,
                                  size_t max_paths,
                                  size_t *total);

/* Publishes the first GNSS device with a valid fix as the "gnss" self point. */
esp_err_t solar_os_map_update_self(void);
const char *solar_os_map_kind_name(solar_os_map_kind_t kind);

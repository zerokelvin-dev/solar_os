#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_map_geo.h"

/* A hidden directory, so a card browsed on a desktop shows the user's files
 * rather than ours. */
#define SOLAR_OS_PLACES_DIR ".map"
#define SOLAR_OS_PLACES_FILE "points.bin"

/*
 * The one source that is kept across a restart. Points published under any
 * other source live in RAM only, because their producer republishes as soon
 * as it hears a position again and a stored copy would be a stale claim.
 * Persistence is part of the contract, so the name a producer has to use to
 * get it is named here rather than spelled out in each caller.
 */
#define SOLAR_OS_PLACES_SOURCE_USER "user"

#define SOLAR_OS_PLACES_CAPACITY 96U
#define SOLAR_OS_PLACES_SOURCE_MAX 16U
#define SOLAR_OS_PLACES_KEY_MAX 40U
#define SOLAR_OS_PLACES_LABEL_MAX 32U
#define SOLAR_OS_PLACES_DETAIL_MAX 64U
#define SOLAR_OS_PLACES_PATH_CAPACITY 48U

typedef enum {
    SOLAR_OS_PLACES_KIND_SELF = 0,
    SOLAR_OS_PLACES_KIND_NODE = 1,
    SOLAR_OS_PLACES_KIND_WAYPOINT = 2,
} solar_os_places_kind_t;

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
    solar_os_places_kind_t kind;
    int32_t latitude_e7;
    int32_t longitude_e7;
    /* Wall-clock time of the position, or 0 when the producer has none. */
    uint64_t timestamp_ms;
} solar_os_places_publish_t;

/*
 * An id names a point for as long as this boot lasts and no longer: the
 * store hands out ids from one, so a point restored from the card is not
 * the point it was before the restart. Nothing should write an id down.
 */
typedef struct {
    uint32_t id;
    solar_os_places_kind_t kind;
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint64_t timestamp_ms;
    /* Uptime in milliseconds when the point was last published, which is
     * what an age is measured from. It wraps; subtract, never compare. */
    uint32_t updated_ms;
    char source[SOLAR_OS_PLACES_SOURCE_MAX];
    char key[SOLAR_OS_PLACES_KEY_MAX];
    char label[SOLAR_OS_PLACES_LABEL_MAX];
    char detail[SOLAR_OS_PLACES_DETAIL_MAX];
} solar_os_places_point_t;

/*
 * A path is a line between two points, held as references rather than as
 * coordinates so that it follows its endpoints as they move. A path whose
 * endpoint is removed or evicted is removed with it: a line to somewhere
 * the places no longer know is worse than no line.
 */
typedef struct {
    const char *source;
    const char *key;
    const char *label;
    uint32_t from_id;
    uint32_t to_id;
} solar_os_places_path_publish_t;

typedef struct {
    uint32_t id;
    uint32_t from_id;
    uint32_t to_id;
    uint32_t updated_ms;
    char source[SOLAR_OS_PLACES_SOURCE_MAX];
    char key[SOLAR_OS_PLACES_KEY_MAX];
    char label[SOLAR_OS_PLACES_LABEL_MAX];
} solar_os_places_path_t;

typedef struct {
    bool initialized;
    size_t capacity;
    size_t count;
    size_t path_capacity;
    size_t path_count;
    /* Node points dropped to make room since the store was set up, which
     * only ever goes up. */
    uint32_t evicted;
    /* Changes on every publish, removal and clear. */
    uint32_t generation;
} solar_os_places_status_t;

esp_err_t solar_os_places_init(void);
/*
 * Places or moves a point. The store is full when every slot holds a point
 * no eviction may take, which is to say a waypoint or a position, and a
 * publish that would have to take one of those returns ESP_ERR_NO_MEM
 * rather than dropping it. A producer that wants to know whether a point it
 * placed earlier is still there asks for it by id.
 */
esp_err_t solar_os_places_publish(const solar_os_places_publish_t *point, uint32_t *id);
esp_err_t solar_os_places_get(uint32_t id, solar_os_places_point_t *point);
esp_err_t solar_os_places_remove(uint32_t id);
/* Removes the points of one source. A path is removed with either of its
 * endpoints, but a path whose endpoints are somebody else's points is not:
 * a producer that published paths of its own gives those up separately,
 * with solar_os_places_path_remove_source. */
esp_err_t solar_os_places_remove_source(const char *source, size_t *removed);
esp_err_t solar_os_places_clear(void);
/* Newest first. Returns the number copied; total counts every stored point.
 * A null array asks for the total alone. */
size_t solar_os_places_snapshot(solar_os_places_point_t *points,
                             size_t max_points,
                             size_t *total);
esp_err_t solar_os_places_get_status(solar_os_places_status_t *status);

esp_err_t solar_os_places_path_publish(const solar_os_places_path_publish_t *path,
                                    uint32_t *id);
esp_err_t solar_os_places_path_remove(uint32_t id);
esp_err_t solar_os_places_path_remove_source(const char *source, size_t *removed);
/* As above: a null array asks for the total alone. */
size_t solar_os_places_path_snapshot(solar_os_places_path_t *paths,
                                  size_t max_paths,
                                  size_t *total);

/* Publishes the first GNSS device with a valid fix as the "gnss" self point. */
esp_err_t solar_os_places_update_self(void);
const char *solar_os_places_kind_name(solar_os_places_kind_t kind);

#include "solar_os_map_geojson.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_inflate.h"
#include "solar_os_map_geo.h"
#include "solar_os_map_layers.h"
#include "solar_os_memory.h"

#define GEOJSON_CHUNK 512U
#define GEOJSON_TOKEN_MAX 32U
#define GEOJSON_DEPTH_MAX 8

typedef struct {
    void (*ring_begin)(void *user, bool open);
    void (*point)(void *user, int32_t latitude_e7, int32_t longitude_e7);
    void (*ring_end)(void *user);
    void *user;

    bool in_string;
    bool escaped;
    bool expect_type_value;
    bool pending_coordinates;
    bool in_coordinates;
    bool open_geometry;
    bool ring_active;
    int depth;
    int position_depth;
    unsigned axis;
    double longitude;
    char token[GEOJSON_TOKEN_MAX];
    size_t token_len;
} geojson_scan_t;

static void token_reset(geojson_scan_t *scan)
{
    scan->token_len = 0U;
    scan->token[0] = '\0';
}

static void token_push(geojson_scan_t *scan, char ch)
{
    if (scan->token_len + 1U < sizeof(scan->token)) {
        scan->token[scan->token_len++] = ch;
        scan->token[scan->token_len] = '\0';
    }
}

static int32_t degrees_to_e7(double value)
{
    const double scaled = value * 10000000.0;
    if (scaled > 1800000000.0) {
        return 1800000000;
    }
    if (scaled < -1800000000.0) {
        return -1800000000;
    }
    return (int32_t)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}

/* A number token ends at a comma or a closing bracket. */
static void flush_number(geojson_scan_t *scan)
{
    if (scan->token_len == 0U) {
        return;
    }
    const double value = strtod(scan->token, NULL);
    token_reset(scan);
    if (scan->axis == 0U) {
        scan->longitude = value;
        scan->axis = 1U;
        return;
    }
    scan->axis = 0U;
    if (!scan->ring_active) {
        scan->ring_active = true;
        if (scan->ring_begin != NULL) {
            scan->ring_begin(scan->user, scan->open_geometry);
        }
    }
    if (scan->point != NULL) {
        scan->point(scan->user, degrees_to_e7(value),
                    degrees_to_e7(scan->longitude));
    }
}

static void end_ring(geojson_scan_t *scan)
{
    if (!scan->ring_active) {
        return;
    }
    scan->ring_active = false;
    if (scan->ring_end != NULL) {
        scan->ring_end(scan->user);
    }
}

static void scan_string_end(geojson_scan_t *scan)
{
    if (scan->expect_type_value) {
        scan->expect_type_value = false;
        /* Lines and multilines are drawn open; everything else is an area. */
        scan->open_geometry = strstr(scan->token, "Line") != NULL;
    } else if (strcmp(scan->token, "type") == 0) {
        scan->expect_type_value = true;
    } else if (strcmp(scan->token, "coordinates") == 0) {
        scan->pending_coordinates = true;
    }
    token_reset(scan);
}

static void scan_byte(geojson_scan_t *scan, char ch)
{
    if (scan->in_string) {
        if (scan->escaped) {
            scan->escaped = false;
        } else if (ch == '\\') {
            scan->escaped = true;
        } else if (ch == '"') {
            scan->in_string = false;
            scan_string_end(scan);
        } else {
            token_push(scan, ch);
        }
        return;
    }
    if (ch == '"') {
        scan->in_string = true;
        token_reset(scan);
        return;
    }

    if (!scan->in_coordinates) {
        if (ch == '[' && scan->pending_coordinates) {
            scan->pending_coordinates = false;
            scan->in_coordinates = true;
            scan->depth = 1;
            scan->position_depth = 0;
            scan->axis = 0U;
            token_reset(scan);
        }
        return;
    }

    switch (ch) {
    case '[':
        scan->depth++;
        if (scan->depth > GEOJSON_DEPTH_MAX) {
            scan->in_coordinates = false;
            end_ring(scan);
        }
        return;
    case ']':
        flush_number(scan);
        if (scan->depth == scan->position_depth - 1 || scan->position_depth == 0) {
            end_ring(scan);
        }
        scan->depth--;
        if (scan->depth <= 0) {
            end_ring(scan);
            scan->in_coordinates = false;
            scan->axis = 0U;
        }
        return;
    case ',':
        flush_number(scan);
        return;
    default:
        break;
    }
    if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ||
        ch == 'e' || ch == 'E') {
        if (scan->token_len == 0U) {
            /* The array holding numbers is the position depth. */
            scan->position_depth = scan->depth;
        }
        token_push(scan, ch);
    }
}

static esp_err_t scan_file(solar_os_inflate_t *reader, geojson_scan_t *scan)
{
    char chunk[GEOJSON_CHUNK];
    esp_err_t error = solar_os_inflate_rewind(reader);
    if (error != ESP_OK) {
        return error;
    }
    size_t read = 0U;
    while ((read = solar_os_inflate_read(reader, chunk, sizeof(chunk))) > 0U) {
        for (size_t index = 0U; index < read; index++) {
            scan_byte(scan, chunk[index]);
        }
    }
    end_ring(scan);
    return solar_os_inflate_failed(reader) ? ESP_FAIL : ESP_OK;
}

typedef struct {
    solar_os_map_class_t klass;
    uint32_t rings;
    uint32_t points;
    uint32_t ring_points;
    bool open;

    uint8_t *counts;
    uint8_t *bounds;
    uint8_t *coordinates;
    int32_t lat_min;
    int32_t lat_max;
    int32_t lon_min;
    int32_t lon_max;
    uint32_t ring_index;
    uint32_t point_index;
    uint32_t ring_start;
    bool writing;
    /* Measured while packing, and written into the header. */
    solar_os_map_spacing_t spacing;
    int32_t last_lat;
    int32_t last_lon;
} geojson_build_t;

static void write_u32(uint8_t *out, uint32_t value);
static void write_i32(uint8_t *out, int32_t value);

static void build_ring_begin(void *user, bool open)
{
    geojson_build_t *build = user;
    build->ring_points = 0U;
    build->ring_start = build->point_index;
    build->open = open;
    build->lat_min = 0;
    build->lat_max = 0;
    build->lon_min = 0;
    build->lon_max = 0;
}

static void build_point(void *user, int32_t latitude_e7, int32_t longitude_e7)
{
    geojson_build_t *build = user;
    if (build->ring_points != 0U) {
        /* How far apart the source placed its vertices, which is what says
         * whether its outline can be believed at a given zoom. */
        solar_os_map_spacing_add(&build->spacing, build->last_lat,
                                 build->last_lon, latitude_e7, longitude_e7);
    }
    build->last_lat = latitude_e7;
    build->last_lon = longitude_e7;
    if (build->ring_points == 0U) {
        build->lat_min = latitude_e7;
        build->lat_max = latitude_e7;
        build->lon_min = longitude_e7;
        build->lon_max = longitude_e7;
    } else {
        if (latitude_e7 < build->lat_min) {
            build->lat_min = latitude_e7;
        }
        if (latitude_e7 > build->lat_max) {
            build->lat_max = latitude_e7;
        }
        if (longitude_e7 < build->lon_min) {
            build->lon_min = longitude_e7;
        }
        if (longitude_e7 > build->lon_max) {
            build->lon_max = longitude_e7;
        }
    }
    build->ring_points++;
    if (build->writing && build->point_index < build->points) {
        write_i32(&build->coordinates[build->point_index * 8U], latitude_e7);
        write_i32(&build->coordinates[build->point_index * 8U + 4U], longitude_e7);
        build->point_index++;
    }
}

static void build_ring_end(void *user)
{
    geojson_build_t *build = user;
    if (build->ring_points < 2U) {
        /* A lone position is a marker, not geometry this map can draw. */
        build->point_index = build->ring_start;
        build->ring_points = 0U;
        return;
    }
    if (build->writing) {
        if (build->ring_index < build->rings) {
            write_i32(&build->bounds[build->ring_index * 16U], build->lat_min);
            write_i32(&build->bounds[build->ring_index * 16U + 4U], build->lat_max);
            write_i32(&build->bounds[build->ring_index * 16U + 8U], build->lon_min);
            write_i32(&build->bounds[build->ring_index * 16U + 12U], build->lon_max);
            write_i32(&build->counts[build->ring_index++ * 4U],
                      (int32_t)(build->ring_points |
                                ((uint32_t)build->klass <<
                                 SOLAR_OS_MAP_RING_CLASS_SHIFT) |
                                (build->open ? SOLAR_OS_MAP_RING_OPEN : 0U)));
        }
    } else {
        build->rings++;
        build->points += build->ring_points;
    }
    build->ring_points = 0U;
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void write_i32(uint8_t *out, int32_t value)
{
    write_u32(out, (uint32_t)value);
}

esp_err_t solar_os_map_geojson_read(solar_os_inflate_t *reader,
                                    solar_os_map_class_t klass,
                                    uint8_t **out,
                                    size_t *out_size)
{
    if (reader == NULL || out == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    geojson_build_t build = {.klass = klass};
    geojson_scan_t scan = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .user = &build,
    };
    esp_err_t error = scan_file(reader, &scan);
    if (error != ESP_OK) {
        return error;
    }
    if (build.rings == 0U || build.points == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    /*
     * Counted from the file, so bounded before they are multiplied out:
     * size_t is thirty-two bits on the device and a long enough file wraps
     * the size to something small, which would then be written past.
     */
    if (build.rings > SOLAR_OS_MAP_LAYER_MAX_BYTES / 20U ||
        build.points > SOLAR_OS_MAP_LAYER_MAX_BYTES / 8U ||
        SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 20U +
                (size_t)build.points * 8U > SOLAR_OS_MAP_LAYER_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t size = SOLAR_OS_MAP_LAYER_HEADER +
                        (size_t)build.rings * 20U + (size_t)build.points * 8U;
    uint8_t *data = solar_os_memory_calloc(1U,
                                           size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.geojson");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(data, SOLAR_OS_MAP_LAYER_MAGIC, 4U);
    data[4] = (uint8_t)SOLAR_OS_MAP_LAYER_VERSION;
    /* Measured on the first scan, so it is known before the second. */
    const uint16_t spacing = solar_os_map_spacing_hundreds(&build.spacing);
    data[6] = (uint8_t)(spacing & 0xFFU);
    data[7] = (uint8_t)((spacing >> 8) & 0xFFU);
    write_u32(&data[8], build.rings);
    write_u32(&data[12], build.points);

    build.counts = &data[SOLAR_OS_MAP_LAYER_HEADER];
    build.bounds =
        &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 4U];
    build.coordinates =
        &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 20U];
    build.writing = true;
    build.ring_points = 0U;

    geojson_scan_t second = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .user = &build,
    };
    error = scan_file(reader, &second);
    if (error == ESP_OK &&
        (build.ring_index != build.rings || build.point_index != build.points)) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        solar_os_memory_free(data);
        return error;
    }
    *out = data;
    *out_size = size;
    return ESP_OK;
}

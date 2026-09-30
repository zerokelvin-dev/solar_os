#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Map geometry as rings of 1e7-degree coordinates. Layer zero is the world
 * outline compiled into the firmware, so a map draws a coastline with no
 * storage, no network, and no tile service. Loaded layers stack on top of
 * it in the order they were added.
 *
 * A ring's stored count carries SOLAR_OS_MAP_RING_OPEN when the ring is a
 * line rather than an area, so roads and tracks draw unfilled, and its
 * class in the bits below that.
 *
 * Layout: header, then a count per ring, then bounds per ring as four
 * coordinates, then the coordinates themselves.
 *
 * The header carries the layer's resolution, in hundreds of metres: the
 * typical distance between two of its vertices. A renderer zoomed in far
 * past it knows the shape it holds is no longer telling the truth there.
 * Zero means no limit, for geometry surveyed finer than it is ever drawn.
 */
#define SOLAR_OS_MAP_LAYER_MAGIC "SOMB"
#define SOLAR_OS_MAP_LAYER_VERSION 2U
#define SOLAR_OS_MAP_LAYER_HEADER 16U
#define SOLAR_OS_MAP_LAYER_MAX 4U
#define SOLAR_OS_MAP_LAYER_NAME_MAX 32U
#define SOLAR_OS_MAP_RING_OPEN 0x80000000UL
#define SOLAR_OS_MAP_RING_CLASS_SHIFT 24U
#define SOLAR_OS_MAP_RING_CLASS_MASK 0x7F000000UL
#define SOLAR_OS_MAP_RING_COUNT_MASK 0x00FFFFFFUL

/*
 * What a ring represents, so a renderer can tell water from land. A display
 * with colour uses it for colour; one without keeps drawing every class the
 * same, which is what the map looked like before classes existed.
 */
typedef enum {
    SOLAR_OS_MAP_CLASS_LAND = 0,
    SOLAR_OS_MAP_CLASS_WATER = 1,
    SOLAR_OS_MAP_CLASS_ROAD = 2,
    SOLAR_OS_MAP_CLASS_RAIL = 3,
    SOLAR_OS_MAP_CLASS_BUILDING = 4,
    SOLAR_OS_MAP_CLASS_BOUNDARY = 5,
    SOLAR_OS_MAP_CLASS_COUNT,
} solar_os_map_class_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t ring_count;
    uint32_t point_count;
    /* Typical spacing between vertices, in metres; zero means no limit. */
    uint32_t resolution_m;
} solar_os_map_geometry_t;

typedef struct {
    const int32_t *coordinates; /* latitude, longitude pairs */
    size_t point_count;
    bool open;
    solar_os_map_class_t klass;
    /* Bounds carried in the file so a ring off screen costs one compare
     * rather than a projection of every vertex it holds. */
    int32_t lat_min;
    int32_t lat_max;
    int32_t lon_min;
    int32_t lon_max;
} solar_os_map_ring_t;

/* The generated world outline compiled into the firmware. */
extern const uint8_t solar_os_map_basemap_world[];
extern const size_t solar_os_map_basemap_world_size;

esp_err_t solar_os_map_geometry_parse(const uint8_t *data,
                                      size_t size,
                                      solar_os_map_geometry_t *geometry);
/*
 * Rings are read in order through a cursor. Reaching one by index would
 * cost the sum of the lengths before it, which is quadratic over a layer
 * holding thousands of rings.
 */
typedef struct {
    uint32_t index;
    size_t offset;
} solar_os_map_ring_cursor_t;

bool solar_os_map_geometry_next(const solar_os_map_geometry_t *geometry,
                                solar_os_map_ring_cursor_t *cursor,
                                solar_os_map_ring_t *ring);
/* Vertices in the largest ring, so a renderer can size one scratch buffer. */
size_t solar_os_map_geometry_longest_ring(const solar_os_map_geometry_t *geometry);

/* Layer zero is the built-in world and is always present. */
size_t solar_os_map_layer_count(void);
const solar_os_map_geometry_t *solar_os_map_layer(size_t index);
const char *solar_os_map_layer_name(size_t index);
const char *solar_os_map_class_name(solar_os_map_class_t klass);
/* Largest ring across every layer. */
size_t solar_os_map_layer_longest_ring(void);

/* Adds a packed layer or a GeoJSON file on top of the layers already loaded. */
esp_err_t solar_os_map_layer_load(const char *path);
esp_err_t solar_os_map_layer_remove(size_t index);
void solar_os_map_layer_clear(void);

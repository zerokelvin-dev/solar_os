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
 * line rather than an area, so roads and tracks draw unfilled.
 */
#define SOLAR_OS_MAP_LAYER_MAGIC "SOMB"
#define SOLAR_OS_MAP_LAYER_VERSION 1U
#define SOLAR_OS_MAP_LAYER_HEADER 16U
#define SOLAR_OS_MAP_LAYER_MAX 4U
#define SOLAR_OS_MAP_LAYER_NAME_MAX 32U
#define SOLAR_OS_MAP_RING_OPEN 0x80000000UL

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t ring_count;
    uint32_t point_count;
} solar_os_map_geometry_t;

typedef struct {
    const int32_t *coordinates; /* latitude, longitude pairs */
    size_t point_count;
    bool open;
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
/* Largest ring across every layer. */
size_t solar_os_map_layer_longest_ring(void);

/* Adds a packed layer or a GeoJSON file on top of the layers already loaded. */
esp_err_t solar_os_map_layer_load(const char *path);
esp_err_t solar_os_map_layer_remove(size_t index);
void solar_os_map_layer_clear(void);

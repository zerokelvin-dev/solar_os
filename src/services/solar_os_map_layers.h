#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Imported layers live one file per layer, named for the layer. The
 * directory is the index: what is in it is what loads. */
#define SOLAR_OS_MAP_DIR ".map"
#define SOLAR_OS_MAP_LAYER_DIR ".map/layers"

/*
 * Map geometry as rings of 1e7-degree coordinates. Every layer is loaded
 * from storage and they stack in the order they were loaded, so a map is
 * only ever the maps the operator kept. Nothing is compiled in: geometry
 * belongs on the card, where it can be replaced without a new firmware.
 *
 * Layout: a header, then a count per ring, then bounds per ring as four
 * coordinates, then the coordinates. A ring's count carries
 * SOLAR_OS_MAP_RING_OPEN when the ring is a line rather than an area, and
 * its class in the bits below that.
 *
 * The header's resolution is the typical distance between two vertices, in
 * hundreds of metres, so a renderer can tell when it has zoomed in past
 * what the layer supports. Zero means no limit.
 */
#define SOLAR_OS_MAP_LAYER_MAGIC "SOMB"
#define SOLAR_OS_MAP_LAYER_VERSION 2U
#define SOLAR_OS_MAP_LAYER_HEADER 16U
/*
 * Slots, which are cheap: a slot is a name and a handful of counts. What is
 * actually scarce is the geometry they point at, so the slot count is set
 * generously and SOLAR_OS_MAP_LAYER_BUDGET_BYTES is what bites first.
 */
#define SOLAR_OS_MAP_LAYER_MAX 12U
/*
 * How much packed geometry may be held at once. Measured on an 8 MB PSRAM
 * board: a world map is 0.6 MiB, a quarter-degree cell of a dense city is
 * 1.2 to 1.7 MiB, and a rural one a fraction of that. Five leaves the app
 * its scratch buffers and the rest of the system its room, and holds a
 * world plus three city cells or a world plus a great many quiet ones.
 */
#define SOLAR_OS_MAP_LAYER_BUDGET_BYTES (5U * 1024U * 1024U)
/* The most one layer may claim to hold, so a bad header cannot ask for
 * more memory than the device has by writing a large number in it. */
#define SOLAR_OS_MAP_LAYER_MAX_BYTES (8U * 1024U * 1024U)
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
    /* A border within a country: a state, a province, a region. */
    SOLAR_OS_MAP_CLASS_REGION = 6,
    /*
     * A road somebody would not plan a journey along: drawn lighter, so the
     * roads that carry the traffic read first. Added after REGION rather
     * than beside ROAD so every layer packed before it still means what it
     * said - an old file simply has no minor roads in it.
     */
    SOLAR_OS_MAP_CLASS_ROAD_MINOR = 7,
    /*
     * A piece of the edge of a water area, which the source held as a
     * relation: many ways, in no order, facing either way, islands among
     * them. The pieces are not joined. A renderer fills all of a layer's
     * pieces together, since a scanline needs every edge of a closed
     * curve and not the curve in order, and the islands come out as holes
     * by the same parity. A piece is drawn as a line like any open ring.
     */
    SOLAR_OS_MAP_CLASS_WATER_EDGE = 8,
    /*
     * A motorway or trunk road: what a road atlas still shows when the
     * rest of the roads have gone. Its own class so that the crowding
     * rule keeps it on a regional view, and a new one rather than a
     * narrowing of ROAD, so a layer packed before it keeps its meaning.
     */
    SOLAR_OS_MAP_CLASS_HIGHWAY = 9,
    SOLAR_OS_MAP_CLASS_COUNT,
} solar_os_map_class_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t ring_count;
    uint32_t point_count;
    /* Typical spacing between vertices, in metres; zero means no limit. */
    uint32_t resolution_m;
    /* Everything the layer covers, so a renderer can tell how much of the
     * screen it is about to occupy before reading a single ring. */
    int32_t lat_min;
    int32_t lat_max;
    int32_t lon_min;
    int32_t lon_max;
    /* Rings of each class, so a renderer can thin out what there is most of
     * and keep what is scarce. A city holds a hundred times as many roads
     * as rivers, and it is the rivers that are worth the room. */
    uint32_t class_rings[SOLAR_OS_MAP_CLASS_COUNT];
    /* What each class covers, so how crowded a class is can be judged over
     * its own ground: a cell's roads cover the cell, while the edge of its
     * lake covers the whole lake and would make the roads look sparse. */
    int32_t class_lat_min[SOLAR_OS_MAP_CLASS_COUNT];
    int32_t class_lat_max[SOLAR_OS_MAP_CLASS_COUNT];
    int32_t class_lon_min[SOLAR_OS_MAP_CLASS_COUNT];
    int32_t class_lon_max[SOLAR_OS_MAP_CLASS_COUNT];
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

/* How many layers are loaded, which is none until one is. */
size_t solar_os_map_layer_count(void);
/*
 * Changes every time a layer is added or removed. A renderer holds pointers
 * into a layer's own memory, which removing it frees, so watching this lets
 * the renderer drop a frame rather than draw from freed memory.
 */
uint32_t solar_os_map_layer_generation(void);
const solar_os_map_geometry_t *solar_os_map_layer(size_t index);
const char *solar_os_map_layer_name(size_t index);
/* Largest ring across every layer. */
size_t solar_os_map_layer_longest_ring(void);

/*
 * A layer that has been kept but not read. Knowing what is on the card is
 * a directory listing; reading one in is megabytes, so the two are asked
 * for separately and nothing loads until somebody wants it.
 */
typedef struct {
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    uint64_t size_bytes;
    bool loaded;
} solar_os_map_stored_t;

size_t solar_os_map_layer_stored(solar_os_map_stored_t *stored, size_t limit);
/* How many are kept, which may be more than a listing was given room for. */
size_t solar_os_map_layer_stored_count(void);
/*
 * Loads a layer by whatever the reader has to hand: a path to a file
 * anywhere, the name of one already kept, or its place in the kept list.
 * A file from elsewhere is copied in first, so afterwards it is kept like
 * any other and can be named or numbered the same way.
 */
esp_err_t solar_os_map_layer_load_named(const char *what);
bool solar_os_map_layer_is_loaded(const char *name);
/*
 * Whether a map of this name is on the card. Walks the directory, so the
 * answer does not depend on how many are kept: a caller that listed them
 * into an array first could only see as many as the array held, and read
 * everything past that as absent.
 */
bool solar_os_map_layer_is_kept(const char *name);
/*
 * Keeps packed geometry as a map of this name, which is how anything that
 * fetches one hands it over. The suffix is added here, so where kept maps
 * live and what they are called stays the one thing that knows.
 */
esp_err_t solar_os_map_layer_keep(const char *name,
                                  const uint8_t *data,
                                  size_t size);
/* Gives back the memory of a loaded layer by name, keeping its file. */
esp_err_t solar_os_map_layer_unload(const char *name);
/* Throws away the kept copy. Unloading only gives back the memory. */
esp_err_t solar_os_map_layer_forget(const char *name);

/*
 * Adds a packed layer or a GeoJSON file on top of the layers already
 * loaded, and keeps a copy so it can be loaded again without fetching.
 */
esp_err_t solar_os_map_layer_load(const char *path);
/*
 * Says a layer is still wanted. Loading is what fills the budget and this
 * is what decides who leaves when it is full: the layer nobody has seen
 * for longest. A caller that draws should touch whatever covers its view,
 * including what it culled, or the world map will be evicted the moment
 * somebody zooms into a city and will not come back on the way out.
 */
void solar_os_map_layer_touch(size_t index);
/*
 * Keeps a layer through any eviction. The world is a backdrop rather than
 * a place: it covers every view, it is cheap, and a map without it is a
 * blank screen. Leaving that to whether it was drawn recently made it the
 * first thing evicted whenever nothing was drawing, which is backwards.
 */
void solar_os_map_layer_pin(size_t index);
bool solar_os_map_layer_pinned(size_t index);
/* Packed geometry held by every loaded layer, against the budget. */
size_t solar_os_map_layer_bytes(void);
esp_err_t solar_os_map_layer_remove(size_t index);
void solar_os_map_layer_clear(void);

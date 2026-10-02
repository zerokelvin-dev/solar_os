#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_map_layers.h"
#include "solar_os_map_project.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    return calloc(count, size);
}

void solar_os_memory_free(void *pointer)
{
    free(pointer);
}

/*
 * Reading a packed layer lives beside loading one, and loading one needs
 * storage. Nothing here loads anything, so these only have to exist.
 */
bool solar_os_storage_is_mounted(void) { return false; }
esp_err_t solar_os_storage_default_path(const char *relative_path,
                                        char *buffer,
                                        size_t capacity)
{
    (void)relative_path;
    (void)buffer;
    (void)capacity;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_storage_makedirs(const char *path, bool exist_ok)
{
    (void)path;
    (void)exist_ok;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_storage_remove(const char *path)
{
    (void)path;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_storage_copy_file(const char *source_path,
                                     const char *dest_path)
{
    (void)source_path;
    (void)dest_path;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_storage_scandir(const char *path,
                                   size_t cursor,
                                   size_t limit,
                                   solar_os_storage_entry_t *entries,
                                   size_t *entry_count,
                                   size_t *next_cursor,
                                   bool *has_more)
{
    (void)path;
    (void)cursor;
    (void)limit;
    (void)entries;
    (void)entry_count;
    (void)next_cursor;
    (void)has_more;
    return ESP_ERR_INVALID_STATE;
}

#define VERTEX_MAX 4096U
static solar_os_map_vertex_t vertices[VERTEX_MAX];

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/*
 * Builds a one-ring layer around the given points, so a test can talk
 * about a shape rather than about bytes.
 */
static uint8_t *pack_ring(const int32_t *lat_lon,
                          uint32_t points,
                          solar_os_map_class_t klass,
                          bool open,
                          uint32_t resolution_m,
                          size_t *out_size)
{
    const size_t size = SOLAR_OS_MAP_LAYER_HEADER + 20U +
                        (size_t)points * 8U;
    uint8_t *data = calloc(1U, size);
    assert(data != NULL);
    memcpy(data, SOLAR_OS_MAP_LAYER_MAGIC, 4U);
    data[4] = (uint8_t)SOLAR_OS_MAP_LAYER_VERSION;
    data[6] = (uint8_t)((resolution_m / 100U) & 0xFFU);
    data[7] = (uint8_t)(((resolution_m / 100U) >> 8) & 0xFFU);
    write_u32(&data[8], 1U);
    write_u32(&data[12], points);
    write_u32(&data[SOLAR_OS_MAP_LAYER_HEADER],
              points | ((uint32_t)klass << SOLAR_OS_MAP_RING_CLASS_SHIFT) |
                  (open ? SOLAR_OS_MAP_RING_OPEN : 0U));

    int32_t lat_min = lat_lon[0];
    int32_t lat_max = lat_lon[0];
    int32_t lon_min = lat_lon[1];
    int32_t lon_max = lat_lon[1];
    for (uint32_t i = 0U; i < points; i++) {
        const int32_t lat = lat_lon[i * 2U];
        const int32_t lon = lat_lon[i * 2U + 1U];
        lat_min = lat < lat_min ? lat : lat_min;
        lat_max = lat > lat_max ? lat : lat_max;
        lon_min = lon < lon_min ? lon : lon_min;
        lon_max = lon > lon_max ? lon : lon_max;
    }
    uint8_t *bounds = &data[SOLAR_OS_MAP_LAYER_HEADER + 4U];
    write_u32(&bounds[0], (uint32_t)lat_min);
    write_u32(&bounds[4], (uint32_t)lat_max);
    write_u32(&bounds[8], (uint32_t)lon_min);
    write_u32(&bounds[12], (uint32_t)lon_max);
    uint8_t *coords = &data[SOLAR_OS_MAP_LAYER_HEADER + 20U];
    for (uint32_t i = 0U; i < points; i++) {
        write_u32(&coords[i * 8U], (uint32_t)lat_lon[i * 2U]);
        write_u32(&coords[i * 8U + 4U], (uint32_t)lat_lon[i * 2U + 1U]);
    }
    *out_size = size;
    return data;
}

static bool first_ring(const uint8_t *data,
                       size_t size,
                       solar_os_map_geometry_t *geometry,
                       solar_os_map_ring_t *ring)
{
    assert(solar_os_map_geometry_parse(data, size, geometry) == ESP_OK);
    solar_os_map_ring_cursor_t cursor = {0};
    return solar_os_map_geometry_next(geometry, &cursor, ring);
}

/* The whole world on a small screen, where a turn is a few hundred px. */
static solar_os_map_view_t world_view(void)
{
    const solar_os_map_view_t view = {
        .center_lat_e7 = 0,
        .center_lon_e7 = 0,
        .meters_per_col = 200000U,
        .cols = 320U,
        .rows = 214U,
    };
    return view;
}

/*
 * A ring crossing the meridian opposite the centre must stay one shape.
 * Measuring each vertex afresh jumps a whole turn there and leaves two
 * pieces that cannot be filled, which is what carrying longitude fixes.
 */
static void test_a_ring_crossing_the_far_meridian_stays_whole(void)
{
    /* Four points stepping east across 180 degrees. */
    static const int32_t points[] = {
        100000000, 1700000000,
        100000000, 1750000000,
        100000000, -1750000000,
        100000000, -1700000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 4U, SOLAR_OS_MAP_CLASS_LAND, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));

    /* Centre the view on the far side, so the crossing is in front of it. */
    solar_os_map_view_t view = world_view();
    view.center_lon_e7 = 1800000000;
    solar_os_map_span_t span = {0};
    const size_t kept = solar_os_map_project_ring(&view, &ring, 0, 0, vertices,
                                                  VERTEX_MAX, &span);
    assert(kept == 4U);
    /* Every step is small: no vertex leaps a whole turn of the world. */
    for (size_t i = 1U; i < kept; i++) {
        const int step = vertices[i].x - vertices[i - 1U].x;
        assert(step > 0);
        assert(step < (int)view.cols);
    }
    free(data);
}

/*
 * A ring that travels more than a full turn has gone round a pole. Joining
 * its ends draws a chord the width of the world, which a scanline fill
 * reads as an edge on every row - the Antarctica stripes.
 */
static void test_a_ring_round_a_pole_is_closed_past_it(void)
{
    /*
     * Ten points marching west around the far south in 45 degree steps:
     * 405 degrees in all, so it passes its own start rather than merely
     * reaching it. Longitudes are wrapped into range as stored data is.
     */
    int32_t points[10 * 2];
    for (int i = 0; i < 10; i++) {
        int64_t lon = 1800000000LL - (int64_t)i * 450000000LL;
        while (lon < -1800000000LL) {
            lon += 3600000000LL;
        }
        points[i * 2] = -800000000;
        points[i * 2 + 1] = (int32_t)lon;
    }
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 10U, SOLAR_OS_MAP_CLASS_LAND, false, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));

    const solar_os_map_view_t view = world_view();
    solar_os_map_span_t span = {0};
    size_t kept = solar_os_map_project_ring(&view, &ring, 0, 0, vertices,
                                            VERTEX_MAX, &span);
    assert(kept == 10U);
    /* It came the whole way round, and said where. */
    assert(span.full_turn >= 3U);

    const size_t closed = solar_os_map_close_over_pole(&view, &ring, &span, 0,
                                                       vertices, VERTEX_MAX,
                                                       kept);
    /* Cut at the turn and given two vertices to close through the pole. */
    assert(closed == span.full_turn + 2U);
    /* Both of them sit off the screen, below a southern ring. */
    assert(vertices[closed - 1U].y > (int)view.rows);
    assert(vertices[closed - 2U].y > (int)view.rows);
    free(data);
}

/* An open ring is never closed over a pole: a line has no inside. */
static void test_an_open_ring_is_left_open(void)
{
    static const int32_t points[] = {
        -800000000, 1700000000,
        -800000000, 0,
        -800000000, -1700000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 3U, SOLAR_OS_MAP_CLASS_LAND, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    const solar_os_map_view_t view = world_view();
    solar_os_map_span_t span = {0};
    const size_t kept = solar_os_map_project_ring(&view, &ring, 0, 0, vertices,
                                                  VERTEX_MAX, &span);
    assert(solar_os_map_close_over_pole(&view, &ring, &span, 0, vertices,
                                        VERTEX_MAX, kept) == kept);
    free(data);
}

/* Vertices closer together than a step go, but the ends never do. */
static void test_thinning_keeps_both_ends(void)
{
    int32_t points[20 * 2];
    for (int i = 0; i < 20; i++) {
        points[i * 2] = 430000000 + i * 100;      /* a hair apart */
        points[i * 2 + 1] = -790000000 + i * 100;
    }
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 20U, SOLAR_OS_MAP_CLASS_LAND, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    const solar_os_map_view_t view = world_view();
    solar_os_map_span_t span = {0};

    /* No step keeps everything. */
    assert(solar_os_map_project_ring(&view, &ring, 0, 0, vertices, VERTEX_MAX,
                                     &span) == 20U);
    /* A step wider than the whole ring keeps only its two ends. */
    const size_t thin = solar_os_map_project_ring(&view, &ring, 1000000, 0,
                                                  vertices, VERTEX_MAX, &span);
    assert(thin == 2U);
    free(data);
}

/* A caller's buffer is never overrun, however many points a ring has. */
static void test_a_small_buffer_is_respected(void)
{
    int32_t points[64 * 2];
    for (int i = 0; i < 64; i++) {
        points[i * 2] = 430000000 + i * 1000000;
        points[i * 2 + 1] = -790000000 + i * 1000000;
    }
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 64U, SOLAR_OS_MAP_CLASS_LAND, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    const solar_os_map_view_t view = world_view();
    solar_os_map_span_t span = {0};
    assert(solar_os_map_project_ring(&view, &ring, 0, 0, vertices, 8U,
                                     &span) == 8U);
    free(data);
}

/* The row offset is added to every vertex, and only to the row. */
static void test_the_offset_moves_only_the_row(void)
{
    static const int32_t points[] = {
        430000000, -790000000,
        440000000, -780000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(points, 2U, SOLAR_OS_MAP_CLASS_LAND, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    const solar_os_map_view_t view = world_view();
    solar_os_map_span_t span = {0};
    solar_os_map_vertex_t plain[4];
    assert(solar_os_map_project_ring(&view, &ring, 0, 0, plain, 4U,
                                     &span) == 2U);
    assert(solar_os_map_project_ring(&view, &ring, 0, 14, vertices, VERTEX_MAX,
                                     &span) == 2U);
    for (size_t i = 0U; i < 2U; i++) {
        assert(vertices[i].x == plain[i].x);
        assert(vertices[i].y == plain[i].y + 14);
    }
    free(data);
}

/*
 * An area too small to see goes, whatever it is. This is what keeps a city
 * layer from projecting tens of thousands of buildings a pixel across.
 */
static void test_a_ring_smaller_than_a_pixel_goes(void)
{
    static const int32_t tiny[] = {
        430000000, -790000000,
        430000100, -790000000,
        430000100, -789999900,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(tiny, 3U, SOLAR_OS_MAP_CLASS_LAND, false, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 430000000;
    view.center_lon_e7 = -790000000;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    bool outline = true;
    assert(!solar_os_map_ring_wanted(&cull, &view, &geometry, &ring,
                                     &outline));
    free(data);
}

/* A ring nowhere near the view goes without being looked at. */
static void test_a_ring_outside_the_view_goes(void)
{
    static const int32_t far_away[] = {
        -300000000, 1500000000,
        -310000000, 1510000000,
        -300000000, 1510000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(far_away, 3U, SOLAR_OS_MAP_CLASS_LAND, false, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 430000000;
    view.center_lon_e7 = -790000000;
    view.meters_per_col = 100U;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    bool outline = true;
    assert(!solar_os_map_ring_wanted(&cull, &view, &geometry, &ring,
                                     &outline));
    free(data);
}

/*
 * Roads come in by scale and not by how crowded the cell is: two cells
 * are never equally crowded, and judged each on its own they switched
 * tiers at different zooms, with the seam between them showing. A
 * motorway is drawn at any scale; the lesser tiers each have theirs.
 */
static void test_road_tiers_come_in_by_scale(void)
{
    static const int32_t lane[] = {
        430000000, -790000000,
        431000000, -791000000,
    };
    static const struct {
        solar_os_map_class_t klass;
        uint32_t in;
        uint32_t out;
    } tiers[] = {
        {SOLAR_OS_MAP_CLASS_HIGHWAY, 100000U, 0U},
        {SOLAR_OS_MAP_CLASS_RAIL, 181U, 256U},
        {SOLAR_OS_MAP_CLASS_ROAD, 91U, 128U},
        {SOLAR_OS_MAP_CLASS_ROAD_MINOR, 45U, 64U},
    };
    for (size_t t = 0U; t < sizeof(tiers) / sizeof(tiers[0]); t++) {
        size_t size = 0U;
        uint8_t *data = pack_ring(lane, 2U, tiers[t].klass, true, 0U, &size);
        solar_os_map_geometry_t geometry;
        solar_os_map_ring_t ring;
        assert(first_ring(data, size, &geometry, &ring));
        solar_os_map_view_t view = world_view();
        view.center_lat_e7 = 430000000;
        view.center_lon_e7 = -790000000;
        solar_os_map_cull_t cull;
        bool outline = true;
        view.meters_per_col = tiers[t].in;
        solar_os_map_cull_prepare(&view, &geometry, &cull);
        assert(solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
        if (tiers[t].out != 0U) {
            view.meters_per_col = tiers[t].out;
            solar_os_map_cull_prepare(&view, &geometry, &cull);
            assert(!solar_os_map_ring_wanted(&cull, &view, &geometry, &ring,
                                             &outline));
        }
        free(data);
    }
}

/*
 * A line of any other kind is judged by how crowded its kind is over the
 * ground that kind covers - not the ground the whole layer covers, since a
 * cell carrying the edge of its lake covers the lake.
 */
static void test_a_class_is_crowded_over_its_own_ground(void)
{
    static const int32_t creek[] = {
        430000000, -790000000,
        431000000, -791000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(creek, 2U, SOLAR_OS_MAP_CLASS_WATER, true, 0U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 430000000;
    view.center_lon_e7 = -790000000;
    view.meters_per_col = 100U;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    bool outline = true;
    assert(solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    geometry.class_rings[SOLAR_OS_MAP_CLASS_WATER] = 1000000U;
    geometry.lat_min = 400000000;
    geometry.lat_max = 460000000;
    geometry.lon_min = -800000000;
    geometry.lon_max = -740000000;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    assert(!solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    free(data);
}


/* Several rings in one layer, each its own class and openness. */
static uint8_t *pack_rings(const int32_t *const *lat_lon,
                           const uint32_t *points,
                           const solar_os_map_class_t *klass,
                           const bool *open,
                           uint32_t rings,
                           size_t *out_size)
{
    uint32_t total = 0U;
    for (uint32_t r = 0U; r < rings; r++) {
        total += points[r];
    }
    const size_t size = SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 20U +
                        (size_t)total * 8U;
    uint8_t *data = calloc(1U, size);
    assert(data != NULL);
    memcpy(data, SOLAR_OS_MAP_LAYER_MAGIC, 4U);
    data[4] = (uint8_t)SOLAR_OS_MAP_LAYER_VERSION;
    write_u32(&data[8], rings);
    write_u32(&data[12], total);
    uint8_t *counts = &data[SOLAR_OS_MAP_LAYER_HEADER];
    uint8_t *bounds = &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 4U];
    uint8_t *coords = &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 20U];
    for (uint32_t r = 0U; r < rings; r++) {
        write_u32(&counts[r * 4U],
                  points[r] | ((uint32_t)klass[r] << SOLAR_OS_MAP_RING_CLASS_SHIFT) |
                      (open[r] ? SOLAR_OS_MAP_RING_OPEN : 0U));
        int32_t lat_min = lat_lon[r][0];
        int32_t lat_max = lat_lon[r][0];
        int32_t lon_min = lat_lon[r][1];
        int32_t lon_max = lat_lon[r][1];
        for (uint32_t i = 0U; i < points[r]; i++) {
            const int32_t lat = lat_lon[r][i * 2U];
            const int32_t lon = lat_lon[r][i * 2U + 1U];
            lat_min = lat < lat_min ? lat : lat_min;
            lat_max = lat > lat_max ? lat : lat_max;
            lon_min = lon < lon_min ? lon : lon_min;
            lon_max = lon > lon_max ? lon : lon_max;
            write_u32(coords, (uint32_t)lat);
            write_u32(coords + 4U, (uint32_t)lon);
            coords += 8U;
        }
        write_u32(&bounds[r * 16U], (uint32_t)lat_min);
        write_u32(&bounds[r * 16U + 4U], (uint32_t)lat_max);
        write_u32(&bounds[r * 16U + 8U], (uint32_t)lon_min);
        write_u32(&bounds[r * 16U + 12U], (uint32_t)lon_max);
    }
    *out_size = size;
    return data;
}

/*
 * A lake whose edge arrives in pieces fills by parity: every row the edge
 * crosses is crossed an even number of times, however the pieces are cut.
 * One piece here is 150 m long, under the two-pixel thinning step at this
 * scale; dropping it as too small to see broke every row it spanned, and
 * put a green streak across the harbour.
 */
static void test_water_edge_pieces_cross_each_row_evenly(void)
{
    /* A 2 km square, clockwise, cut into four pieces at the corners and
     * one sliver near the south-west corner; the island a 400 m square. */
    static const int32_t north[] = {430100000, -790100000, 430100000, -789900000};
    static const int32_t east[] = {430100000, -789900000, 429900000, -789900000};
    static const int32_t south[] = {429900000, -789900000, 429900000, -790086000};
    static const int32_t sliver[] = {429900000, -790086000, 429900000, -790100000};
    static const int32_t west[] = {429900000, -790100000, 430100000, -790100000};
    static const int32_t island[] = {
        430020000, -790020000, 430020000, -789980000,
        429980000, -789980000, 429980000, -790020000,
    };
    const int32_t *const rings[] = {north, east, south, sliver, west, island};
    const uint32_t points[] = {2U, 2U, 2U, 2U, 2U, 4U};
    const solar_os_map_class_t klass[] = {
        SOLAR_OS_MAP_CLASS_WATER_EDGE, SOLAR_OS_MAP_CLASS_WATER_EDGE,
        SOLAR_OS_MAP_CLASS_WATER_EDGE, SOLAR_OS_MAP_CLASS_WATER_EDGE,
        SOLAR_OS_MAP_CLASS_WATER_EDGE, SOLAR_OS_MAP_CLASS_WATER_EDGE,
    };
    const bool open[] = {true, true, true, true, true, false};
    size_t size = 0U;
    uint8_t *data = pack_rings(rings, points, klass, open, 6U, &size);
    solar_os_map_geometry_t geometry;
    assert(solar_os_map_geometry_parse(data, size, &geometry) == ESP_OK);

    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 430000000;
    view.center_lon_e7 = -790000000;
    view.meters_per_col = 100U;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);

    solar_os_map_vertex_t scratch[16];
    uint32_t crossings[4096];
    const size_t found = solar_os_map_water_crossings(
        &view, &geometry, &cull, 10, (int)view.rows, scratch, 16U, crossings,
        4096U);
    assert(found > 0U);

    size_t row_counts[256] = {0};
    for (size_t i = 0U; i < found; i++) {
        const int row = SOLAR_OS_MAP_CROSSING_ROW(crossings[i]);
        assert(row >= 0 && row < (int)view.rows);
        row_counts[row]++;
        /* Sorted: each row's crossings run left to right. */
        if (i > 0U && SOLAR_OS_MAP_CROSSING_ROW(crossings[i - 1U]) == row) {
            assert(SOLAR_OS_MAP_CROSSING_COL(crossings[i - 1U]) <=
                   SOLAR_OS_MAP_CROSSING_COL(crossings[i]));
        }
    }
    size_t lake_rows = 0U;
    size_t island_rows = 0U;
    for (size_t row = 0U; row < view.rows; row++) {
        assert(row_counts[row] % 2U == 0U);
        lake_rows += row_counts[row] >= 2U;
        island_rows += row_counts[row] == 4U;
    }
    /* As many rows of lake as the square is tall on this screen, and of
     * island as the island is. */
    int x = 0;
    int north_row = 0;
    int south_row = 0;
    solar_os_map_project_raw(&view, 430100000, -790000000, &x, &north_row);
    solar_os_map_project_raw(&view, 429900000, -790000000, &x, &south_row);
    const size_t square = (size_t)(south_row - north_row);
    solar_os_map_project_raw(&view, 430020000, -790000000, &x, &north_row);
    solar_os_map_project_raw(&view, 429980000, -790000000, &x, &south_row);
    const size_t islet = (size_t)(south_row - north_row);
    assert(lake_rows + 1U >= square && lake_rows <= square + 1U);
    assert(island_rows + 1U >= islet && island_rows <= islet + 1U);
    free(data);
}

/*
 * A piece of a water edge beside the view is kept: the far shore of a row
 * is what decides whether the near side of it is wet, and dropping the
 * piece that holds it would flip every row it crosses.
 */
static void test_a_water_edge_beside_the_view_is_kept(void)
{
    /* Due east of the view at its latitude, where a lake's far shore is. */
    static const int32_t far_shore[] = {
        429000000, -760000000,
        431000000, -760100000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(far_shore, 2U, SOLAR_OS_MAP_CLASS_WATER_EDGE,
                              true, 0U, &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 430000000;
    view.center_lon_e7 = -790000000;
    view.meters_per_col = 100U;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    bool outline = true;
    assert(solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    /* The same line as plain water is a line off the screen, and goes. */
    free(data);
    data = pack_ring(far_shore, 2U, SOLAR_OS_MAP_CLASS_WATER, true, 0U, &size);
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    assert(!solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    free(data);

    /* A piece a few metres long, well under a pixel at this scale, is
     * kept too: a gap in the curve breaks every row it spans. */
    static const int32_t dock[] = {
        430000000, -790000000,
        430000200, -790000200,
    };
    data = pack_ring(dock, 2U, SOLAR_OS_MAP_CLASS_WATER_EDGE, true, 0U, &size);
    assert(first_ring(data, size, &geometry, &ring));
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    assert(solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    free(data);
}

/*
 * A layer drawn far finer than it was surveyed keeps a land fill, which is
 * still right about what it contains, and loses the outline, which is not.
 * Anything else loses both: a coarse lake outline drawn over a city puts a
 * lake through the middle of it.
 */
static void test_a_coarse_edge_in_view_is_dropped(void)
{
    /* A ring spanning degrees, from a layer sampled every 40 km. */
    static const int32_t big[] = {
        420000000, -800000000,
        450000000, -800000000,
        450000000, -770000000,
        420000000, -770000000,
    };
    /*
     * The view sits on the ring's southern edge. That matters: a coarse
     * ring whose edges are nowhere near the view keeps its outline, because
     * the error is not on screen to see. Only an edge in view is wrong.
     */
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 420000000;
    view.center_lon_e7 = -785000000;
    view.meters_per_col = 100U;

    size_t size = 0U;
    for (unsigned pass = 0U; pass < 2U; pass++) {
        const solar_os_map_class_t klass =
            pass == 0U ? SOLAR_OS_MAP_CLASS_LAND : SOLAR_OS_MAP_CLASS_WATER;
        uint8_t *data = pack_ring(big, 4U, klass, false, 40000U, &size);
        solar_os_map_geometry_t geometry;
        solar_os_map_ring_t ring;
        assert(first_ring(data, size, &geometry, &ring));
        solar_os_map_cull_t cull;
        solar_os_map_cull_prepare(&view, &geometry, &cull);
        assert(cull.coarse);
        bool outline = true;
        const bool wanted =
            solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline);
        /*
         * Neither survives now. The fill's boundary is the outline, so a
         * land ring kept for its fill paints its coast in the same wrong
         * place the outline would have drawn it.
         */
        assert(!wanted);
        free(data);
    }
}

/*
 * The same coarse layer, viewed where none of its edges reach: the middle
 * of a continent is reliably land however crudely its coast was drawn, so
 * the ring keeps both its fill and its outline.
 */
static void test_a_coarse_ring_far_from_its_edges_is_kept(void)
{
    static const int32_t big[] = {
        400000000, -820000000,
        470000000, -820000000,
        470000000, -750000000,
        400000000, -750000000,
    };
    size_t size = 0U;
    uint8_t *data = pack_ring(big, 4U, SOLAR_OS_MAP_CLASS_LAND, false, 40000U,
                              &size);
    solar_os_map_geometry_t geometry;
    solar_os_map_ring_t ring;
    assert(first_ring(data, size, &geometry, &ring));

    /* Well inside the ring, at a zoom where the layer is still coarse. */
    solar_os_map_view_t view = world_view();
    view.center_lat_e7 = 435000000;
    view.center_lon_e7 = -785000000;
    view.meters_per_col = 100U;
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(&view, &geometry, &cull);
    assert(cull.coarse);
    bool outline = false;
    assert(solar_os_map_ring_wanted(&cull, &view, &geometry, &ring, &outline));
    assert(outline);
    free(data);
}

int main(void)
{
    test_a_ring_crossing_the_far_meridian_stays_whole();
    test_a_ring_round_a_pole_is_closed_past_it();
    test_an_open_ring_is_left_open();
    test_thinning_keeps_both_ends();
    test_a_small_buffer_is_respected();
    test_the_offset_moves_only_the_row();
    test_a_ring_smaller_than_a_pixel_goes();
    test_a_ring_outside_the_view_goes();
    test_a_water_edge_beside_the_view_is_kept();
    test_water_edge_pieces_cross_each_row_evenly();
    test_road_tiers_come_in_by_scale();
    test_a_class_is_crowded_over_its_own_ground();
    test_a_coarse_edge_in_view_is_dropped();
    test_a_coarse_ring_far_from_its_edges_is_kept();
    printf("map_project_test ok\n");
    return 0;
}

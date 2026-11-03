#include "solar_os_map_app.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_places.h"
#include "solar_os_map_base.h"
#include "solar_os_map_fetch.h"
#include "solar_os_map_layers.h"
#include "solar_os_map_project.h"
#include "solar_os_network.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"
#include "solar_os_timezone.h"
#include "solar_os_memory.h"

#define MAP_APP_SELF_POLL_MS 5000U
/* Long enough to read a line of text without having to catch it. */
#define MAP_APP_SAY_MS 6000U
/* Panning moves this fraction of the view, small enough that the shapes
 * stay readable from one step to the next. */
#define MAP_APP_PAN_DIVISOR 8
/* ...but never more than this fraction of the way round the world, or a
 * step zoomed right out crosses an ocean in one press. */
#define MAP_APP_PAN_WORLD_STEPS 22
/* Where the map opens with no position of its own: north enough that the
 * continents rather than the southern ocean fill the screen. */
#define MAP_APP_HOME_LAT_E7 200000000
/* The system status bar owns the top of a graphical session and draws over
 * whatever is under it, so the map starts below it and claims no title row
 * of its own. */
#define MAP_APP_HEADER_H 14
#define MAP_APP_INFO_H 12
/* Keeps a shift inside an int; past it the world is wider than any screen. */
#define MAP_APP_SHIFT_LIMIT 1.0e8F
/* Turns of the world either side of the view a ring may be placed at. */
#define MAP_APP_REPEAT_MAX 3
/* A cell is worth showing between these sizes: any wider and it is the
 * ground underfoot, any narrower and there is nothing to aim at. */
#define MAP_APP_CELL_MAX_SCREENS 2
#define MAP_APP_CELL_MIN_DIVISOR 4

/*
 * What the frame on the screen was drawn from. A tick finding all of it
 * unchanged has nothing to add by drawing it again. Nothing tells an app
 * that something else has painted over it, so a blind repaint stays as a
 * floor rather than a rate.
 */
typedef struct {
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_px;
    uint32_t map_generation;
    uint32_t layer_generation;
    uint32_t selected_id;
    size_t path_count;
    bool follow_self;
    bool feedback;
    bool valid;
    uint32_t ticks;
} map_app_painted_t;

typedef struct {
    solar_os_places_point_t *points;
    solar_os_places_path_t *paths;
    size_t path_count;
    solar_os_map_vertex_t *scratch;
    int *crossings;
    int *edges;
    size_t scratch_max;
    /* Where every piece of a water edge crosses a row, as row and column
     * in one word, gathered for the whole frame before any of it is
     * filled. */
    uint32_t *wet;
    /* The cell under the reticle, and what is known about it. */
    char cell_file[SOLAR_OS_MAP_LAYER_NAME_MAX];
    bool cell_kept;
    bool cell_loaded;
    /* A fetch runs elsewhere; these notice when it has finished. */
    bool watching_fetch;
    uint32_t fetch_generation;
    bool watching_base;
    uint32_t base_generation;
    /* Whether the row was showing work last tick, so its end redraws. */
    bool was_working;
    /* The cell a fetch was started for, so it can be drawn when it lands. */
    char fetching[SOLAR_OS_MAP_LAYER_NAME_MAX];
    uint32_t cell_generation;
    size_t count;
    size_t total;
    uint32_t generation;
    uint32_t selected_id;
    uint32_t last_self_poll_ms;
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_px;
    bool follow_self;
    bool centered;
    char feedback[64];
    /* When the message stops being shown. A message cleared after one frame
     * was gone before it could be read, which made every refusal invisible
     * and a fetch that failed look like one that never answered. */
    uint32_t feedback_until_ms;
    /* What the last frame was drawn from, so an unchanged tick can skip it. */
    map_app_painted_t painted;
} map_app_state_t;

static void *map_app_state;
#define map_app (*(map_app_state_t *)map_app_state)

static uint32_t map_app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const solar_os_places_point_t *map_app_self(void)
{
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].kind == SOLAR_OS_PLACES_KIND_SELF) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static const solar_os_places_point_t *map_app_point_by_id(uint32_t id)
{
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].id == id) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static const solar_os_places_point_t *map_app_selected(void)
{
    for (size_t i = 0; map_app.selected_id != 0 && i < map_app.count; i++) {
        if (map_app.points[i].id == map_app.selected_id) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

/*
 * Colour where the display has it, and the same black outline everywhere
 * else. A board with a monochrome surface draws exactly what it drew before
 * classes existed rather than a worse version of it.
 */
static bool map_app_colour(const solar_os_gfx_t *gfx)
{
    return solar_os_gfx_format(gfx) == SOLAR_OS_DISPLAY_FORMAT_INDEX8;
}

static solar_os_gfx_color_t map_app_class_color(const solar_os_gfx_t *gfx,
                                                solar_os_map_class_t klass)
{
    if (!map_app_colour(gfx)) {
        return SOLAR_OS_GFX_COLOR_BLACK;
    }
    /*
     * Colours sit on the palette's own levels, multiples of 51, so none
     * shift when quantised. Roads take their greys from the separate grey
     * ramp, whose levels are multiples of 255/22, and read against land
     * without the harshness of black. The roads worth planning a journey
     * along are darkest; the rest are a third of the way to white again,
     * so a dense city reads as a hierarchy rather than as a solid block.
     */
    switch (klass) {
    case SOLAR_OS_MAP_CLASS_WATER:
    case SOLAR_OS_MAP_CLASS_WATER_EDGE:
        return solar_os_gfx_rgb(153, 204, 255);
    case SOLAR_OS_MAP_CLASS_HIGHWAY:
        return solar_os_gfx_rgb(58, 58, 58);
    case SOLAR_OS_MAP_CLASS_ROAD:
        return solar_os_gfx_rgb(93, 93, 93);
    case SOLAR_OS_MAP_CLASS_ROAD_MINOR:
        return solar_os_gfx_rgb(116, 116, 116);
    case SOLAR_OS_MAP_CLASS_RAIL:
        return solar_os_gfx_rgb(102, 102, 102);
    case SOLAR_OS_MAP_CLASS_BUILDING:
        return solar_os_gfx_rgb(204, 153, 102);
    case SOLAR_OS_MAP_CLASS_BOUNDARY:
        return solar_os_gfx_rgb(150, 150, 150);
    case SOLAR_OS_MAP_CLASS_REGION:
        /* Lighter than a country's border, which it sits inside. */
        return solar_os_gfx_rgb(185, 185, 185);
    default:
        return solar_os_gfx_rgb(153, 255, 153);
    }
}

static int map_app_area_height(const solar_os_gfx_t *gfx)
{
    const int height = (int)solar_os_gfx_height(gfx) - MAP_APP_HEADER_H -
                       MAP_APP_INFO_H;
    return height > 0 ? height : 0;
}

static solar_os_map_view_t map_app_view(const solar_os_gfx_t *gfx)
{
    const solar_os_map_view_t view = {
        .center_lat_e7 = map_app.center_lat_e7,
        .center_lon_e7 = map_app.center_lon_e7,
        .meters_per_col = map_app.meters_per_px,
        .cols = solar_os_gfx_width(gfx),
        .rows = (size_t)map_app_area_height(gfx),
    };
    return view;
}

static void map_app_fit(const solar_os_gfx_t *gfx)
{
    if (gfx == NULL || map_app.count == 0U) {
        return;
    }
    int32_t lat[SOLAR_OS_PLACES_CAPACITY];
    int32_t lon[SOLAR_OS_PLACES_CAPACITY];
    for (size_t i = 0; i < map_app.count; i++) {
        lat[i] = map_app.points[i].latitude_e7;
        lon[i] = map_app.points[i].longitude_e7;
    }
    map_app.meters_per_px =
        solar_os_map_fit_scale(lat,
                               lon,
                               map_app.count,
                               map_app.center_lat_e7,
                               map_app.center_lon_e7,
                               solar_os_gfx_width(gfx),
                               (size_t)map_app_area_height(gfx));
}

static void map_app_center_on(const solar_os_places_point_t *point)
{
    map_app.center_lat_e7 = point->latitude_e7;
    map_app.center_lon_e7 = point->longitude_e7;
    map_app.centered = true;
}

static void map_app_refresh(void)
{
    map_app.count = solar_os_places_snapshot(map_app.points,
                                          SOLAR_OS_PLACES_CAPACITY,
                                          &map_app.total);
    map_app.path_count = solar_os_places_path_snapshot(map_app.paths,
                                                    SOLAR_OS_PLACES_PATH_CAPACITY,
                                                    NULL);
    solar_os_places_status_t status;
    if (solar_os_places_get_status(&status) == ESP_OK) {
        map_app.generation = status.generation;
    }
    if (map_app.selected_id != 0 && map_app_selected() == NULL) {
        map_app.selected_id = 0;
    }

    const solar_os_places_point_t *self = map_app_self();
    if (self != NULL && map_app.follow_self) {
        map_app_center_on(self);
    } else if (!map_app.centered && map_app.count > 0) {
        map_app_center_on(self != NULL ? self : &map_app.points[0]);
        map_app.follow_self = self != NULL;
    }
}

static void map_app_poll_self(void)
{
    map_app.last_self_poll_ms = map_app_now_ms();
    (void)solar_os_places_update_self();
}

static void map_app_format_age(uint32_t updated_ms, char *text, size_t text_len)
{
    const uint32_t seconds = (map_app_now_ms() - updated_ms) / 1000U;
    if (seconds < 90U) {
        snprintf(text, text_len, "%us", (unsigned)seconds);
    } else if (seconds < 5400U) {
        snprintf(text, text_len, "%um", (unsigned)((seconds + 30U) / 60U));
    } else {
        snprintf(text, text_len, "%uh", (unsigned)((seconds + 1800U) / 3600U));
    }
}

/*
 * Sized against whatever is loaded now, since layers can be added while the
 * app is open and their longest rings would otherwise be dropped.
 */
static void map_app_size_scratch(void)
{
    /* Two spare vertices, so a ring that goes round a pole has room for
     * the two points that close it along one. */
    const size_t longest = solar_os_map_layer_longest_ring() + 2U;
    if (longest <= 2U || longest <= map_app.scratch_max) {
        return;
    }
    solar_os_map_vertex_t *grown =
        solar_os_memory_calloc(longest,
                               sizeof(*grown),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "app.map.rings");
    int *crossings =
        solar_os_memory_calloc(longest,
                               sizeof(*crossings),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "app.map.fill");
    int *edges =
        solar_os_memory_calloc(longest,
                               sizeof(*edges),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "app.map.edges");
    if (grown == NULL || crossings == NULL || edges == NULL) {
        solar_os_memory_free(grown);
        solar_os_memory_free(crossings);
        solar_os_memory_free(edges);
        return;
    }
    solar_os_memory_free(map_app.scratch);
    solar_os_memory_free(map_app.crossings);
    solar_os_memory_free(map_app.edges);
    map_app.scratch = grown;
    map_app.crossings = crossings;
    map_app.edges = edges;
    map_app.scratch_max = longest;
}

/*
 * Fills a projected ring by scanlines. solar_os_gfx_fill_polygon takes
 * sixteen vertices and a coastline runs to hundreds.
 */
static void map_app_fill_ring(solar_os_gfx_t *gfx,
                              size_t count,
                              int top,
                              int bottom,
                              int height)
{
    if (map_app.crossings == NULL || map_app.edges == NULL || count < 3U) {
        return;
    }
    /* The strokes reach the band kept for the status bar, so the ground
     * has to as well, or the map draws roads over the colour of the sea. */
    if (top < 0) {
        top = 0;
    }
    if (bottom > MAP_APP_HEADER_H + height) {
        bottom = MAP_APP_HEADER_H + height;
    }
    /* Only edges crossing the visible rows can contribute, and zoomed into
     * a coastline almost none do. Found once rather than per row. */
    size_t edge_count = 0U;
    for (size_t i = 0U; i < count; i++) {
        const int ay = map_app.scratch[i].y;
        const int by = map_app.scratch[(i + 1U) % count].y;
        const int low = ay < by ? ay : by;
        const int high = ay < by ? by : ay;
        if (high > top && low < bottom) {
            map_app.edges[edge_count++] = (int)i;
        }
    }
    for (int row = top; row < bottom; row++) {
        size_t found = 0U;
        for (size_t e = 0U; e < edge_count && found < count; e++) {
            const size_t i = (size_t)map_app.edges[e];
            const solar_os_map_vertex_t *a = &map_app.scratch[i];
            const solar_os_map_vertex_t *b = &map_app.scratch[(i + 1U) % count];
            if ((a->y <= row && b->y > row) || (b->y <= row && a->y > row)) {
                const int span = b->y - a->y;
                map_app.crossings[found++] =
                    a->x + (row - a->y) * (b->x - a->x) / span;
            }
        }
        if (found < 2U) {
            continue;
        }
        for (size_t i = 1U; i < found; i++) {
            const int key = map_app.crossings[i];
            size_t j = i;
            while (j > 0U && map_app.crossings[j - 1U] > key) {
                map_app.crossings[j] = map_app.crossings[j - 1U];
                j--;
            }
            map_app.crossings[j] = key;
        }
        for (size_t i = 0U; i + 1U < found; i += 2U) {
            const int start = map_app.crossings[i];
            const int width = map_app.crossings[i + 1U] - start;
            if (width > 0) {
                solar_os_gfx_fill_rect(gfx, start, row, width, 1);
            }
        }
    }
}

/*
 * Draws the projected ring at every repeat of the world that reaches the
 * view. Carried longitude puts a ring at one place on an endless line
 * rather than wrapping onto the screen by itself, and at the widest scale
 * the world is narrower than the screen and tiles more than once.
 */
static void map_app_draw_ring(solar_os_gfx_t *gfx,
                              const solar_os_map_view_t *view,
                              const solar_os_map_ring_t *ring,
                              size_t kept,
                              bool outline,
                              bool fill)
{
    int left = map_app.scratch[0].x;
    int right = left;
    int top = map_app.scratch[0].y;
    int bottom = top;
    for (size_t point = 1U; point < kept; point++) {
        const solar_os_map_vertex_t *v = &map_app.scratch[point];
        left = v->x < left ? v->x : left;
        right = v->x > right ? v->x : right;
        top = v->y < top ? v->y : top;
        bottom = v->y > bottom ? v->y : bottom;
    }

    const int span = (int)view->cols;
    const float world_px = solar_os_map_world_px(view);
    int first = 0;
    int last = 0;
    if (world_px >= 1.0F) {
        first = (int)floorf((float)(-right) / world_px);
        last = (int)ceilf((float)(span - left) / world_px);
        first = first < -MAP_APP_REPEAT_MAX ? -MAP_APP_REPEAT_MAX : first;
        last = last > MAP_APP_REPEAT_MAX ? MAP_APP_REPEAT_MAX : last;
    }

    const size_t segments = ring->open ? kept - 1U : kept;
    int shifted = 0;
    for (int repeat = first; repeat <= last; repeat++) {
        const float offset = (float)repeat * world_px;
        if (offset < -MAP_APP_SHIFT_LIMIT || offset > MAP_APP_SHIFT_LIMIT) {
            continue;
        }
        const int shift = (int)offset;
        if (left + shift >= span || right + shift < 0) {
            continue;
        }
        if (shift != shifted) {
            for (size_t point = 0U; point < kept; point++) {
                map_app.scratch[point].x += shift - shifted;
            }
            shifted = shift;
        }
        if (fill && map_app_colour(gfx)) {
            map_app_fill_ring(gfx, kept, top, bottom, (int)view->rows);
        }
        if (!outline) {
            continue;
        }
        for (size_t point = 0U; point < segments; point++) {
            const solar_os_map_vertex_t *a = &map_app.scratch[point];
            const solar_os_map_vertex_t *b = &map_app.scratch[(point + 1U) % kept];
            solar_os_gfx_line(gfx, a->x, a->y, b->x, b->y);
        }
    }
}

/*
 * Rings of coordinates, so no tiles are needed. A city layer is tens of
 * thousands of segments, most of them too small or too far outside to see,
 * so everything that can be thrown away is thrown away before a vertex is
 * projected.
 */
/*
 * Rings of coordinates, so no tiles are needed. A city layer is tens of
 * thousands of segments, most of them too small or too crowded to see, so
 * everything that can be thrown away is thrown away before a vertex is
 * projected.
 */
/*
 * Crossings a frame may hold. A row through a harbour crosses every slip,
 * lagoon and bay from one end of the lake to the other, so there is no
 * sensible cap per row; one for the frame is a quarter of a megabyte and
 * tens of times what a city needs.
 */
#define MAP_APP_WET_MAX 32768U

/*
 * Fills the water whose edge a layer holds in pieces: the rows' crossings,
 * found and sorted by the projection, are filled between pairs.
 */
static void map_app_fill_water_edges(solar_os_gfx_t *gfx,
                                     const solar_os_map_view_t *view,
                                     const solar_os_map_geometry_t *geometry,
                                     const solar_os_map_cull_t *cull)
{
    const int top = MAP_APP_HEADER_H;
    if (map_app.wet == NULL) {
        map_app.wet = solar_os_memory_calloc(MAP_APP_WET_MAX, sizeof(*map_app.wet),
                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                             "app.map.wet");
        if (map_app.wet == NULL) {
            return;
        }
    }
    const size_t found = solar_os_map_water_crossings(
        view, geometry, cull, top, map_app_area_height(gfx), map_app.scratch,
        map_app.scratch_max, map_app.wet, MAP_APP_WET_MAX);
    solar_os_gfx_set_color(gfx, map_app_class_color(gfx, SOLAR_OS_MAP_CLASS_WATER));
    for (size_t i = 0U; i + 1U < found;) {
        const int row = SOLAR_OS_MAP_CROSSING_ROW(map_app.wet[i]);
        if (SOLAR_OS_MAP_CROSSING_ROW(map_app.wet[i + 1U]) != row) {
            i++;
            continue;
        }
        const int start = SOLAR_OS_MAP_CROSSING_COL(map_app.wet[i]);
        const int end = SOLAR_OS_MAP_CROSSING_COL(map_app.wet[i + 1U]);
        if (end > start) {
            solar_os_gfx_fill_rect(gfx, start, top + row, end - start, 1);
        }
        i += 2U;
    }
}

static void map_app_draw_geometry(solar_os_gfx_t *gfx,
                                  const solar_os_map_view_t *view,
                                  const solar_os_map_geometry_t *geometry)
{
    if (geometry == NULL || map_app.scratch == NULL) {
        return;
    }
    solar_os_map_cull_t cull;
    solar_os_map_cull_prepare(view, geometry, &cull);

    /* The water goes down first, under the roads, whatever order the
     * pieces of its edge arrived in. */
    if (geometry->class_rings[SOLAR_OS_MAP_CLASS_WATER_EDGE] != 0U &&
        map_app_colour(gfx)) {
        map_app_fill_water_edges(gfx, view, geometry, &cull);
    }

    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(geometry, &cursor, &ring)) {
        bool outline = true;
        if (!solar_os_map_ring_wanted(&cull, view, geometry, &ring, &outline)) {
            continue;
        }
        solar_os_map_span_t span;
        size_t kept = solar_os_map_project_ring(view, &ring, cull.step,
                                  MAP_APP_HEADER_H,
                                  map_app.scratch,
                                  map_app.scratch_max, &span);
        if (kept < 2U) {
            continue;
        }
        kept = solar_os_map_close_over_pole(view, &ring, &span,
                                        MAP_APP_HEADER_H,
                                        map_app.scratch,
                                        map_app.scratch_max, kept);
        solar_os_gfx_set_color(gfx, map_app_class_color(gfx, ring.klass));
        /* A closed piece of a water edge was filled with the rest; filled
         * alone it would paint an island blue. */
        const bool fill = !ring.open &&
                          ring.klass != SOLAR_OS_MAP_CLASS_WATER_EDGE;
        map_app_draw_ring(gfx, view, &ring, kept, outline, fill);
    }
}

/*
 * Drawing holds pointers into a layer's own memory, which unloading frees.
 * Holding the table still for a whole frame would make unloading wait on
 * the renderer, so the renderer watches the generation instead and gives up
 * the frame when it changes. The next tick draws from the table as it
 * stands. A change between the check and the read after it still loses,
 * which crashes rather than draws a wrong picture.
 */
/*
 * Whether any of the layer would land on the screen. This is what counts
 * as having been seen, rather than whether a ring survived the cull: a
 * world outline is culled away entirely when somebody zooms into a city,
 * and a layer evicted for that would leave nothing to draw on the way
 * back out.
 */
static bool map_app_layer_in_view(const solar_os_map_view_t *view,
                                  const solar_os_map_geometry_t *geometry)
{
    int32_t lat_min = 0;
    int32_t lat_max = 0;
    int32_t lon_half = 0;
    solar_os_map_view_bounds(view, &lat_min, &lat_max, &lon_half);
    if (geometry->lat_max < lat_min || geometry->lat_min > lat_max) {
        return false;
    }
    /* Longitude as a signed offset from the centre, so a layer spanning
     * the far meridian is not taken for one on the other side of it. */
    const int32_t west =
        solar_os_map_relative_lon_delta(view->center_lon_e7, geometry->lon_min);
    const int32_t east =
        solar_os_map_relative_lon_delta(view->center_lon_e7, geometry->lon_max);
    if (east < west) {
        /* It wraps, so it covers the centre by definition. */
        return true;
    }
    return east >= -lon_half && west <= lon_half;
}

/*
 * Whether anything in view was surveyed finely enough to place a coast at
 * this zoom. A layer that does not say how finely it was surveyed is taken
 * at its word, which is what layers packed before this measurement existed
 * get.
 */
static bool map_app_can_place_a_coast(const solar_os_map_view_t *view)
{
    for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
        const solar_os_map_geometry_t *geometry = solar_os_map_layer(index);
        if (geometry == NULL || !map_app_layer_in_view(view, geometry)) {
            continue;
        }
        solar_os_map_cull_t cull;
        solar_os_map_cull_prepare(view, geometry, &cull);
        if (!cull.coarse) {
            return true;
        }
    }
    return false;
}

/*
 * The world goes down first, then everything else in the order it was
 * loaded. Layers stack, and a city loaded before the world would otherwise
 * be painted over by the continent it sits on.
 */
static void map_app_draw_layers(solar_os_gfx_t *gfx,
                                const solar_os_map_view_t *view)
{
    const uint32_t generation = solar_os_map_layer_generation();
    map_app_size_scratch();
    for (int world = 1; world >= 0; world--) {
        for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
            if (solar_os_map_layer_generation() != generation) {
                return;
            }
            if (solar_os_map_layer_pinned(index) != (world != 0)) {
                continue;
            }
            const solar_os_map_geometry_t *geometry = solar_os_map_layer(index);
            if (map_app_layer_in_view(view, geometry)) {
                solar_os_map_layer_touch(index);
            }
            map_app_draw_geometry(gfx, view, geometry);
        }
    }
}

static void map_app_draw_point(solar_os_gfx_t *gfx,
                               const solar_os_places_point_t *point,
                               int x,
                               int y,
                               bool chosen)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_circle(gfx, x, y, 4);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    switch (point->kind) {
    case SOLAR_OS_PLACES_KIND_SELF:
        solar_os_gfx_fill_circle(gfx, x, y, 3);
        solar_os_gfx_circle(gfx, x, y, 5);
        break;
    case SOLAR_OS_PLACES_KIND_WAYPOINT:
        solar_os_gfx_line(gfx, x - 3, y, x + 3, y);
        solar_os_gfx_line(gfx, x, y - 3, x, y + 3);
        break;
    default:
        solar_os_gfx_circle(gfx, x, y, 3);
        break;
    }
    if (chosen) {
        solar_os_gfx_rect(gfx, x - 6, y - 6, 13, 13);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, x + 8, y + 4, point->label);
    }
}

/*
 * Whether a cell's file is already kept, and whether it is loaded. Asked of
 * the card rather than remembered, but only when the cell under the reticle
 * changes, since panning within one cell asks the same question.
 */
static bool map_app_cell_is_kept(const char *file)
{
    /* Asked again when the cell changes and when a layer comes or goes,
     * since either changes the answer and neither is frequent. Panning
     * inside one cell with nothing loading asks nothing. */
    const uint32_t generation = solar_os_map_layer_generation();
    if (strcmp(file, map_app.cell_file) != 0 ||
        generation != map_app.cell_generation) {
        map_app.cell_generation = generation;
        strlcpy(map_app.cell_file, file, sizeof(map_app.cell_file));
        /*
         * Asked of the store by name. Listing the kept maps into an array
         * first meant only as many could be seen as the array held, so a
         * cell kept after the twelfth read as absent and a press fetched
         * it again from a service that had already sent it.
         */
        map_app.cell_kept = solar_os_map_layer_is_kept(file);
        map_app.cell_loaded = solar_os_map_layer_is_loaded(file);
    }
    return map_app.cell_kept;
}

/*
 * What the map is waiting on, or nothing. Both downloads run on tasks of
 * their own, so the app stays responsive and only has to say so.
 */
static const char *map_app_working(void)
{
    if (solar_os_map_fetch_busy()) {
        return solar_os_map_fetch_status();
    }
    if (solar_os_map_base_busy()) {
        return solar_os_map_base_status();
    }
    return NULL;
}

static bool map_app_online(void)
{
    solar_os_network_path_info_t path;
    return solar_os_network_path_get_preferred(&path) && path.ready;
}

/*
 * The cell the reticle sits in, drawn as the area a map would be fetched or
 * loaded for. It appears only when it has something to offer: a file
 * already kept, or a network to fetch one over. With neither, an outline
 * promising a map that cannot arrive is worse than no outline.
 */
static void map_app_draw_cell(solar_os_gfx_t *gfx,
                              const solar_os_map_view_t *view,
                              int area)
{
    char name[SOLAR_OS_MAP_CELL_NAME_MAX];
    solar_os_map_cell_name(map_app.center_lat_e7, map_app.center_lon_e7,
                           name, sizeof(name));
    char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
    (void)snprintf(file, sizeof(file), "%s.bin", name);
    const bool kept = map_app_cell_is_kept(file);
    if (!kept && !map_app_online()) {
        return;
    }

    int32_t corner_lat = 0;
    int32_t corner_lon = 0;
    solar_os_map_cell_corner(map_app.center_lat_e7, map_app.center_lon_e7,
                             &corner_lat, &corner_lon);
    int left = 0;
    int bottom = 0;
    int right = 0;
    int top = 0;
    solar_os_map_project_raw(view, corner_lat, corner_lon, &left, &bottom);
    solar_os_map_project_raw(view, corner_lat + SOLAR_OS_MAP_CELL_E7,
                             corner_lon + SOLAR_OS_MAP_CELL_E7, &right, &top);
    const int wide = right - left;
    /*
     * Shown only while the cell is something you could act on. Wider than a
     * couple of screens it is the ground underfoot rather than a cell, and
     * narrower than a quarter of one there is nothing to read inside it and
     * nothing worth aiming at.
     */
    if (wide > (int)view->cols * MAP_APP_CELL_MAX_SCREENS ||
        wide < (int)view->cols / MAP_APP_CELL_MIN_DIVISOR) {
        return;
    }

    top += MAP_APP_HEADER_H;
    bottom += MAP_APP_HEADER_H;
    /* Faint, and nothing the land is drawn in: the edge of a cell is a
     * fact about the fetch, and grey read as one more road. */
    solar_os_gfx_set_color(gfx, map_app_colour(gfx)
                                    ? solar_os_gfx_rgb(255, 255, 102)
                                    : SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
    solar_os_gfx_rect(gfx, left, top, wide, bottom - top);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /*
     * A loaded cell says nothing: it is already on the map, and the map is
     * the label. The others say what a press would do.
     */
    if (map_app.cell_loaded) {
        return;
    }
    const char *action = solar_os_map_fetch_busy() ? "wait"
                         : kept                     ? "load"
                                                    : "fetch";
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    /* Centred on whatever of the cell is on screen, so the word stays put
     * as the edges run off it. */
    const int visible_left = left > 0 ? left : 0;
    const int visible_right = right < (int)view->cols ? right : (int)view->cols;
    const int visible_top = top > MAP_APP_HEADER_H ? top : MAP_APP_HEADER_H;
    const int visible_bottom =
        bottom < MAP_APP_HEADER_H + area ? bottom : MAP_APP_HEADER_H + area;
    const int width = (int)solar_os_gfx_text_width(gfx, action);
    const int x = visible_left + (visible_right - visible_left - width) / 2;
    const int y = visible_top + (visible_bottom - visible_top) / 2;
    solar_os_gfx_text(gfx, x, y, action);
}

/* A reticle at the centre, which panning moves the map under. The gap in
 * the middle keeps whatever is underneath visible. */
static void map_app_draw_centre(solar_os_gfx_t *gfx, int area)
{
    const int x = (int)solar_os_gfx_width(gfx) / 2;
    const int y = MAP_APP_HEADER_H + area / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, x - 5, y, x - 2, y);
    solar_os_gfx_line(gfx, x + 2, y, x + 5, y);
    solar_os_gfx_line(gfx, x, y - 5, x, y - 2);
    solar_os_gfx_line(gfx, x, y + 2, x, y + 5);
}

/*
 * The time where the reticle is, at the far end of the bar. The offset is
 * an hour for every fifteen degrees of longitude, which is a time zone
 * before politics. Naming the real zone would mean carrying every zone
 * boundary, so the label gives the offset it used instead.
 */
static void map_app_draw_clock(solar_os_gfx_t *gfx, int baseline, int width)
{
    solar_os_datetime_t utc;
    if (solar_os_time_get_utc_datetime(&utc) != ESP_OK ||
        !solar_os_time_datetime_is_valid(&utc)) {
        return;
    }
    int offset = (int)((map_app.center_lon_e7 + (map_app.center_lon_e7 < 0
                                                     ? -75000000
                                                     : 75000000)) /
                       150000000);
    if (offset > 12) {
        offset = 12;
    } else if (offset < -12) {
        offset = -12;
    }
    int hour = (int)utc.hour + offset;
    if (hour < 0) {
        hour += 24;
    } else if (hour >= 24) {
        hour -= 24;
    }
    char text[24];
    (void)snprintf(text, sizeof(text), "%02d:%02u UTC%+d",
                   hour, (unsigned)utc.minute, offset);
    const int text_width = (int)solar_os_gfx_text_width(gfx, text);
    const int x = width - text_width - 3;
    if (x < 4) {
        return;
    }
    solar_os_gfx_text(gfx, x, baseline, text);
}

/*
 * What to say when there is nothing to draw. A device that has never had a
 * world shows an empty sea, which looks like a fault rather than a state,
 * so it says which of the two things it is waiting for: a network, or the
 * download it is already doing over one.
 */
static void map_app_draw_empty(solar_os_gfx_t *gfx, int top, int area)
{
    const char *lines[2] = {NULL, NULL};
    if (solar_os_map_base_busy()) {
        lines[0] = "getting the world";
        lines[1] = solar_os_map_base_status();
    } else if (!map_app_online()) {
        lines[0] = "no world map yet";
        lines[1] = "connect to wifi to download maps";
    } else {
        lines[0] = "no world map yet";
        lines[1] = "run map base to download it";
    }
    const int width = (int)solar_os_gfx_width(gfx);
    int baseline = top + area / 2 - 4;
    for (size_t index = 0U; index < 2U; index++) {
        if (lines[index] == NULL || lines[index][0] == '\0') {
            continue;
        }
        const int text_width = (int)solar_os_gfx_text_width(gfx, lines[index]);
        int x = (width - text_width) / 2;
        if (x < 2) {
            x = 2;
        }
        solar_os_gfx_text(gfx, x, baseline, lines[index]);
        baseline += 11;
    }
}

/*
 * Credit for the data, where the map is drawn rather than in a menu nobody
 * opens. OpenStreetMap's licence asks for it whenever its data is shown,
 * and asks for the word OpenStreetMap specifically; the short form is used
 * when the row is too narrow for the long one.
 *
 * Shown whether or not an OpenStreetMap layer is loaded. The world and
 * border layers are Natural Earth, which is public domain and asks for
 * nothing, so this credits a source the reader may not be looking at;
 * crediting too often is the harmless direction, and a line that comes and
 * goes is worse to read than one that stays.
 *
 * The sign is written as an escape rather than as a byte, so the source
 * stays plain ASCII while the text reaching the display is UTF-8, which is
 * what the font and the drawing calls expect.
 */
static void map_app_draw_credit(solar_os_gfx_t *gfx, int baseline, int taken)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const char *credit = "\u00a9 OpenStreetMap contributors";
    int text = (int)solar_os_gfx_text_width(gfx, credit);
    if (taken + text + 6 > width) {
        credit = "\u00a9 OpenStreetMap";
        text = (int)solar_os_gfx_text_width(gfx, credit);
    }
    if (taken + text + 6 > width) {
        return;
    }
    solar_os_gfx_text(gfx, width - text - 3, baseline, credit);
}

static void map_app_draw_scale_bar(solar_os_gfx_t *gfx, int bottom)
{
    const solar_os_map_view_t view = map_app_view(gfx);
    const uint32_t meters = solar_os_map_view_resolution(&view);
    const int bar = 50;
    char distance[16];
    solar_os_map_format_distance(meters * (uint32_t)bar, distance, sizeof(distance));
    const int y = bottom - 6;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 6, y, 6 + bar, y);
    solar_os_gfx_line(gfx, 6, y - 3, 6, y + 3);
    solar_os_gfx_line(gfx, 6 + bar, y - 3, 6 + bar, y + 3);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6 + bar + 5, y + 3, distance);
    map_app_draw_credit(gfx, y + 3,
                        6 + bar + 5 + (int)solar_os_gfx_text_width(gfx, distance));
}

/* What the row says about a selected point: where it is, how far off and
 * which way, and how long ago it said so. */
static void map_app_describe_point(const solar_os_places_point_t *point,
                                   char *line,
                                   size_t capacity)
{
    char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
    char age[8];
    char away[32] = "";
    solar_os_map_format_coord(point->latitude_e7, point->longitude_e7, coord);
    map_app_format_age(point->updated_ms, age, sizeof(age));

    const solar_os_places_point_t *self = map_app_self();
    if (self != NULL && self->id != point->id) {
        char reach[16];
        solar_os_map_format_distance(
            solar_os_map_distance_m(self->latitude_e7, self->longitude_e7,
                                    point->latitude_e7, point->longitude_e7),
            reach, sizeof(reach));
        (void)snprintf(away, sizeof(away), " %s %03u", reach,
                       (unsigned)solar_os_map_bearing_deg(
                           self->latitude_e7, self->longitude_e7,
                           point->latitude_e7, point->longitude_e7));
    }
    (void)snprintf(line, capacity, "%s %s%s %s",
                   point->label, coord, away, age);
}

/*
 * Says something in the lower bar and keeps saying it long enough to be
 * read. Every refusal the map reports comes through here, which is what
 * makes a failed fetch tell the reader why instead of going quiet.
 */
static void map_app_say(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

static void map_app_say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)vsnprintf(map_app.feedback, sizeof(map_app.feedback), format, args);
    va_end(args);
    map_app.feedback_until_ms = map_app_now_ms() + MAP_APP_SAY_MS;
}

static void map_app_draw_info(solar_os_gfx_t *gfx, int top, int width)
{
    char line[96];
    const solar_os_places_point_t *point = map_app_selected();
    /*
     * Work in progress outranks anything else, and lasts as long as the
     * work does. A fetch takes about a minute; a message that expired
     * after a few seconds left the row back on coordinates while the
     * request was still running, which read as nothing having happened.
     */
    const char *working = map_app_working();
    if (working != NULL) {
        strlcpy(line, working, sizeof(line));
    } else if (map_app.feedback[0] != '\0') {
        strlcpy(line, map_app.feedback, sizeof(line));
    } else if (point != NULL) {
        map_app_describe_point(point, line, sizeof(line));
    } else {
        /* With nothing selected the centre is what the reader is aiming,
         * so the reticle's position is what the row should report. */
        const solar_os_map_view_t view = map_app_view(gfx);
        char centre[SOLAR_OS_MAP_COORD_TEXT_MAX];
        char distance[16];
        solar_os_map_format_coord(map_app.center_lat_e7,
                                  map_app.center_lon_e7, centre);
        solar_os_map_format_distance(solar_os_map_view_resolution(&view),
                                     distance, sizeof(distance));
        (void)snprintf(line, sizeof(line), "%s  %s/px%s", centre, distance,
                       map_app.follow_self ? "  following" : "");
    }

    const int baseline = top + MAP_APP_INFO_H - 3;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, top, width, MAP_APP_INFO_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 3, baseline, line);
    map_app_draw_clock(gfx, baseline, width);
}

/* A blind repaint stays a floor rather than a rate. */
#define MAP_APP_REPAINT_TICKS 15U

/* Whether the screen already holds what a frame drawn now would hold.
 * Records it either way, so it stays true until something moves. */
static bool map_app_frame_is_current(void)
{
    solar_os_places_status_t status;
    const uint32_t generation =
        solar_os_places_get_status(&status) == ESP_OK ? status.generation : 0U;
    const bool same =
        map_app.painted.valid &&
        map_app.painted.center_lat_e7 == map_app.center_lat_e7 &&
        map_app.painted.center_lon_e7 == map_app.center_lon_e7 &&
        map_app.painted.meters_per_px == map_app.meters_per_px &&
        map_app.painted.map_generation == generation &&
        map_app.painted.layer_generation == solar_os_map_layer_generation() &&
        map_app.painted.selected_id == map_app.selected_id &&
        map_app.painted.path_count == map_app.path_count &&
        map_app.painted.follow_self == map_app.follow_self &&
        map_app.painted.feedback == (map_app.feedback[0] != '\0');
    map_app.painted.center_lat_e7 = map_app.center_lat_e7;
    map_app.painted.center_lon_e7 = map_app.center_lon_e7;
    map_app.painted.meters_per_px = map_app.meters_per_px;
    map_app.painted.map_generation = generation;
    map_app.painted.layer_generation = solar_os_map_layer_generation();
    map_app.painted.selected_id = map_app.selected_id;
    map_app.painted.path_count = map_app.path_count;
    map_app.painted.follow_self = map_app.follow_self;
    map_app.painted.feedback = map_app.feedback[0] != '\0';
    map_app.painted.valid = true;
    return same;
}

static void map_app_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int area = map_app_area_height(gfx);
    const solar_os_map_view_t view = map_app_view(gfx);

    /*
     * Sea under everything, but only where something in view can say where
     * the sea is. A world outline sampled every fifty kilometres cannot
     * place a coastline at street level, and painting the ground as sea on
     * its word is what put a city under water. With nothing fine enough
     * loaded, the ground claims nothing and waits for a fetched cell.
     */
    const bool sea = !map_app_colour(gfx) || map_app_can_place_a_coast(&view);
    solar_os_gfx_clear(gfx, !map_app_colour(gfx) ? SOLAR_OS_GFX_COLOR_WHITE
                            : sea ? solar_os_gfx_rgb(204, 255, 255)
                                  : solar_os_gfx_rgb(255, 255, 255));
    map_app_draw_layers(gfx, &view);

    /* Paths sit under the markers, dashed so they read as links rather
     * than as terrain. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
    for (size_t i = 0U; i < map_app.path_count; i++) {
        const solar_os_places_point_t *from =
            map_app_point_by_id(map_app.paths[i].from_id);
        const solar_os_places_point_t *to =
            map_app_point_by_id(map_app.paths[i].to_id);
        if (from == NULL || to == NULL) {
            continue;
        }
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        solar_os_map_project_raw(&view, from->latitude_e7, from->longitude_e7,
                                 &x0, &y0);
        solar_os_map_project_raw(&view, to->latitude_e7, to->longitude_e7,
                                 &x1, &y1);
        if (x1 - x0 > (int)view.cols || x0 - x1 > (int)view.cols) {
            continue;
        }
        solar_os_gfx_line(gfx, x0, y0 + MAP_APP_HEADER_H, x1,
                          y1 + MAP_APP_HEADER_H);
    }
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);

    const solar_os_places_point_t *selected = map_app_selected();
    /* Oldest first so the newest position draws on top. */
    for (size_t i = map_app.count; i-- > 0;) {
        const solar_os_places_point_t *point = &map_app.points[i];
        size_t col = 0U;
        size_t row = 0U;
        if (!solar_os_map_project(&view,
                                  point->latitude_e7,
                                  point->longitude_e7,
                                  &col,
                                  &row)) {
            continue;
        }
        map_app_draw_point(gfx,
                           point,
                           (int)col,
                           (int)row + MAP_APP_HEADER_H,
                           selected != NULL && selected->id == point->id);
    }

    if (solar_os_map_layer_count() == 0U) {
        map_app_draw_empty(gfx, MAP_APP_HEADER_H, area);
    }
    map_app_draw_cell(gfx, &view, area);
    map_app_draw_centre(gfx, area);
    map_app_draw_scale_bar(gfx, MAP_APP_HEADER_H + area);
    map_app_draw_info(gfx, height - MAP_APP_INFO_H, width);
    solar_os_gfx_present(gfx);
}

static void map_app_select_next(bool forward)
{
    if (map_app.count == 0) {
        map_app.selected_id = 0;
        return;
    }
    size_t index = map_app.count;
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].id == map_app.selected_id) {
            index = i;
            break;
        }
    }
    if (index == map_app.count) {
        index = forward ? 0U : map_app.count - 1U;
    } else if (forward) {
        index = (index + 1U) % map_app.count;
    } else {
        index = (index + map_app.count - 1U) % map_app.count;
    }
    map_app.selected_id = map_app.points[index].id;
}

static void map_app_pan(solar_os_context_t *ctx, int columns, int rows)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const solar_os_map_view_t view = map_app_view(gfx);
    const float world_px = solar_os_map_world_px(&view);
    const int world_step = world_px > 0.0F
                               ? (int)(world_px / MAP_APP_PAN_WORLD_STEPS)
                               : 0;
    int across = (int)(view.cols / MAP_APP_PAN_DIVISOR);
    int down = (int)(view.rows / MAP_APP_PAN_DIVISOR);
    if (world_step > 0 && across > world_step) {
        across = world_step;
    }
    if (world_step > 0 && down > world_step) {
        down = world_step;
    }
    solar_os_map_pan(&view,
                     columns * (across > 0 ? across : 1),
                     -rows * (down > 0 ? down : 1),
                     &map_app.center_lat_e7,
                     &map_app.center_lon_e7);
    map_app.follow_self = false;
    map_app.centered = true;
}

static void map_app_zoom(int direction)
{
    map_app.meters_per_px =
        solar_os_map_scale_step(map_app.meters_per_px, direction);
}

/*
 * Where the map was looking when it was last closed. Outlives the app's own
 * state, which is wiped on every start, but not a restart.
 */
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("map view retained between app launches")
static struct {
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_px;
    bool valid;
} map_app_last_view;

/* The whole world, centred, filling as much of the screen as it fits. */
static void map_app_world_view(const solar_os_gfx_t *gfx)
{
    /*
     * Opening on the equator at the scale that fits the whole world spends
     * half the screen on empty southern ocean. A step closer, centred in
     * the northern hemisphere, puts the land where the reader is looking.
     */
    map_app.center_lat_e7 = MAP_APP_HOME_LAT_E7;
    map_app.center_lon_e7 = 0;
    map_app.meters_per_px = solar_os_map_scale_step(
        solar_os_map_world_scale(
            gfx != NULL ? solar_os_gfx_width(gfx) : 0U,
            gfx != NULL ? (size_t)map_app_area_height(gfx) : 0U),
        -1);
    map_app.centered = false;
    map_app.follow_self = false;
}

/*
 * The board's own map key, pressed with the map already open: where you
 * are, if that is known, and otherwise the world. The same key opened the
 * map from the shell, so pressing it again means "show me the map", not
 * "show me more of this corner of it".
 */
static void map_app_go_home(solar_os_context_t *ctx)
{
    map_app_poll_self();
    map_app_refresh();
    const solar_os_places_point_t *self = map_app_self();
    if (self != NULL) {
        map_app_center_on(self);
        map_app.follow_self = true;
        return;
    }
    map_app_world_view(solar_os_context_gfx(ctx));
}

static void map_app_center_selected(void)
{
    const solar_os_places_point_t *point = map_app_selected();
    if (point == NULL) {
        point = map_app_self();
    }
    if (point == NULL) {
        map_app_say("nothing to centre on");
        return;
    }
    map_app_center_on(point);
    map_app.follow_self = point->kind == SOLAR_OS_PLACES_KIND_SELF;
}

static void map_app_delete_selected(void)
{
    const solar_os_places_point_t *point = map_app_selected();
    if (point == NULL) {
        map_app_say("no point selected");
        return;
    }
    if (solar_os_places_remove(point->id) == ESP_OK) {
        map_app_say("point removed");
    }
    map_app.selected_id = 0;
    map_app_refresh();
}

static esp_err_t map_app_start(solar_os_context_t *ctx)
{
    memset(&map_app, 0, sizeof(map_app));
    esp_err_t err = solar_os_places_init();
    if (err != ESP_OK) {
        return err;
    }
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    map_app.points = solar_os_memory_calloc(SOLAR_OS_PLACES_CAPACITY,
                                            sizeof(*map_app.points),
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "app.map");
    map_app.paths = solar_os_memory_calloc(SOLAR_OS_PLACES_PATH_CAPACITY,
                                           sizeof(*map_app.paths),
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "app.map.paths");
    if (map_app.points == NULL || map_app.paths == NULL) {
        solar_os_memory_free(map_app.points);
        solar_os_memory_free(map_app.paths);
        memset(&map_app, 0, sizeof(map_app));
        return ESP_ERR_NO_MEM;
    }
    map_app_size_scratch();
    /* An indexed surface, and so colour, is only allocated for an app that
     * says it is drawing. */
    solar_os_context_set_graphics_active(ctx, true);
    if (map_app_last_view.valid) {
        map_app.center_lat_e7 = map_app_last_view.center_lat_e7;
        map_app.center_lon_e7 = map_app_last_view.center_lon_e7;
        map_app.meters_per_px = map_app_last_view.meters_per_px;
        map_app.centered = true;
    } else {
        map_app_world_view(solar_os_context_gfx(ctx));
    }
    /*
     * A map without the world is a blank screen with a city on it, which
     * is nobody's idea of a map, however the city got there. The world
     * that is kept is loaded whatever else already is, and on a device
     * that has never had one it is asked for, once, over whatever network
     * there is. A quarter of a megabyte buys every coastline and border.
     */
    if (solar_os_map_base_kept() > 0U) {
        (void)solar_os_map_base_load();
    } else if (map_app_online() && solar_os_map_base_fetch() == ESP_OK) {
        map_app.watching_base = true;
        map_app.base_generation = solar_os_map_base_generation();
    }
    map_app_poll_self();
    map_app_refresh();
    map_app_render(ctx);
    return ESP_OK;
}

static void map_app_forget_frame(void)
{
    map_app.painted.valid = false;
    map_app.painted.ticks = 0U;
}

static void map_app_remember_view(void)
{
    if (map_app.meters_per_px == 0U) {
        return;
    }
    map_app_last_view.center_lat_e7 = map_app.center_lat_e7;
    map_app_last_view.center_lon_e7 = map_app.center_lon_e7;
    map_app_last_view.meters_per_px = map_app.meters_per_px;
    map_app_last_view.valid = true;
}

static void map_app_stop(solar_os_context_t *ctx)
{
    map_app_remember_view();
    solar_os_context_set_graphics_active(ctx, false);
    solar_os_memory_free(map_app.points);
    solar_os_memory_free(map_app.paths);
    solar_os_memory_free(map_app.scratch);
    solar_os_memory_free(map_app.crossings);
    solar_os_memory_free(map_app.edges);
    solar_os_memory_free(map_app.wet);
    memset(&map_app, 0, sizeof(map_app));
}

static void map_app_suspend(solar_os_context_t *ctx)
{
    map_app_remember_view();
    solar_os_context_set_graphics_active(ctx, false);
}

static void map_app_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    map_app_forget_frame();
    /* A resume onto state that was never started has no scale yet. */
    if (map_app.meters_per_px == 0U) {
        map_app_world_view(solar_os_context_gfx(ctx));
    }
    map_app_refresh();
    map_app_render(ctx);
}

static void map_app_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "map", buffer_len);
    }
}

/*
 * A press on the cell under the reticle does the one thing that cell needs:
 * put away what is loaded, load what is kept, and ask OpenStreetMap for a
 * cell that is neither. The asking happens on a task of its own, so the
 * press returns and the map keeps drawing while the answer arrives.
 */
static void map_app_cell_press(solar_os_context_t *ctx)
{
    char name[SOLAR_OS_MAP_CELL_NAME_MAX];
    solar_os_map_cell_name(map_app.center_lat_e7, map_app.center_lon_e7,
                           name, sizeof(name));
    char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
    (void)snprintf(file, sizeof(file), "%s.bin", name);
    const bool kept = map_app_cell_is_kept(file);

    if (map_app.cell_loaded) {
        (void)solar_os_map_layer_unload(file);
        map_app.cell_file[0] = '\0';
        map_app_say("%s unloaded", name);
        return;
    }
    if (!kept) {
        if (!map_app_online()) {
            map_app_say("%s needs a network", name);
            return;
        }
        /* Started, not waited for: the answer takes tens of seconds, and
         * waiting here would stop the map redrawing or being left. */
        if (solar_os_map_fetch_busy()) {
            map_app_say("already fetching, this takes a minute");
            return;
        }
        if (solar_os_map_fetch_cell(map_app.center_lat_e7,
                                    map_app.center_lon_e7) != ESP_OK) {
            map_app_say("%s", solar_os_map_fetch_status());
            return;
        }
        map_app.fetch_generation = solar_os_map_fetch_generation();
        map_app.watching_fetch = true;
        strlcpy(map_app.fetching, file, sizeof(map_app.fetching));
        map_app_say("fetching %s", name);
        return;
    }

    const esp_err_t error = solar_os_map_layer_load_named(file);
    map_app.cell_file[0] = '\0';
    map_app_say("%s %s", name,
                   error == ESP_OK ? "loaded" : "would not load");
}

static bool map_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (map_app_now_ms() - map_app.last_self_poll_ms >= MAP_APP_SELF_POLL_MS) {
            map_app_poll_self();
        }
        if (map_app.feedback[0] != '\0' &&
            (int32_t)(map_app_now_ms() - map_app.feedback_until_ms) >= 0) {
            map_app.feedback[0] = '\0';
            map_app_forget_frame();
        }
        /* The words change as the work moves between asking and packing. */
        const bool working = map_app_working() != NULL;
        if (working || map_app.was_working) {
            map_app_forget_frame();
        }
        map_app.was_working = working;
        solar_os_places_status_t status;
        if (solar_os_places_get_status(&status) == ESP_OK &&
            status.generation != map_app.generation) {
            map_app_refresh();
        }
        if (map_app.watching_base &&
            solar_os_map_base_generation() != map_app.base_generation) {
            map_app.watching_base = false;
            (void)solar_os_map_base_load();
            map_app_say("%s", solar_os_map_base_status());
            map_app_forget_frame();
        }
        if (map_app.watching_fetch &&
            solar_os_map_fetch_generation() != map_app.fetch_generation) {
            map_app.watching_fetch = false;
            /*
             * One press asked for a map, so one press gets a map: what the
             * fetch brought back is drawn rather than waiting to be asked
             * for a second time.
             */
            if (map_app.fetching[0] != '\0' &&
                solar_os_map_layer_is_kept(map_app.fetching) &&
                solar_os_map_layer_load_named(map_app.fetching) == ESP_OK) {
                map_app_say("%s loaded", map_app.fetching);
            } else {
                map_app_say("%s", solar_os_map_fetch_status());
            }
            map_app.fetching[0] = '\0';
            /* Whatever it found, what is on the card has changed. */
            map_app.cell_file[0] = '\0';
            map_app_forget_frame();
        }
        map_app.painted.ticks++;
        if (map_app_frame_is_current() &&
            map_app.painted.ticks < MAP_APP_REPAINT_TICKS) {
            return true;
        }
        map_app.painted.ticks = 0U;
        map_app_render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ENTER:
        map_app_cell_press(ctx);
        map_app_forget_frame();
        map_app_render(ctx);
        return true;
    case SOLAR_OS_KEY_UP:
    case 'k':
        map_app_pan(ctx, 0, 1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        map_app_pan(ctx, 0, -1);
        break;
    case SOLAR_OS_KEY_LEFT:
    case 'h':
        map_app_pan(ctx, -1, 0);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case 'l':
        map_app_pan(ctx, 1, 0);
        break;
    case '+':
    case '=':
        map_app_zoom(-1);
        break;
    case '-':
    case '_':
        map_app_zoom(1);
        break;
    case '\t':
    case 'n':
        map_app_select_next(true);
        break;
    case 'p':
        map_app_select_next(false);
        break;
    case 'c':
    case 'C':
        map_app_center_selected();
        break;
    case 'f':
    case 'F':
        map_app_fit(solar_os_context_gfx(ctx));
        break;
    case SOLAR_OS_KEY_F2:
        map_app_go_home(ctx);
        break;
    case 'd':
    case 'D':
        map_app_delete_selected();
        break;
    case 'r':
    case 'R':
        map_app_poll_self();
        map_app_refresh();
        break;
    default:
        return true;
    }

    map_app_render(ctx);
    return true;
}

const solar_os_app_t solar_os_map_app = {
    .name = "map",
    .summary = "plot positions from GNSS, radios and networks",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = map_app_start,
    .suspend = map_app_suspend,
    .resume = map_app_resume,
    .stop = map_app_stop,
    .event = map_app_event,
    .title = map_app_title,
    .state_slot = &map_app_state,
    .state_size = sizeof(map_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 1000U,
    .tick_deadline_ms = 150U,
};

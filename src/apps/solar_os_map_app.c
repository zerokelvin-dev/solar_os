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
/* Narrower than this on screen, a cell is too small to aim at. */
#define MAP_APP_CELL_MIN_PX 20

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
    bool labels_hidden;
    bool feedback;
    /* The progress line, which changes while nothing else does. */
    char working[64];
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
    /* The cell under the reticle, and what is known about each tier of it. */
    char cell_name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    bool tier_kept[SOLAR_OS_MAP_TIER_COUNT];
    bool tier_loaded[SOLAR_OS_MAP_TIER_COUNT];
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
    bool labels_hidden;
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
    case SOLAR_OS_MAP_CLASS_STREET:
        return solar_os_gfx_rgb(139, 139, 139);
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
    if ((geometry->class_rings[SOLAR_OS_MAP_CLASS_WATER_EDGE] != 0U ||
         geometry->class_rings[SOLAR_OS_MAP_CLASS_WATER_SEAM] != 0U) &&
        map_app_colour(gfx)) {
        map_app_fill_water_edges(gfx, view, geometry, &cull);
    }

    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(geometry, &cursor, &ring)) {
        bool outline = true;
        /* A seam only closes a shore for the fill; it is not a shore. */
        if (ring.klass == SOLAR_OS_MAP_CLASS_WATER_SEAM ||
            !solar_os_map_ring_wanted(&cull, view, geometry, &ring, &outline)) {
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
            } else if (world == 0) {
                /* An out-of-view cell adds nothing, since it closes its own
                 * water with WATER_SEAM. World layers are still drawn, as
                 * the far-shore rule in solar_os_map_project.c needs them. */
                continue;
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
    }
}

/* The small font's box, and where its baseline sits in it. */
#define MAP_APP_LABEL_HEIGHT 9
#define MAP_APP_LABEL_BASELINE 7
#define MAP_APP_LABEL_MAX_PX 96

/* What a point is called on the map, or NULL for a point left unlabelled:
 * your own position is "you", a place you added is its own name, and the
 * selected point is named whatever it is. */
static const char *map_app_label_text(const solar_os_places_point_t *point,
                                      bool chosen)
{
    if (point->kind == SOLAR_OS_PLACES_KIND_SELF) {
        return "you";
    }
    if (chosen || strcmp(point->source, SOLAR_OS_PLACES_SOURCE_USER) == 0) {
        return point->label[0] != '\0' ? point->label : NULL;
    }
    return NULL;
}

/* Cut to the width cap, ending in dots, so one long name cannot cross the
 * screen. */
static void map_app_fit_label(solar_os_gfx_t *gfx, const char *text, char *out,
                              size_t out_len)
{
    strlcpy(out, text, out_len);
    if ((int)solar_os_gfx_text_width(gfx, out) <= MAP_APP_LABEL_MAX_PX) {
        return;
    }
    size_t len = strlen(out);
    while (len > 0U) {
        out[--len] = '\0';
        char cut[SOLAR_OS_PLACES_LABEL_MAX + 4U];
        (void)snprintf(cut, sizeof(cut), "%s...", out);
        if ((int)solar_os_gfx_text_width(gfx, cut) <= MAP_APP_LABEL_MAX_PX) {
            strlcpy(out, cut, out_len);
            return;
        }
    }
}

typedef struct {
    size_t point;
    int x;
    int y;
    uint32_t rank;
} map_app_label_pick_t;

static int map_app_compare_pick(const void *a, const void *b)
{
    const uint32_t x = ((const map_app_label_pick_t *)a)->rank;
    const uint32_t y = ((const map_app_label_pick_t *)b)->rank;
    return x < y ? -1 : x > y;
}

/*
 * The label goes on after every marker, so none sits under it, for the
 * labelled point nearest the reticle only: the one being aimed at. It sits
 * to the right of its marker if there is room, otherwise left, above or
 * below, and is left off if none of those is clear.
 */
static void map_app_draw_labels(solar_os_gfx_t *gfx,
                                const int *markers,
                                const size_t *marker_points,
                                size_t marker_count,
                                int area_top,
                                int area_bottom)
{
    if (map_app.labels_hidden || marker_count == 0U) {
        return;
    }
    map_app_label_pick_t *picks = solar_os_memory_calloc(
        marker_count, sizeof(*picks), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "app.map.labels");
    solar_os_map_label_t *labels = solar_os_memory_calloc(
        marker_count, sizeof(*labels), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "app.map.labels");
    char (*texts)[SOLAR_OS_PLACES_LABEL_MAX + 4U] = solar_os_memory_calloc(
        marker_count, sizeof(*texts), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "app.map.labels");
    if (picks == NULL || labels == NULL || texts == NULL) {
        solar_os_memory_free(picks);
        solar_os_memory_free(labels);
        solar_os_memory_free(texts);
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int cx = width / 2;
    const int cy = (area_top + area_bottom) / 2;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    size_t count = 0U;
    for (size_t m = 0U; m < marker_count; m++) {
        const solar_os_places_point_t *point = &map_app.points[marker_points[m]];
        const bool chosen = point->id == map_app.selected_id;
        const char *text = map_app_label_text(point, chosen);
        if (text == NULL) {
            continue;
        }
        const int x = markers[m * 2U];
        const int y = markers[m * 2U + 1U];
        const int dx = x - cx;
        const int dy = y - cy;
        const uint32_t far = (uint32_t)(dx * dx + dy * dy);
        picks[count] = (map_app_label_pick_t){
            .point = marker_points[m],
            .x = x,
            .y = y,
            .rank = far,
        };
        count++;
    }
    qsort(picks, count, sizeof(*picks), map_app_compare_pick);
    /* One label, the point nearest the reticle: the one being aimed at. */
    count = count > 1U ? 1U : count;
    for (size_t i = 0U; i < count; i++) {
        const solar_os_places_point_t *point = &map_app.points[picks[i].point];
        map_app_fit_label(gfx,
                          map_app_label_text(point,
                                             point->id == map_app.selected_id),
                          texts[i], sizeof(texts[i]));
        labels[i] = (solar_os_map_label_t){
            .x = picks[i].x,
            .y = picks[i].y,
            .width = (int)solar_os_gfx_text_width(gfx, texts[i]),
            .height = MAP_APP_LABEL_HEIGHT,
        };
    }
    solar_os_map_place_labels(labels, count, markers, marker_count, 0, area_top,
                              width, area_bottom);
    const bool colour = map_app_colour(gfx);
    for (size_t i = 0U; i < count; i++) {
        if (!labels[i].placed) {
            continue;
        }
        const int x = labels[i].left;
        const int baseline = labels[i].top + MAP_APP_LABEL_BASELINE;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        if (colour) {
            /* An outline rather than a box, so the map shows round it. */
            solar_os_gfx_text(gfx, x - 1, baseline, texts[i]);
            solar_os_gfx_text(gfx, x + 1, baseline, texts[i]);
            solar_os_gfx_text(gfx, x, baseline - 1, texts[i]);
            solar_os_gfx_text(gfx, x, baseline + 1, texts[i]);
        } else {
            solar_os_gfx_fill_rect(gfx, x - 1, labels[i].top, labels[i].width + 2,
                                   labels[i].height);
        }
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, x, baseline, texts[i]);
    }
    solar_os_memory_free(picks);
    solar_os_memory_free(labels);
    solar_os_memory_free(texts);
}

/*
 * Whether a cell's file is already kept, and whether it is loaded. Asked of
 * the card rather than remembered, but only when the cell under the reticle
 * changes, since panning within one cell asks the same question.
 */
static bool map_app_online(void);

static void map_app_tier_file(solar_os_map_tier_t tier, char *file, size_t len)
{
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    solar_os_map_tier_name(map_app.center_lat_e7, map_app.center_lon_e7, tier,
                           name, sizeof(name));
    (void)snprintf(file, len, "%s.bin", name);
}

/* Asked again when the cell changes and when a layer comes or goes, since
 * either changes the answer and neither is frequent. Panning inside one
 * cell with nothing loading asks nothing. The store is asked by name, so
 * however many maps are kept, one kept is never read as absent. */
static void map_app_cell_refresh(void)
{
    /* Keyed by the finest tier's cell, which names every coarser one. */
    char cell[SOLAR_OS_MAP_LAYER_NAME_MAX];
    map_app_tier_file(SOLAR_OS_MAP_TIER_COUNT - 1, cell, sizeof(cell));
    const uint32_t generation = solar_os_map_layer_generation();
    if (strcmp(cell, map_app.cell_name) == 0 &&
        generation == map_app.cell_generation) {
        return;
    }
    map_app.cell_generation = generation;
    strlcpy(map_app.cell_name, cell, sizeof(map_app.cell_name));
    for (int tier = 0; tier < SOLAR_OS_MAP_TIER_COUNT; tier++) {
        char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
        map_app_tier_file((solar_os_map_tier_t)tier, file, sizeof(file));
        map_app.tier_kept[tier] = solar_os_map_layer_is_kept(file);
        map_app.tier_loaded[tier] = solar_os_map_layer_is_loaded(file);
    }
}

/*
 * The tier a press on the cell would act on: the coarsest one not yet
 * loaded, if it is drawn at this scale and can be loaded from the card or
 * fetched over a network. -1 when a press has nothing to do, so nothing
 * that would not be drawn is ever asked for.
 */
static int map_app_cell_width(const solar_os_map_view_t *view,
                              solar_os_map_tier_t tier)
{
    const int32_t size = solar_os_map_tier_cell_e7(tier);
    int32_t corner_lat = 0;
    int32_t corner_lon = 0;
    solar_os_map_cell_corner_sized(map_app.center_lat_e7, map_app.center_lon_e7,
                                   size, &corner_lat, &corner_lon);
    int left = 0;
    int right = 0;
    int y = 0;
    solar_os_map_project_raw(view, corner_lat, corner_lon, &left, &y);
    solar_os_map_project_raw(view, corner_lat, corner_lon + size, &right, &y);
    return right - left;
}

static int map_app_next_tier(const solar_os_map_view_t *view, bool *kept)
{
    map_app_cell_refresh();
    for (int tier = 0; tier < SOLAR_OS_MAP_TIER_COUNT; tier++) {
        if (view->meters_per_col >
            solar_os_map_tier_max_mpp((solar_os_map_tier_t)tier)) {
            return -1;
        }
        if (map_app.tier_loaded[tier]) {
            continue;
        }
        /* The outline and the press share this, so a cell too small to aim
         * at is neither outlined nor acted on. */
        if (map_app_cell_width(view, (solar_os_map_tier_t)tier) <
            MAP_APP_CELL_MIN_PX) {
            return -1;
        }
        *kept = map_app.tier_kept[tier];
        return (*kept || map_app_online()) ? tier : -1;
    }
    return -1;
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
 * loaded for. It appears only while a press would do something: load a
 * tier that is kept, or fetch one over a network, and only a tier drawn at
 * this scale. Otherwise an outline promises a map that will not come.
 */
static void map_app_draw_cell(solar_os_gfx_t *gfx,
                              const solar_os_map_view_t *view,
                              int area)
{
    bool kept = false;
    const int tier = map_app_next_tier(view, &kept);
    const bool busy = solar_os_map_fetch_busy();
    if (tier < 0 && !busy) {
        return;
    }

    /* The cell drawn is the one the press would act on, whose size is its
     * tier's. */
    const int32_t size =
        solar_os_map_tier_cell_e7(tier < 0 ? SOLAR_OS_MAP_TIER_BASE
                                           : (solar_os_map_tier_t)tier);
    int32_t corner_lat = 0;
    int32_t corner_lon = 0;
    solar_os_map_cell_corner_sized(map_app.center_lat_e7, map_app.center_lon_e7,
                                   size, &corner_lat, &corner_lon);
    int left = 0;
    int bottom = 0;
    int right = 0;
    int top = 0;
    solar_os_map_project_raw(view, corner_lat, corner_lon, &left, &bottom);
    solar_os_map_project_raw(view, corner_lat + size, corner_lon + size, &right,
                             &top);
    const int wide = right - left;

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
    const char *title;
    const char *body;
    if (solar_os_map_base_busy()) {
        title = "Getting the world";
        body = solar_os_map_base_status();
    } else if (!map_app_online()) {
        title = "No world map yet";
        body = "connect to wifi to download maps";
    } else {
        title = "No world map yet";
        body = "run map base to download it";
    }
    const int width = (int)solar_os_gfx_width(gfx);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    int content = (int)solar_os_gfx_text_width(gfx, title);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const bool has_body = body != NULL && body[0] != '\0';
    if (has_body && (int)solar_os_gfx_text_width(gfx, body) > content) {
        content = (int)solar_os_gfx_text_width(gfx, body);
    }
    int box_w = content + 16;
    if (box_w > width - 16) {
        box_w = width - 16;
    }
    const int box_h = has_body ? 38 : 24;
    const int x = (width - box_w) / 2;
    const int y = top + (area - box_h) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, box_w, box_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, box_w, box_h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    solar_os_gfx_text(gfx, x + 8, y + 16, title);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    if (has_body) {
        solar_os_gfx_text(gfx, x + 8, y + 31, body);
    }
}

/*
 * Credit for the data, where the map is drawn rather than in a menu nobody
 * opens: bottom right, on the scale bar's row.
 *
 * Which credit depends on what is in view. A fetched cell, or anything else
 * that is not the world, is taken to be OpenStreetMap, whose licence asks
 * for credit wherever the data is shown; a layer the reader packed from
 * somewhere else is credited too, which is the harmless direction. With
 * only the world in view the credit is Natural Earth's, which is public
 * domain and asks for none but suggests this wording. Each has a short form
 * for a row too narrow for the long one.
 *
 * The sign is written as an escape rather than as a byte, so the source
 * stays plain ASCII while the text reaching the display is UTF-8, which is
 * what the font and the drawing calls expect.
 */
static void map_app_draw_credit(solar_os_gfx_t *gfx,
                                const solar_os_map_view_t *view,
                                int baseline,
                                int taken)
{
    bool world = false;
    bool other = false;
    for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
        if (!map_app_layer_in_view(view, solar_os_map_layer(index))) {
            continue;
        }
        if (solar_os_map_layer_pinned(index)) {
            world = true;
        } else {
            other = true;
        }
    }
    if (!world && !other) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const char *credit = other ? "\u00a9 OpenStreetMap contributors"
                               : "Made with Natural Earth";
    int text = (int)solar_os_gfx_text_width(gfx, credit);
    if (taken + text + 6 > width) {
        credit = other ? "\u00a9 OpenStreetMap" : "Natural Earth";
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
    map_app_draw_credit(gfx, &view, y + 3,
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
    const char *working = map_app_working();
    if (working == NULL) {
        working = "";
    }
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
        map_app.painted.labels_hidden == map_app.labels_hidden &&
        map_app.painted.feedback == (map_app.feedback[0] != '\0') &&
        strncmp(map_app.painted.working, working,
                sizeof(map_app.painted.working) - 1U) == 0;
    strlcpy(map_app.painted.working, working, sizeof(map_app.painted.working));
    map_app.painted.center_lat_e7 = map_app.center_lat_e7;
    map_app.painted.center_lon_e7 = map_app.center_lon_e7;
    map_app.painted.meters_per_px = map_app.meters_per_px;
    map_app.painted.map_generation = generation;
    map_app.painted.layer_generation = solar_os_map_layer_generation();
    map_app.painted.selected_id = map_app.selected_id;
    map_app.painted.path_count = map_app.path_count;
    map_app.painted.follow_self = map_app.follow_self;
    map_app.painted.labels_hidden = map_app.labels_hidden;
    map_app.painted.feedback = map_app.feedback[0] != '\0';
    map_app.painted.valid = true;
    return same;
}

/*
 * Every tier of the cell under the reticle that is kept and drawn at this
 * scale goes on the map without being asked: a press is for fetching, and
 * a map already on the card only has to be zoomed or panned into.
 */
static void map_app_load_kept_tiers(const solar_os_map_view_t *view)
{
    /* At most one load per tier, so a layer that loads and is put straight
     * back by the memory budget cannot keep this frame loading it. */
    for (int round = 0; round < SOLAR_OS_MAP_TIER_COUNT; round++) {
        bool kept = false;
        const int tier = map_app_next_tier(view, &kept);
        if (tier < 0 || !kept) {
            return;
        }
        char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
        map_app_tier_file((solar_os_map_tier_t)tier, file, sizeof(file));
        if (solar_os_map_layer_load_named(file) != ESP_OK) {
            map_app_say("%s would not load", file);
            return;
        }
    }
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
    map_app_load_kept_tiers(&view);

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
    int *markers = solar_os_memory_calloc(map_app.count + 1U, 2U * sizeof(int),
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "app.map.labels");
    size_t *marker_points = solar_os_memory_calloc(
        map_app.count + 1U, sizeof(size_t), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "app.map.labels");
    size_t marker_count = 0U;
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
        if (markers != NULL && marker_points != NULL) {
            markers[marker_count * 2U] = (int)col;
            markers[marker_count * 2U + 1U] = (int)row + MAP_APP_HEADER_H;
            marker_points[marker_count++] = i;
        }
    }
    map_app_draw_labels(gfx, markers, marker_points, marker_count,
                        MAP_APP_HEADER_H, MAP_APP_HEADER_H + area);
    solar_os_memory_free(markers);
    solar_os_memory_free(marker_points);

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
 * A press on the cell under the reticle adds one tier of detail: the
 * coarsest tier drawn at this scale that is not on the map yet, loaded
 * from the card if it is kept and asked of OpenStreetMap if it is not. The
 * asking happens on a task of its own, one at a time, so the press returns
 * and the map keeps drawing while the answer arrives.
 */
static void map_app_cell_press(solar_os_context_t *ctx)
{
    (void)ctx;
    if (solar_os_map_fetch_busy()) {
        map_app_say("already fetching, this takes a minute");
        return;
    }
    const solar_os_map_view_t view = map_app_view(solar_os_context_gfx(ctx));
    bool kept = false;
    const int tier = map_app_next_tier(&view, &kept);
    if (tier < 0) {
        const bool finer =
            !map_app.tier_loaded[SOLAR_OS_MAP_TIER_COUNT - 1];
        map_app_say("%s", map_app_cell_width(&view, SOLAR_OS_MAP_TIER_BASE) <
                                  MAP_APP_CELL_MIN_PX
                              ? "zoom in to pick a cell"
                          : !finer ? "all detail loaded here"
                          : map_app_online() ? "zoom in for more detail"
                                             : "more detail needs a network");
        return;
    }
    char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
    map_app_tier_file((solar_os_map_tier_t)tier, file, sizeof(file));
    if (kept) {
        const esp_err_t error = solar_os_map_layer_load_named(file);
        map_app_say("%s %s", file,
                    error == ESP_OK ? "loaded" : "would not load");
        return;
    }
    /* Started, not waited for: the answer takes tens of seconds, and
     * waiting here would stop the map redrawing or being left. */
    if (solar_os_map_fetch_cell(map_app.center_lat_e7, map_app.center_lon_e7,
                                (solar_os_map_tier_t)tier) != ESP_OK) {
        map_app_say("%s", solar_os_map_fetch_status());
        return;
    }
    map_app.fetch_generation = solar_os_map_fetch_generation();
    map_app.watching_fetch = true;
    strlcpy(map_app.fetching, file, sizeof(map_app.fetching));
    map_app_say("fetching %s", file);
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
            map_app.cell_generation--;
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
    case 'w':
    case 'W':
        map_app_pan(ctx, 0, 1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 's':
    case 'S':
        map_app_pan(ctx, 0, -1);
        break;
    case SOLAR_OS_KEY_LEFT:
    case 'a':
    case 'A':
        map_app_pan(ctx, -1, 0);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case 'd':
    case 'D':
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
    case 'r':
    case 'R':
        map_app_delete_selected();
        break;
    case 'g':
    case 'G':
        map_app_poll_self();
        map_app_refresh();
        break;
    case 'l':
    case 'L':
        map_app.labels_hidden = !map_app.labels_hidden;
        map_app_say("labels %s", map_app.labels_hidden ? "hidden" : "shown");
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

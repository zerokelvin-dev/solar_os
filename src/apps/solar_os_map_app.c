#include "solar_os_map_app.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_map.h"
#include "solar_os_map_layers.h"
#include "solar_os_time.h"
#include "solar_os_timezone.h"
#include "solar_os_memory.h"

#define MAP_APP_SELF_POLL_MS 5000U
/* Panning moves this fraction of the view, small enough that the shapes
 * stay readable from one step to the next. */
#define MAP_APP_PAN_DIVISOR 8
/* ...but never more than this fraction of the way round the world, or a
 * step zoomed right out crosses an ocean and there is nothing to follow
 * from one press to the next. A screen-sized step is a world-sized step
 * once the whole world is on the screen. */
#define MAP_APP_PAN_WORLD_STEPS 22
/* Where the map opens with no position of its own: far enough north that
 * the continents, rather than the southern ocean, fill the screen. */
#define MAP_APP_HOME_LAT_E7 200000000
/* The system status bar owns the top of a graphical session and draws over
 * whatever is under it, so the map starts below it and claims no title row
 * of its own. */
#define MAP_APP_HEADER_H 14
#define MAP_APP_INFO_H 12
/* Detail finer than this many pixels is dropped before it is drawn. */
#define MAP_APP_DETAIL_PX 2
/* A shift no larger than this keeps the offset inside an int; past it the
 * world is wider than any screen and only the ring in front of us matters. */
#define MAP_APP_SHIFT_LIMIT 1.0e8F
/* Turns of the world either side of the view a ring may be placed at. Two
 * covers the widest scale, where the world is narrower than the screen. */
#define MAP_APP_REPEAT_MAX 3
/* How many vertex spacings across a ring has to be before its shape means
 * anything. Below this it is a handful of points standing in for an
 * outline, and where that outline falls is an accident of sampling. */
#define MAP_APP_RING_MIN_SPACINGS 8

typedef struct {
    int x;
    int y;
} map_app_vertex_t;

typedef struct {
    solar_os_map_point_t *points;
    solar_os_map_path_t *paths;
    size_t path_count;
    map_app_vertex_t *scratch;
    int *crossings;
    int *edges;
    size_t scratch_max;
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
    char feedback[48];
} map_app_state_t;

static void *map_app_state;
#define map_app (*(map_app_state_t *)map_app_state)

static uint32_t map_app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const solar_os_map_point_t *map_app_self(void)
{
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].kind == SOLAR_OS_MAP_KIND_SELF) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static const solar_os_map_point_t *map_app_point_by_id(uint32_t id)
{
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].id == id) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static const solar_os_map_point_t *map_app_selected(void)
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
     * Every colour sits on the levels the indexed palette is built from,
     * which are multiples of 51, so none of them shift when quantised.
     * Roads are black because they have to read against land, and land is
     * the lightest thing on the map.
     */
    switch (klass) {
    case SOLAR_OS_MAP_CLASS_WATER:
        return solar_os_gfx_rgb(153, 204, 255);
    case SOLAR_OS_MAP_CLASS_ROAD:
        return SOLAR_OS_GFX_COLOR_BLACK;
    case SOLAR_OS_MAP_CLASS_RAIL:
        return solar_os_gfx_rgb(102, 102, 102);
    case SOLAR_OS_MAP_CLASS_BUILDING:
        return solar_os_gfx_rgb(204, 153, 102);
    case SOLAR_OS_MAP_CLASS_BOUNDARY:
        return solar_os_gfx_rgb(153, 51, 153);
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
    int32_t lat[SOLAR_OS_MAP_CAPACITY];
    int32_t lon[SOLAR_OS_MAP_CAPACITY];
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

static void map_app_center_on(const solar_os_map_point_t *point)
{
    map_app.center_lat_e7 = point->latitude_e7;
    map_app.center_lon_e7 = point->longitude_e7;
    map_app.centered = true;
}

static void map_app_refresh(void)
{
    map_app.count = solar_os_map_snapshot(map_app.points,
                                          SOLAR_OS_MAP_CAPACITY,
                                          &map_app.total);
    map_app.path_count = solar_os_map_path_snapshot(map_app.paths,
                                                    SOLAR_OS_MAP_PATH_CAPACITY,
                                                    NULL);
    solar_os_map_status_t status;
    if (solar_os_map_get_status(&status) == ESP_OK) {
        map_app.generation = status.generation;
    }
    if (map_app.selected_id != 0 && map_app_selected() == NULL) {
        map_app.selected_id = 0;
    }

    const solar_os_map_point_t *self = map_app_self();
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
    (void)solar_os_map_update_self();
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
 * Layers can be loaded while the app is open, so the scratch buffer is
 * sized against whatever is loaded now rather than against what was loaded
 * when the app started. Without this a layer added later would have its
 * longest rings silently dropped.
 */
static void map_app_size_scratch(void)
{
    /* Two spare vertices, so a ring that goes round a pole has room for
     * the two points that close it along one. */
    const size_t longest = solar_os_map_layer_longest_ring() + 2U;
    if (longest <= 2U || longest <= map_app.scratch_max) {
        return;
    }
    map_app_vertex_t *grown =
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
 * sixteen vertices and a coastline runs to hundreds, so the crossings are
 * gathered here and drawn as horizontal runs.
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
    /*
     * Only the edges that cross the visible rows can contribute a crossing,
     * and once a coastline is zoomed into, almost none of them do. Finding
     * them once beats rediscovering it on every row: a continent at city
     * zoom is thousands of edges and a handful that matter.
     */
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
            const map_app_vertex_t *a = &map_app.scratch[i];
            const map_app_vertex_t *b = &map_app.scratch[(i + 1U) % count];
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
 * Whether two longitudes sit on opposite sides of the meridian facing away
 * from the view centre. Measured from the centre, a segment crossing it
 * reads as a jump most of the way around the world rather than the short
 * step it is, and drawing it would streak a line across the map.
 */
static bool map_app_lon_wraps(int32_t lon_a, int32_t lon_b)
{
    const int64_t delta = (int64_t)lon_b - (int64_t)lon_a;
    return delta > SOLAR_OS_MAP_LON_MAX_E7 || delta < -SOLAR_OS_MAP_LON_MAX_E7;
}

/*
 * Whether a ring is too few vertex spacings across for its shape to mean
 * anything. Lake Ontario at 1:110m is seventeen points for three hundred
 * kilometres, which is not a lake so much as a rumour of one.
 */
static bool map_app_ring_is_coarser_than_itself(const solar_os_map_ring_t *ring,
                                                uint32_t resolution_m)
{
    if (resolution_m == 0U) {
        return false;
    }
    /* Metres to 1e7 degrees, near enough at any latitude for a threshold. */
    const int64_t spacing_e7 =
        (int64_t)resolution_m * 10000000LL / 111320LL;
    if (spacing_e7 <= 0) {
        return false;
    }
    const int64_t lat_span = (int64_t)ring->lat_max - (int64_t)ring->lat_min;
    const int64_t lon_span = (int64_t)ring->lon_max - (int64_t)ring->lon_min;
    const int64_t span = lat_span > lon_span ? lat_span : lon_span;
    return span < spacing_e7 * MAP_APP_RING_MIN_SPACINGS;
}

/*
 * Whether any of a ring's segments runs through the view, tested on the
 * stored coordinates so that a vertex far outside, whose projection is
 * clamped, cannot make an edge look as though it crosses the screen.
 */
static bool map_app_ring_edge_in_view(const solar_os_map_ring_t *ring,
                                      const solar_os_map_view_t *view,
                                      int32_t lat_min,
                                      int32_t lat_max,
                                      int32_t lon_half)
{
    const size_t segments = ring->open ? ring->point_count - 1U
                                       : ring->point_count;
    for (size_t point = 0U; point < segments; point++) {
        const size_t next = (point + 1U) % ring->point_count;
        const int32_t lat_a = ring->coordinates[point * 2U];
        const int32_t lat_b = ring->coordinates[next * 2U];
        if ((lat_a < lat_min && lat_b < lat_min) ||
            (lat_a > lat_max && lat_b > lat_max)) {
            continue;
        }
        const int32_t lon_a =
            solar_os_map_relative_lon(view, ring->coordinates[point * 2U + 1U]);
        const int32_t lon_b =
            solar_os_map_relative_lon(view, ring->coordinates[next * 2U + 1U]);
        /* A segment crossing the far meridian arrives inverted, and one of
         * its halves reaches the view from either side. */
        if (map_app_lon_wraps(lon_a, lon_b)) {
            return true;
        }
        if ((lon_a < -lon_half && lon_b < -lon_half) ||
            (lon_a > lon_half && lon_b > lon_half)) {
            continue;
        }
        return true;
    }
    return false;
}

/*
 * Geometry is drawn from rings of coordinates, so it needs no tiles. Two
 * things are thrown away before any of it reaches the screen, because a
 * layer holding a whole city is tens of thousands of segments and most of
 * them are neither visible nor distinguishable:
 *
 *  - a ring whose bounds fall outside the view, or which is smaller than a
 *    couple of pixels, is skipped without projecting a single vertex;
 *  - within a ring, vertices closer together than a couple of pixels are
 *    dropped, since drawing them costs time to produce a smudge.
 *
 * Without the second one a dense layer seen from far away is not detail,
 * it is a solid black area.
 */
static void map_app_draw_geometry(solar_os_gfx_t *gfx,
                                  const solar_os_map_view_t *view,
                                  const solar_os_map_geometry_t *geometry)
{
    if (geometry == NULL || map_app.scratch == NULL) {
        return;
    }
    int32_t lat_min = 0;
    int32_t lat_max = 0;
    int32_t lon_half = 0;
    solar_os_map_view_bounds(view, &lat_min, &lat_max, &lon_half);
    const int32_t step = MAP_APP_DETAIL_PX * solar_os_map_view_pixel_e7(view);
    const int span = (int)view->cols;
    /*
     * A layer knows how far apart its vertices sit. Once the screen is
     * narrower than a single one of those, an outline crossing it is not a
     * coastline, it is one straight segment that happens to land here: at
     * 1:110m Lake Ontario is seventeen points for three hundred kilometres,
     * so zoomed into Toronto its shore fell across downtown. Below that the
     * layer's boundaries stop being drawn.
     *
     * A ring whose boundary stays off the screen still tells the truth,
     * because being inside it does not depend on where its edge runs, so
     * the continent underneath keeps filling and only the lake goes.
     */
    const uint64_t view_span_m =
        (uint64_t)view->cols * (uint64_t)view->meters_per_col;
    const bool coarse = geometry->resolution_m > 0U &&
                        view_span_m < (uint64_t)geometry->resolution_m;

    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(geometry, &cursor, &ring)) {
        if (ring.lat_max < lat_min || ring.lat_min > lat_max) {
            continue;
        }
        const int32_t low = solar_os_map_relative_lon(view, ring.lon_min);
        const int32_t high = solar_os_map_relative_lon(view, ring.lon_max);
        /* A ring straddling the far meridian comes back inverted; it has to
         * be drawn rather than judged on a range that no longer means
         * anything. */
        if (low <= high && (high < -lon_half || low > lon_half)) {
            continue;
        }
        /*
         * An area smaller than a couple of pixels is a dot worth nothing.
         * A line that small is usually one piece of a road that carries on
         * in the next ring, so dropping it would break the road rather
         * than simplify it.
         */
        if (!ring.open &&
            (int64_t)ring.lat_max - (int64_t)ring.lat_min < step &&
            (int64_t)ring.lon_max - (int64_t)ring.lon_min < step) {
            continue;
        }
        /*
         * Too coarse to place an edge here. A ring only a few vertex
         * spacings across is a cartoon of a shape rather than a shape, and
         * nothing it says about this view is worth drawing, so it goes: at
         * 1:110m Lake Ontario is seventeen points, and its shore fell
         * across downtown Toronto. A ring far larger than the spacing is
         * still right about what it contains even where its edge is not,
         * so it keeps its fill and loses only the outline, which is the
         * part that would be a line in the wrong place.
         */
        bool outline = true;
        if (coarse && map_app_ring_edge_in_view(&ring, view, lat_min, lat_max,
                                                lon_half)) {
            if (map_app_ring_is_coarser_than_itself(&ring,
                                                    geometry->resolution_m)) {
                continue;
            }
            outline = false;
        }

        size_t kept = 0U;
        int32_t last_lat = 0;
        int32_t last_lon = 0;
        int64_t relative = 0;
        int64_t relative_first = 0;
        /* Where the ring has come the whole way round, if it does. */
        size_t full_turn = 0U;
        for (size_t point = 0U; point < ring.point_count; point++) {
            const int32_t lat = ring.coordinates[point * 2U];
            const int32_t lon = ring.coordinates[point * 2U + 1U];
            const bool ends = point == 0U || point + 1U == ring.point_count;
            if (!ends) {
                const int64_t moved = (int64_t)(lat > last_lat ? lat - last_lat
                                                               : last_lat - lat) +
                                      (int64_t)(lon > last_lon ? lon - last_lon
                                                               : last_lon - lon);
                if (moved < step) {
                    continue;
                }
            }
            if (kept >= map_app.scratch_max) {
                break;
            }
            /*
             * Longitude is carried along the ring rather than measured
             * afresh at each vertex. Measured afresh it jumps a whole turn
             * where the ring crosses the meridian opposite the centre, and
             * the ring arrives as two pieces that cannot be filled: Africa,
             * Europe and Asia are one ring of 1298 points, so centring on
             * Toronto left every one of them unfilled. Carried, the ring
             * simply runs past half a turn and stays a single shape.
             */
            if (kept == 0U) {
                relative = (int64_t)solar_os_map_relative_lon(view, lon);
                relative_first = relative;
            } else {
                relative +=
                    (int64_t)solar_os_map_relative_lon_delta(last_lon, lon);
                const int64_t gone = relative - relative_first;
                if (full_turn == 0U &&
                    (gone > (int64_t)SOLAR_OS_MAP_LON_MAX_E7 * 2 ||
                     gone < -((int64_t)SOLAR_OS_MAP_LON_MAX_E7 * 2))) {
                    full_turn = kept;
                }
            }
            int x = 0;
            int y = 0;
            solar_os_map_project_rel(view, lat, relative, &x, &y);
            map_app.scratch[kept].x = x;
            map_app.scratch[kept].y = y + MAP_APP_HEADER_H;
            kept++;
            last_lat = lat;
            last_lon = lon;
        }
        if (kept < 2U) {
            continue;
        }
        /*
         * A ring that comes back a whole turn from where it started has
         * gone round a pole rather than round an area: Antarctica travels
         * 361 degrees, and its first and last points are a degree apart on
         * the globe but a full turn apart once longitude is carried. Joining
         * those two directly draws a chord the width of the world, and a
         * scanline fill reads it as an edge crossing every row, which
         * striped the bottom of the map in land and sea.
         *
         * Closing it through the pole instead is what the shape means: down
         * to the pole, along it, and back. Mercator puts the pole far below
         * any screen, so the fill simply runs off the bottom, which is what
         * standing on Antarctica looks like.
         */
        const int64_t turn = (int64_t)SOLAR_OS_MAP_LON_MAX_E7 * 2;
        const int64_t travelled = relative - relative_first;
        if (!ring.open && full_turn >= 3U && kept + 2U <= map_app.scratch_max) {
            /*
             * Natural Earth's Antarctica comes round 361 degrees, not 360:
             * the last degree retraces the first. Kept, a scanline crosses
             * that strip twice and the even-odd rule leaves a slit down it,
             * so the ring is cut where it finished its one turn.
             */
            kept = full_turn;
            const int64_t closing =
                travelled > 0 ? relative_first + turn : relative_first - turn;
            /*
             * The closing edge runs off the screen rather than to the pole
             * itself. Mercator never reaches a pole, so it is clamped at
             * 85 degrees, and zoomed out that clamp lands inside the view:
             * closing there left a strip of sea along the bottom of the
             * world below Antarctica.
             */
            const int edge = ring.lat_min + ring.lat_max < 0
                                 ? MAP_APP_HEADER_H + (int)view->rows + 4
                                 : MAP_APP_HEADER_H - 4;
            int x = 0;
            solar_os_map_project_rel(view, 0, closing, &x, NULL);
            map_app.scratch[kept].x = x;
            map_app.scratch[kept].y = edge;
            kept++;
            solar_os_map_project_rel(view, 0, relative_first, &x, NULL);
            map_app.scratch[kept].x = x;
            map_app.scratch[kept].y = edge;
            kept++;
        }

        const size_t segments = ring.open ? kept - 1U : kept;
        solar_os_gfx_set_color(gfx, map_app_class_color(gfx, ring.klass));
        int left = map_app.scratch[0].x;
        int right = left;
        int top = map_app.scratch[0].y;
        int bottom = top;
        for (size_t point = 1U; point < kept; point++) {
            const map_app_vertex_t *v = &map_app.scratch[point];
            if (v->x < left) {
                left = v->x;
            }
            if (v->x > right) {
                right = v->x;
            }
            if (v->y < top) {
                top = v->y;
            }
            if (v->y > bottom) {
                bottom = v->y;
            }
        }
        /*
         * Longitude now runs on past half a turn, so a ring sits at one
         * place on an endless line rather than wrapping onto the screen by
         * itself. The world repeats every turn, so the ring is drawn at
         * each repeat that reaches the view: one of them at any scale where
         * the world is wider than the screen, which is every scale but the
         * one showing all of it.
         */
        /*
         * At the widest scale the whole world is two hundred pixels on a
         * three-hundred-and-twenty pixel screen, so it tiles more than once
         * and a ring can need placing two turns over. Taking one turn
         * either side left a band of the view empty, and panning slid that
         * band across Antarctica, which came and went with it.
         */
        const float world_px = solar_os_map_world_px(view);
        int first = 0;
        int last = 0;
        if (world_px >= 1.0F) {
            first = (int)floorf((float)(-right) / world_px);
            last = (int)ceilf((float)(span - left) / world_px);
            if (first < -MAP_APP_REPEAT_MAX) {
                first = -MAP_APP_REPEAT_MAX;
            }
            if (last > MAP_APP_REPEAT_MAX) {
                last = MAP_APP_REPEAT_MAX;
            }
        }
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
            if (!ring.open && map_app_colour(gfx)) {
                map_app_fill_ring(gfx, kept, top, bottom, (int)view->rows);
            }
            if (!outline) {
                continue;
            }
            for (size_t point = 0U; point < segments; point++) {
                const map_app_vertex_t *a = &map_app.scratch[point];
                const map_app_vertex_t *b = &map_app.scratch[(point + 1U) % kept];
                solar_os_gfx_line(gfx, a->x, a->y, b->x, b->y);
            }
        }
    }
}

static void map_app_draw_layers(solar_os_gfx_t *gfx,
                                const solar_os_map_view_t *view)
{
    map_app_size_scratch();
    for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
        map_app_draw_geometry(gfx, view, solar_os_map_layer(index));
    }
}

static void map_app_draw_point(solar_os_gfx_t *gfx,
                               const solar_os_map_point_t *point,
                               int x,
                               int y,
                               bool chosen)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_circle(gfx, x, y, 4);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    switch (point->kind) {
    case SOLAR_OS_MAP_KIND_SELF:
        solar_os_gfx_fill_circle(gfx, x, y, 3);
        solar_os_gfx_circle(gfx, x, y, 5);
        break;
    case SOLAR_OS_MAP_KIND_WAYPOINT:
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
 * A reticle at the centre of the view. Panning moves the map under it, so
 * it is what you line up with a target when you are working down from a
 * zoomed-out view; the gap in the middle keeps whatever is underneath
 * visible.
 */
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
 * The time where the reticle is, at the far end of the bar. A map is read
 * outdoors, where the hour and the light left are part of the same question
 * as where you are -- and the hour that matters is the one at the place you
 * are looking at, not the one the device is set to.
 *
 * The offset comes from longitude, an hour for every fifteen degrees, which
 * is what a time zone is before politics gets to it. Naming the real zone
 * would need the boundaries of every one of them on board, so the label
 * says which offset it used rather than claiming a name it cannot know.
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
}

static void map_app_draw_info(solar_os_gfx_t *gfx, int top, int width)
{
    char line[96];
    const solar_os_map_point_t *point = map_app_selected();
    char distance[16];
    const solar_os_map_view_t scale_view = map_app_view(gfx);
    solar_os_map_format_distance(solar_os_map_view_resolution(&scale_view),
                                 distance,
                                 sizeof(distance));
    if (map_app.feedback[0] != '\0') {
        strlcpy(line, map_app.feedback, sizeof(line));
    } else if (point == NULL) {
        /* With nothing selected the centre is what the reader is aiming,
         * so the reticle's position is what the row should report. */
        char centre[SOLAR_OS_MAP_COORD_TEXT_MAX];
        solar_os_map_format_coord(map_app.center_lat_e7,
                                  map_app.center_lon_e7,
                                  centre);
        snprintf(line,
                 sizeof(line),
                 "%s  %s/px%s",
                 centre,
                 distance,
                 map_app.follow_self ? "  following" : "");
    } else {
        char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
        char age[8];
        char away[32] = "";
        char reach[16];
        solar_os_map_format_coord(point->latitude_e7, point->longitude_e7, coord);
        map_app_format_age(point->updated_ms, age, sizeof(age));
        const solar_os_map_point_t *self = map_app_self();
        if (self != NULL && self->id != point->id) {
            solar_os_map_format_distance(
                solar_os_map_distance_m(self->latitude_e7,
                                        self->longitude_e7,
                                        point->latitude_e7,
                                        point->longitude_e7),
                reach,
                sizeof(reach));
            snprintf(away,
                     sizeof(away),
                     " %s %03u",
                     reach,
                     (unsigned)solar_os_map_bearing_deg(self->latitude_e7,
                                                        self->longitude_e7,
                                                        point->latitude_e7,
                                                        point->longitude_e7));
        }
        snprintf(line, sizeof(line), "%s %s%s %s",
                 point->label, coord, away, age);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, top, width, MAP_APP_INFO_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 3, top + MAP_APP_INFO_H - 3, line);
    map_app_draw_clock(gfx, top + MAP_APP_INFO_H - 3, width);
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

    /* Sea under everything, so land drawn over it reads as land. */
    solar_os_gfx_clear(gfx,
                       map_app_colour(gfx) ? solar_os_gfx_rgb(204, 255, 255)
                                           : SOLAR_OS_GFX_COLOR_WHITE);
    map_app_draw_layers(gfx, &view);

    /* Paths sit under the markers, dashed so they read as links rather
     * than as terrain. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
    for (size_t i = 0U; i < map_app.path_count; i++) {
        const solar_os_map_point_t *from =
            map_app_point_by_id(map_app.paths[i].from_id);
        const solar_os_map_point_t *to =
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

    const solar_os_map_point_t *selected = map_app_selected();
    /* Oldest first so the newest position draws on top. */
    for (size_t i = map_app.count; i-- > 0;) {
        const solar_os_map_point_t *point = &map_app.points[i];
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

    map_app_draw_centre(gfx, area);
    map_app_draw_scale_bar(gfx, MAP_APP_HEADER_H + area);
    map_app_draw_info(gfx, height - MAP_APP_INFO_H, width);
    map_app.feedback[0] = '\0';
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

static void map_app_center_selected(void)
{
    const solar_os_map_point_t *point = map_app_selected();
    if (point == NULL) {
        point = map_app_self();
    }
    if (point == NULL) {
        strlcpy(map_app.feedback, "nothing to centre on", sizeof(map_app.feedback));
        return;
    }
    map_app_center_on(point);
    map_app.follow_self = point->kind == SOLAR_OS_MAP_KIND_SELF;
}

static void map_app_delete_selected(void)
{
    const solar_os_map_point_t *point = map_app_selected();
    if (point == NULL) {
        strlcpy(map_app.feedback, "no point selected", sizeof(map_app.feedback));
        return;
    }
    if (solar_os_map_remove(point->id) == ESP_OK) {
        strlcpy(map_app.feedback, "point removed", sizeof(map_app.feedback));
    }
    map_app.selected_id = 0;
    map_app_refresh();
}

static esp_err_t map_app_start(solar_os_context_t *ctx)
{
    memset(&map_app, 0, sizeof(map_app));
    esp_err_t err = solar_os_map_init();
    if (err != ESP_OK) {
        return err;
    }
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    map_app.points = solar_os_memory_calloc(SOLAR_OS_MAP_CAPACITY,
                                            sizeof(*map_app.points),
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "app.map");
    map_app.paths = solar_os_memory_calloc(SOLAR_OS_MAP_PATH_CAPACITY,
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
    map_app_world_view(solar_os_context_gfx(ctx));
    map_app_poll_self();
    map_app_refresh();
    map_app_render(ctx);
    return ESP_OK;
}

static void map_app_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
    solar_os_memory_free(map_app.points);
    solar_os_memory_free(map_app.paths);
    solar_os_memory_free(map_app.scratch);
    solar_os_memory_free(map_app.crossings);
    solar_os_memory_free(map_app.edges);
    memset(&map_app, 0, sizeof(map_app));
}

static void map_app_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void map_app_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
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

static bool map_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (map_app_now_ms() - map_app.last_self_poll_ms >= MAP_APP_SELF_POLL_MS) {
            map_app_poll_self();
        }
        solar_os_map_status_t status;
        if (solar_os_map_get_status(&status) == ESP_OK &&
            status.generation != map_app.generation) {
            map_app_refresh();
        }
        /*
         * Redrawn every tick, not only when a point moves. Anything else
         * that paints the display leaves the map holding pixels it no
         * longer owns, and there is no event that says so, so the only way
         * to stay on screen is to keep putting it back.
         */
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

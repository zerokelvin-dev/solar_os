#include "solar_os_map_project.h"

#include <stdint.h>
#include <stdlib.h>

/* Whether two longitudes sit either side of the meridian facing away from
 * the centre, where a short step reads as most of a turn. */
static bool map_lon_wraps(int32_t lon_a, int32_t lon_b)
{
    const int64_t delta = (int64_t)lon_b - (int64_t)lon_a;
    return delta > SOLAR_OS_MAP_LON_MAX_E7 || delta < -SOLAR_OS_MAP_LON_MAX_E7;
}

/* Whether any of a ring's segments runs through the view. Tested on stored
 * coordinates, so a vertex far outside cannot look like a crossing once its
 * projection is clamped. */
static bool map_ring_edge_in_view(const solar_os_map_ring_t *ring,
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
        if (map_lon_wraps(lon_a, lon_b)) {
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

void solar_os_map_cull_prepare(const solar_os_map_view_t *view,
                                 const solar_os_map_geometry_t *geometry,
                                 solar_os_map_cull_t *cull)
{
    solar_os_map_view_bounds(view, &cull->lat_min, &cull->lat_max,
                             &cull->lon_half);
    /* Reaching past the screen and clipping back keeps a way that crosses
     * an edge from showing only the stub of itself. */
    const int32_t margin = (int32_t)(((int64_t)cull->lat_max - cull->lat_min) *
                                     SOLAR_OS_MAP_OVERSCAN_PERCENT / 200);
    cull->lat_min -= margin;
    cull->lat_max += margin;
    const int64_t wider =
        (int64_t)cull->lon_half * (100 + SOLAR_OS_MAP_OVERSCAN_PERCENT) / 100;
    cull->lon_half = wider > SOLAR_OS_MAP_LON_MAX_E7
                         ? SOLAR_OS_MAP_LON_MAX_E7
                         : (int32_t)wider;
    cull->step = SOLAR_OS_MAP_DETAIL_PX * solar_os_map_view_pixel_e7(view);

    /*
     * An outline is unplaceable once one vertex spacing covers a good part
     * of the screen: what crosses it is a straight segment between two
     * samples, not the shape it stands for. A quarter of the width is the
     * point where a lake sampled every forty kilometres stops being a lake
     * and starts being a line through whatever is underneath it.
     */
    const uint64_t span_m =
        (uint64_t)view->cols * (uint64_t)view->meters_per_col;
    cull->coarse = geometry->resolution_m > 0U &&
                   span_m < (uint64_t)geometry->resolution_m *
                                SOLAR_OS_MAP_COARSE_SCREENS;

    /*
     * The room each class has on screen, found by projecting the corners
     * of the ground that class covers, so nothing here needs to know that
     * a degree of longitude is shorter away from the equator. Shared among
     * the class's rings it says whether they can be told apart, and because
     * twice the width holds four times the features, the answer does not
     * depend on how large an area the class covers -- only on how densely
     * it is drawn. Judged per class and not per layer because a cell that
     * carries the whole edge of its lake covers the whole lake, which would
     * make a city's roads look as sparse as a county's.
     */
    for (size_t klass = 0U; klass < SOLAR_OS_MAP_CLASS_COUNT; klass++) {
        cull->room[klass] = 0;
        if (geometry->class_rings[klass] == 0U) {
            continue;
        }
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        solar_os_map_project_raw(view, geometry->class_lat_min[klass],
                                 geometry->class_lon_min[klass], &left,
                                 &bottom);
        solar_os_map_project_raw(view, geometry->class_lat_max[klass],
                                 geometry->class_lon_max[klass], &right,
                                 &top);
        const int wide = right > left ? right - left : left - right;
        const int tall = bottom > top ? bottom - top : top - bottom;
        const int64_t extent = wide > tall ? wide : tall;
        cull->room[klass] = extent * extent * 100;
    }
}

/*
 * Whether a ring is worth drawing, and whether its outline is. An area too
 * small to see goes; a line goes when its own kind is too crowded to tell
 * apart, which drops a city's roads before its rivers. A ring from a layer
 * drawn finer than it was surveyed keeps its fill, which is still right
 * about what it contains, and loses the outline, which is not.
 */
bool solar_os_map_ring_wanted(const solar_os_map_cull_t *cull,
                                const solar_os_map_view_t *view,
                                const solar_os_map_geometry_t *geometry,
                                const solar_os_map_ring_t *ring,
                                bool *outline)
{
    *outline = true;
    if (ring->lat_max < cull->lat_min || ring->lat_min > cull->lat_max) {
        return false;
    }
    /*
     * A piece of a water edge is kept wherever it lies beside the view, and
     * however many pieces there are: a row's far shore, off the screen to
     * one side, is what decides which side of the near one is wet.
     */
    const bool edge_piece = ring->klass == SOLAR_OS_MAP_CLASS_WATER_EDGE;
    const int32_t low = solar_os_map_relative_lon(view, ring->lon_min);
    const int32_t high = solar_os_map_relative_lon(view, ring->lon_max);
    /* A ring straddling the far meridian comes back inverted, and has to be
     * drawn rather than judged on a range that no longer means anything. */
    if (!edge_piece && low <= high &&
        (high < -cull->lon_half || low > cull->lon_half)) {
        return false;
    }
    if (edge_piece) {
        /* Never dropped for being short, either: a few metres of dock
         * missing from the curve breaks every row they span. */
    } else if (ring->open) {
        /* Roads and rail come in by scale, the same in every cell. */
        const uint32_t limit =
            ring->klass == SOLAR_OS_MAP_CLASS_HIGHWAY    ? UINT32_MAX
            : ring->klass == SOLAR_OS_MAP_CLASS_RAIL     ? SOLAR_OS_MAP_RAIL_MAX_MPP
            : ring->klass == SOLAR_OS_MAP_CLASS_ROAD     ? SOLAR_OS_MAP_ROAD_MAX_MPP
            : ring->klass == SOLAR_OS_MAP_CLASS_ROAD_MINOR ? SOLAR_OS_MAP_ROAD_MINOR_MAX_MPP
                                                           : 0U;
        if (limit != 0U) {
            if (view->meters_per_col > limit) {
                return false;
            }
        } else {
            /* Everything else - rivers, borders - by how crowded its kind
             * is over the ground it covers. */
            const uint32_t crowd = geometry->class_rings[ring->klass];
            if (crowd == 0U ||
                cull->room[ring->klass] <
                    (int64_t)crowd * SOLAR_OS_MAP_FEATURE_AREA) {
                return false;
            }
        }
    } else if ((int64_t)ring->lat_max - (int64_t)ring->lat_min < cull->step &&
               (int64_t)ring->lon_max - (int64_t)ring->lon_min < cull->step) {
        return false;
    }
    /*
     * Too coarse to place an edge, with that edge in view: neither the
     * outline nor the fill survives, because the fill's boundary is the
     * outline and carries exactly the same error. Keeping the fill and
     * dropping only the outline was the earlier rule, and it is what put a
     * city under water - a coast sampled every fifty kilometres, filled at
     * thirty metres a pixel, paints sea several kilometres inland with no
     * line to admit it.
     *
     * A ring whose edge is nowhere near the view keeps both: the middle of
     * a continent is reliably land however crudely its coast was drawn.
     */
    if (cull->coarse &&
        map_ring_edge_in_view(ring, view, cull->lat_min, cull->lat_max,
                              cull->lon_half)) {
        return false;
    }
    return true;
}

/*
 * Projects a ring into the scratch buffer, thinning vertices closer than a
 * step. Longitude is carried along the ring rather than measured afresh at
 * each vertex, which would jump a whole turn where the ring crosses the
 * meridian opposite the centre and leave two pieces that cannot be filled.
 *
 * Reports where the ring came the whole way round, if it did, and the
 * longitude it started and ended at.
 */
size_t solar_os_map_project_ring(const solar_os_map_view_t *view,
                                 const solar_os_map_ring_t *ring,
                                 int32_t step,
                                 int y_offset,
                                 solar_os_map_vertex_t *out,
                                 size_t capacity,
                                 solar_os_map_span_t *span)
{
    size_t kept = 0U;
    int32_t last_lat = 0;
    int32_t last_lon = 0;
    span->first = 0;
    span->last = 0;
    span->full_turn = 0U;

    for (size_t point = 0U; point < ring->point_count; point++) {
        const int32_t lat = ring->coordinates[point * 2U];
        const int32_t lon = ring->coordinates[point * 2U + 1U];
        const bool ends = point == 0U || point + 1U == ring->point_count;
        if (!ends) {
            const int64_t moved =
                (int64_t)(lat > last_lat ? lat - last_lat : last_lat - lat) +
                (int64_t)(lon > last_lon ? lon - last_lon : last_lon - lon);
            if (moved < step) {
                continue;
            }
        }
        if (kept >= capacity) {
            break;
        }
        if (kept == 0U) {
            span->last = (int64_t)solar_os_map_relative_lon(view, lon);
            span->first = span->last;
        } else {
            span->last +=
                (int64_t)solar_os_map_relative_lon_delta(last_lon, lon);
            const int64_t gone = span->last - span->first;
            if (span->full_turn == 0U &&
                (gone > SOLAR_OS_MAP_TURN_E7 || gone < -SOLAR_OS_MAP_TURN_E7)) {
                span->full_turn = kept;
            }
        }
        int x = 0;
        int y = 0;
        solar_os_map_project_rel(view, lat, span->last, &x, &y);
        out[kept].x = x;
        out[kept].y = y + y_offset;
        kept++;
        last_lat = lat;
        last_lon = lon;
    }
    return kept;
}

static int map_compare_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a;
    const uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

size_t solar_os_map_water_crossings(const solar_os_map_view_t *view,
                                    const solar_os_map_geometry_t *geometry,
                                    const solar_os_map_cull_t *cull,
                                    int y_offset,
                                    int rows,
                                    solar_os_map_vertex_t *scratch,
                                    size_t scratch_max,
                                    uint32_t *out,
                                    size_t capacity)
{
    const int top = y_offset;
    const int bottom = y_offset + rows;
    size_t found = 0U;
    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(geometry, &cursor, &ring)) {
        bool outline = true;
        if (ring.klass != SOLAR_OS_MAP_CLASS_WATER_EDGE ||
            !solar_os_map_ring_wanted(cull, view, geometry, &ring, &outline)) {
            continue;
        }
        solar_os_map_span_t span;
        const size_t kept = solar_os_map_project_ring(view, &ring, cull->step,
                                                      y_offset, scratch,
                                                      scratch_max, &span);
        if (kept < 2U) {
            continue;
        }
        const size_t segments = ring.open ? kept - 1U : kept;
        for (size_t i = 0U; i < segments; i++) {
            const solar_os_map_vertex_t *a = &scratch[i];
            const solar_os_map_vertex_t *b = &scratch[(i + 1U) % kept];
            if (a->y == b->y) {
                continue;
            }
            /* Half-open, so a row through a shared vertex is crossed once. */
            int low = a->y < b->y ? a->y : b->y;
            int high = a->y < b->y ? b->y : a->y;
            low = low < top ? top : low;
            high = high > bottom ? bottom : high;
            for (int row = low; row < high && found < capacity; row++) {
                int x = a->x + (row - a->y) * (b->x - a->x) / (b->y - a->y);
                /* A shore far off the screen only matters for its side. */
                x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
                out[found++] = ((uint32_t)(row - top) << 16) |
                               (uint32_t)(x + 32768);
            }
        }
    }
    qsort(out, found, sizeof(*out), map_compare_u32);
    return found;
}

/*
 * A ring ending a whole turn from where it started has gone round a pole,
 * not round an area. Joining its ends directly draws a chord the width of
 * the world, which a scanline fill reads as an edge crossing every row.
 * Closing it past the pole instead is what the shape means, and the fill
 * runs off the bottom of the screen.
 */
size_t solar_os_map_close_over_pole(const solar_os_map_view_t *view,
                                    const solar_os_map_ring_t *ring,
                                    const solar_os_map_span_t *span,
                                    int y_offset,
                                    solar_os_map_vertex_t *out,
                                    size_t capacity,
                                    size_t kept)
{
    if (ring->open || span->full_turn < 3U ||
        kept + 2U > capacity) {
        return kept;
    }
    /* Cut where it finished its turn: a ring coming round slightly more
     * than once retraces its start, and a scanline crossing that strip
     * twice leaves a slit down it. */
    kept = span->full_turn;
    const int64_t travelled = span->last - span->first;
    const int64_t closing = travelled > 0 ? span->first + SOLAR_OS_MAP_TURN_E7
                                          : span->first - SOLAR_OS_MAP_TURN_E7;
    /* Closed off the screen rather than at the pole: Mercator never reaches
     * one, and the 85 degree clamp it stops at can land inside the view. */
    const int edge = ring->lat_min + ring->lat_max < 0
                         ? y_offset + (int)view->rows + 4
                         : y_offset - 4;
    int x = 0;
    solar_os_map_project_rel(view, 0, closing, &x, NULL);
    out[kept].x = x;
    out[kept].y = edge;
    kept++;
    solar_os_map_project_rel(view, 0, span->first, &x, NULL);
    out[kept].x = x;
    out[kept].y = edge;
    return kept + 1U;
}

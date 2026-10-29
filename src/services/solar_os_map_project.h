#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_map_geo.h"
#include "solar_os_map_layers.h"

/*
 * Turning a layer's stored rings into screen vertices, and deciding which
 * rings are worth the work. A city layer is tens of thousands of segments,
 * most of them too small, too crowded or too far outside to see, so almost
 * all of them are thrown away before a vertex is projected.
 *
 * Nothing here draws. These are functions of a view and a ring, writing
 * vertices into a buffer the caller owns, which is what lets the hard
 * parts - longitude carried along a ring, a ring that encircles a pole,
 * four different reasons to discard one - be tested on a host without a
 * display, a board or a frame.
 */

/* Pixels a vertex must move before it is worth keeping. */
#define SOLAR_OS_MAP_DETAIL_PX 2
/* How far past the view to keep geometry, so a way crossing an edge is
 * clipped rather than shown as a stub of itself. */
#define SOLAR_OS_MAP_OVERSCAN_PERCENT 10
/* A layer is too coarse to place once one vertex spacing covers this
 * much of the screen, as a divisor of the width. */
#define SOLAR_OS_MAP_COARSE_SCREENS 4
/* Vertex spacings a ring must span before its shape means anything. */
#define SOLAR_OS_MAP_RING_MIN_SPACINGS 8
/*
 * Screen area one feature of a class needs before that class is drawn, in
 * hundredths of a square pixel. A feature is a whole way, which is many
 * segments, and twenty square pixels is about where a grid of them stops
 * being a smear and becomes streets. For a city's quarter-degree cell this
 * brings the tertiaries in below about 80 m a pixel, the arterials below
 * about 150, and leaves the motorways on a regional view by themselves.
 */
#define SOLAR_OS_MAP_FEATURE_AREA 2000
/*
 * The scale, in metres per pixel, past which a tier of road or rail is no
 * longer drawn. Fixed by tier rather than judged from how crowded a layer
 * is, because a layer is one cell and two cells are never equally crowded:
 * judged each on its own, neighbouring cells switched tiers at different
 * zooms and the seam between them showed. Motorways are always drawn.
 */
#define SOLAR_OS_MAP_RAIL_MAX_MPP 181U
#define SOLAR_OS_MAP_ROAD_MAX_MPP 91U
#define SOLAR_OS_MAP_ROAD_MINOR_MAX_MPP 45U
/* A whole turn of longitude, in 1e7 degrees. */
#define SOLAR_OS_MAP_TURN_E7 ((int64_t)SOLAR_OS_MAP_LON_MAX_E7 * 2)

typedef struct {
    int x;
    int y;
} solar_os_map_vertex_t;

/* The longitude a projected ring starts and ends at, and where it came the
 * whole way round, if it did. */
typedef struct {
    int64_t first;
    int64_t last;
    size_t full_turn;
} solar_os_map_span_t;

/*
 * What a layer is measured against for one frame: the ground the view
 * covers, how fine a vertex has to be to be worth keeping, whether the
 * layer is drawn more finely than it was surveyed, and how much room each
 * class of its rings has between them.
 */
typedef struct {
    int32_t lat_min;
    int32_t lat_max;
    int32_t lon_half;
    int32_t step;
    bool coarse;
    int64_t room[SOLAR_OS_MAP_CLASS_COUNT];
} solar_os_map_cull_t;

void solar_os_map_cull_prepare(const solar_os_map_view_t *view,
                               const solar_os_map_geometry_t *geometry,
                               solar_os_map_cull_t *cull);

/* Whether a ring is worth drawing, and whether its outline is. */
bool solar_os_map_ring_wanted(const solar_os_map_cull_t *cull,
                              const solar_os_map_view_t *view,
                              const solar_os_map_geometry_t *geometry,
                              const solar_os_map_ring_t *ring,
                              bool *outline);

/*
 * Projects a ring into out, thinning vertices closer together than step,
 * and reports how many were written. y_offset is added to every row, so a
 * caller drawing below a header does not have to walk the result again.
 */
size_t solar_os_map_project_ring(const solar_os_map_view_t *view,
                                 const solar_os_map_ring_t *ring,
                                 int32_t step,
                                 int y_offset,
                                 solar_os_map_vertex_t *out,
                                 size_t capacity,
                                 solar_os_map_span_t *span);

/*
 * Where every piece of a water edge in a layer crosses each row of the
 * view, one word per crossing: the row (counted from y_offset) in the high
 * half and the column plus 32768 in the low half, sorted, so that each
 * row's crossings pair off left to right. Nothing joins the pieces: a
 * scanline fill needs every edge of a closed curve and not the curve in
 * order, so islands are holes by the same parity. Pieces beside the view
 * are projected like the rest, since a row's far shore decides which side
 * of the near one is wet. Returns how many crossings were found, and stops
 * at capacity.
 */
size_t solar_os_map_water_crossings(const solar_os_map_view_t *view,
                                    const solar_os_map_geometry_t *geometry,
                                    const solar_os_map_cull_t *cull,
                                    int y_offset,
                                    int rows,
                                    solar_os_map_vertex_t *scratch,
                                    size_t scratch_max,
                                    uint32_t *out,
                                    size_t capacity);
#define SOLAR_OS_MAP_CROSSING_ROW(word) ((int)((word) >> 16))
#define SOLAR_OS_MAP_CROSSING_COL(word) ((int)((word) & 0xFFFFU) - 32768)

/* Closes a ring that went round a pole, and reports the new vertex count. */
size_t solar_os_map_close_over_pole(const solar_os_map_view_t *view,
                                    const solar_os_map_ring_t *ring,
                                    const solar_os_map_span_t *span,
                                    int y_offset,
                                    solar_os_map_vertex_t *out,
                                    size_t capacity,
                                    size_t kept);

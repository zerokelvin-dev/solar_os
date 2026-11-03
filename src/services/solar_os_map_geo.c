#include "solar_os_map_geo.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_EARTH_RADIUS_M 6371008.8
#define MAP_METERS_PER_DEGREE 111319.49079327357
#define MAP_DEG_TO_RAD 0.017453292519943295
#define MAP_E7 10000000.0
#define MAP_PI 3.14159265358979323846

/* Metres of ground per pixel. */
/*
 * Metres per pixel at each press of zoom: a half step of the doubling that
 * web maps call a zoom level, so each press is the square root of two
 * closer, the same ratio as a stop on a camera. A series that doubles was
 * steep going from a region to a city, and one that went 1, 2, 5 was
 * uneven as well.
 */
static const uint32_t map_scales[] = {
    1U,     2U,     3U,     4U,     6U,     8U,     11U,    16U,
    23U,    32U,    45U,    64U,    91U,    128U,   181U,   256U,
    362U,   512U,   724U,   1024U,  1448U,  2048U,  2896U,  4096U,
    5793U,  8192U,  11585U, 16384U, 23170U, 32768U, 46341U, 65536U,
    92682U, 131072U, 185364U,
};

static size_t map_scale_count(void)
{
    return sizeof(map_scales) / sizeof(map_scales[0]);
}

bool solar_os_map_coord_valid(int32_t lat_e7, int32_t lon_e7)
{
    return lat_e7 >= -SOLAR_OS_MAP_LAT_MAX_E7 &&
           lat_e7 <= SOLAR_OS_MAP_LAT_MAX_E7 &&
           lon_e7 >= -SOLAR_OS_MAP_LON_MAX_E7 &&
           lon_e7 <= SOLAR_OS_MAP_LON_MAX_E7;
}

uint32_t solar_os_map_distance_m(int32_t lat_a_e7,
                                 int32_t lon_a_e7,
                                 int32_t lat_b_e7,
                                 int32_t lon_b_e7)
{
    const double lat_a = (double)lat_a_e7 / MAP_E7 * MAP_DEG_TO_RAD;
    const double lat_b = (double)lat_b_e7 / MAP_E7 * MAP_DEG_TO_RAD;
    const double d_lat = lat_b - lat_a;
    const double d_lon = ((double)lon_b_e7 - (double)lon_a_e7) / MAP_E7 *
                         MAP_DEG_TO_RAD;
    const double s_lat = sin(d_lat / 2.0);
    const double s_lon = sin(d_lon / 2.0);
    double h = s_lat * s_lat + cos(lat_a) * cos(lat_b) * s_lon * s_lon;
    if (h > 1.0) {
        h = 1.0;
    }
    const double meters = 2.0 * MAP_EARTH_RADIUS_M * asin(sqrt(h));
    return meters >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)(meters + 0.5);
}

void solar_os_map_spacing_add(solar_os_map_spacing_t *spacing,
                              int32_t lat_a,
                              int32_t lon_a,
                              int32_t lat_b,
                              int32_t lon_b)
{
    if (spacing == NULL) {
        return;
    }
    /* A segment across the far meridian is an artefact of the wrap, not a
     * span the surveyor drew. */
    const int64_t d_lon = (int64_t)lon_b - (int64_t)lon_a;
    if (d_lon > SOLAR_OS_MAP_LON_MAX_E7 || d_lon < -SOLAR_OS_MAP_LON_MAX_E7) {
        return;
    }
    const double span = solar_os_map_distance_m(lat_a, lon_a, lat_b, lon_b);
    if (!(span > 0.0)) {
        return;
    }
    /* Bucket zero is a metre or less, and each after it doubles. */
    size_t bucket = 0U;
    double edge = 1.0;
    const size_t last = (sizeof(spacing->buckets) / sizeof(spacing->buckets[0])) - 1U;
    while (bucket < last && span > edge) {
        edge *= 2.0;
        bucket++;
    }
    spacing->buckets[bucket]++;
    spacing->counted++;
}

uint16_t solar_os_map_spacing_hundreds(const solar_os_map_spacing_t *spacing)
{
    if (spacing == NULL || spacing->counted == 0U) {
        return 0U;
    }
    const size_t count = sizeof(spacing->buckets) / sizeof(spacing->buckets[0]);
    const uint32_t half = spacing->counted / 2U;
    uint32_t seen = 0U;
    size_t median = 0U;
    for (size_t bucket = 0U; bucket < count; bucket++) {
        seen += spacing->buckets[bucket];
        if (seen > half) {
            median = bucket;
            break;
        }
    }
    /*
     * Bucket n holds spans between two powers of two, so the middle of it
     * on the scale it was bucketed by - the geometric mean of its edges -
     * is the honest answer. Taking an edge instead would report a layer as
     * half again coarser or finer than it is.
     */
    const double metres = median == 0U ? 1.0 : ldexp(1.0, (int)median) / sqrt(2.0);
    const double hundreds = metres / 100.0;
    if (hundreds >= 65535.0) {
        return 65535U;
    }
    /*
     * Anything measured is at least one, because zero means "not measured"
     * and a layer surveyed every ten metres is not that. It only costs
     * accuracy below a hundred metres, where no layer is coarse anyway.
     */
    const uint16_t rounded = (uint16_t)(hundreds + 0.5);
    return rounded == 0U ? 1U : rounded;
}

uint16_t solar_os_map_bearing_deg(int32_t lat_a_e7,
                                  int32_t lon_a_e7,
                                  int32_t lat_b_e7,
                                  int32_t lon_b_e7)
{
    const double lat_a = (double)lat_a_e7 / MAP_E7 * MAP_DEG_TO_RAD;
    const double lat_b = (double)lat_b_e7 / MAP_E7 * MAP_DEG_TO_RAD;
    const double d_lon = ((double)lon_b_e7 - (double)lon_a_e7) / MAP_E7 *
                         MAP_DEG_TO_RAD;
    const double y = sin(d_lon) * cos(lat_b);
    const double x = cos(lat_a) * sin(lat_b) -
                     sin(lat_a) * cos(lat_b) * cos(d_lon);
    double degrees = atan2(y, x) / MAP_DEG_TO_RAD;
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    const long rounded = lround(degrees);
    return (uint16_t)(rounded % 360L);
}

#define MAP_PROJECT_LIMIT 30000.0F
#define MAP_LON_FULL_E7 3600000000LL

int32_t solar_os_map_relative_lon(const solar_os_map_view_t *view,
                                  int32_t lon_e7)
{
    if (view == NULL) {
        return lon_e7;
    }
    int64_t delta = (int64_t)lon_e7 - (int64_t)view->center_lon_e7;
    if (delta > SOLAR_OS_MAP_LON_MAX_E7) {
        delta -= MAP_LON_FULL_E7;
    } else if (delta < -SOLAR_OS_MAP_LON_MAX_E7) {
        delta += MAP_LON_FULL_E7;
    }
    return (int32_t)delta;
}

/* Rounds towards minus infinity, so a cell south or west of zero belongs to
 * the cell below it rather than the one above. */
static int32_t map_cell_floor(int32_t value_e7)
{
    const int32_t cell = SOLAR_OS_MAP_CELL_E7;
    const int32_t down = value_e7 / cell;
    return (value_e7 < 0 && value_e7 % cell != 0) ? down - 1 : down;
}

void solar_os_map_cell_corner(int32_t lat_e7,
                              int32_t lon_e7,
                              int32_t *corner_lat_e7,
                              int32_t *corner_lon_e7)
{
    if (corner_lat_e7 != NULL) {
        *corner_lat_e7 = map_cell_floor(lat_e7) * SOLAR_OS_MAP_CELL_E7;
    }
    if (corner_lon_e7 != NULL) {
        *corner_lon_e7 = map_cell_floor(lon_e7) * SOLAR_OS_MAP_CELL_E7;
    }
}

bool solar_os_map_cell_parse(const char *name,
                             int32_t *lat_e7,
                             int32_t *lon_e7)
{
    if (name == NULL) {
        return false;
    }
    const char north = name[0];
    if (north != 'n' && north != 's' && north != 'N' && north != 'S') {
        return false;
    }
    long latitude = 0;
    size_t index = 1U;
    for (size_t digit = 0U; digit < 4U; digit++, index++) {
        if (name[index] < '0' || name[index] > '9') {
            return false;
        }
        latitude = latitude * 10 + (name[index] - '0');
    }
    const char east = name[index++];
    if (east != 'e' && east != 'w' && east != 'E' && east != 'W') {
        return false;
    }
    long longitude = 0;
    for (size_t digit = 0U; digit < 5U; digit++, index++) {
        if (name[index] < '0' || name[index] > '9') {
            return false;
        }
        longitude = longitude * 10 + (name[index] - '0');
    }
    if (name[index] != '\0') {
        return false;
    }
    /* Hundredths of a degree, which is what a cell corner is named in. */
    if (lat_e7 != NULL) {
        *lat_e7 = (int32_t)((north == 's' || north == 'S' ? -latitude
                                                          : latitude) * 100000);
    }
    if (lon_e7 != NULL) {
        *lon_e7 = (int32_t)((east == 'w' || east == 'W' ? -longitude
                                                        : longitude) * 100000);
    }
    return true;
}

void solar_os_map_cell_name(int32_t lat_e7,
                            int32_t lon_e7,
                            char *name,
                            size_t capacity)
{
    if (name == NULL || capacity == 0U) {
        return;
    }
    int32_t lat = 0;
    int32_t lon = 0;
    solar_os_map_cell_corner(lat_e7, lon_e7, &lat, &lon);
    /* Hundredths of a degree, which is as fine as any cell corner can be. */
    const long centi_lat = (long)(lat / 100000);
    const long centi_lon = (long)(lon / 100000);
    (void)snprintf(name, capacity, "%c%04ld%c%05ld",
                   centi_lat < 0 ? 's' : 'n',
                   centi_lat < 0 ? -centi_lat : centi_lat,
                   centi_lon < 0 ? 'w' : 'e',
                   centi_lon < 0 ? -centi_lon : centi_lon);
}

int32_t solar_os_map_relative_lon_delta(int32_t from_e7, int32_t to_e7)
{
    int64_t delta = (int64_t)to_e7 - (int64_t)from_e7;
    if (delta > SOLAR_OS_MAP_LON_MAX_E7) {
        delta -= MAP_LON_FULL_E7;
    } else if (delta < -SOLAR_OS_MAP_LON_MAX_E7) {
        delta += MAP_LON_FULL_E7;
    }
    return (int32_t)delta;
}

/*
 * Web Mercator. The world is a square of map_world_px pixels holding 360
 * degrees of longitude, and latitude is stretched so that a small shape
 * keeps its proportions anywhere on it.
 */
static float map_world_px(const solar_os_map_view_t *view)
{
    return (float)(2.0 * MAP_PI * MAP_EARTH_RADIUS_M) /
           (float)view->meters_per_col;
}

static float map_mercator_y(int32_t lat_e7)
{
    int32_t clamped = lat_e7;
    if (clamped > SOLAR_OS_MAP_LAT_LIMIT_E7) {
        clamped = SOLAR_OS_MAP_LAT_LIMIT_E7;
    } else if (clamped < -SOLAR_OS_MAP_LAT_LIMIT_E7) {
        clamped = -SOLAR_OS_MAP_LAT_LIMIT_E7;
    }
    const float lat = (float)clamped / (float)MAP_E7;
    return logf(tanf((45.0F + lat * 0.5F) * (float)MAP_DEG_TO_RAD));
}

uint32_t solar_os_map_view_resolution(const solar_os_map_view_t *view)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return 0U;
    }
    const float lat = (float)view->center_lat_e7 / (float)MAP_E7;
    const float ground =
        (float)view->meters_per_col * cosf(lat * (float)MAP_DEG_TO_RAD);
    return ground < 1.0F ? 1U : (uint32_t)ground;
}

int32_t solar_os_map_view_pixel_e7(const solar_os_map_view_t *view)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return 0;
    }
    const float degrees = 360.0F / map_world_px(view);
    const float scaled = degrees * (float)MAP_E7;
    return scaled < 1.0F ? 1 : (int32_t)scaled;
}

void solar_os_map_view_bounds(const solar_os_map_view_t *view,
                              int32_t *lat_min,
                              int32_t *lat_max,
                              int32_t *lon_half)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return;
    }
    const int rows = (int)(view->rows / 2U) + 1;
    int32_t top = 0;
    int32_t bottom = 0;
    int32_t ignored = 0;
    solar_os_map_pan(view, 0, -rows, &top, &ignored);
    solar_os_map_pan(view, 0, rows, &bottom, &ignored);
    if (lat_max != NULL) {
        *lat_max = top;
    }
    if (lat_min != NULL) {
        *lat_min = bottom;
    }
    if (lon_half != NULL) {
        const float half =
            (float)(view->cols / 2U + 1U) * 360.0F / map_world_px(view);
        const float scaled = half * (float)MAP_E7;
        *lon_half = scaled >= (float)SOLAR_OS_MAP_LON_MAX_E7
                        ? SOLAR_OS_MAP_LON_MAX_E7
                        : (int32_t)scaled;
    }
}

/*
 * The world size and the centre's Mercator position depend only on the
 * view, and projecting a layer needs them once per vertex. Remembered for
 * the view last seen, which is one view for a whole frame; a second caller
 * costs a recomputation and nothing worse.
 */
static struct {
    int32_t center_lat_e7;
    uint32_t meters_per_col;
    size_t rows;
    float world_px;
    float center_merc;
    float merc_per_degree;
    /*
     * The three divisors of a projection, inverted once per view. The
     * chip's FPU multiplies in a cycle and has no divide at all, so a
     * divide is a library call forty times as long, and a frame projects
     * tens of thousands of vertices.
     */
    float px_per_lon_e7;
    float merc_per_lat_e7;
    float px_per_merc;
    bool linear;
    bool valid;
} map_projection_cache;

static float map_projection_merc(const solar_os_map_view_t *view, int32_t lat_e7)
{
    if (map_projection_cache.linear) {
        /* The same clamp the curve itself uses. Mercator runs away to
         * infinity at a pole, and walking the slope straight past the
         * limit puts a shore hundreds of pixels from where it belongs. */
        int32_t clamped = lat_e7;
        if (clamped > SOLAR_OS_MAP_LAT_LIMIT_E7) {
            clamped = SOLAR_OS_MAP_LAT_LIMIT_E7;
        } else if (clamped < -SOLAR_OS_MAP_LAT_LIMIT_E7) {
            clamped = -SOLAR_OS_MAP_LAT_LIMIT_E7;
        }
        return map_projection_cache.center_merc +
               (float)(clamped - view->center_lat_e7) *
                   map_projection_cache.merc_per_lat_e7;
    }
    return map_mercator_y(lat_e7);
}

static void map_projection_constants(const solar_os_map_view_t *view,
                                     float *world_px,
                                     float *center_merc)
{
    if (!map_projection_cache.valid ||
        map_projection_cache.center_lat_e7 != view->center_lat_e7 ||
        map_projection_cache.meters_per_col != view->meters_per_col ||
        map_projection_cache.rows != view->rows) {
        map_projection_cache.center_lat_e7 = view->center_lat_e7;
        map_projection_cache.meters_per_col = view->meters_per_col;
        map_projection_cache.rows = view->rows;
        map_projection_cache.world_px = map_world_px(view);
        map_projection_cache.center_merc = map_mercator_y(view->center_lat_e7);
        /*
         * Mercator's latitude term costs a tangent and a logarithm, and a
         * city is thousands of vertices inside a degree or two. Over a span
         * that small the curve is a straight line to well under a pixel, so
         * close in it is walked by its slope instead.
         */
        const float visible_degrees =
            (float)view->rows * 360.0F / map_projection_cache.world_px;
        int32_t center_clamped = view->center_lat_e7;
        if (center_clamped > SOLAR_OS_MAP_LAT_LIMIT_E7) {
            center_clamped = SOLAR_OS_MAP_LAT_LIMIT_E7;
        } else if (center_clamped < -SOLAR_OS_MAP_LAT_LIMIT_E7) {
            center_clamped = -SOLAR_OS_MAP_LAT_LIMIT_E7;
        }
        const float center_lat = (float)center_clamped / (float)MAP_E7;
        map_projection_cache.linear = visible_degrees < 2.0F;
        map_projection_cache.merc_per_degree =
            (float)MAP_DEG_TO_RAD / cosf(center_lat * (float)MAP_DEG_TO_RAD);
        map_projection_cache.px_per_lon_e7 =
            map_projection_cache.world_px / 360.0F / (float)MAP_E7;
        map_projection_cache.merc_per_lat_e7 =
            map_projection_cache.merc_per_degree / (float)MAP_E7;
        map_projection_cache.px_per_merc =
            map_projection_cache.world_px / (float)(2.0 * MAP_PI);
        map_projection_cache.valid = true;
    }
    *world_px = map_projection_cache.world_px;
    *center_merc = map_projection_cache.center_merc;
}

float solar_os_map_world_px(const solar_os_map_view_t *view)
{
    return map_world_px(view);
}

void solar_os_map_project_rel(const solar_os_map_view_t *view,
                              int32_t lat_e7,
                              int64_t rel_lon_e7,
                              int *x,
                              int *y)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return;
    }
    float world = 0.0F;
    float center_merc = 0.0F;
    map_projection_constants(view, &world, &center_merc);
    (void)world;
    float c = (float)(view->cols / 2U) +
              (float)rel_lon_e7 * map_projection_cache.px_per_lon_e7;
    float r = (float)(view->rows / 2U) +
              (center_merc - map_projection_merc(view, lat_e7)) *
                  map_projection_cache.px_per_merc;
    if (c > MAP_PROJECT_LIMIT) {
        c = MAP_PROJECT_LIMIT;
    } else if (c < -MAP_PROJECT_LIMIT) {
        c = -MAP_PROJECT_LIMIT;
    }
    if (r > MAP_PROJECT_LIMIT) {
        r = MAP_PROJECT_LIMIT;
    } else if (r < -MAP_PROJECT_LIMIT) {
        r = -MAP_PROJECT_LIMIT;
    }

    if (x != NULL) {
        *x = (int)c;
    }
    if (y != NULL) {
        *y = (int)r;
    }
}

void solar_os_map_project_raw(const solar_os_map_view_t *view,
                              int32_t lat_e7,
                              int32_t lon_e7,
                              int *x,
                              int *y)
{
    solar_os_map_project_rel(view, lat_e7,
                             (int64_t)solar_os_map_relative_lon(view, lon_e7),
                             x, y);
}

void solar_os_map_pan(const solar_os_map_view_t *view,
                      int columns,
                      int rows,
                      int32_t *out_lat_e7,
                      int32_t *out_lon_e7)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return;
    }
    const float world = map_world_px(view);
    double lon = (double)view->center_lon_e7 / MAP_E7 +
                 (double)columns * 360.0 / (double)world;
    while (lon > 180.0) {
        lon -= 360.0;
    }
    while (lon < -180.0) {
        lon += 360.0;
    }
    const double merc = (double)map_mercator_y(view->center_lat_e7) -
                        (double)rows * 2.0 * MAP_PI / (double)world;
    const double lat = (2.0 * atan(exp(merc)) - MAP_PI / 2.0) / MAP_DEG_TO_RAD;
    if (out_lat_e7 != NULL) {
        const long scaled = lround(lat * MAP_E7);
        *out_lat_e7 = scaled > SOLAR_OS_MAP_LAT_LIMIT_E7
                          ? SOLAR_OS_MAP_LAT_LIMIT_E7
                          : (scaled < -SOLAR_OS_MAP_LAT_LIMIT_E7
                                 ? -SOLAR_OS_MAP_LAT_LIMIT_E7
                                 : (int32_t)scaled);
    }
    if (out_lon_e7 != NULL) {
        *out_lon_e7 = (int32_t)lround(lon * MAP_E7);
    }
}

bool solar_os_map_project(const solar_os_map_view_t *view,
                          int32_t lat_e7,
                          int32_t lon_e7,
                          size_t *col,
                          size_t *row)
{
    if (view == NULL || view->cols == 0U || view->rows == 0U ||
        view->meters_per_col == 0U) {
        return false;
    }
    int x = 0;
    int y = 0;
    solar_os_map_project_raw(view, lat_e7, lon_e7, &x, &y);
    if (x < 0 || y < 0 || (size_t)x >= view->cols || (size_t)y >= view->rows) {
        return false;
    }
    if (col != NULL) {
        *col = (size_t)x;
    }
    if (row != NULL) {
        *row = (size_t)y;
    }
    return true;
}

uint32_t solar_os_map_world_scale(size_t cols, size_t rows)
{
    if (cols == 0U || rows == 0U) {
        return map_scales[sizeof(map_scales) / sizeof(map_scales[0]) - 1U];
    }
    /* Fit the full width of the world; Mercator is square, so the poles
     * fall off the top and bottom of a screen wider than it is tall. */
    (void)rows;
    const double fit = 2.0 * MAP_PI * MAP_EARTH_RADIUS_M / (double)cols;
    return fit < 1.0 ? 1U : (uint32_t)fit;
}

uint32_t solar_os_map_scale_step(uint32_t meters_per_col, int direction)
{
    const size_t count = map_scale_count();
    if (direction > 0) {
        for (size_t i = 0U; i < count; i++) {
            if (map_scales[i] > meters_per_col) {
                return map_scales[i];
            }
        }
        return map_scales[count - 1U] > meters_per_col ? map_scales[count - 1U]
                                                       : meters_per_col;
    }
    for (size_t i = count; i-- > 0;) {
        if (map_scales[i] < meters_per_col) {
            return map_scales[i];
        }
    }
    return meters_per_col;
}

uint32_t solar_os_map_fit_scale(const int32_t *lat_e7,
                              const int32_t *lon_e7,
                              size_t count,
                              int32_t center_lat_e7,
                              int32_t center_lon_e7,
                              size_t cols,
                              size_t rows)
{
    solar_os_map_view_t view = {
        .center_lat_e7 = center_lat_e7,
        .center_lon_e7 = center_lon_e7,
        .cols = cols,
        .rows = rows,
    };
    for (size_t scale = 0U; scale < map_scale_count(); scale++) {
        view.meters_per_col = map_scales[scale];
        bool all = true;
        for (size_t i = 0U; i < count && all; i++) {
            all = solar_os_map_project(&view, lat_e7[i], lon_e7[i], NULL, NULL);
        }
        if (all) {
            return view.meters_per_col;
        }
    }
    return map_scales[map_scale_count() - 1U];
}

void solar_os_map_format_coord(int32_t lat_e7,
                               int32_t lon_e7,
                               char text[SOLAR_OS_MAP_COORD_TEXT_MAX])
{
    const int64_t lat = lat_e7;
    const int64_t lon = lon_e7;
    const int64_t lat_abs = lat < 0 ? -lat : lat;
    const int64_t lon_abs = lon < 0 ? -lon : lon;
    snprintf(text,
             SOLAR_OS_MAP_COORD_TEXT_MAX,
             "%d.%05d%c %d.%05d%c",
             (int)(lat_abs / 10000000),
             (int)((lat_abs % 10000000) / 100),
             lat < 0 ? 'S' : 'N',
             (int)(lon_abs / 10000000),
             (int)((lon_abs % 10000000) / 100),
             lon < 0 ? 'W' : 'E');
}

void solar_os_map_format_distance(uint32_t meters, char *text, size_t text_len)
{
    if (text == NULL || text_len == 0U) {
        return;
    }
    if (meters < 1000U) {
        snprintf(text, text_len, "%u m", (unsigned)meters);
    } else if (meters < 100000U) {
        snprintf(text,
                 text_len,
                 "%u.%u km",
                 (unsigned)(meters / 1000U),
                 (unsigned)((meters % 1000U) / 100U));
    } else {
        snprintf(text, text_len, "%u km", (unsigned)((meters + 500U) / 1000U));
    }
}

bool solar_os_map_parse_degrees(const char *text, bool latitude, int32_t *e7)
{
    if (text == NULL || e7 == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || !isfinite(value)) {
        return false;
    }
    if (*end != '\0') {
        const char suffix = (char)toupper((unsigned char)*end);
        if (end[1] != '\0') {
            return false;
        }
        if (latitude && suffix == 'S') {
            value = -fabs(value);
        } else if (latitude && suffix == 'N') {
            value = fabs(value);
        } else if (!latitude && suffix == 'W') {
            value = -fabs(value);
        } else if (!latitude && suffix == 'E') {
            value = fabs(value);
        } else {
            return false;
        }
    }
    const double limit = latitude ? 90.0 : 180.0;
    if (value < -limit || value > limit) {
        return false;
    }
    *e7 = (int32_t)lround(value * MAP_E7);
    return true;
}

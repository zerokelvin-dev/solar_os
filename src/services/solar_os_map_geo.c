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
static const uint32_t map_scales[] = {
    1U,      2U,      5U,      10U,     20U,     50U,     100U,
    200U,    500U,    1000U,   2000U,   5000U,   10000U,  20000U,
    50000U,  100000U, 200000U,
};

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
 * view, but projecting a layer calls this once per vertex and a city is
 * tens of thousands of them. They are remembered for the view last seen,
 * which is the same view for the whole of a frame. One renderer draws at a
 * time, so the worst a second caller could cause is a recomputation.
 */
static struct {
    int32_t center_lat_e7;
    uint32_t meters_per_col;
    float world_px;
    float center_merc;
    float merc_per_degree;
    bool linear;
    bool valid;
} map_projection_cache;

static float map_projection_merc(const solar_os_map_view_t *view, int32_t lat_e7)
{
    if (map_projection_cache.linear) {
        return map_projection_cache.center_merc +
               (float)(lat_e7 - view->center_lat_e7) / (float)MAP_E7 *
                   map_projection_cache.merc_per_degree;
    }
    return map_mercator_y(lat_e7);
}

static void map_projection_constants(const solar_os_map_view_t *view,
                                     float *world_px,
                                     float *center_merc)
{
    if (!map_projection_cache.valid ||
        map_projection_cache.center_lat_e7 != view->center_lat_e7 ||
        map_projection_cache.meters_per_col != view->meters_per_col) {
        map_projection_cache.center_lat_e7 = view->center_lat_e7;
        map_projection_cache.meters_per_col = view->meters_per_col;
        map_projection_cache.world_px = map_world_px(view);
        map_projection_cache.center_merc = map_mercator_y(view->center_lat_e7);
        /*
         * Mercator's latitude term costs a tangent and a logarithm, which
         * is most of the work in projecting a city: thousands of vertices
         * spanning a fraction of a degree. Over a span that small the curve
         * is a straight line to well under a pixel, so close in it is
         * walked by its slope, which is the secant of the latitude. Zoomed
         * out, where the span is degrees and the curve is a curve, it is
         * computed properly.
         */
        const float visible_degrees =
            (float)view->rows * 360.0F / map_projection_cache.world_px;
        const float center_lat =
            (float)view->center_lat_e7 / (float)MAP_E7;
        map_projection_cache.linear = visible_degrees < 2.0F;
        map_projection_cache.merc_per_degree =
            (float)MAP_DEG_TO_RAD / cosf(center_lat * (float)MAP_DEG_TO_RAD);
        map_projection_cache.valid = true;
    }
    *world_px = map_projection_cache.world_px;
    *center_merc = map_projection_cache.center_merc;
}

void solar_os_map_project_raw(const solar_os_map_view_t *view,
                              int32_t lat_e7,
                              int32_t lon_e7,
                              int *x,
                              int *y)
{
    if (view == NULL || view->meters_per_col == 0U) {
        return;
    }
    float world = 0.0F;
    float center_merc = 0.0F;
    map_projection_constants(view, &world, &center_merc);
    const float d_lon =
        (float)solar_os_map_relative_lon(view, lon_e7) / (float)MAP_E7;
    float c = (float)(view->cols / 2U) + world * d_lon / 360.0F;
    float r = (float)(view->rows / 2U) +
              world * (center_merc - map_projection_merc(view, lat_e7)) /
                  (float)(2.0 * MAP_PI);
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
    const size_t count = solar_os_map_scale_count();
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

size_t solar_os_map_scale_count(void)
{
    return sizeof(map_scales) / sizeof(map_scales[0]);
}

uint32_t solar_os_map_scale_meters_per_col(size_t index)
{
    const size_t count = solar_os_map_scale_count();
    return map_scales[index < count ? index : count - 1U];
}

size_t solar_os_map_scale_index(uint32_t meters_per_col)
{
    size_t best = 0U;
    for (size_t i = 0U; i < solar_os_map_scale_count(); i++) {
        if (map_scales[i] <= meters_per_col) {
            best = i;
        }
    }
    return best;
}

size_t solar_os_map_fit_scale(const int32_t *lat_e7,
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
    for (size_t scale = 0U; scale < solar_os_map_scale_count(); scale++) {
        view.meters_per_col = map_scales[scale];
        bool all = true;
        for (size_t i = 0U; i < count && all; i++) {
            all = solar_os_map_project(&view, lat_e7[i], lon_e7[i], NULL, NULL);
        }
        if (all) {
            return scale;
        }
    }
    return solar_os_map_scale_count() - 1U;
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

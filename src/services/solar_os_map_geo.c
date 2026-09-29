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

static const uint32_t map_scales[] = {
    10U,      20U,      50U,      100U,     200U,     500U,     1000U,
    2000U,    5000U,    10000U,   20000U,   50000U,   100000U,  200000U,
    500000U,  1000000U, 2000000U, 5000000U,
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
    const double center_lat = (double)view->center_lat_e7 / MAP_E7;
    double d_lon = ((double)lon_e7 - (double)view->center_lon_e7) / MAP_E7;
    if (d_lon > 180.0) {
        d_lon -= 360.0;
    } else if (d_lon < -180.0) {
        d_lon += 360.0;
    }
    const double d_lat = ((double)lat_e7 - (double)view->center_lat_e7) / MAP_E7;
    const double east_m = d_lon * cos(center_lat * MAP_DEG_TO_RAD) *
                          MAP_METERS_PER_DEGREE;
    const double north_m = d_lat * MAP_METERS_PER_DEGREE;
    const double cell_w = (double)view->meters_per_col;
    const double cell_h = cell_w * 2.0;
    const double col_offset = floor(east_m / cell_w + 0.5);
    const double row_offset = floor(-north_m / cell_h + 0.5);
    const double c = (double)(view->cols / 2U) + col_offset;
    const double r = (double)(view->rows / 2U) + row_offset;
    if (c < 0.0 || r < 0.0 || c >= (double)view->cols ||
        r >= (double)view->rows) {
        return false;
    }
    if (col != NULL) {
        *col = (size_t)c;
    }
    if (row != NULL) {
        *row = (size_t)r;
    }
    return true;
}

void solar_os_map_offset(int32_t lat_e7,
                         int32_t lon_e7,
                         int64_t east_m,
                         int64_t north_m,
                         int32_t *out_lat_e7,
                         int32_t *out_lon_e7)
{
    double lat = (double)lat_e7 / MAP_E7 +
                 (double)north_m / MAP_METERS_PER_DEGREE;
    if (lat > 90.0) {
        lat = 90.0;
    } else if (lat < -90.0) {
        lat = -90.0;
    }
    double cos_lat = cos(lat * MAP_DEG_TO_RAD);
    if (cos_lat < 0.01) {
        cos_lat = 0.01;
    }
    double lon = (double)lon_e7 / MAP_E7 +
                 (double)east_m / (MAP_METERS_PER_DEGREE * cos_lat);
    while (lon > 180.0) {
        lon -= 360.0;
    }
    while (lon < -180.0) {
        lon += 360.0;
    }
    if (out_lat_e7 != NULL) {
        *out_lat_e7 = (int32_t)lround(lat * MAP_E7);
    }
    if (out_lon_e7 != NULL) {
        *out_lon_e7 = (int32_t)lround(lon * MAP_E7);
    }
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

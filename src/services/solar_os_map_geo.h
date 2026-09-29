#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Coordinates are signed degrees scaled by 1e7 (the GNSS fix format). */
#define SOLAR_OS_MAP_LAT_MAX_E7 900000000
#define SOLAR_OS_MAP_LON_MAX_E7 1800000000
#define SOLAR_OS_MAP_COORD_TEXT_MAX 28U

typedef struct {
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_col;
    size_t cols;
    size_t rows;
} solar_os_map_view_t;

/*
 * Longitude relative to the view centre, normalised to plus or minus 180
 * degrees. Two vertices of one segment whose relative longitudes differ by
 * more than 180 degrees lie either side of the meridian opposite the view
 * centre, where the projection wraps.
 */
int32_t solar_os_map_relative_lon(const solar_os_map_view_t *view,
                                  int32_t lon_e7);

/* Pixel coordinates without a bounds check, clamped so a polygon that runs
 * far off screen still rasterises. */
void solar_os_map_project_raw(const solar_os_map_view_t *view,
                              int32_t lat_e7,
                              int32_t lon_e7,
                              int *x,
                              int *y);

bool solar_os_map_coord_valid(int32_t lat_e7, int32_t lon_e7);

/* Great-circle distance in metres and initial bearing in degrees (0 = north). */
uint32_t solar_os_map_distance_m(int32_t lat_a_e7,
                                 int32_t lon_a_e7,
                                 int32_t lat_b_e7,
                                 int32_t lon_b_e7);
uint16_t solar_os_map_bearing_deg(int32_t lat_a_e7,
                                  int32_t lon_a_e7,
                                  int32_t lat_b_e7,
                                  int32_t lon_b_e7);

/*
 * Equirectangular projection around the view centre, into square pixels.
 * Returns false when the pixel falls outside the view.
 */
bool solar_os_map_project(const solar_os_map_view_t *view,
                          int32_t lat_e7,
                          int32_t lon_e7,
                          size_t *col,
                          size_t *row);

/* Moves a coordinate by metres east/north; latitude clamps at the poles. */
void solar_os_map_offset(int32_t lat_e7,
                         int32_t lon_e7,
                         int64_t east_m,
                         int64_t north_m,
                         int32_t *out_lat_e7,
                         int32_t *out_lon_e7);

/*
 * Metres per pixel at which the whole world fits the given area, and the
 * next scale in or out of the fixed ladder. Zero metres per pixel is never
 * a valid scale, so a view that still holds it has not been set up.
 */
uint32_t solar_os_map_world_scale(size_t cols, size_t rows);
uint32_t solar_os_map_scale_step(uint32_t meters_per_col, int direction);

size_t solar_os_map_scale_count(void);
uint32_t solar_os_map_scale_meters_per_col(size_t index);
/* Index of the largest scale not exceeding meters_per_col (clamped). */
size_t solar_os_map_scale_index(uint32_t meters_per_col);
/* Smallest scale at which every coordinate fits the view around its centre. */
size_t solar_os_map_fit_scale(const int32_t *lat_e7,
                              const int32_t *lon_e7,
                              size_t count,
                              int32_t center_lat_e7,
                              int32_t center_lon_e7,
                              size_t cols,
                              size_t rows);

void solar_os_map_format_coord(int32_t lat_e7,
                               int32_t lon_e7,
                               char text[SOLAR_OS_MAP_COORD_TEXT_MAX]);
void solar_os_map_format_distance(uint32_t meters, char *text, size_t text_len);

/* Accepts "43.6532", "-79.3832", or a trailing N/S/E/W. */
bool solar_os_map_parse_degrees(const char *text, bool latitude, int32_t *e7);

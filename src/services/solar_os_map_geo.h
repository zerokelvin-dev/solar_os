#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Coordinates are signed degrees scaled by 1e7 (the GNSS fix format). */
#define SOLAR_OS_MAP_LAT_MAX_E7 900000000
#define SOLAR_OS_MAP_LON_MAX_E7 1800000000
#define SOLAR_OS_MAP_COORD_TEXT_MAX 28U

/* Mercator cannot represent the poles; the projection stops here. */
#define SOLAR_OS_MAP_LAT_LIMIT_E7 850511300

/*
 * The view is Web Mercator. Longitude maps to x and latitude to y with no
 * term from the view centre, so panning only ever shifts the picture and
 * never restretches it, and shapes stay locally correct at every latitude.
 *
 * meters_per_col is the ground a pixel covers at the equator, which is what
 * keeps a pan from rescaling the map. At any other latitude a pixel covers
 * that times the cosine of the latitude, which is what a scale bar has to
 * report.
 */
typedef struct {
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_col;
    size_t cols;
    size_t rows;
} solar_os_map_view_t;

/* Ground per pixel at a latitude, for a scale bar or a distance readout. */
uint32_t solar_os_map_view_resolution(const solar_os_map_view_t *view);

/*
 * What the view can see, in coordinates: the latitude range it spans and
 * half the longitude it spans either side of its centre. Geometry outside
 * that costs nothing to reject and everything to project.
 */
void solar_os_map_view_bounds(const solar_os_map_view_t *view,
                              int32_t *lat_min,
                              int32_t *lat_max,
                              int32_t *lon_half);

/* Degrees, scaled by 1e7, that one pixel of the view covers. */
int32_t solar_os_map_view_pixel_e7(const solar_os_map_view_t *view);

/* Moves the centre by a pixel offset, which panning a Mercator view is. */
void solar_os_map_pan(const solar_os_map_view_t *view,
                      int columns,
                      int rows,
                      int32_t *out_lat_e7,
                      int32_t *out_lon_e7);

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
/*
 * The world is divided into cells of a fixed size in degrees, so a cell is
 * arithmetic rather than a list: any position names the cell holding it,
 * and the name says where it is. A cell is the unit a map area is fetched,
 * kept and loaded in.
 */
#define SOLAR_OS_MAP_CELL_E7 2500000
#define SOLAR_OS_MAP_CELL_NAME_MAX 16U

/* Names the cell holding a position, as n4350w07925 for its lower corner. */
void solar_os_map_cell_name(int32_t lat_e7,
                            int32_t lon_e7,
                            char *name,
                            size_t capacity);
/* The position a cell name stands for, being its lower corner. */
bool solar_os_map_cell_parse(const char *name,
                             int32_t *lat_e7,
                             int32_t *lon_e7);

/* The lower corner of the cell holding a position. */
void solar_os_map_cell_corner(int32_t lat_e7,
                              int32_t lon_e7,
                              int32_t *corner_lat_e7,
                              int32_t *corner_lon_e7);

/* The shorter way round from one longitude to another, in 1e7 degrees. */
int32_t solar_os_map_relative_lon_delta(int32_t from_e7, int32_t to_e7);

/* Pixels that the whole three hundred and sixty degrees spans at this scale. */
float solar_os_map_world_px(const solar_os_map_view_t *view);

/*
 * Projects a longitude already measured from the view centre. It may run
 * beyond half a turn, which is how a ring stays one continuous shape while
 * crossing the meridian opposite the centre instead of being torn in two.
 */
void solar_os_map_project_rel(const solar_os_map_view_t *view,
                              int32_t lat_e7,
                              int64_t rel_lon_e7,
                              int *x,
                              int *y);

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

/*
 * How finely a layer was surveyed, measured from the layer itself while it
 * is packed. A renderer zoomed in past this spacing is looking at a shape
 * the source never claimed to place that precisely, and can stop believing
 * the outline rather than drawing a coastline through the middle of a city.
 *
 * The median is taken, not the mean, because one long segment across an
 * ocean should not make a finely surveyed coast look coarse. Storing every
 * span to sort them is out of the question on this device, so spans are
 * counted into power-of-two buckets and the median bucket is reported -
 * which is ample for a threshold compared against whole multiples.
 */
typedef struct {
    uint32_t buckets[24];
    uint32_t counted;
} solar_os_map_spacing_t;

/* Measures one segment. Zero-length segments are ignored, as the script
 * that packs layers on a desktop also ignores them. */
void solar_os_map_spacing_add(solar_os_map_spacing_t *spacing,
                              int32_t lat_a,
                              int32_t lon_a,
                              int32_t lat_b,
                              int32_t lon_b);

/* The median spacing in hundreds of metres, as the header stores it. */
uint16_t solar_os_map_spacing_hundreds(const solar_os_map_spacing_t *spacing);
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

/*
 * Metres per pixel at which the whole world fits the given area, and the
 * next scale in or out of the fixed ladder. Zero metres per pixel is never
 * a valid scale, so a view that still holds it has not been set up.
 */
uint32_t solar_os_map_world_scale(size_t cols, size_t rows);
uint32_t solar_os_map_scale_step(uint32_t meters_per_col, int direction);

/*
 * Metres per pixel of the smallest scale at which every coordinate fits the
 * view around its centre. The ladder itself stays inside this file: a scale
 * handed out as a position in it was read back as a scale by anything that
 * forgot to convert, and one metre per pixel is what an unconverted zero
 * means.
 */
uint32_t solar_os_map_fit_scale(const int32_t *lat_e7,
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

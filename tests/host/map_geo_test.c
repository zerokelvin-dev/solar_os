#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_map_geo.h"

/* Toronto and Ottawa: 351 km apart on a bearing of about 55 degrees. */
#define TORONTO_LAT 436532000
#define TORONTO_LON -793832000
#define OTTAWA_LAT 454215000
#define OTTAWA_LON -757000000

static void test_geo(void)
{
    assert(solar_os_map_coord_valid(TORONTO_LAT, TORONTO_LON));
    assert(!solar_os_map_coord_valid(900000001, 0));
    assert(!solar_os_map_coord_valid(0, -1800000001));

    const uint32_t meters =
        solar_os_map_distance_m(TORONTO_LAT, TORONTO_LON, OTTAWA_LAT, OTTAWA_LON);
    assert(meters > 348000U && meters < 354000U);
    assert(solar_os_map_distance_m(TORONTO_LAT, TORONTO_LON, TORONTO_LAT, TORONTO_LON) == 0U);

    const uint16_t bearing =
        solar_os_map_bearing_deg(TORONTO_LAT, TORONTO_LON, OTTAWA_LAT, OTTAWA_LON);
    assert(bearing > 50U && bearing < 60U);
    /* Due north and due west from Toronto. */
    assert(solar_os_map_bearing_deg(TORONTO_LAT, TORONTO_LON, TORONTO_LAT + 100000, TORONTO_LON) == 0U);
    assert(solar_os_map_bearing_deg(TORONTO_LAT, TORONTO_LON, TORONTO_LAT, TORONTO_LON - 100000) == 270U);
}

static void test_project(void)
{
    const solar_os_map_view_t view = {
        .center_lat_e7 = TORONTO_LAT,
        .center_lon_e7 = TORONTO_LON,
        .meters_per_col = 100U,
        .cols = 81U,
        .rows = 21U,
    };
    size_t col = 0U;
    size_t row = 0U;
    assert(solar_os_map_project(&view, TORONTO_LAT, TORONTO_LON, &col, &row));
    assert(col == 40U && row == 10U);

    /* Ottawa is far outside a view a few kilometres across. */
    assert(!solar_os_map_project(&view, OTTAWA_LAT, OTTAWA_LON, &col, &row));

    /*
     * The property that matters: Mercator puts longitude in x and latitude
     * in y with no term from the view centre, so a shape keeps its size on
     * screen wherever the view is centred. An equirectangular projection
     * centred on a moving latitude restretches the map on every pan, which
     * is what this guards against.
     */
    solar_os_map_view_t wide = {
        .center_lat_e7 = 500000000,
        .center_lon_e7 = 0,
        .meters_per_col = 2000U,
        .cols = 320U,
        .rows = 214U,
    };
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    solar_os_map_project_raw(&wide, 500000000, 0, &x0, &y0);
    solar_os_map_project_raw(&wide, 510000000, 10000000, &x1, &y1);
    const int box_w = x1 - x0;
    const int box_h = y1 - y0;
    assert(box_w > 0 && box_h < 0);
    const int32_t centres[] = {0, 550000000, 600000000, -300000000};
    for (size_t i = 0U; i < sizeof(centres) / sizeof(centres[0]); i++) {
        wide.center_lat_e7 = centres[i];
        solar_os_map_project_raw(&wide, 500000000, 0, &x0, &y0);
        solar_os_map_project_raw(&wide, 510000000, 10000000, &x1, &y1);
        /* One pixel of slack for rounding, no more. */
        assert(x1 - x0 == box_w);
        assert(y1 - y0 >= box_h - 1 && y1 - y0 <= box_h + 1);
    }

    /* Panning is a pixel offset. Screen rows run down, so a positive row
     * offset moves the view south. */
    wide.center_lat_e7 = TORONTO_LAT;
    wide.center_lon_e7 = TORONTO_LON;
    int32_t lat = 0;
    int32_t lon = 0;
    solar_os_map_pan(&wide, 40, 27, &lat, &lon);
    assert(lon > wide.center_lon_e7);
    assert(lat < wide.center_lat_e7);
    solar_os_map_view_t moved = wide;
    moved.center_lat_e7 = lat;
    moved.center_lon_e7 = lon;
    int32_t lat_back = 0;
    int32_t lon_back = 0;
    solar_os_map_pan(&moved, -40, -27, &lat_back, &lon_back);
    assert(lat_back > wide.center_lat_e7 - 1000 &&
           lat_back < wide.center_lat_e7 + 1000);
    assert(lon_back > wide.center_lon_e7 - 1000 &&
           lon_back < wide.center_lon_e7 + 1000);

    /* Metres per pixel is quoted at the equator and shrinks with latitude. */
    wide.center_lat_e7 = 0;
    assert(solar_os_map_view_resolution(&wide) == wide.meters_per_col);
    wide.center_lat_e7 = 600000000;
    const uint32_t north = solar_os_map_view_resolution(&wide);
    assert(north > 950U && north < 1050U);

    /* Mercator cannot reach the poles, so panning stops short of them. */
    wide.center_lat_e7 = SOLAR_OS_MAP_LAT_LIMIT_E7;
    solar_os_map_pan(&wide, 0, 10000, &lat, &lon);
    assert(lat <= SOLAR_OS_MAP_LAT_LIMIT_E7);

    /* Geometry far outside the view still projects, clamped, so a polygon
     * crossing the screen still rasterises. */
    solar_os_map_project_raw(&view, -TORONTO_LAT, -TORONTO_LON, &x0, &y0);
    assert(x0 >= -30000 && x0 <= 30000 && y0 >= -30000 && y0 <= 30000);

    /*
     * Longitude wraps at the meridian opposite the view centre. Two points
     * a fraction of a degree apart there get relative longitudes at
     * opposite extremes, which is how a renderer knows to break the line
     * between them rather than draw it back across the whole map.
     */
    assert(solar_os_map_relative_lon(&view, TORONTO_LON) == 0);
    const int32_t before = solar_os_map_relative_lon(&view, 1003841880);
    const int32_t after = solar_os_map_relative_lon(&view, 1008933560);
    assert(before > 1790000000 && before <= SOLAR_OS_MAP_LON_MAX_E7);
    assert(after < -1790000000 && after >= -SOLAR_OS_MAP_LON_MAX_E7);
    assert((int64_t)after - (int64_t)before < -SOLAR_OS_MAP_LON_MAX_E7);
    assert(solar_os_map_relative_lon(&view, TORONTO_LON + 10000000) == 10000000);
    assert(solar_os_map_relative_lon(&view, TORONTO_LON - 10000000) == -10000000);

    /* The world view puts all 360 degrees across the display width. */
    const uint32_t world = solar_os_map_world_scale(320U, 214U);
    solar_os_map_view_t whole = {
        .center_lat_e7 = 0,
        .center_lon_e7 = 0,
        .meters_per_col = world,
        .cols = 320U,
        .rows = 214U,
    };
    int left = 0;
    int right = 0;
    solar_os_map_project_raw(&whole, 0, -1799000000, &left, NULL);
    solar_os_map_project_raw(&whole, 0, 1799000000, &right, NULL);
    assert(left >= 0 && left < 4);
    assert(right > 316 && right <= 320);
    assert(solar_os_map_scale_step(world, 1) > world);
    assert(solar_os_map_scale_step(world, -1) < world);
}

static void test_text(void)
{
    char text[SOLAR_OS_MAP_COORD_TEXT_MAX];
    solar_os_map_format_coord(TORONTO_LAT, TORONTO_LON, text);
    assert(strcmp(text, "43.65320N 79.38320W") == 0);
    solar_os_map_format_coord(-334000000, 1512000000, text);
    assert(strcmp(text, "33.40000S 151.20000E") == 0);

    char distance[16];
    solar_os_map_format_distance(950U, distance, sizeof(distance));
    assert(strcmp(distance, "950 m") == 0);
    solar_os_map_format_distance(1250U, distance, sizeof(distance));
    assert(strcmp(distance, "1.2 km") == 0);
    solar_os_map_format_distance(351000U, distance, sizeof(distance));
    assert(strcmp(distance, "351 km") == 0);

    int32_t value = 0;
    assert(solar_os_map_parse_degrees("43.6532", true, &value) && value == TORONTO_LAT);
    assert(solar_os_map_parse_degrees("-79.3832", false, &value) && value == TORONTO_LON);
    assert(solar_os_map_parse_degrees("79.3832W", false, &value) && value == TORONTO_LON);
    assert(solar_os_map_parse_degrees("43.6532N", true, &value) && value == TORONTO_LAT);
    assert(!solar_os_map_parse_degrees("43.6532E", true, &value));
    assert(!solar_os_map_parse_degrees("91.0", true, &value));
    assert(!solar_os_map_parse_degrees("181.0", false, &value));
    assert(!solar_os_map_parse_degrees("north", true, &value));
    assert(!solar_os_map_parse_degrees("", true, &value));
    assert(!solar_os_map_parse_degrees("43.65NN", true, &value));
}

/*
 * A layer must say how finely it was surveyed, or a renderer cannot tell
 * when it has zoomed in past what the data supports - which is how a
 * coastline sampled every hundred kilometres ends up drawn through the
 * middle of a city.
 */
static void test_a_layer_reports_its_own_spacing(void)
{
    solar_os_map_spacing_t spacing = {0};
    assert(solar_os_map_spacing_hundreds(&spacing) == 0U);

    /* Vertices about 10 km apart, along a meridian where a degree is
     * 111320 m: 0.09 degrees is 10019 m. */
    for (int step = 0; step < 32; step++) {
        solar_os_map_spacing_add(&spacing, 430000000 + step * 900000, 0,
                                 430000000 + (step + 1) * 900000, 0);
    }
    const uint16_t ten_km = solar_os_map_spacing_hundreds(&spacing);
    /* Bucketed by doubling, so 10019 m lands in the 8192-16384 band and is
     * reported as that band's geometric middle: 11585 m, or 115 hundreds. */
    assert(ten_km >= 40U && ten_km <= 120U);

    /* One enormous span must not drag the median: the median is what says
     * how the source was surveyed, and an ocean crossing is one segment. */
    solar_os_map_spacing_add(&spacing, -800000000, 0, 800000000, 0);
    assert(solar_os_map_spacing_hundreds(&spacing) == ten_km);

    /* A finer layer reports a smaller number than a coarser one. */
    solar_os_map_spacing_t fine = {0};
    for (int step = 0; step < 32; step++) {
        solar_os_map_spacing_add(&fine, 430000000 + step * 900, 0,
                                 430000000 + (step + 1) * 900, 0);
    }
    assert(solar_os_map_spacing_hundreds(&fine) < ten_km);

    /* A segment that only looks long because it crosses the far meridian
     * is an artefact of the wrap, not something the surveyor drew. */
    solar_os_map_spacing_t wrapped = {0};
    solar_os_map_spacing_add(&wrapped, 0, 1790000000, 0, -1790000000);
    assert(wrapped.counted == 0U);
}

int main(void)
{
    test_a_layer_reports_its_own_spacing();
    test_geo();
    test_project();
    test_text();
    printf("map_geo_test ok\n");
    return 0;
}

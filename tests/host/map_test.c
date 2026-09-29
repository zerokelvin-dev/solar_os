#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_map.h"
#include "solar_os_memory.h"

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    now += 1000;
    return now;
}

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    return calloc(count, size);
}

void solar_os_memory_free(void *pointer)
{
    free(pointer);
}

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

static uint32_t publish(const char *source,
                        const char *key,
                        solar_os_map_kind_t kind,
                        int32_t lat)
{
    const solar_os_map_publish_t point = {
        .source = source,
        .key = key,
        .label = key,
        .kind = kind,
        .latitude_e7 = lat,
        .longitude_e7 = TORONTO_LON,
    };
    uint32_t id = 0U;
    assert(solar_os_map_publish(&point, &id) == ESP_OK);
    assert(id != 0U);
    return id;
}

static void test_store(void)
{
    assert(solar_os_map_init() == ESP_OK);

    const uint32_t home = publish("user", "home", SOLAR_OS_MAP_KIND_WAYPOINT, TORONTO_LAT);
    solar_os_map_point_t point;
    assert(solar_os_map_get(home, &point) == ESP_OK);
    assert(strcmp(point.label, "home") == 0);
    assert(point.kind == SOLAR_OS_MAP_KIND_WAYPOINT);

    /* The same (source, key) moves the point instead of adding one. */
    const uint32_t again = publish("user", "home", SOLAR_OS_MAP_KIND_WAYPOINT, OTTAWA_LAT);
    assert(again == home);
    assert(solar_os_map_get(home, &point) == ESP_OK);
    assert(point.latitude_e7 == OTTAWA_LAT);

    solar_os_map_status_t status;
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.count == 1U);
    const uint32_t generation = status.generation;

    /* A different source with the same key is a different point. */
    publish("gnss", "home", SOLAR_OS_MAP_KIND_SELF, TORONTO_LAT);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.count == 2U);
    assert(status.generation != generation);

    /* Invalid coordinates are refused. */
    const solar_os_map_publish_t bad = {
        .source = "user",
        .key = "bad",
        .label = "bad",
        .kind = SOLAR_OS_MAP_KIND_WAYPOINT,
        .latitude_e7 = 900000001,
        .longitude_e7 = 0,
    };
    assert(solar_os_map_publish(&bad, NULL) == ESP_ERR_INVALID_ARG);

    /* Control characters in labels are sanitized. */
    const solar_os_map_publish_t noisy = {
        .source = "user",
        .key = "noisy",
        .label = "a\nb\tc",
        .kind = SOLAR_OS_MAP_KIND_WAYPOINT,
        .latitude_e7 = TORONTO_LAT,
        .longitude_e7 = TORONTO_LON,
    };
    uint32_t noisy_id = 0U;
    assert(solar_os_map_publish(&noisy, &noisy_id) == ESP_OK);
    assert(solar_os_map_get(noisy_id, &point) == ESP_OK);
    assert(strcmp(point.label, "a b c") == 0);
    assert(solar_os_map_remove(noisy_id) == ESP_OK);
    assert(solar_os_map_remove(noisy_id) == ESP_ERR_NOT_FOUND);

    /* Filling with nodes evicts the oldest node, never the waypoint. */
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        char key[16];
        snprintf(key, sizeof(key), "n%zu", i);
        publish("meshcore", key, SOLAR_OS_MAP_KIND_NODE, TORONTO_LAT);
    }
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.count == SOLAR_OS_MAP_CAPACITY);
    assert(status.evicted > 0U);
    assert(solar_os_map_get(home, &point) == ESP_OK);

    /* Snapshots are newest first and bounded by the caller's buffer. */
    solar_os_map_point_t page[4];
    size_t total = 0U;
    const size_t count = solar_os_map_snapshot(page, 4U, &total);
    assert(count == 4U);
    assert(total == SOLAR_OS_MAP_CAPACITY);
    for (size_t i = 1U; i < count; i++) {
        assert(page[i - 1U].id > page[i].id);
    }

    size_t removed = 0U;
    assert(solar_os_map_remove_source("meshcore", &removed) == ESP_OK);
    assert(removed > 0U);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.count == 2U);

    assert(solar_os_map_clear() == ESP_OK);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.count == 0U);
    assert(solar_os_map_get(home, &point) == ESP_ERR_NOT_FOUND);
}

/* A path is two point references, and outlives neither of them. */
static void test_paths(void)
{
    assert(solar_os_map_clear() == ESP_OK);
    const uint32_t home = publish("user", "home", SOLAR_OS_MAP_KIND_WAYPOINT, TORONTO_LAT);
    const uint32_t peer = publish("mesh", "peer", SOLAR_OS_MAP_KIND_NODE, OTTAWA_LAT);
    const uint32_t other = publish("mesh", "other", SOLAR_OS_MAP_KIND_NODE, TORONTO_LAT);

    const solar_os_map_path_publish_t hop = {
        .source = "mesh",
        .key = "home-peer",
        .label = "hop 1",
        .from_id = home,
        .to_id = peer,
    };
    uint32_t path_id = 0U;
    assert(solar_os_map_path_publish(&hop, &path_id) == ESP_OK);
    assert(path_id != 0U);

    solar_os_map_status_t status;
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.path_count == 1U);

    /* Republishing the same key moves the path rather than adding one. */
    const solar_os_map_path_publish_t moved = {
        .source = "mesh",
        .key = "home-peer",
        .from_id = home,
        .to_id = other,
    };
    uint32_t again = 0U;
    assert(solar_os_map_path_publish(&moved, &again) == ESP_OK);
    assert(again == path_id);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.path_count == 1U);

    solar_os_map_path_t paths[4];
    size_t total = 0U;
    assert(solar_os_map_path_snapshot(paths, 4U, &total) == 1U);
    assert(total == 1U);
    assert(paths[0].to_id == other);
    assert(strcmp(paths[0].label, "home-peer") == 0);

    /* Both endpoints must exist, and a path cannot join a point to itself. */
    const solar_os_map_path_publish_t missing = {
        .source = "mesh", .key = "ghost", .from_id = home, .to_id = 424242U,
    };
    assert(solar_os_map_path_publish(&missing, NULL) == ESP_ERR_NOT_FOUND);
    const solar_os_map_path_publish_t loop = {
        .source = "mesh", .key = "loop", .from_id = home, .to_id = home,
    };
    assert(solar_os_map_path_publish(&loop, NULL) == ESP_ERR_INVALID_ARG);

    /* Removing an endpoint takes the path with it. */
    assert(solar_os_map_remove(other) == ESP_OK);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.path_count == 0U);

    /* So does removing a whole source. */
    const solar_os_map_path_publish_t again_hop = {
        .source = "mesh", .key = "home-peer", .from_id = home, .to_id = peer,
    };
    assert(solar_os_map_path_publish(&again_hop, NULL) == ESP_OK);
    size_t removed = 0U;
    assert(solar_os_map_remove_source("mesh", &removed) == ESP_OK);
    assert(removed == 1U);
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.path_count == 0U);

    /* And so does an eviction, which is a removal the producer never asked
     * for: fill the store with nodes until the peer is pushed out. */
    const uint32_t anchor = publish("user", "anchor", SOLAR_OS_MAP_KIND_WAYPOINT, TORONTO_LAT);
    const uint32_t victim = publish("mesh", "victim", SOLAR_OS_MAP_KIND_NODE, OTTAWA_LAT);
    const solar_os_map_path_publish_t doomed = {
        .source = "user", .key = "doomed", .from_id = anchor, .to_id = victim,
    };
    assert(solar_os_map_path_publish(&doomed, NULL) == ESP_OK);
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        char key[16];
        snprintf(key, sizeof(key), "n%zu", i);
        publish("flood", key, SOLAR_OS_MAP_KIND_NODE, TORONTO_LAT);
    }
    assert(solar_os_map_get_status(&status) == ESP_OK);
    assert(status.evicted > 0U);
    assert(status.path_count == 0U);
    assert(solar_os_map_clear() == ESP_OK);
}

int main(void)
{
    test_geo();
    test_project();
    test_text();
    test_store();
    test_paths();
    printf("map_test ok\n");
    return 0;
}

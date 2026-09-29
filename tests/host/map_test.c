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

    /* Pixels are square: a row covers the same ground as a column. */
    int32_t lat = 0;
    int32_t lon = 0;
    solar_os_map_offset(TORONTO_LAT, TORONTO_LON, 100, 0, &lat, &lon);
    assert(solar_os_map_project(&view, lat, lon, &col, &row));
    assert(col == 41U && row == 10U);

    solar_os_map_offset(TORONTO_LAT, TORONTO_LON, 0, 100, &lat, &lon);
    assert(solar_os_map_project(&view, lat, lon, &col, &row));
    assert(col == 40U && row == 9U);

    /* Geometry far outside the view still projects, clamped, so a polygon
     * crossing the screen still rasterises. */
    int x = 0;
    int y = 0;
    solar_os_map_project_raw(&view, -TORONTO_LAT, -TORONTO_LON, &x, &y);
    assert(x >= -30000 && x <= 30000 && y >= -30000 && y <= 30000);

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
    /* Points either side of the centre stay adjacent. */
    assert(solar_os_map_relative_lon(&view, TORONTO_LON + 10000000) == 10000000);
    assert(solar_os_map_relative_lon(&view, TORONTO_LON - 10000000) == -10000000);

    /* Ottawa is far outside a view 8 km wide. */
    assert(!solar_os_map_project(&view, OTTAWA_LAT, OTTAWA_LON, &col, &row));

    const int32_t lats[] = {TORONTO_LAT, OTTAWA_LAT};
    const int32_t lons[] = {TORONTO_LON, OTTAWA_LON};
    const size_t scale = solar_os_map_fit_scale(
        lats, lons, 2U, TORONTO_LAT, TORONTO_LON, view.cols, view.rows);
    solar_os_map_view_t fitted = view;
    fitted.meters_per_col = solar_os_map_scale_meters_per_col(scale);
    assert(solar_os_map_project(&fitted, OTTAWA_LAT, OTTAWA_LON, NULL, NULL));
    if (scale > 0U) {
        fitted.meters_per_col = solar_os_map_scale_meters_per_col(scale - 1U);
        assert(!solar_os_map_project(&fitted, OTTAWA_LAT, OTTAWA_LON, NULL, NULL));
    }

    assert(solar_os_map_scale_meters_per_col(solar_os_map_scale_index(150U)) == 100U);
    assert(solar_os_map_scale_meters_per_col(solar_os_map_scale_index(0U)) == 1U);
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

int main(void)
{
    test_geo();
    test_project();
    test_text();
    test_store();
    printf("map_test ok\n");
    return 0;
}

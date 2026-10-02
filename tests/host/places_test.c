#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_places.h"
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

/*
 * No storage under test, which is also the case on a device with nothing
 * mounted: the places keep their points in memory and writes none of them.
 */
bool solar_os_storage_is_mounted(void)
{
    return false;
}

esp_err_t solar_os_storage_default_path(const char *relative_path,
                                        char *buffer,
                                        size_t capacity)
{
    (void)relative_path;
    (void)buffer;
    (void)capacity;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_storage_mkdir(const char *path)
{
    (void)path;
    return ESP_ERR_INVALID_STATE;
}

/* Toronto and Ottawa: 351 km apart on a bearing of about 55 degrees. */
#define TORONTO_LAT 436532000
#define TORONTO_LON -793832000
#define OTTAWA_LAT 454215000
#define OTTAWA_LON -757000000

static uint32_t publish(const char *source,
                        const char *key,
                        solar_os_places_kind_t kind,
                        int32_t lat)
{
    const solar_os_places_publish_t point = {
        .source = source,
        .key = key,
        .label = key,
        .kind = kind,
        .latitude_e7 = lat,
        .longitude_e7 = TORONTO_LON,
    };
    uint32_t id = 0U;
    assert(solar_os_places_publish(&point, &id) == ESP_OK);
    assert(id != 0U);
    return id;
}

static void test_store(void)
{
    assert(solar_os_places_init() == ESP_OK);

    const uint32_t home = publish("user", "home", SOLAR_OS_PLACES_KIND_WAYPOINT, TORONTO_LAT);
    solar_os_places_point_t point;
    assert(solar_os_places_get(home, &point) == ESP_OK);
    assert(strcmp(point.label, "home") == 0);
    assert(point.kind == SOLAR_OS_PLACES_KIND_WAYPOINT);

    /* The same (source, key) moves the point instead of adding one. */
    const uint32_t again = publish("user", "home", SOLAR_OS_PLACES_KIND_WAYPOINT, OTTAWA_LAT);
    assert(again == home);
    assert(solar_os_places_get(home, &point) == ESP_OK);
    assert(point.latitude_e7 == OTTAWA_LAT);

    solar_os_places_status_t status;
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.count == 1U);
    const uint32_t generation = status.generation;

    /* A different source with the same key is a different point. */
    publish("gnss", "home", SOLAR_OS_PLACES_KIND_SELF, TORONTO_LAT);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.count == 2U);
    assert(status.generation != generation);

    /* Invalid coordinates are refused. */
    const solar_os_places_publish_t bad = {
        .source = "user",
        .key = "bad",
        .label = "bad",
        .kind = SOLAR_OS_PLACES_KIND_WAYPOINT,
        .latitude_e7 = 900000001,
        .longitude_e7 = 0,
    };
    assert(solar_os_places_publish(&bad, NULL) == ESP_ERR_INVALID_ARG);

    /* Control characters in labels are sanitized. */
    const solar_os_places_publish_t noisy = {
        .source = "user",
        .key = "noisy",
        .label = "a\nb\tc",
        .kind = SOLAR_OS_PLACES_KIND_WAYPOINT,
        .latitude_e7 = TORONTO_LAT,
        .longitude_e7 = TORONTO_LON,
    };
    uint32_t noisy_id = 0U;
    assert(solar_os_places_publish(&noisy, &noisy_id) == ESP_OK);
    assert(solar_os_places_get(noisy_id, &point) == ESP_OK);
    assert(strcmp(point.label, "a b c") == 0);
    assert(solar_os_places_remove(noisy_id) == ESP_OK);
    assert(solar_os_places_remove(noisy_id) == ESP_ERR_NOT_FOUND);

    /* Filling with nodes evicts the oldest node, never the waypoint. */
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        char key[16];
        snprintf(key, sizeof(key), "n%zu", i);
        publish("meshcore", key, SOLAR_OS_PLACES_KIND_NODE, TORONTO_LAT);
    }
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.count == SOLAR_OS_PLACES_CAPACITY);
    assert(status.evicted > 0U);
    assert(solar_os_places_get(home, &point) == ESP_OK);

    /* Snapshots are newest first and bounded by the caller's buffer. */
    solar_os_places_point_t page[4];
    size_t total = 0U;
    const size_t count = solar_os_places_snapshot(page, 4U, &total);
    assert(count == 4U);
    assert(total == SOLAR_OS_PLACES_CAPACITY);
    for (size_t i = 1U; i < count; i++) {
        assert(page[i - 1U].id > page[i].id);
    }

    size_t removed = 0U;
    assert(solar_os_places_remove_source("meshcore", &removed) == ESP_OK);
    assert(removed > 0U);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.count == 2U);

    assert(solar_os_places_clear() == ESP_OK);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.count == 0U);
    assert(solar_os_places_get(home, &point) == ESP_ERR_NOT_FOUND);
}

/* A path is two point references, and outlives neither of them. */
static void test_paths(void)
{
    assert(solar_os_places_clear() == ESP_OK);
    const uint32_t home = publish("user", "home", SOLAR_OS_PLACES_KIND_WAYPOINT, TORONTO_LAT);
    const uint32_t peer = publish("mesh", "peer", SOLAR_OS_PLACES_KIND_NODE, OTTAWA_LAT);
    const uint32_t other = publish("mesh", "other", SOLAR_OS_PLACES_KIND_NODE, TORONTO_LAT);

    const solar_os_places_path_publish_t hop = {
        .source = "mesh",
        .key = "home-peer",
        .label = "hop 1",
        .from_id = home,
        .to_id = peer,
    };
    uint32_t path_id = 0U;
    assert(solar_os_places_path_publish(&hop, &path_id) == ESP_OK);
    assert(path_id != 0U);

    solar_os_places_status_t status;
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.path_count == 1U);

    /* Republishing the same key moves the path rather than adding one. */
    const solar_os_places_path_publish_t moved = {
        .source = "mesh",
        .key = "home-peer",
        .from_id = home,
        .to_id = other,
    };
    uint32_t again = 0U;
    assert(solar_os_places_path_publish(&moved, &again) == ESP_OK);
    assert(again == path_id);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.path_count == 1U);

    solar_os_places_path_t paths[4];
    size_t total = 0U;
    assert(solar_os_places_path_snapshot(paths, 4U, &total) == 1U);
    assert(total == 1U);
    assert(paths[0].to_id == other);
    assert(strcmp(paths[0].label, "home-peer") == 0);

    /* Both endpoints must exist, and a path cannot join a point to itself. */
    const solar_os_places_path_publish_t missing = {
        .source = "mesh", .key = "ghost", .from_id = home, .to_id = 424242U,
    };
    assert(solar_os_places_path_publish(&missing, NULL) == ESP_ERR_NOT_FOUND);
    const solar_os_places_path_publish_t loop = {
        .source = "mesh", .key = "loop", .from_id = home, .to_id = home,
    };
    assert(solar_os_places_path_publish(&loop, NULL) == ESP_ERR_INVALID_ARG);

    /* Removing an endpoint takes the path with it. */
    assert(solar_os_places_remove(other) == ESP_OK);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.path_count == 0U);

    /* So does removing a whole source. */
    const solar_os_places_path_publish_t again_hop = {
        .source = "mesh", .key = "home-peer", .from_id = home, .to_id = peer,
    };
    assert(solar_os_places_path_publish(&again_hop, NULL) == ESP_OK);
    size_t removed = 0U;
    assert(solar_os_places_remove_source("mesh", &removed) == ESP_OK);
    assert(removed == 1U);
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.path_count == 0U);

    /* And so does an eviction, which is a removal the producer never asked
     * for: fill the store with nodes until the peer is pushed out. */
    const uint32_t anchor = publish("user", "anchor", SOLAR_OS_PLACES_KIND_WAYPOINT, TORONTO_LAT);
    const uint32_t victim = publish("mesh", "victim", SOLAR_OS_PLACES_KIND_NODE, OTTAWA_LAT);
    const solar_os_places_path_publish_t doomed = {
        .source = "user", .key = "doomed", .from_id = anchor, .to_id = victim,
    };
    assert(solar_os_places_path_publish(&doomed, NULL) == ESP_OK);
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        char key[16];
        snprintf(key, sizeof(key), "n%zu", i);
        publish("flood", key, SOLAR_OS_PLACES_KIND_NODE, TORONTO_LAT);
    }
    assert(solar_os_places_get_status(&status) == ESP_OK);
    assert(status.evicted > 0U);
    assert(status.path_count == 0U);
    assert(solar_os_places_clear() == ESP_OK);
}

/*
 * Every entry point has to stand being the first one called. The path half
 * of the store used to reach for a lock that only solar_os_places_init creates,
 * so a shell asking for the paths before anything had opened the places took
 * the lock before it existed. This runs before any other test so that
 * nothing has set the store up first.
 */
static void test_cold(void)
{
    size_t total = 1U;
    assert(solar_os_places_path_snapshot(NULL, 0U, &total) == 0U);
    assert(total == 0U);
    assert(solar_os_places_path_remove(1U) == ESP_ERR_NOT_FOUND);
    size_t removed = 1U;
    assert(solar_os_places_path_remove_source("mesh", &removed) == ESP_OK);
    assert(removed == 0U);
}

int main(void)
{
    test_cold();
    test_store();
    test_paths();
    printf("places_test ok\n");
    return 0;
}

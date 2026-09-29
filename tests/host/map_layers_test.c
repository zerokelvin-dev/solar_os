#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_map_geojson.h"
#include "solar_os_map_layers.h"
#include "solar_os_memory.h"

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

static uint8_t *read_geojson(const char *text, size_t *size)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    uint8_t *packed = NULL;
    const esp_err_t error = solar_os_map_geojson_read(file, &packed, size);
    fclose(file);
    return error == ESP_OK ? packed : NULL;
}

static void test_builtin_world(void)
{
    solar_os_map_geometry_t world;
    assert(solar_os_map_geometry_parse(solar_os_map_basemap_world,
                                       solar_os_map_basemap_world_size,
                                       &world) == ESP_OK);
    assert(world.ring_count > 100U);
    assert(world.point_count > 4000U);

    size_t counted = 0U;
    for (uint32_t index = 0U; index < world.ring_count; index++) {
        solar_os_map_ring_t ring;
        assert(solar_os_map_geometry_ring(&world, index, &ring));
        assert(ring.point_count >= 3U);
        assert(!ring.open);
        counted += ring.point_count;
    }
    assert(counted == world.point_count);
    assert(solar_os_map_geometry_longest_ring(&world) > 0U);

    /* Layer zero is the built-in world and cannot be removed. */
    assert(solar_os_map_layer_count() == 1U);
    assert(solar_os_map_layer(0) != NULL);
    assert(strcmp(solar_os_map_layer_name(0), "world") == 0);
    assert(solar_os_map_layer_remove(0) == ESP_ERR_INVALID_ARG);
    assert(solar_os_map_layer(1) == NULL);
}

static void test_truncated(void)
{
    solar_os_map_geometry_t geometry;
    for (size_t size = 0U; size < solar_os_map_basemap_world_size; size++) {
        assert(solar_os_map_geometry_parse(solar_os_map_basemap_world,
                                           size,
                                           &geometry) != ESP_OK);
    }
    const uint8_t wrong_magic[32] = {'N', 'O', 'P', 'E'};
    assert(solar_os_map_geometry_parse(wrong_magic, sizeof(wrong_magic),
                                       &geometry) == ESP_ERR_INVALID_ARG);
}

static void test_geojson_polygon(void)
{
    static const char *const text =
        "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
        "\"properties\":{\"name\":\"box, with a comma\"},"
        "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":"
        "[[[0,0],[1,0],[1,1],[0,1]]]}}]}";
    size_t size = 0U;
    uint8_t *packed = read_geojson(text, &size);
    assert(packed != NULL);

    solar_os_map_geometry_t geometry;
    assert(solar_os_map_geometry_parse(packed, size, &geometry) == ESP_OK);
    assert(geometry.ring_count == 1U);
    assert(geometry.point_count == 4U);

    solar_os_map_ring_t ring;
    assert(solar_os_map_geometry_ring(&geometry, 0U, &ring));
    assert(ring.point_count == 4U);
    assert(!ring.open);
    /* GeoJSON stores longitude first; the layer stores latitude first. */
    assert(ring.coordinates[0] == 0 && ring.coordinates[1] == 0);
    assert(ring.coordinates[2] == 0 && ring.coordinates[3] == 10000000);
    assert(ring.coordinates[4] == 10000000 && ring.coordinates[5] == 10000000);
    free(packed);
}

static void test_geojson_shapes(void)
{
    /* A multipolygon, a line and a bare point in one collection. */
    static const char *const text =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"geometry\":{\"type\":\"MultiPolygon\",\"coordinates\":"
        "[[[[0,0],[1,0],[1,1]]],[[[5,5],[6,5],[6,6],[5,6]]]]}},"
        "{\"geometry\":{\"type\":\"LineString\",\"coordinates\":"
        "[[-79.38,43.65],[-75.70,45.42]]}},"
        "{\"geometry\":{\"type\":\"Point\",\"coordinates\":[10,20]}}]}";
    size_t size = 0U;
    uint8_t *packed = read_geojson(text, &size);
    assert(packed != NULL);

    solar_os_map_geometry_t geometry;
    assert(solar_os_map_geometry_parse(packed, size, &geometry) == ESP_OK);
    /* The lone point is not geometry the map can draw. */
    assert(geometry.ring_count == 3U);
    assert(geometry.point_count == 3U + 4U + 2U);

    solar_os_map_ring_t ring;
    assert(solar_os_map_geometry_ring(&geometry, 0U, &ring));
    assert(ring.point_count == 3U && !ring.open);
    assert(solar_os_map_geometry_ring(&geometry, 1U, &ring));
    assert(ring.point_count == 4U && !ring.open);
    assert(solar_os_map_geometry_ring(&geometry, 2U, &ring));
    assert(ring.point_count == 2U && ring.open);
    assert(ring.coordinates[0] == 436500000);
    assert(ring.coordinates[1] == -793800000);
    free(packed);
}

static void test_geojson_rejects(void)
{
    size_t size = 0U;
    assert(read_geojson("{\"type\":\"Feature\"}", &size) == NULL);
    assert(read_geojson("", &size) == NULL);
    /* Coordinates that are only a marker yield no drawable geometry. */
    assert(read_geojson("{\"geometry\":{\"type\":\"Point\","
                        "\"coordinates\":[1,2]}}", &size) == NULL);
}

int main(void)
{
    test_builtin_world();
    test_truncated();
    test_geojson_polygon();
    test_geojson_shapes();
    test_geojson_rejects();
    printf("map_layers_test ok\n");
    return 0;
}

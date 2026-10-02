#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

#include "solar_os_map_geojson.h"
#include "solar_os_map_layers.h"
#include "solar_os_inflate.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

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

/* No storage under test, so nothing is imported and nothing is kept. */
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

esp_err_t solar_os_storage_makedirs(const char *path, bool exist_ok)
{
    (void)path;
    (void)exist_ok;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_storage_remove(const char *path)
{
    (void)path;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_storage_copy_file(const char *source_path,
                                     const char *dest_path)
{
    (void)source_path;
    (void)dest_path;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_storage_scandir(const char *path,
                                   size_t cursor,
                                   size_t limit,
                                   solar_os_storage_entry_t *entries,
                                   size_t *entry_count,
                                   size_t *next_cursor,
                                   bool *has_more)
{
    (void)path;
    (void)cursor;
    (void)limit;
    (void)entries;
    (void)entry_count;
    (void)next_cursor;
    (void)has_more;
    return ESP_ERR_INVALID_STATE;
}

static uint8_t *read_geojson(const char *text, size_t *size)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    solar_os_inflate_t *reader = NULL;
    assert(solar_os_inflate_open(file, &reader) == ESP_OK);
    uint8_t *packed = NULL;
    const esp_err_t error = solar_os_map_geojson_read(reader, SOLAR_OS_MAP_CLASS_LAND, &packed, size);
    solar_os_inflate_close(reader);
    fclose(file);
    return error == ESP_OK ? packed : NULL;
}

/* An area and a line, which between them are every shape a layer holds. */
static uint8_t *pack_sample_layer(size_t *size)
{
    static const char *const text =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Polygon\","
        "\"coordinates\":[[[-79.5,43.5],[-79.2,43.5],[-79.2,43.8],"
        "[-79.5,43.8],[-79.5,43.5]]]}},"
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"LineString\","
        "\"coordinates\":[[-79.4,43.6],[-79.3,43.65],[-79.25,43.7]]}}]}";
    return read_geojson(text, size);
}

/*
 * A layer built the way the generator builds one: packed, parsed back, and
 * walked. Nothing is compiled in any more, so this is the shape of every
 * layer the map will ever see.
 */
static void test_packed_layer(void)
{
    size_t size = 0U;
    uint8_t *packed = pack_sample_layer(&size);
    assert(packed != NULL);

    solar_os_map_geometry_t layer;
    assert(solar_os_map_geometry_parse(packed, size, &layer) == ESP_OK);
    assert(layer.ring_count > 0U);
    assert(layer.point_count >= layer.ring_count * 2U);

    size_t counted = 0U;
    uint32_t rings = 0U;
    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(&layer, &cursor, &ring)) {
        assert(ring.point_count >= 2U);
        if (!ring.open) {
            /* An area needs three points to enclose anything. */
            assert(ring.point_count >= 3U);
        }
        assert(ring.lat_min <= ring.lat_max);
        assert(ring.lon_min <= ring.lon_max);
        counted += ring.point_count;
        rings++;
    }
    assert(rings == layer.ring_count);
    assert(counted == layer.point_count);
    assert(solar_os_map_geometry_longest_ring(&layer) > 0U);

    /* With nothing compiled in, a map starts empty. */
    assert(solar_os_map_layer_count() == 0U);
    assert(solar_os_map_layer(0) == NULL);
    assert(solar_os_map_layer_remove(0) == ESP_ERR_INVALID_ARG);

    free(packed);
}

/*
 * What a name means is decided by its first character, so the answer never
 * depends on what happens to be on the card.
 */
static void test_load_named(void)
{
    assert(solar_os_map_layer_load_named(NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_map_layer_load_named("") == ESP_ERR_INVALID_ARG);

    /* A slash is a path, so this is a missing file rather than a missing
     * kept map, and it never reaches the card's list. */
    assert(solar_os_map_layer_load_named("/no/such/file.bin") ==
           ESP_ERR_NOT_FOUND);

    /* With no storage there is nothing kept, so a cell, a number and a
     * name all come back the same way rather than being mistaken for a
     * path. */
    assert(solar_os_map_layer_load_named("n4350w07950") == ESP_ERR_NOT_FOUND);
    assert(solar_os_map_layer_load_named("0") == ESP_ERR_NOT_FOUND);
    assert(solar_os_map_layer_load_named("12") == ESP_ERR_NOT_FOUND);
    assert(solar_os_map_layer_load_named("toronto.bin") == ESP_ERR_NOT_FOUND);

    /* Nothing was loaded by any of that. */
    assert(solar_os_map_layer_count() == 0U);
}

static void test_truncated(void)
{
    solar_os_map_geometry_t geometry;
    /* Every prefix of a real layer is refused, however long. */
    size_t full = 0U;
    uint8_t *packed = pack_sample_layer(&full);
    assert(packed != NULL);
    for (size_t size = 0U; size < full; size++) {
        assert(solar_os_map_geometry_parse(packed, size, &geometry) != ESP_OK);
    }
    free(packed);
    const uint8_t wrong_magic[32] = {'N', 'O', 'P', 'E'};
    assert(solar_os_map_geometry_parse(wrong_magic, sizeof(wrong_magic),
                                       &geometry) == ESP_ERR_INVALID_ARG);
}

/* Writes bytes to a file of its own and hands back the path to it. */
static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1U, size, file) == size);
    assert(fclose(file) == 0);
}

static void write_gzip_file(const char *path, const void *data, size_t size)
{
    gzFile out = gzopen(path, "wb9");
    assert(out != NULL);
    assert(gzwrite(out, data, (unsigned)size) == (int)size);
    assert(gzclose(out) == Z_OK);
}

/*
 * A map is a map however it was stored: the same bytes kept in the clear
 * and kept deflated must import to the same geometry, and neither the
 * command nor the name of the file says which one it was.
 */
static void test_import_deflated_or_not(void)
{
    size_t size = 0U;
    uint8_t *packed = pack_sample_layer(&size);
    assert(packed != NULL);

    const char *plain_path = "map_layers_test_plain.bin";
    const char *deflated_path = "map_layers_test_deflated.bin";
    write_file(plain_path, packed, size);
    write_gzip_file(deflated_path, packed, size);
    /* Deflating has to have actually made it smaller, or this proves nothing. */
    FILE *check = fopen(deflated_path, "rb");
    assert(check != NULL);
    assert(fseek(check, 0, SEEK_END) == 0);
    assert((size_t)ftell(check) < size);
    assert(fclose(check) == 0);

    solar_os_map_layer_clear();
    assert(solar_os_map_layer_load(plain_path) == ESP_OK);
    assert(solar_os_map_layer_load(deflated_path) == ESP_OK);
    assert(solar_os_map_layer_count() == 2U);

    const solar_os_map_geometry_t *from_plain = solar_os_map_layer(0U);
    const solar_os_map_geometry_t *from_deflated = solar_os_map_layer(1U);
    assert(from_plain != NULL && from_deflated != NULL);
    assert(from_plain->ring_count == from_deflated->ring_count);
    assert(from_plain->point_count == from_deflated->point_count);
    assert(from_plain->size == from_deflated->size);
    assert(memcmp(from_plain->data, from_deflated->data, from_plain->size) == 0);

    /* GeoJSON comes in the same two ways, and reads the same both times. */
    static const char *const text =
        "{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[1,0],[1,1],[0,1]]]}";
    const char *plain_json = "map_layers_test_plain.geojson";
    const char *deflated_json = "map_layers_test_deflated.geojson";
    write_file(plain_json, text, strlen(text));
    write_gzip_file(deflated_json, text, strlen(text));
    solar_os_map_layer_clear();
    assert(solar_os_map_layer_load(plain_json) == ESP_OK);
    assert(solar_os_map_layer_load(deflated_json) == ESP_OK);
    assert(solar_os_map_layer_count() == 2U);
    assert(solar_os_map_layer(0U)->size == solar_os_map_layer(1U)->size);
    assert(memcmp(solar_os_map_layer(0U)->data, solar_os_map_layer(1U)->data,
                  solar_os_map_layer(0U)->size) == 0);

    solar_os_map_layer_clear();
    free(packed);
    (void)unlink(plain_path);
    (void)unlink(deflated_path);
    (void)unlink(plain_json);
    (void)unlink(deflated_json);
}

/*
 * Slots run out before memory does on a small enough map, and what leaves
 * must be whatever nobody has looked at for longest - never the layer just
 * asked for, and never one the drawing code says is still on screen.
 */
static void test_the_unseen_layer_leaves(void)
{
    size_t size = 0U;
    uint8_t *packed = pack_sample_layer(&size);
    assert(packed != NULL);
    solar_os_map_layer_clear();

    char paths[SOLAR_OS_MAP_LAYER_MAX + 1U][64];
    for (size_t index = 0U; index <= SOLAR_OS_MAP_LAYER_MAX; index++) {
        (void)snprintf(paths[index], sizeof(paths[index]),
                       "map_layers_test_%02u.bin", (unsigned)index);
        write_file(paths[index], packed, size);
    }

    /* Fill every slot, then keep the first one wanted. */
    for (size_t index = 0U; index < SOLAR_OS_MAP_LAYER_MAX; index++) {
        assert(solar_os_map_layer_load(paths[index]) == ESP_OK);
    }
    assert(solar_os_map_layer_count() == SOLAR_OS_MAP_LAYER_MAX);
    assert(solar_os_map_layer_bytes() == size * SOLAR_OS_MAP_LAYER_MAX);
    solar_os_map_layer_touch(0U);
    /* And a pinned layer stays whatever happens, without being touched. */
    solar_os_map_layer_pin(1U);

    /* One more than fits: the count holds and something has gone. */
    assert(solar_os_map_layer_load(paths[SOLAR_OS_MAP_LAYER_MAX]) == ESP_OK);
    assert(solar_os_map_layer_count() == SOLAR_OS_MAP_LAYER_MAX);

    /* The touched one and the pinned one both survived; the oldest of the
     * rest is what went. */
    assert(solar_os_map_layer_is_loaded("map_layers_test_00.bin"));
    assert(solar_os_map_layer_is_loaded("map_layers_test_01.bin"));
    assert(!solar_os_map_layer_is_loaded("map_layers_test_02.bin"));
    /* And the layer just asked for is present, not dropped for itself. */
    char newest[64];
    (void)snprintf(newest, sizeof(newest), "map_layers_test_%02u.bin",
                   (unsigned)SOLAR_OS_MAP_LAYER_MAX);
    assert(solar_os_map_layer_is_loaded(newest));

    solar_os_map_layer_clear();
    assert(solar_os_map_layer_bytes() == 0U);
    for (size_t index = 0U; index <= SOLAR_OS_MAP_LAYER_MAX; index++) {
        (void)unlink(paths[index]);
    }
    free(packed);
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

    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    assert(solar_os_map_geometry_next(&geometry, &cursor, &ring));
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

    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    assert(solar_os_map_geometry_next(&geometry, &cursor, &ring));
    assert(ring.point_count == 3U && !ring.open);
    assert(solar_os_map_geometry_next(&geometry, &cursor, &ring));
    assert(ring.point_count == 4U && !ring.open);
    assert(solar_os_map_geometry_next(&geometry, &cursor, &ring));
    assert(ring.point_count == 2U && ring.open);
    assert(!solar_os_map_geometry_next(&geometry, &cursor, &ring));
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
    test_packed_layer();
    test_load_named();
    test_truncated();
    test_import_deflated_or_not();
    test_the_unseen_layer_leaves();
    test_geojson_polygon();
    test_geojson_shapes();
    test_geojson_rejects();
    printf("map_layers_test ok\n");
    return 0;
}

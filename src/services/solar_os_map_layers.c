#include "solar_os_map_layers.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_map_geojson.h"
#include "solar_os_memory.h"

typedef struct {
    uint8_t *data;
    solar_os_map_geometry_t geometry;
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
} map_layer_t;

static solar_os_map_geometry_t map_world;
static map_layer_t map_layers[SOLAR_OS_MAP_LAYER_MAX];
static size_t map_layer_loaded;

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

esp_err_t solar_os_map_geometry_parse(const uint8_t *data,
                                      size_t size,
                                      solar_os_map_geometry_t *geometry)
{
    if (data == NULL || geometry == NULL || size < SOLAR_OS_MAP_LAYER_HEADER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(data, SOLAR_OS_MAP_LAYER_MAGIC, 4U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t version = (uint16_t)(data[4] | (data[5] << 8));
    if (version != SOLAR_OS_MAP_LAYER_VERSION) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t resolution = (uint32_t)(data[6] | (data[7] << 8)) * 100U;
    const uint32_t rings = read_u32(&data[8]);
    const uint32_t points = read_u32(&data[12]);
    const size_t expected = SOLAR_OS_MAP_LAYER_HEADER +
                            (size_t)rings * 4U + (size_t)rings * 16U +
                            (size_t)points * 8U;
    if (rings == 0U || points < rings * 2U || expected > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t counted = 0U;
    for (uint32_t index = 0U; index < rings; index++) {
        const uint32_t count =
            read_u32(&data[SOLAR_OS_MAP_LAYER_HEADER + index * 4U]) &
            SOLAR_OS_MAP_RING_COUNT_MASK;
        if (count < 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        counted += count;
    }
    if (counted != points) {
        return ESP_ERR_INVALID_SIZE;
    }
    geometry->resolution_m = resolution;
    geometry->data = data;
    geometry->size = size;
    geometry->ring_count = rings;
    geometry->point_count = points;
    return ESP_OK;
}

bool solar_os_map_geometry_next(const solar_os_map_geometry_t *geometry,
                                solar_os_map_ring_cursor_t *cursor,
                                solar_os_map_ring_t *ring)
{
    if (geometry == NULL || geometry->data == NULL || cursor == NULL ||
        ring == NULL || cursor->index >= geometry->ring_count) {
        return false;
    }
    const uint8_t *counts = &geometry->data[SOLAR_OS_MAP_LAYER_HEADER];
    const uint32_t stored = read_u32(&counts[cursor->index * 4U]);
    const size_t count = stored & SOLAR_OS_MAP_RING_COUNT_MASK;
    const size_t bounds_at = SOLAR_OS_MAP_LAYER_HEADER +
                             (size_t)geometry->ring_count * 4U +
                             (size_t)cursor->index * 16U;
    const size_t start = SOLAR_OS_MAP_LAYER_HEADER +
                         (size_t)geometry->ring_count * 20U + cursor->offset * 8U;
    ring->coordinates = (const int32_t *)(const void *)&geometry->data[start];
    ring->point_count = count;
    ring->open = (stored & SOLAR_OS_MAP_RING_OPEN) != 0U;
    const uint32_t klass = (stored & SOLAR_OS_MAP_RING_CLASS_MASK) >>
                           SOLAR_OS_MAP_RING_CLASS_SHIFT;
    ring->klass = klass < SOLAR_OS_MAP_CLASS_COUNT
                      ? (solar_os_map_class_t)klass
                      : SOLAR_OS_MAP_CLASS_LAND;
    ring->lat_min = (int32_t)read_u32(&geometry->data[bounds_at]);
    ring->lat_max = (int32_t)read_u32(&geometry->data[bounds_at + 4U]);
    ring->lon_min = (int32_t)read_u32(&geometry->data[bounds_at + 8U]);
    ring->lon_max = (int32_t)read_u32(&geometry->data[bounds_at + 12U]);
    cursor->index++;
    cursor->offset += count;
    return true;
}

size_t solar_os_map_geometry_longest_ring(const solar_os_map_geometry_t *geometry)
{
    if (geometry == NULL || geometry->data == NULL) {
        return 0U;
    }
    const uint8_t *counts = &geometry->data[SOLAR_OS_MAP_LAYER_HEADER];
    size_t longest = 0U;
    for (uint32_t index = 0U; index < geometry->ring_count; index++) {
        const size_t count =
            read_u32(&counts[index * 4U]) & SOLAR_OS_MAP_RING_COUNT_MASK;
        if (count > longest) {
            longest = count;
        }
    }
    return longest;
}

size_t solar_os_map_layer_count(void)
{
    return map_layer_loaded + 1U;
}

const solar_os_map_geometry_t *solar_os_map_layer(size_t index)
{
    if (index == 0U) {
        if (map_world.data == NULL &&
            solar_os_map_geometry_parse(solar_os_map_basemap_world,
                                        solar_os_map_basemap_world_size,
                                        &map_world) != ESP_OK) {
            return NULL;
        }
        return &map_world;
    }
    if (index > map_layer_loaded) {
        return NULL;
    }
    return &map_layers[index - 1U].geometry;
}

const char *solar_os_map_layer_name(size_t index)
{
    if (index == 0U) {
        return "world";
    }
    if (index > map_layer_loaded) {
        return "";
    }
    return map_layers[index - 1U].name;
}

size_t solar_os_map_layer_longest_ring(void)
{
    size_t longest = 0U;
    for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
        const size_t rings =
            solar_os_map_geometry_longest_ring(solar_os_map_layer(index));
        if (rings > longest) {
            longest = rings;
        }
    }
    return longest;
}

const char *solar_os_map_class_name(solar_os_map_class_t klass)
{
    switch (klass) {
    case SOLAR_OS_MAP_CLASS_WATER:
        return "water";
    case SOLAR_OS_MAP_CLASS_ROAD:
        return "road";
    case SOLAR_OS_MAP_CLASS_RAIL:
        return "rail";
    case SOLAR_OS_MAP_CLASS_BUILDING:
        return "building";
    case SOLAR_OS_MAP_CLASS_BOUNDARY:
        return "boundary";
    default:
        return "land";
    }
}

static void layer_name_from_path(const char *path, char *name, size_t name_len)
{
    const char *slash = strrchr(path, '/');
    snprintf(name, name_len, "%s", slash != NULL ? slash + 1U : path);
}

static esp_err_t layer_read_packed(FILE *file, uint8_t **out, size_t *out_size)
{
    if (fseek(file, 0, SEEK_END) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = solar_os_memory_calloc(1U,
                                           (size_t)size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.layer");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    *out = data;
    *out_size = (size_t)size;
    return ESP_OK;
}

esp_err_t solar_os_map_layer_load(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (map_layer_loaded >= SOLAR_OS_MAP_LAYER_MAX) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char magic[4] = {0};
    const bool packed = fread(magic, 1U, sizeof(magic), file) == sizeof(magic) &&
                        memcmp(magic, SOLAR_OS_MAP_LAYER_MAGIC, 4U) == 0;
    rewind(file);

    uint8_t *data = NULL;
    size_t size = 0U;
    esp_err_t error = packed ? layer_read_packed(file, &data, &size)
                             : solar_os_map_geojson_read(file, &data, &size);
    fclose(file);
    if (error != ESP_OK) {
        return error;
    }

    map_layer_t *layer = &map_layers[map_layer_loaded];
    error = solar_os_map_geometry_parse(data, size, &layer->geometry);
    if (error != ESP_OK) {
        solar_os_memory_free(data);
        memset(layer, 0, sizeof(*layer));
        return error;
    }
    layer->data = data;
    layer_name_from_path(path, layer->name, sizeof(layer->name));
    map_layer_loaded++;
    return ESP_OK;
}

esp_err_t solar_os_map_layer_remove(size_t index)
{
    if (index == 0U || index > map_layer_loaded) {
        return ESP_ERR_INVALID_ARG;
    }
    map_layer_t *layer = &map_layers[index - 1U];
    solar_os_memory_free(layer->data);
    for (size_t current = index - 1U; current + 1U < map_layer_loaded; current++) {
        map_layers[current] = map_layers[current + 1U];
    }
    map_layer_loaded--;
    memset(&map_layers[map_layer_loaded], 0, sizeof(map_layers[0]));
    return ESP_OK;
}

void solar_os_map_layer_clear(void)
{
    while (map_layer_loaded > 0U) {
        (void)solar_os_map_layer_remove(map_layer_loaded);
    }
}

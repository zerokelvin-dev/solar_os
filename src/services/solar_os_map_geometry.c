#include "solar_os_map_layers.h"

#include <string.h>

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)(value >> 8);
}

/* The fewest bits that bring both spans of a ring within sixteen. A span is
 * at most 2^32 - 1, so sixteen always does. */
static uint8_t ring_shift(int32_t lat_min,
                          int32_t lat_max,
                          int32_t lon_min,
                          int32_t lon_max)
{
    const uint32_t lat_span = (uint32_t)lat_max - (uint32_t)lat_min;
    const uint32_t lon_span = (uint32_t)lon_max - (uint32_t)lon_min;
    const uint32_t span = lat_span > lon_span ? lat_span : lon_span;
    uint8_t shift = 0U;
    while ((span >> shift) > 0xFFFFU) {
        shift++;
    }
    return shift;
}

size_t solar_os_map_geometry_pack(uint8_t *data, size_t size)
{
    if (data == NULL || size < SOLAR_OS_MAP_LAYER_HEADER) {
        return 0U;
    }
    const uint32_t rings = read_u32(&data[8]);
    const uint32_t points = read_u32(&data[12]);
    const size_t start = SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 20U;
    if (start + (size_t)points * 8U > size) {
        return 0U;
    }
    /* A point's packed four bytes never land past its own unpacked eight, so
     * walking forward only overwrites points already read. */
    size_t point = 0U;
    for (uint32_t index = 0U; index < rings; index++) {
        const size_t count =
            read_u32(&data[SOLAR_OS_MAP_LAYER_HEADER + index * 4U]) &
            SOLAR_OS_MAP_RING_COUNT_MASK;
        if (count > (size_t)points - point) {
            return 0U;
        }
        const uint8_t *bounds =
            &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 4U + index * 16U];
        const uint32_t lat_min = read_u32(&bounds[0]);
        const uint32_t lon_min = read_u32(&bounds[8]);
        const uint8_t shift = ring_shift((int32_t)lat_min,
                                         (int32_t)read_u32(&bounds[4]),
                                         (int32_t)lon_min,
                                         (int32_t)read_u32(&bounds[12]));
        for (size_t end = point + count; point < end; point++) {
            const uint32_t lat = read_u32(&data[start + point * 8U]);
            const uint32_t lon = read_u32(&data[start + point * 8U + 4U]);
            write_u16(&data[start + point * 4U],
                      (uint16_t)((lat - lat_min) >> shift));
            write_u16(&data[start + point * 4U + 2U],
                      (uint16_t)((lon - lon_min) >> shift));
        }
    }
    return start + (size_t)points * 4U;
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
    /*
     * Every ring costs twenty bytes of table and every point four, so a
     * file of this size cannot hold more than this many of either. Checked
     * before the multiplication, because size_t is thirty-two bits on the
     * device and the products of a crafted header wrap to nothing.
     *
     * A caller that sized its buffer from this same header gets nothing from
     * these three, because they then compare the header against itself. What
     * holds such a file together is the per-ring check below.
     */
    if (rings == 0U || rings > size / 20U || points > size / 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t expected = SOLAR_OS_MAP_LAYER_HEADER +
                            (size_t)rings * 4U + (size_t)rings * 16U +
                            (size_t)points * 4U;
    if (points < (size_t)rings * 2U || expected > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t counted = 0U;
    uint32_t class_rings[SOLAR_OS_MAP_CLASS_COUNT] = {0};
    for (uint32_t index = 0U; index < rings; index++) {
        const uint32_t word =
            read_u32(&data[SOLAR_OS_MAP_LAYER_HEADER + index * 4U]);
        const uint32_t count = word & SOLAR_OS_MAP_RING_COUNT_MASK;
        /*
         * Checked against the room left rather than summed and compared at
         * the end: a ring count runs to sixteen million, size_t is thirty-two
         * bits on the device, and a few hundred crafted rings sum past the
         * wrap to land back on the right total. The subtraction cannot
         * underflow because counted never passes points.
         */
        if (count < 2U || count > points - counted) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t klass =
            (word & SOLAR_OS_MAP_RING_CLASS_MASK) >> SOLAR_OS_MAP_RING_CLASS_SHIFT;
        if (klass < SOLAR_OS_MAP_CLASS_COUNT) {
            class_rings[klass]++;
        }
        counted += count;
    }
    if (counted != points) {
        return ESP_ERR_INVALID_SIZE;
    }
    geometry->resolution_m = resolution;
    memcpy(geometry->class_rings, class_rings, sizeof(class_rings));
    geometry->lat_min = INT32_MAX;
    geometry->lat_max = INT32_MIN;
    geometry->lon_min = INT32_MAX;
    geometry->lon_max = INT32_MIN;
    for (size_t klass = 0U; klass < SOLAR_OS_MAP_CLASS_COUNT; klass++) {
        geometry->class_lat_min[klass] = INT32_MAX;
        geometry->class_lat_max[klass] = INT32_MIN;
        geometry->class_lon_min[klass] = INT32_MAX;
        geometry->class_lon_max[klass] = INT32_MIN;
    }
    const size_t bounds = SOLAR_OS_MAP_LAYER_HEADER + (size_t)rings * 4U;
    for (uint32_t index = 0U; index < rings; index++) {
        const uint8_t *entry = &data[bounds + (size_t)index * 16U];
        const int32_t lat_min = (int32_t)read_u32(&entry[0]);
        const int32_t lat_max = (int32_t)read_u32(&entry[4]);
        const int32_t lon_min = (int32_t)read_u32(&entry[8]);
        const int32_t lon_max = (int32_t)read_u32(&entry[12]);
        geometry->lat_min = lat_min < geometry->lat_min ? lat_min
                                                       : geometry->lat_min;
        geometry->lat_max = lat_max > geometry->lat_max ? lat_max
                                                       : geometry->lat_max;
        geometry->lon_min = lon_min < geometry->lon_min ? lon_min
                                                       : geometry->lon_min;
        geometry->lon_max = lon_max > geometry->lon_max ? lon_max
                                                       : geometry->lon_max;
        const uint32_t klass =
            (read_u32(&data[SOLAR_OS_MAP_LAYER_HEADER + index * 4U]) &
             SOLAR_OS_MAP_RING_CLASS_MASK) >> SOLAR_OS_MAP_RING_CLASS_SHIFT;
        if (klass < SOLAR_OS_MAP_CLASS_COUNT) {
            int32_t *c_lat_min = &geometry->class_lat_min[klass];
            int32_t *c_lat_max = &geometry->class_lat_max[klass];
            int32_t *c_lon_min = &geometry->class_lon_min[klass];
            int32_t *c_lon_max = &geometry->class_lon_max[klass];
            *c_lat_min = lat_min < *c_lat_min ? lat_min : *c_lat_min;
            *c_lat_max = lat_max > *c_lat_max ? lat_max : *c_lat_max;
            *c_lon_min = lon_min < *c_lon_min ? lon_min : *c_lon_min;
            *c_lon_max = lon_max > *c_lon_max ? lon_max : *c_lon_max;
        }
    }
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
    /* Never hand out a ring reaching past the points there are. */
    if (count > (size_t)geometry->point_count - cursor->offset) {
        return false;
    }
    const size_t bounds_at = SOLAR_OS_MAP_LAYER_HEADER +
                             (size_t)geometry->ring_count * 4U +
                             (size_t)cursor->index * 16U;
    const size_t start = SOLAR_OS_MAP_LAYER_HEADER +
                         (size_t)geometry->ring_count * 20U + cursor->offset * 4U;
    ring->points = (const uint16_t *)(const void *)&geometry->data[start];
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
    ring->shift = ring_shift(ring->lat_min, ring->lat_max,
                             ring->lon_min, ring->lon_max);
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

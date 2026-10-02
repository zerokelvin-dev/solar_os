#include "solar_os_map_layers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_inflate.h"
#include "solar_os_map_geojson.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

typedef struct {
    /* Owned memory, freed when the layer goes. */
    uint8_t *data;
    size_t size;
    solar_os_map_geometry_t geometry;
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    /*
     * When this was last wanted. Ordering between layers seen in the same
     * frame follows the order they were drawn, not true recency, which is
     * enough: what matters is that a layer nobody has drawn at all carries
     * an older stamp than every layer that was.
     */
    uint32_t seen;
    /* Never leaves: the backdrop, not a place somebody panned to. */
    bool pinned;
} map_layer_t;

static map_layer_t map_layers[SOLAR_OS_MAP_LAYER_MAX];
static size_t map_layer_loaded;
/* Counts upward, so the smallest seen is the one nobody has looked at. */
static uint32_t map_layer_clock;

static bool layer_evict_oldest(size_t keep);
static esp_err_t layer_find_kept(const char *what, char *path, size_t capacity);

/*
 * Whether a kept or loaded map goes by this name. The suffix is the
 * store's business, not the caller's: a cell is shown without one and a
 * file is listed with one, and both name the same map.
 */
static bool layer_name_matches(const char *stored, const char *asked)
{
    if (stored == NULL || asked == NULL) {
        return false;
    }
    if (strcmp(stored, asked) == 0) {
        return true;
    }
    const size_t length = strlen(asked);
    return strncmp(stored, asked, length) == 0 &&
           strcmp(&stored[length], ".bin") == 0;
}
static uint32_t map_layer_generation;


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
    /*
     * Every ring costs twenty bytes of table and every point eight, so a
     * file of this size cannot hold more than this many of either. Checked
     * before the multiplication, because size_t is thirty-two bits on the
     * device and the products of a crafted header wrap to nothing.
     *
     * A caller that sized its buffer from this same header gets nothing from
     * these three, because they then compare the header against itself. What
     * holds such a file together is the per-ring check below.
     */
    if (rings == 0U || rings > size / 20U || points > size / 8U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t expected = SOLAR_OS_MAP_LAYER_HEADER +
                            (size_t)rings * 4U + (size_t)rings * 16U +
                            (size_t)points * 8U;
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
    return map_layer_loaded;
}

uint32_t solar_os_map_layer_generation(void)
{
    return map_layer_generation;
}

const solar_os_map_geometry_t *solar_os_map_layer(size_t index)
{
    if (index >= map_layer_loaded) {
        return NULL;
    }
    return &map_layers[index].geometry;
}

const char *solar_os_map_layer_name(size_t index)
{
    if (index >= map_layer_loaded) {
        return "";
    }
    return map_layers[index].name;
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


static void layer_name_from_path(const char *path, char *name, size_t name_len)
{
    const char *slash = strrchr(path, '/');
    snprintf(name, name_len, "%s", slash != NULL ? slash + 1U : path);
}

/*
 * The header says how many rings and points follow, so the size of a packed
 * layer is read out of it rather than asked of the file, which a deflated
 * one cannot answer. The counts come off the card, so they are bounded
 * before they are multiplied out into an allocation.
 */
static esp_err_t layer_read_packed(solar_os_inflate_t *reader,
                                   uint8_t **out,
                                   size_t *out_size)
{
    uint8_t header[SOLAR_OS_MAP_LAYER_HEADER];
    if (solar_os_inflate_read(reader, header, sizeof(header)) !=
        sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t rings = read_u32(&header[8]);
    const uint32_t points = read_u32(&header[12]);
    if (rings == 0U || points == 0U ||
        rings > SOLAR_OS_MAP_LAYER_MAX_BYTES / 20U ||
        points > SOLAR_OS_MAP_LAYER_MAX_BYTES / 8U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t size = sizeof(header) + (size_t)rings * 20U +
                        (size_t)points * 8U;
    if (size > SOLAR_OS_MAP_LAYER_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = solar_os_memory_calloc(1U,
                                           size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.layer");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(data, header, sizeof(header));
    const size_t rest = size - sizeof(header);
    if (solar_os_inflate_read(reader, &data[sizeof(header)], rest) != rest) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    *out = data;
    *out_size = size;
    return ESP_OK;
}

static esp_err_t map_layer_load_file(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (map_layer_loaded >= SOLAR_OS_MAP_LAYER_MAX &&
        !layer_evict_oldest(SOLAR_OS_MAP_LAYER_MAX)) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /*
     * Everything arrives through the reader, so a map kept deflated reads
     * exactly like one kept in the clear and neither needs saying which it
     * is. What kind of map it is comes from its first four bytes, not from
     * what somebody called the file.
     */
    solar_os_inflate_t *reader = NULL;
    esp_err_t error = solar_os_inflate_open(file, &reader);
    if (error != ESP_OK) {
        fclose(file);
        return error;
    }
    char magic[4] = {0};
    const bool packed =
        solar_os_inflate_read(reader, magic, sizeof(magic)) == sizeof(magic) &&
        memcmp(magic, SOLAR_OS_MAP_LAYER_MAGIC, 4U) == 0;
    error = solar_os_inflate_rewind(reader);

    uint8_t *data = NULL;
    size_t size = 0U;
    if (error == ESP_OK) {
        error = packed ? layer_read_packed(reader, &data, &size)
                       : solar_os_map_geojson_read(
                             reader, SOLAR_OS_MAP_CLASS_LAND, &data, &size);
    }
    solar_os_inflate_close(reader);
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
    layer->size = size;
    layer->seen = ++map_layer_clock;
    layer_name_from_path(path, layer->name, sizeof(layer->name));
    map_layer_loaded++;
    map_layer_generation++;
    return ESP_OK;
}

size_t solar_os_map_layer_bytes(void)
{
    size_t total = 0U;
    for (size_t index = 0U; index < map_layer_loaded; index++) {
        total += map_layers[index].size;
    }
    return total;
}

void solar_os_map_layer_touch(size_t index)
{
    if (index < map_layer_loaded) {
        map_layers[index].seen = ++map_layer_clock;
    }
}

void solar_os_map_layer_pin(size_t index)
{
    if (index < map_layer_loaded) {
        map_layers[index].pinned = true;
    }
}

bool solar_os_map_layer_pinned(size_t index)
{
    return index < map_layer_loaded && map_layers[index].pinned;
}

/*
 * Puts away whatever nobody has looked at for longest, and never the layer
 * just asked for - somebody wanting a map and getting it dropped again to
 * make room for itself would be worse than refusing.
 */
static bool layer_evict_oldest(size_t keep)
{
    size_t oldest = SOLAR_OS_MAP_LAYER_MAX;
    for (size_t index = 0U; index < map_layer_loaded; index++) {
        if (index == keep || map_layers[index].pinned) {
            continue;
        }
        if (oldest == SOLAR_OS_MAP_LAYER_MAX ||
            map_layers[index].seen < map_layers[oldest].seen) {
            oldest = index;
        }
    }
    if (oldest == SOLAR_OS_MAP_LAYER_MAX) {
        return false;
    }
    return solar_os_map_layer_remove(oldest) == ESP_OK;
}


/* Where imported layers are kept, created if it is not there yet. */
static esp_err_t layer_store_dir(char *path, size_t capacity)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error =
        solar_os_storage_default_path(SOLAR_OS_MAP_LAYER_DIR, path, capacity);
    if (error != ESP_OK) {
        return error;
    }
    return solar_os_storage_makedirs(path, true);
}

/* The imported copy of one layer, named for the layer itself. */
static esp_err_t layer_store_path(const char *name, char *path, size_t capacity)
{
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t error = layer_store_dir(directory, sizeof(directory));
    if (error != ESP_OK) {
        return error;
    }
    (void)snprintf(path, capacity, "%s/%s", directory, name);
    return ESP_OK;
}

/*
 * Loading a layer imports it: the file is copied in beside the map's own
 * points, so it can be loaded again without fetching. There is no list of
 * layers to keep in step with the files, because the files are the list,
 * keyed by the layer's own name. Nothing is read at startup; what is on the
 * card is a directory listing until somebody asks for one of them.
 */
esp_err_t solar_os_map_layer_load(const char *path)
{
    /* Loading what is already loaded replaced nothing and appended a second
     * copy of it, drawn twice and counted twice against the slots. */
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    layer_name_from_path(path, name, sizeof(name));
    for (size_t index = 0U; index < map_layer_loaded; index++) {
        if (strcmp(map_layers[index].name, name) == 0) {
            (void)solar_os_map_layer_remove(index);
            break;
        }
    }
    const esp_err_t error = map_layer_load_file(path);
    if (error != ESP_OK) {
        return error;
    }
    /*
     * A layer's size is only known once it is read, so the budget is
     * brought back afterwards rather than predicted. The peak is one layer
     * over, which is why the budget leaves that much room.
     */
    while (map_layer_loaded > 1U &&
           solar_os_map_layer_bytes() > SOLAR_OS_MAP_LAYER_BUDGET_BYTES) {
        if (!layer_evict_oldest(map_layer_loaded - 1U)) {
            break;
        }
    }
    char stored[SOLAR_OS_STORAGE_PATH_MAX];
    if (layer_store_path(name, stored, sizeof(stored)) != ESP_OK) {
        return ESP_OK;
    }
    if (strcmp(stored, path) != 0) {
        (void)solar_os_storage_copy_file(path, stored);
    }
    return ESP_OK;
}

/*
 * Walks the kept maps, handing each to a visitor until it says to stop.
 * The directory is the list, so every question about what is kept is this
 * walk with a different visitor, and none of them needs an array big
 * enough to hold the answer first.
 */
typedef bool (*layer_kept_fn)(void *user, const char *name, uint64_t size);

static void layer_walk_kept(layer_kept_fn visit, void *user)
{
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    if (layer_store_dir(directory, sizeof(directory)) != ESP_OK) {
        return;
    }
    size_t cursor = 0U;
    bool more = true;
    while (more) {
        solar_os_storage_entry_t entries[8];
        size_t count = 0U;
        if (solar_os_storage_scandir(directory, cursor, 8U, entries, &count,
                                     &cursor, &more) != ESP_OK) {
            return;
        }
        for (size_t index = 0U; index < count; index++) {
            if (entries[index].metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
                continue;
            }
            if (!visit(user, entries[index].name,
                       entries[index].metadata.size_bytes)) {
                return;
            }
        }
    }
}

typedef struct {
    solar_os_map_stored_t *stored;
    size_t limit;
    size_t found;
} layer_list_t;

static bool layer_list_visit(void *user, const char *name, uint64_t size)
{
    layer_list_t *list = user;
    strlcpy(list->stored[list->found].name, name,
            sizeof(list->stored[list->found].name));
    list->stored[list->found].size_bytes = size;
    list->stored[list->found].loaded = solar_os_map_layer_is_loaded(name);
    list->found++;
    return list->found < list->limit;
}

/*
 * What has been kept, without reading any of it. A layer is megabytes and
 * a name is nothing, so knowing what is on the card costs a directory
 * listing, and only what somebody asks for is read into memory.
 */
size_t solar_os_map_layer_stored(solar_os_map_stored_t *stored, size_t limit)
{
    if (stored == NULL || limit == 0U) {
        return 0U;
    }
    layer_list_t list = {.stored = stored, .limit = limit};
    layer_walk_kept(layer_list_visit, &list);
    return list.found;
}

/* How many are kept, which is more than any one listing may show. */
static bool layer_count_visit(void *user, const char *name, uint64_t size)
{
    (void)name;
    (void)size;
    (*(size_t *)user)++;
    return true;
}

size_t solar_os_map_layer_stored_count(void)
{
    size_t count = 0U;
    layer_walk_kept(layer_count_visit, &count);
    return count;
}

/*
 * Throws away the copy kept on the card. Separate from unloading, which
 * only gives back the memory: a reader who puts a map away expects to find
 * it again, and fetching it a second time costs somebody else's bandwidth.
 */
esp_err_t solar_os_map_layer_forget(const char *name)
{
    char stored[SOLAR_OS_STORAGE_PATH_MAX];
    if (name == NULL || layer_find_kept(name, stored, sizeof(stored)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Memory first, then the file: forgetting a loaded map should not
     * leave it drawn from a copy that no longer exists. */
    (void)solar_os_map_layer_unload(name);
    return solar_os_storage_remove(stored);
}

/*
 * Whatever the reader has to hand, told apart by its first character, so
 * the answer never depends on what happens to be on the card:
 *
 *   /  a path to a file anywhere, copied in and kept like any other
 *   n  or s, a cell, named the way the map shows it
 *   0  a digit, a place in the kept list
 *
 * Anything else is taken as the name of a kept map, which is how a file
 * somebody brought along keeps working after it has been copied in.
 */
typedef struct {
    const char *what;
    size_t asked;
    /* A place in the list when looking by number, or none when by name. */
    bool by_index;
    size_t wanted;
    size_t seen;
    char name[SOLAR_OS_MAP_LAYER_NAME_MAX];
    bool found;
} layer_seek_t;

static bool layer_seek_visit(void *user, const char *name, uint64_t size)
{
    (void)size;
    layer_seek_t *seek = user;
    /* Named with or without the extension, since a cell is shown without
     * one and a file is listed with one. */
    const bool matches = seek->by_index
                             ? seek->seen++ == seek->wanted
                             : layer_name_matches(name, seek->what);
    if (!matches) {
        return true;
    }
    strlcpy(seek->name, name, sizeof(seek->name));
    seek->found = true;
    return false;
}

esp_err_t solar_os_map_layer_keep(const char *name,
                                  const uint8_t *data,
                                  size_t size)
{
    if (name == NULL || data == NULL || size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char file[SOLAR_OS_MAP_LAYER_NAME_MAX];
    (void)snprintf(file, sizeof(file), "%s.bin", name);
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t error = layer_store_path(file, path, sizeof(path));
    if (error != ESP_OK) {
        return error;
    }
    FILE *out = fopen(path, "wb");
    if (out == NULL) {
        return ESP_FAIL;
    }
    const bool wrote = fwrite(data, 1U, size, out) == size;
    if (fclose(out) != 0 || !wrote) {
        (void)solar_os_storage_remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t solar_os_map_layer_unload(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = map_layer_loaded; index-- > 0U;) {
        if (layer_name_matches(map_layers[index].name, name)) {
            return solar_os_map_layer_remove(index);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool solar_os_map_layer_is_kept(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    layer_seek_t seek = {.what = name, .asked = strlen(name)};
    layer_walk_kept(layer_seek_visit, &seek);
    return seek.found;
}

static esp_err_t layer_find_kept(const char *what, char *path, size_t capacity)
{
    layer_seek_t seek = {.what = what, .asked = strlen(what)};
    layer_walk_kept(layer_seek_visit, &seek);
    return seek.found ? layer_store_path(seek.name, path, capacity)
                      : ESP_ERR_NOT_FOUND;
}

/* The kept list is the directory, so a number is a place in that walk and
 * is not limited by how many layers can be loaded at once. */
static esp_err_t layer_find_kept_index(size_t wanted,
                                       char *path,
                                       size_t capacity)
{
    layer_seek_t seek = {.by_index = true, .wanted = wanted};
    layer_walk_kept(layer_seek_visit, &seek);
    return seek.found ? layer_store_path(seek.name, path, capacity)
                      : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_map_layer_load_named(const char *what)
{
    if (what == NULL || what[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (what[0] == '/') {
        return solar_os_map_layer_load(what);
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (what[0] >= '0' && what[0] <= '9') {
        const esp_err_t error =
            layer_find_kept_index(strtoul(what, NULL, 10), path, sizeof(path));
        return error == ESP_OK ? solar_os_map_layer_load(path) : error;
    }

    const esp_err_t error = layer_find_kept(what, path, sizeof(path));
    return error == ESP_OK ? solar_os_map_layer_load(path) : error;
}

bool solar_os_map_layer_is_loaded(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (size_t index = 0U; index < map_layer_loaded; index++) {
        if (layer_name_matches(map_layers[index].name, name)) {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_map_layer_remove(size_t index)
{
    if (index >= map_layer_loaded) {
        return ESP_ERR_INVALID_ARG;
    }
    map_layer_t *layer = &map_layers[index];
    solar_os_memory_free(layer->data);
    for (size_t current = index; current + 1U < map_layer_loaded; current++) {
        map_layers[current] = map_layers[current + 1U];
    }
    map_layer_loaded--;
    map_layer_generation++;
    memset(&map_layers[map_layer_loaded], 0, sizeof(map_layers[0]));
    return ESP_OK;
}

void solar_os_map_layer_clear(void)
{
    for (size_t index = map_layer_loaded; index-- > 0U;) {
        (void)solar_os_map_layer_remove(index);
    }
}

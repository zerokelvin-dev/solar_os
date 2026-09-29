#include "solar_os_map.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"

typedef struct {
    bool used;
    solar_os_map_point_t point;
} map_slot_t;

typedef struct {
    bool used;
    solar_os_map_path_t path;
} map_path_slot_t;

static StaticSemaphore_t map_mutex_storage;
static SemaphoreHandle_t map_mutex;
static map_slot_t *map_slots;
static map_path_slot_t map_paths[SOLAR_OS_MAP_PATH_CAPACITY];
static size_t map_path_count;
static size_t map_count;
static uint32_t map_next_id = 1U;
static uint32_t map_evicted;
static uint32_t map_generation;

static uint32_t map_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void map_lock(void)
{
    xSemaphoreTake(map_mutex, portMAX_DELAY);
}

static void map_unlock(void)
{
    xSemaphoreGive(map_mutex);
}

static void map_copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = 0U;
    if (source != NULL) {
        while (source[length] != '\0' && length + 1U < capacity) {
            const unsigned char ch = (unsigned char)source[length];
            destination[length] = ch < 0x20U || ch == 0x7fU ? ' ' : (char)ch;
            length++;
        }
    }
    destination[length] = '\0';
}

static bool map_kind_valid(solar_os_map_kind_t kind)
{
    return kind == SOLAR_OS_MAP_KIND_SELF || kind == SOLAR_OS_MAP_KIND_NODE ||
           kind == SOLAR_OS_MAP_KIND_WAYPOINT;
}

/* Updated times wrap; compare as signed differences. */
static bool map_older(const solar_os_map_point_t *a, const solar_os_map_point_t *b)
{
    const int32_t delta = (int32_t)(a->updated_ms - b->updated_ms);
    return delta != 0 ? delta < 0 : a->id < b->id;
}

esp_err_t solar_os_map_init(void)
{
    if (map_slots != NULL) {
        return ESP_OK;
    }
    if (map_mutex == NULL) {
        map_mutex = xSemaphoreCreateMutexStatic(&map_mutex_storage);
        if (map_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    map_slot_t *slots = solar_os_memory_calloc(SOLAR_OS_MAP_CAPACITY,
                                               sizeof(*slots),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "service.map");
    if (slots == NULL) {
        return ESP_ERR_NO_MEM;
    }
    map_lock();
    if (map_slots == NULL) {
        map_slots = slots;
        slots = NULL;
    }
    map_unlock();
    solar_os_memory_free(slots);
    return ESP_OK;
}

/* A path outlives neither of its endpoints. */
static void map_drop_paths_locked(uint32_t point_id)
{
    for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
        if (map_paths[i].used &&
            (map_paths[i].path.from_id == point_id ||
             map_paths[i].path.to_id == point_id)) {
            map_paths[i].used = false;
            map_path_count--;
        }
    }
}

static bool map_point_exists_locked(uint32_t id)
{
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (map_slots[i].used && map_slots[i].point.id == id) {
            return true;
        }
    }
    return false;
}

static map_slot_t *map_find_locked(const char *source, const char *key)
{
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (map_slots[i].used &&
            strcmp(map_slots[i].point.source, source) == 0 &&
            strcmp(map_slots[i].point.key, key) == 0) {
            return &map_slots[i];
        }
    }
    return NULL;
}

static map_slot_t *map_allocate_locked(void)
{
    map_slot_t *victim = NULL;
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (!map_slots[i].used) {
            return &map_slots[i];
        }
        if (map_slots[i].point.kind == SOLAR_OS_MAP_KIND_NODE &&
            (victim == NULL || map_older(&map_slots[i].point, &victim->point))) {
            victim = &map_slots[i];
        }
    }
    if (victim != NULL) {
        map_drop_paths_locked(victim->point.id);
        victim->used = false;
        map_count--;
        map_evicted++;
    }
    return victim;
}

esp_err_t solar_os_map_publish(const solar_os_map_publish_t *point, uint32_t *id)
{
    if (id != NULL) {
        *id = 0U;
    }
    if (point == NULL || point->source == NULL || point->source[0] == '\0' ||
        point->key == NULL || point->key[0] == '\0' ||
        !map_kind_valid(point->kind) ||
        !solar_os_map_coord_valid(point->latitude_e7, point->longitude_e7)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_map_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_map_point_t entry = {0};
    map_copy_text(entry.source, sizeof(entry.source), point->source);
    map_copy_text(entry.key, sizeof(entry.key), point->key);
    map_copy_text(entry.label,
                  sizeof(entry.label),
                  point->label != NULL && point->label[0] != '\0' ? point->label
                                                                  : point->key);
    map_copy_text(entry.detail, sizeof(entry.detail), point->detail);
    entry.kind = point->kind;
    entry.latitude_e7 = point->latitude_e7;
    entry.longitude_e7 = point->longitude_e7;
    entry.timestamp_ms = point->timestamp_ms;
    entry.updated_ms = map_now_ms();

    map_lock();
    map_slot_t *slot = map_find_locked(entry.source, entry.key);
    if (slot != NULL) {
        entry.id = slot->point.id;
    } else {
        slot = map_allocate_locked();
        if (slot == NULL) {
            map_unlock();
            return ESP_ERR_NO_MEM;
        }
        entry.id = map_next_id++;
        if (map_next_id == 0U) {
            map_next_id = 1U;
        }
        map_count++;
    }
    slot->used = true;
    slot->point = entry;
    map_generation++;
    map_unlock();

    if (id != NULL) {
        *id = entry.id;
    }
    return ESP_OK;
}

esp_err_t solar_os_map_get(uint32_t id, solar_os_map_point_t *point)
{
    if (id == 0U || point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (map_slots == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (map_slots[i].used && map_slots[i].point.id == id) {
            *point = map_slots[i].point;
            err = ESP_OK;
            break;
        }
    }
    map_unlock();
    return err;
}

esp_err_t solar_os_map_remove(uint32_t id)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (map_slots == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (map_slots[i].used && map_slots[i].point.id == id) {
            map_drop_paths_locked(id);
            map_slots[i].used = false;
            map_count--;
            map_generation++;
            err = ESP_OK;
            break;
        }
    }
    map_unlock();
    return err;
}

esp_err_t solar_os_map_remove_source(const char *source, size_t *removed)
{
    if (removed != NULL) {
        *removed = 0U;
    }
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (map_slots == NULL) {
        return ESP_OK;
    }
    size_t count = 0U;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (map_slots[i].used && strcmp(map_slots[i].point.source, source) == 0) {
            map_drop_paths_locked(map_slots[i].point.id);
            map_slots[i].used = false;
            map_count--;
            count++;
        }
    }
    if (count > 0U) {
        map_generation++;
    }
    map_unlock();
    if (removed != NULL) {
        *removed = count;
    }
    return ESP_OK;
}

esp_err_t solar_os_map_clear(void)
{
    if (map_slots == NULL) {
        return ESP_OK;
    }
    map_lock();
    memset(map_slots, 0, SOLAR_OS_MAP_CAPACITY * sizeof(*map_slots));
    memset(map_paths, 0, sizeof(map_paths));
    map_count = 0U;
    map_path_count = 0U;
    map_generation++;
    map_unlock();
    return ESP_OK;
}

size_t solar_os_map_snapshot(solar_os_map_point_t *points,
                             size_t max_points,
                             size_t *total)
{
    if (total != NULL) {
        *total = 0U;
    }
    if (map_slots == NULL) {
        return 0U;
    }
    size_t copied = 0U;
    map_lock();
    if (total != NULL) {
        *total = map_count;
    }
    for (size_t i = 0U; points != NULL && i < SOLAR_OS_MAP_CAPACITY; i++) {
        if (!map_slots[i].used) {
            continue;
        }
        /* Insertion into a newest-first list capped at max_points. */
        size_t position = copied;
        while (position > 0U &&
               map_older(&points[position - 1U], &map_slots[i].point)) {
            position--;
        }
        if (position >= max_points) {
            continue;
        }
        const size_t last = copied < max_points ? copied : max_points - 1U;
        memmove(&points[position + 1U],
                &points[position],
                (last - position) * sizeof(*points));
        points[position] = map_slots[i].point;
        if (copied < max_points) {
            copied++;
        }
    }
    map_unlock();
    return copied;
}

esp_err_t solar_os_map_get_status(solar_os_map_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->capacity = SOLAR_OS_MAP_CAPACITY;
    status->path_capacity = SOLAR_OS_MAP_PATH_CAPACITY;
    if (map_slots == NULL) {
        return ESP_OK;
    }
    map_lock();
    status->initialized = true;
    status->count = map_count;
    status->path_count = map_path_count;
    status->evicted = map_evicted;
    status->generation = map_generation;
    map_unlock();
    return ESP_OK;
}

esp_err_t solar_os_map_path_publish(const solar_os_map_path_publish_t *path,
                                    uint32_t *id)
{
    if (id != NULL) {
        *id = 0U;
    }
    if (path == NULL || path->source == NULL || path->source[0] == '\0' ||
        path->key == NULL || path->key[0] == '\0' ||
        path->from_id == 0U || path->to_id == 0U ||
        path->from_id == path->to_id) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = solar_os_map_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_map_path_t entry = {0};
    map_copy_text(entry.source, sizeof(entry.source), path->source);
    map_copy_text(entry.key, sizeof(entry.key), path->key);
    map_copy_text(entry.label,
                  sizeof(entry.label),
                  path->label != NULL && path->label[0] != '\0' ? path->label
                                                                 : path->key);
    entry.from_id = path->from_id;
    entry.to_id = path->to_id;
    entry.updated_ms = map_now_ms();

    map_lock();
    if (!map_point_exists_locked(path->from_id) ||
        !map_point_exists_locked(path->to_id)) {
        map_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    map_path_slot_t *slot = NULL;
    for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
        if (map_paths[i].used &&
            strcmp(map_paths[i].path.source, entry.source) == 0 &&
            strcmp(map_paths[i].path.key, entry.key) == 0) {
            slot = &map_paths[i];
            entry.id = slot->path.id;
            break;
        }
    }
    if (slot == NULL) {
        for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
            if (!map_paths[i].used) {
                slot = &map_paths[i];
                break;
            }
        }
        if (slot == NULL) {
            map_unlock();
            return ESP_ERR_NO_MEM;
        }
        entry.id = map_next_id++;
        if (map_next_id == 0U) {
            map_next_id = 1U;
        }
        map_path_count++;
    }
    slot->used = true;
    slot->path = entry;
    map_generation++;
    map_unlock();

    if (id != NULL) {
        *id = entry.id;
    }
    return ESP_OK;
}

esp_err_t solar_os_map_path_remove(uint32_t id)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
        if (map_paths[i].used && map_paths[i].path.id == id) {
            map_paths[i].used = false;
            map_path_count--;
            map_generation++;
            err = ESP_OK;
            break;
        }
    }
    map_unlock();
    return err;
}

esp_err_t solar_os_map_path_remove_source(const char *source, size_t *removed)
{
    if (removed != NULL) {
        *removed = 0U;
    }
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t count = 0U;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
        if (map_paths[i].used &&
            strcmp(map_paths[i].path.source, source) == 0) {
            map_paths[i].used = false;
            map_path_count--;
            count++;
        }
    }
    if (count > 0U) {
        map_generation++;
    }
    map_unlock();
    if (removed != NULL) {
        *removed = count;
    }
    return ESP_OK;
}

size_t solar_os_map_path_snapshot(solar_os_map_path_t *paths,
                                  size_t max_paths,
                                  size_t *total)
{
    if (total != NULL) {
        *total = 0U;
    }
    if (paths == NULL || max_paths == 0U) {
        return 0U;
    }
    size_t written = 0U;
    map_lock();
    for (size_t i = 0U; i < SOLAR_OS_MAP_PATH_CAPACITY; i++) {
        if (!map_paths[i].used) {
            continue;
        }
        if (written < max_paths) {
            paths[written++] = map_paths[i].path;
        }
    }
    if (total != NULL) {
        *total = map_path_count;
    }
    map_unlock();
    return written;
}

const char *solar_os_map_kind_name(solar_os_map_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_MAP_KIND_SELF:
        return "self";
    case SOLAR_OS_MAP_KIND_NODE:
        return "node";
    case SOLAR_OS_MAP_KIND_WAYPOINT:
        return "waypoint";
    default:
        return "unknown";
    }
}

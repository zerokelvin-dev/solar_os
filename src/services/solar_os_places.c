#include "solar_os_places.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

/*
 * Points placed by hand are kept across a restart; points a producer placed
 * are not, since GNSS and a mesh peer republish as soon as they have
 * something to say and a stored copy would be a stale claim.
 *
 * Paths are not stored either: a path is a line between two points, and one
 * outliving what it joined would be a line to nowhere.
 */
#define PLACES_STORE_MAGIC "SOMP"
#define PLACES_STORE_VERSION 1U
#define PLACES_STORE_SOURCE SOLAR_OS_PLACES_SOURCE_USER

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t count;
} places_store_header_t;

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint8_t kind;
    char key[SOLAR_OS_PLACES_KEY_MAX];
    char label[SOLAR_OS_PLACES_LABEL_MAX];
    char detail[SOLAR_OS_PLACES_DETAIL_MAX];
} places_store_record_t;

typedef struct {
    bool used;
    solar_os_places_point_t point;
} places_slot_t;

typedef struct {
    bool used;
    solar_os_places_path_t path;
} places_path_slot_t;

static StaticSemaphore_t places_mutex_storage;
static SemaphoreHandle_t places_mutex;
static places_slot_t *places_slots;
static places_path_slot_t places_paths[SOLAR_OS_PLACES_PATH_CAPACITY];
static size_t places_path_count;
static size_t places_count;
static uint32_t places_next_id = 1U;
static uint32_t places_evicted;
static uint32_t places_generation;
/* Set while the store is being read back, so restoring does not rewrite it
 * once for every point it restores. */
static bool places_restoring;

static uint32_t places_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void places_lock(void)
{
    xSemaphoreTake(places_mutex, portMAX_DELAY);
}

static void places_unlock(void)
{
    xSemaphoreGive(places_mutex);
}

static void places_copy_text(char *destination, size_t capacity, const char *source)
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

static bool places_kind_valid(solar_os_places_kind_t kind)
{
    return kind == SOLAR_OS_PLACES_KIND_SELF || kind == SOLAR_OS_PLACES_KIND_NODE ||
           kind == SOLAR_OS_PLACES_KIND_WAYPOINT;
}

/* Updated times wrap; compare as signed differences. */
static bool places_older(const solar_os_places_point_t *a, const solar_os_places_point_t *b)
{
    const int32_t delta = (int32_t)(a->updated_ms - b->updated_ms);
    return delta != 0 ? delta < 0 : a->id < b->id;
}


/* Where the hand-placed points live, or an error when nothing is mounted. */
static esp_err_t places_store_path(char *path, size_t capacity)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = solar_os_storage_default_path(SOLAR_OS_PLACES_DIR,
                                                    directory,
                                                    sizeof(directory));
    if (error != ESP_OK) {
        return error;
    }
    error = solar_os_storage_mkdir(directory);
    if (error != ESP_OK && errno != EEXIST) {
        return error;
    }
    return solar_os_storage_default_path(SOLAR_OS_PLACES_DIR "/" SOLAR_OS_PLACES_FILE,
                                         path,
                                         capacity);
}

/* Writes every hand-placed point. Called with the lock held. */
static void places_store_save_locked(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (places_store_path(path, sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return;
    }
    places_store_header_t header = {.version = PLACES_STORE_VERSION, .count = 0U};
    memcpy(header.magic, PLACES_STORE_MAGIC, sizeof(header.magic));
    for (size_t index = 0U; index < SOLAR_OS_PLACES_CAPACITY; index++) {
        if (places_slots[index].used &&
            strcmp(places_slots[index].point.source, PLACES_STORE_SOURCE) == 0) {
            header.count++;
        }
    }
    if (fwrite(&header, sizeof(header), 1U, file) == 1U) {
        for (size_t index = 0U; index < SOLAR_OS_PLACES_CAPACITY; index++) {
            const solar_os_places_point_t *point = &places_slots[index].point;
            if (!places_slots[index].used ||
                strcmp(point->source, PLACES_STORE_SOURCE) != 0) {
                continue;
            }
            places_store_record_t record = {
                .latitude_e7 = point->latitude_e7,
                .longitude_e7 = point->longitude_e7,
                .kind = (uint8_t)point->kind,
            };
            places_copy_text(record.key, sizeof(record.key), point->key);
            places_copy_text(record.label, sizeof(record.label), point->label);
            places_copy_text(record.detail, sizeof(record.detail), point->detail);
            if (fwrite(&record, sizeof(record), 1U, file) != 1U) {
                break;
            }
        }
    }
    /*
     * A hand-placed point is worth keeping or it would not have been
     * written, and a card that has not been told to write it yet loses it
     * to the next flat battery. Closing the file is not the same as the
     * card holding it.
     */
    if (fflush(file) == 0) {
        const int descriptor = fileno(file);
        if (descriptor >= 0) {
            (void)fsync(descriptor);
        }
    }
    (void)fclose(file);
}

/*
 * Reads the stored points back and publishes them. Publishing is keyed on
 * source and key, so restoring twice settles on the same set rather than
 * two of everything.
 */
static void places_store_restore(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (places_store_path(path, sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return;
    }
    places_restoring = true;
    places_store_header_t header;
    if (fread(&header, sizeof(header), 1U, file) == 1U &&
        memcmp(header.magic, PLACES_STORE_MAGIC, sizeof(header.magic)) == 0 &&
        header.version == PLACES_STORE_VERSION) {
        for (uint16_t index = 0U; index < header.count; index++) {
            places_store_record_t record;
            if (fread(&record, sizeof(record), 1U, file) != 1U) {
                break;
            }
            record.key[sizeof(record.key) - 1U] = '\0';
            record.label[sizeof(record.label) - 1U] = '\0';
            record.detail[sizeof(record.detail) - 1U] = '\0';
            const solar_os_places_publish_t point = {
                .source = PLACES_STORE_SOURCE,
                .key = record.key,
                .label = record.label,
                .detail = record.detail,
                .kind = (solar_os_places_kind_t)record.kind,
                .latitude_e7 = record.latitude_e7,
                .longitude_e7 = record.longitude_e7,
            };
            (void)solar_os_places_publish(&point, NULL);
        }
    }
    places_restoring = false;
    (void)fclose(file);
}

/*
 * Whoever touches the places first sets them up, readers included. A reader that
 * gave up on an empty table instead reported no points at all until
 * something happened to write one, with the stored points sitting unread.
 */
static bool places_ready(void)
{
    return solar_os_places_init() == ESP_OK && places_slots != NULL;
}

esp_err_t solar_os_places_init(void)
{
    if (places_slots != NULL) {
        return ESP_OK;
    }
    if (places_mutex == NULL) {
        places_mutex = xSemaphoreCreateMutexStatic(&places_mutex_storage);
        if (places_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    places_slot_t *slots = solar_os_memory_calloc(SOLAR_OS_PLACES_CAPACITY,
                                               sizeof(*slots),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "service.places");
    if (slots == NULL) {
        return ESP_ERR_NO_MEM;
    }
    places_lock();
    if (places_slots == NULL) {
        places_slots = slots;
        slots = NULL;
    }
    places_unlock();
    solar_os_memory_free(slots);
    places_store_restore();
    return ESP_OK;
}

/* A path outlives neither of its endpoints. */
static void places_drop_paths_locked(uint32_t point_id)
{
    for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
        if (places_paths[i].used &&
            (places_paths[i].path.from_id == point_id ||
             places_paths[i].path.to_id == point_id)) {
            places_paths[i].used = false;
            places_path_count--;
        }
    }
}

static bool places_point_exists_locked(uint32_t id)
{
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (places_slots[i].used && places_slots[i].point.id == id) {
            return true;
        }
    }
    return false;
}

static places_slot_t *places_find_locked(const char *source, const char *key)
{
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (places_slots[i].used &&
            strcmp(places_slots[i].point.source, source) == 0 &&
            strcmp(places_slots[i].point.key, key) == 0) {
            return &places_slots[i];
        }
    }
    return NULL;
}

static places_slot_t *places_allocate_locked(void)
{
    places_slot_t *victim = NULL;
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (!places_slots[i].used) {
            return &places_slots[i];
        }
        if (places_slots[i].point.kind == SOLAR_OS_PLACES_KIND_NODE &&
            (victim == NULL || places_older(&places_slots[i].point, &victim->point))) {
            victim = &places_slots[i];
        }
    }
    if (victim != NULL) {
        places_drop_paths_locked(victim->point.id);
        victim->used = false;
        places_count--;
        places_evicted++;
    }
    return victim;
}

esp_err_t solar_os_places_publish(const solar_os_places_publish_t *point, uint32_t *id)
{
    if (id != NULL) {
        *id = 0U;
    }
    if (point == NULL || point->source == NULL || point->source[0] == '\0' ||
        point->key == NULL || point->key[0] == '\0' ||
        !places_kind_valid(point->kind) ||
        !solar_os_map_coord_valid(point->latitude_e7, point->longitude_e7)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_places_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_places_point_t entry = {0};
    places_copy_text(entry.source, sizeof(entry.source), point->source);
    places_copy_text(entry.key, sizeof(entry.key), point->key);
    places_copy_text(entry.label,
                  sizeof(entry.label),
                  point->label != NULL && point->label[0] != '\0' ? point->label
                                                                  : point->key);
    places_copy_text(entry.detail, sizeof(entry.detail), point->detail);
    entry.kind = point->kind;
    entry.latitude_e7 = point->latitude_e7;
    entry.longitude_e7 = point->longitude_e7;
    entry.timestamp_ms = point->timestamp_ms;
    entry.updated_ms = places_now_ms();

    places_lock();
    places_slot_t *slot = places_find_locked(entry.source, entry.key);
    if (slot != NULL) {
        entry.id = slot->point.id;
    } else {
        slot = places_allocate_locked();
        if (slot == NULL) {
            places_unlock();
            return ESP_ERR_NO_MEM;
        }
        entry.id = places_next_id++;
        if (places_next_id == 0U) {
            places_next_id = 1U;
        }
        places_count++;
    }
    slot->used = true;
    slot->point = entry;
    places_generation++;
    if (!places_restoring && strcmp(entry.source, PLACES_STORE_SOURCE) == 0) {
        places_store_save_locked();
    }
    places_unlock();

    if (id != NULL) {
        *id = entry.id;
    }
    return ESP_OK;
}

esp_err_t solar_os_places_get(uint32_t id, solar_os_places_point_t *point)
{
    if (id == 0U || point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!places_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    places_lock();
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (places_slots[i].used && places_slots[i].point.id == id) {
            *point = places_slots[i].point;
            err = ESP_OK;
            break;
        }
    }
    places_unlock();
    return err;
}

esp_err_t solar_os_places_remove(uint32_t id)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!places_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    places_lock();
    bool stored = false;
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (places_slots[i].used && places_slots[i].point.id == id) {
            stored = strcmp(places_slots[i].point.source, PLACES_STORE_SOURCE) == 0;
            places_drop_paths_locked(id);
            places_slots[i].used = false;
            places_count--;
            places_generation++;
            err = ESP_OK;
            break;
        }
    }
    if (stored) {
        places_store_save_locked();
    }
    places_unlock();
    return err;
}

esp_err_t solar_os_places_remove_source(const char *source, size_t *removed)
{
    if (removed != NULL) {
        *removed = 0U;
    }
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!places_ready()) {
        return ESP_OK;
    }
    size_t count = 0U;
    places_lock();
    for (size_t i = 0U; i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (places_slots[i].used && strcmp(places_slots[i].point.source, source) == 0) {
            places_drop_paths_locked(places_slots[i].point.id);
            places_slots[i].used = false;
            places_count--;
            count++;
        }
    }
    if (count > 0U) {
        places_generation++;
        if (strcmp(source, PLACES_STORE_SOURCE) == 0) {
            places_store_save_locked();
        }
    }
    places_unlock();
    if (removed != NULL) {
        *removed = count;
    }
    return ESP_OK;
}

esp_err_t solar_os_places_clear(void)
{
    if (!places_ready()) {
        return ESP_OK;
    }
    places_lock();
    memset(places_slots, 0, SOLAR_OS_PLACES_CAPACITY * sizeof(*places_slots));
    memset(places_paths, 0, sizeof(places_paths));
    places_count = 0U;
    places_path_count = 0U;
    places_generation++;
    places_store_save_locked();
    places_unlock();
    return ESP_OK;
}

size_t solar_os_places_snapshot(solar_os_places_point_t *points,
                             size_t max_points,
                             size_t *total)
{
    if (total != NULL) {
        *total = 0U;
    }
    if (!places_ready()) {
        return 0U;
    }
    size_t copied = 0U;
    places_lock();
    if (total != NULL) {
        *total = places_count;
    }
    for (size_t i = 0U; points != NULL && i < SOLAR_OS_PLACES_CAPACITY; i++) {
        if (!places_slots[i].used) {
            continue;
        }
        /* Insertion into a newest-first list capped at max_points. */
        size_t position = copied;
        while (position > 0U &&
               places_older(&points[position - 1U], &places_slots[i].point)) {
            position--;
        }
        if (position >= max_points) {
            continue;
        }
        const size_t last = copied < max_points ? copied : max_points - 1U;
        memmove(&points[position + 1U],
                &points[position],
                (last - position) * sizeof(*points));
        points[position] = places_slots[i].point;
        if (copied < max_points) {
            copied++;
        }
    }
    places_unlock();
    return copied;
}

esp_err_t solar_os_places_get_status(solar_os_places_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->capacity = SOLAR_OS_PLACES_CAPACITY;
    status->path_capacity = SOLAR_OS_PLACES_PATH_CAPACITY;
    if (!places_ready()) {
        return ESP_OK;
    }
    places_lock();
    status->initialized = true;
    status->count = places_count;
    status->path_count = places_path_count;
    status->evicted = places_evicted;
    status->generation = places_generation;
    places_unlock();
    return ESP_OK;
}

esp_err_t solar_os_places_path_publish(const solar_os_places_path_publish_t *path,
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
    const esp_err_t err = solar_os_places_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_places_path_t entry = {0};
    places_copy_text(entry.source, sizeof(entry.source), path->source);
    places_copy_text(entry.key, sizeof(entry.key), path->key);
    places_copy_text(entry.label,
                  sizeof(entry.label),
                  path->label != NULL && path->label[0] != '\0' ? path->label
                                                                 : path->key);
    entry.from_id = path->from_id;
    entry.to_id = path->to_id;
    entry.updated_ms = places_now_ms();

    places_lock();
    if (!places_point_exists_locked(path->from_id) ||
        !places_point_exists_locked(path->to_id)) {
        places_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    places_path_slot_t *slot = NULL;
    for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
        if (places_paths[i].used &&
            strcmp(places_paths[i].path.source, entry.source) == 0 &&
            strcmp(places_paths[i].path.key, entry.key) == 0) {
            slot = &places_paths[i];
            entry.id = slot->path.id;
            break;
        }
    }
    if (slot == NULL) {
        for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
            if (!places_paths[i].used) {
                slot = &places_paths[i];
                break;
            }
        }
        if (slot == NULL) {
            places_unlock();
            return ESP_ERR_NO_MEM;
        }
        entry.id = places_next_id++;
        if (places_next_id == 0U) {
            places_next_id = 1U;
        }
        places_path_count++;
    }
    slot->used = true;
    slot->path = entry;
    places_generation++;
    places_unlock();

    if (id != NULL) {
        *id = entry.id;
    }
    return ESP_OK;
}

esp_err_t solar_os_places_path_remove(uint32_t id)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!places_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    places_lock();
    for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
        if (places_paths[i].used && places_paths[i].path.id == id) {
            places_paths[i].used = false;
            places_path_count--;
            places_generation++;
            err = ESP_OK;
            break;
        }
    }
    places_unlock();
    return err;
}

esp_err_t solar_os_places_path_remove_source(const char *source, size_t *removed)
{
    if (removed != NULL) {
        *removed = 0U;
    }
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!places_ready()) {
        return ESP_OK;
    }
    size_t count = 0U;
    places_lock();
    for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
        if (places_paths[i].used &&
            strcmp(places_paths[i].path.source, source) == 0) {
            places_paths[i].used = false;
            places_path_count--;
            count++;
        }
    }
    if (count > 0U) {
        places_generation++;
    }
    places_unlock();
    if (removed != NULL) {
        *removed = count;
    }
    return ESP_OK;
}

size_t solar_os_places_path_snapshot(solar_os_places_path_t *paths,
                                  size_t max_paths,
                                  size_t *total)
{
    if (total != NULL) {
        *total = 0U;
    }
    if (!places_ready()) {
        return 0U;
    }
    size_t written = 0U;
    places_lock();
    for (size_t i = 0U; i < SOLAR_OS_PLACES_PATH_CAPACITY; i++) {
        if (!places_paths[i].used) {
            continue;
        }
        if (paths != NULL && written < max_paths) {
            paths[written++] = places_paths[i].path;
        }
    }
    if (total != NULL) {
        *total = places_path_count;
    }
    places_unlock();
    return written;
}

const char *solar_os_places_kind_name(solar_os_places_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_PLACES_KIND_SELF:
        return "self";
    case SOLAR_OS_PLACES_KIND_NODE:
        return "node";
    case SOLAR_OS_PLACES_KIND_WAYPOINT:
        return "waypoint";
    default:
        return "unknown";
    }
}

#include "solar_os_map_osm.h"

#include <stdbool.h>
#include <string.h>

#include "solar_os_inflate.h"
#include "solar_os_map_geo.h"
#include "solar_os_memory.h"

#define OSM_CHUNK 512U
#define OSM_TOKEN_MAX 24U
#define OSM_KEY_MAX 12U

/*
 * Overpass answers with a list of elements, each carrying a "geometry"
 * array of {"lat":..,"lon":..} positions: a way has one, a relation has one
 * for every way it was built from. Both are read the same way, and a run
 * that ends where it began is an area while one that does not is a line.
 * That is true of a way without asking what kind it is, and it is the
 * honest reading of a relation's pieces, which are fragments of a boundary
 * rather than the boundary itself.
 *
 * Whether a run is closed is only known at its end, so that is where it is
 * reported.
 *
 * An element's tags arrive after its geometry, so what kind of thing a run
 * belongs to is not known until the element closes. The class is therefore
 * reported then, once, for every run the element produced.
 */
typedef struct {
    void (*ring_begin)(void *user);
    void (*point)(void *user, int32_t latitude_e7, int32_t longitude_e7);
    void (*ring_end)(void *user, bool open);
    void (*element_end)(void *user, solar_os_map_class_t klass);
    void *user;

    bool in_string;
    bool escaped;
    bool in_geometry;
    /* What the tags said this element was, gathered as they go past. */
    bool tag_water;
    bool tag_rail;
    bool tag_road;
    bool tag_road_minor;
    bool tag_highway;
    /* Whether the element was a relation: only its members carry a role. */
    bool tag_member;
    /*
     * Overpass reports a query that timed out or ran out of room inside a
     * perfectly good 200, as a "remark" beside the elements. Without
     * noticing it, an answer that failed is indistinguishable from a cell
     * nobody has mapped.
     */
    bool saw_remark;
    int elements_depth;
    bool ring_open;
    bool have_lat;
    bool have_lon;
    int depth;
    int geometry_depth;
    int32_t latitude;
    int32_t longitude;
    int32_t first_lat;
    int32_t first_lon;
    int32_t last_lat;
    int32_t last_lon;
    /* The position just read, held back until the next one arrives, so a
     * run that turns out to be closed can drop the repeat of its first
     * rather than emit it and take it back. */
    bool have_held;
    int32_t held_lat;
    int32_t held_lon;
    uint32_t points;
    char string[OSM_KEY_MAX];
    size_t string_len;
    char key[OSM_KEY_MAX];
    char token[OSM_TOKEN_MAX];
    size_t token_len;
} osm_scan_t;

/* Degrees to 1e7 units, with no float anywhere near it. */
static int32_t parse_e7(const char *text)
{
    bool negative = false;
    const char *p = text;
    if (*p == '-') {
        negative = true;
        p++;
    } else if (*p == '+') {
        p++;
    }
    int64_t whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p++ - '0');
        if (whole > 180) {
            /* Past the domain, so the rest of the digits cannot bring it
             * back and multiplying them out would overflow. */
            whole = 180;
            break;
        }
    }
    int64_t fraction = 0;
    int digits = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && digits < 7) {
            fraction = fraction * 10 + (*p++ - '0');
            digits++;
        }
    }
    while (digits++ < 7) {
        fraction *= 10;
    }
    const int64_t value = whole * 10000000 + fraction;
    return (int32_t)(negative ? -value : value);
}

static void flush_number(osm_scan_t *scan)
{
    if (scan->token_len == 0U) {
        return;
    }
    if (strcmp(scan->key, "lat") == 0) {
        scan->latitude = parse_e7(scan->token);
        scan->have_lat = true;
    } else if (strcmp(scan->key, "lon") == 0) {
        scan->longitude = parse_e7(scan->token);
        scan->have_lon = true;
    }
    scan->token_len = 0U;
    scan->token[0] = '\0';
}

/*
 * Only four keys matter, and the query asked for nothing else. A tag key
 * such as natural or highway never appears outside an element's tags, so
 * there is no need to know which object is being read.
 */
static void value_end(osm_scan_t *scan)
{
    if (scan->key[0] == '\0') {
        return;
    }
    if (strcmp(scan->key, "natural") == 0) {
        scan->tag_water = scan->tag_water || strcmp(scan->string, "water") == 0;
    } else if (strcmp(scan->key, "waterway") == 0) {
        scan->tag_water = true;
    } else if (strcmp(scan->key, "railway") == 0) {
        scan->tag_rail = true;
    } else if (strcmp(scan->key, "role") == 0) {
        scan->tag_member = true;
    } else if (strcmp(scan->key, "highway") == 0) {
        scan->tag_road = true;
        /*
         * Three tiers, so a regional view keeps the motorways after the
         * rest have gone and a city view keeps the arterials after the
         * tertiaries have. Anything not named here is the lowest tier,
         * including a name too long to have been kept whole, which is
         * always a link or a service road.
         */
        scan->tag_highway = strcmp(scan->string, "motorway") == 0 ||
                            strcmp(scan->string, "trunk") == 0;
        scan->tag_road_minor = !scan->tag_highway &&
                               strcmp(scan->string, "primary") != 0 &&
                               strcmp(scan->string, "secondary") != 0;
    }
    scan->key[0] = '\0';
}

/* Water first: a river bank is water before it is anything else. */
static void element_end(osm_scan_t *scan, solar_os_map_class_t fallback)
{
    solar_os_map_class_t klass = fallback;
    if (scan->tag_water) {
        klass = scan->tag_member ? SOLAR_OS_MAP_CLASS_WATER_EDGE
                                 : SOLAR_OS_MAP_CLASS_WATER;
    } else if (scan->tag_rail) {
        klass = SOLAR_OS_MAP_CLASS_RAIL;
    } else if (scan->tag_road) {
        klass = scan->tag_highway     ? SOLAR_OS_MAP_CLASS_HIGHWAY
                : scan->tag_road_minor ? SOLAR_OS_MAP_CLASS_ROAD_MINOR
                                       : SOLAR_OS_MAP_CLASS_ROAD;
    }
    scan->tag_water = false;
    scan->tag_rail = false;
    scan->tag_road = false;
    scan->tag_road_minor = false;
    scan->tag_highway = false;
    scan->tag_member = false;
    if (scan->element_end != NULL) {
        scan->element_end(scan->user, klass);
    }
}

static void position_end(osm_scan_t *scan)
{
    if (!scan->have_lat || !scan->have_lon) {
        return;
    }
    if (scan->points == 0U) {
        scan->first_lat = scan->latitude;
        scan->first_lon = scan->longitude;
        scan->ring_begin(scan->user);
    } else {
        scan->point(scan->user, scan->held_lat, scan->held_lon);
    }
    scan->held_lat = scan->latitude;
    scan->held_lon = scan->longitude;
    scan->have_held = true;
    scan->last_lat = scan->latitude;
    scan->last_lon = scan->longitude;
    scan->points++;
    scan->have_lat = false;
    scan->have_lon = false;
}

/*
 * A closed run repeats its first position last, which the format does not
 * store: the held position is the repeat, so it is simply never emitted.
 */
static void geometry_end(osm_scan_t *scan)
{
    scan->in_geometry = false;
    if (scan->points == 0U) {
        return;
    }
    const bool closed = scan->points > 3U &&
                        scan->first_lat == scan->last_lat &&
                        scan->first_lon == scan->last_lon;
    if (!closed && scan->have_held) {
        scan->point(scan->user, scan->held_lat, scan->held_lon);
    }
    scan->have_held = false;
    scan->ring_end(scan->user, !closed);
    scan->points = 0U;
}

static void scan_char(osm_scan_t *scan, char ch, solar_os_map_class_t fallback)
{
    if (scan->in_string) {
        if (scan->escaped) {
            scan->escaped = false;
        } else if (ch == '\\') {
            scan->escaped = true;
        } else if (ch == '"') {
            scan->in_string = false;
            value_end(scan);
        } else if (scan->string_len + 1U < sizeof(scan->string)) {
            scan->string[scan->string_len++] = ch;
            scan->string[scan->string_len] = '\0';
        }
        return;
    }

    switch (ch) {
    case '"':
        scan->in_string = true;
        scan->string_len = 0U;
        scan->string[0] = '\0';
        return;
    case ':':
        /* The string just read was a key. */
        strlcpy(scan->key, scan->string, sizeof(scan->key));
        if (strcmp(scan->key, "remark") == 0) {
            scan->saw_remark = true;
        }
        return;
    case '[':
        scan->depth++;
        if (strcmp(scan->key, "elements") == 0) {
            scan->elements_depth = scan->depth;
        }
        if (!scan->in_geometry && strcmp(scan->key, "geometry") == 0) {
            scan->in_geometry = true;
            scan->geometry_depth = scan->depth;
            scan->points = 0U;
            scan->have_lat = false;
            scan->have_lon = false;
            scan->have_held = false;
        }
        scan->key[0] = '\0';
        return;
    case '{':
        scan->depth++;
        scan->key[0] = '\0';
        return;
    case '}':
        flush_number(scan);
        if (scan->in_geometry && scan->depth == scan->geometry_depth + 1) {
            position_end(scan);
        }
        if (scan->elements_depth != 0 &&
            scan->depth == scan->elements_depth + 1) {
            element_end(scan, fallback);
        }
        scan->depth--;
        scan->key[0] = '\0';
        return;
    case ']':
        flush_number(scan);
        if (scan->in_geometry && scan->depth == scan->geometry_depth) {
            geometry_end(scan);
        }
        scan->depth--;
        scan->key[0] = '\0';
        return;
    case ',':
        flush_number(scan);
        return;
    default:
        break;
    }
    if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ||
        ch == 'e' || ch == 'E') {
        if (scan->token_len + 1U < sizeof(scan->token)) {
            scan->token[scan->token_len++] = ch;
            scan->token[scan->token_len] = '\0';
        }
    }
}

static esp_err_t scan_file(solar_os_inflate_t *reader,
                           osm_scan_t *scan,
                           solar_os_map_class_t fallback)
{
    char chunk[OSM_CHUNK];
    esp_err_t error = solar_os_inflate_rewind(reader);
    if (error != ESP_OK) {
        return error;
    }
    size_t read = 0U;
    while ((read = solar_os_inflate_read(reader, chunk, sizeof(chunk))) > 0U) {
        for (size_t index = 0U; index < read; index++) {
            scan_char(scan, chunk[index], fallback);
        }
    }
    return solar_os_inflate_failed(reader) ? ESP_FAIL : ESP_OK;
}

typedef struct {
    uint32_t rings;
    uint32_t points;
    uint32_t ring_points;

    uint8_t *counts;
    uint8_t *bounds;
    uint8_t *coordinates;
    int32_t lat_min;
    int32_t lat_max;
    int32_t lon_min;
    int32_t lon_max;
    uint32_t ring_index;
    uint32_t point_index;
    uint32_t ring_start;
    /* The first ring of the element being read, for filling in its class. */
    uint32_t element_ring;
    bool writing;
    /* Measured while packing, and written into the header. */
    solar_os_map_spacing_t spacing;
    int32_t last_lat;
    int32_t last_lon;
} osm_build_t;

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void build_ring_begin(void *user)
{
    osm_build_t *build = user;
    build->ring_points = 0U;
    build->ring_start = build->point_index;
}

static void build_point(void *user, int32_t latitude_e7, int32_t longitude_e7)
{
    osm_build_t *build = user;
    if (build->ring_points != 0U) {
        /* How far apart the source placed its vertices, which is what says
         * whether its outline can be believed at a given zoom. */
        solar_os_map_spacing_add(&build->spacing, build->last_lat,
                                 build->last_lon, latitude_e7, longitude_e7);
    }
    build->last_lat = latitude_e7;
    build->last_lon = longitude_e7;
    if (build->ring_points == 0U) {
        build->lat_min = latitude_e7;
        build->lat_max = latitude_e7;
        build->lon_min = longitude_e7;
        build->lon_max = longitude_e7;
    } else {
        build->lat_min = latitude_e7 < build->lat_min ? latitude_e7 : build->lat_min;
        build->lat_max = latitude_e7 > build->lat_max ? latitude_e7 : build->lat_max;
        build->lon_min = longitude_e7 < build->lon_min ? longitude_e7 : build->lon_min;
        build->lon_max = longitude_e7 > build->lon_max ? longitude_e7 : build->lon_max;
    }
    build->ring_points++;
    if (build->writing && build->point_index < build->points) {
        write_u32(&build->coordinates[build->point_index * 8U],
                  (uint32_t)latitude_e7);
        write_u32(&build->coordinates[build->point_index * 8U + 4U],
                  (uint32_t)longitude_e7);
        build->point_index++;
    }
}

static void build_ring_end(void *user, bool open)
{
    osm_build_t *build = user;
    if (build->ring_points < 2U) {
        build->point_index = build->ring_start;
        build->ring_points = 0U;
        return;
    }
    if (build->writing) {
        if (build->ring_index < build->rings) {
            uint8_t *bounds = &build->bounds[build->ring_index * 16U];
            write_u32(&bounds[0], (uint32_t)build->lat_min);
            write_u32(&bounds[4], (uint32_t)build->lat_max);
            write_u32(&bounds[8], (uint32_t)build->lon_min);
            write_u32(&bounds[12], (uint32_t)build->lon_max);
            write_u32(&build->counts[build->ring_index++ * 4U],
                      build->ring_points |
                          (open ? SOLAR_OS_MAP_RING_OPEN : 0U));
        }
    } else {
        build->rings++;
        build->points += build->ring_points;
    }
    build->ring_points = 0U;
}

/* The class arrives with the tags, after every ring the element made. */
static void build_element_end(void *user, solar_os_map_class_t klass)
{
    osm_build_t *build = user;
    if (build->writing) {
        for (uint32_t ring = build->element_ring; ring < build->ring_index;
             ring++) {
            uint8_t *count = &build->counts[ring * 4U];
            write_u32(count,
                      read_u32(count) |
                          ((uint32_t)klass << SOLAR_OS_MAP_RING_CLASS_SHIFT));
        }
    }
    build->element_ring = build->ring_index;
}

esp_err_t solar_os_map_osm_read(solar_os_inflate_t *reader,
                                solar_os_map_class_t klass,
                                uint8_t **out,
                                size_t *out_size)
{
    if (reader == NULL || out == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = ESP_OK;
    osm_build_t build = {0};
    osm_scan_t scan = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .element_end = build_element_end,
        .user = &build,
    };
    error = scan_file(reader, &scan, klass);
    if (error != ESP_OK) {
        return error;
    }
    if (build.rings == 0U || build.points == 0U) {
        /* Nothing, because the query failed, is not nothing to map. */
        return scan.saw_remark ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    }
    /*
     * Counted from the answer, so bounded before they are multiplied out:
     * size_t is thirty-two bits on the device and a long enough answer wraps
     * the size to something small, which would then be written past.
     */
    if (build.rings > SOLAR_OS_MAP_LAYER_MAX_BYTES / 20U ||
        build.points > SOLAR_OS_MAP_LAYER_MAX_BYTES / 8U ||
        SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 20U +
                (size_t)build.points * 8U > SOLAR_OS_MAP_LAYER_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t size = SOLAR_OS_MAP_LAYER_HEADER +
                        (size_t)build.rings * 20U + (size_t)build.points * 8U;
    uint8_t *data = solar_os_memory_calloc(1U, size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.osm");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(data, SOLAR_OS_MAP_LAYER_MAGIC, 4U);
    data[4] = (uint8_t)SOLAR_OS_MAP_LAYER_VERSION;
    /* Measured on the first scan, so it is known before the second. */
    const uint16_t spacing = solar_os_map_spacing_hundreds(&build.spacing);
    data[6] = (uint8_t)(spacing & 0xFFU);
    data[7] = (uint8_t)((spacing >> 8) & 0xFFU);
    write_u32(&data[8], build.rings);
    write_u32(&data[12], build.points);

    build.counts = &data[SOLAR_OS_MAP_LAYER_HEADER];
    build.bounds = &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 4U];
    build.coordinates =
        &data[SOLAR_OS_MAP_LAYER_HEADER + (size_t)build.rings * 20U];
    build.writing = true;
    build.ring_points = 0U;
    build.element_ring = 0U;

    osm_scan_t second = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .element_end = build_element_end,
        .user = &build,
    };
    error = scan_file(reader, &second, klass);
    if (error == ESP_OK &&
        (build.ring_index != build.rings || build.point_index != build.points)) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        solar_os_memory_free(data);
        return error;
    }
    *out = data;
    *out_size = size;
    return ESP_OK;
}

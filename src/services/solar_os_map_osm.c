#include "solar_os_map_osm.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
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
    void (*element_end)(void *user, solar_os_map_class_t klass, bool area);
    void *user;

    bool in_string;
    bool escaped;
    bool in_geometry;
    /* What the tags said this element was, gathered as they go past. */
    bool tag_water;
    /* Water that is an area rather than a line, so a run of its edge cut
     * by the cell's border is a piece of a shore. */
    bool tag_water_area;
    bool tag_rail;
    bool tag_road;
    bool tag_road_minor;
    bool tag_street;
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
    /*
     * After the map, the answer says which water areas hold each corner of
     * the cell: a run of "area" elements closed by a "corner" marker. Only
     * whether the count is odd matters, which is what the fill counts too.
     */
    uint32_t areas;
    bool corner;
    uint8_t corners[4];
    uint8_t corner_count;
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
    if (strcmp(scan->key, "type") == 0) {
        if (strcmp(scan->string, "area") == 0) {
            scan->areas++;
        } else if (strcmp(scan->string, "corner") == 0) {
            scan->corner = true;
        }
    } else if (strcmp(scan->key, "natural") == 0) {
        const bool water = strcmp(scan->string, "water") == 0;
        scan->tag_water = scan->tag_water || water;
        scan->tag_water_area = scan->tag_water_area || water;
    } else if (strcmp(scan->key, "waterway") == 0) {
        scan->tag_water = true;
    } else if (strcmp(scan->key, "railway") == 0) {
        scan->tag_rail = true;
    } else if (strcmp(scan->key, "role") == 0) {
        scan->tag_member = true;
    } else if (strcmp(scan->key, "highway") == 0) {
        scan->tag_road = true;
        /*
         * Four tiers, so a regional view keeps the motorways after the
         * rest have gone, a city view keeps the arterials, and the streets
         * come in last. Anything not named here is a minor road,
         * including a name too long to have been kept whole, which is
         * always a link or a service road.
         */
        scan->tag_highway = strcmp(scan->string, "motorway") == 0 ||
                            strcmp(scan->string, "trunk") == 0;
        scan->tag_street = strcmp(scan->string, "residential") == 0 ||
                           strcmp(scan->string, "unclassified") == 0 ||
                           strcmp(scan->string, "living_street") == 0;
        scan->tag_road_minor = !scan->tag_highway && !scan->tag_street &&
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
        klass = scan->tag_highway      ? SOLAR_OS_MAP_CLASS_HIGHWAY
                : scan->tag_street     ? SOLAR_OS_MAP_CLASS_STREET
                : scan->tag_road_minor ? SOLAR_OS_MAP_CLASS_ROAD_MINOR
                                       : SOLAR_OS_MAP_CLASS_ROAD;
    }
    const bool area = scan->tag_water_area;
    if (scan->corner) {
        if (scan->corner_count < 4U) {
            scan->corners[scan->corner_count++] = (uint8_t)(scan->areas & 1U);
        }
        scan->areas = 0U;
        scan->corner = false;
    }
    scan->tag_water = false;
    scan->tag_water_area = false;
    scan->tag_rail = false;
    scan->tag_road = false;
    scan->tag_road_minor = false;
    scan->tag_street = false;
    scan->tag_highway = false;
    scan->tag_member = false;
    if (scan->element_end != NULL) {
        scan->element_end(scan->user, klass, area);
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
 * An answer cut to a cell writes null for the positions it left out, so a
 * way that leaves the cell and comes back is two runs, each ending at the
 * first position outside.
 */
static void geometry_break(osm_scan_t *scan)
{
    if (scan->points == 0U) {
        return;
    }
    if (scan->have_held) {
        scan->point(scan->user, scan->held_lat, scan->held_lon);
    }
    scan->have_held = false;
    scan->ring_end(scan->user, true);
    scan->points = 0U;
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
    case 'n':
        if (scan->in_geometry && scan->depth == scan->geometry_depth) {
            geometry_break(scan);
        }
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
    int32_t lat;
    int32_t lon;
} osm_point_t;

typedef struct {
    uint32_t start;
    uint32_t count;
    bool open;
} osm_run_t;

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
    bool writing;
    /* Measured while packing, and written into the header. */
    solar_os_map_spacing_t spacing;
    int32_t last_lat;
    int32_t last_lon;

    const solar_os_map_osm_cell_t *cell;
    /* The element being read: its runs wait for its tags to name them. */
    osm_point_t *element_points;
    size_t element_len;
    size_t element_cap;
    osm_run_t *runs;
    size_t runs_len;
    size_t runs_cap;
    /* Where shores meet the cell's border, as distances round it. */
    int64_t *crossings;
    size_t crossings_len;
    size_t crossings_cap;
    bool failed;
} osm_build_t;

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static bool osm_grow(osm_build_t *build, void **items, size_t *cap, size_t len,
                     size_t size)
{
    if (len < *cap) {
        return true;
    }
    const size_t grown = *cap == 0U ? 256U : *cap * 2U;
    void *bigger = solar_os_memory_realloc(*items, grown * size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.osm");
    if (bigger == NULL) {
        build->failed = true;
        return false;
    }
    *items = bigger;
    *cap = grown;
    return true;
}

/* --- writing a run into the layer, counted on the first pass ----------- */

static void emit_begin(osm_build_t *build)
{
    build->ring_points = 0U;
    build->ring_start = build->point_index;
}

static void emit_point(osm_build_t *build, int32_t latitude_e7, int32_t longitude_e7)
{
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

static void emit_end(osm_build_t *build, bool open, solar_os_map_class_t klass)
{
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
                          (open ? SOLAR_OS_MAP_RING_OPEN : 0U) |
                          ((uint32_t)klass << SOLAR_OS_MAP_RING_CLASS_SHIFT));
        }
    } else {
        build->rings++;
        build->points += build->ring_points;
    }
    build->ring_points = 0U;
}

/* --- cutting a run at the cell's border --------------------------------- */

static bool osm_inside(const solar_os_map_osm_cell_t *cell, const osm_point_t *p)
{
    return p->lat >= cell->south && p->lat <= cell->north &&
           p->lon >= cell->west && p->lon <= cell->east;
}

/* Where the segment from a point inside to one outside meets the border. */
static osm_point_t osm_cut(const solar_os_map_osm_cell_t *cell,
                           const osm_point_t *in,
                           const osm_point_t *out)
{
    double t = 1.0;
    const double dlat = (double)out->lat - (double)in->lat;
    const double dlon = (double)out->lon - (double)in->lon;
    if (out->lat > cell->north) t = fmin(t, ((double)cell->north - in->lat) / dlat);
    if (out->lat < cell->south) t = fmin(t, ((double)cell->south - in->lat) / dlat);
    if (out->lon > cell->east) t = fmin(t, ((double)cell->east - in->lon) / dlon);
    if (out->lon < cell->west) t = fmin(t, ((double)cell->west - in->lon) / dlon);
    osm_point_t cut = {
        .lat = (int32_t)llround((double)in->lat + dlat * t),
        .lon = (int32_t)llround((double)in->lon + dlon * t),
    };
    /* Snapped, so a point the arithmetic left a unit short still lies on
     * the border it was cut at. */
    if (out->lat > cell->north && cut.lat >= cell->north - 1) cut.lat = cell->north;
    if (out->lat < cell->south && cut.lat <= cell->south + 1) cut.lat = cell->south;
    if (out->lon > cell->east && cut.lon >= cell->east - 1) cut.lon = cell->east;
    if (out->lon < cell->west && cut.lon <= cell->west + 1) cut.lon = cell->west;
    cut.lat = cut.lat < cell->south ? cell->south : cut.lat > cell->north ? cell->north : cut.lat;
    cut.lon = cut.lon < cell->west ? cell->west : cut.lon > cell->east ? cell->east : cut.lon;
    return cut;
}

/* How far round the border, anticlockwise from the south-west corner. */
static int64_t osm_round_border(const solar_os_map_osm_cell_t *cell,
                                const osm_point_t *p)
{
    const int64_t wide = (int64_t)cell->east - cell->west;
    const int64_t tall = (int64_t)cell->north - cell->south;
    if (p->lat == cell->south) {
        return (int64_t)p->lon - cell->west;
    }
    if (p->lon == cell->east) {
        return wide + ((int64_t)p->lat - cell->south);
    }
    if (p->lat == cell->north) {
        return wide + tall + ((int64_t)cell->east - p->lon);
    }
    return 2 * wide + tall + ((int64_t)cell->north - p->lat);
}

static osm_point_t osm_border_point(const solar_os_map_osm_cell_t *cell, int64_t at)
{
    const int64_t wide = (int64_t)cell->east - cell->west;
    const int64_t tall = (int64_t)cell->north - cell->south;
    const int64_t perimeter = 2 * (wide + tall);
    at = ((at % perimeter) + perimeter) % perimeter;
    if (at < wide) {
        return (osm_point_t){cell->south, (int32_t)(cell->west + at)};
    }
    at -= wide;
    if (at < tall) {
        return (osm_point_t){(int32_t)(cell->south + at), cell->east};
    }
    at -= tall;
    if (at < wide) {
        return (osm_point_t){cell->north, (int32_t)(cell->east - at)};
    }
    at -= wide;
    return (osm_point_t){(int32_t)(cell->north - at), cell->west};
}

static void osm_note_crossing(osm_build_t *build, const osm_point_t *p)
{
    if (!osm_grow(build, (void **)&build->crossings, &build->crossings_cap,
                  build->crossings_len, sizeof(*build->crossings))) {
        return;
    }
    build->crossings[build->crossings_len++] = osm_round_border(build->cell, p);
}

/* --- simplifying a run -------------------------------------------------- */

/* Distance in metres from p to the line through a and b, on a local plane. */
static float osm_offset_m(const osm_point_t *a, const osm_point_t *b,
                          const osm_point_t *p, float lon_scale)
{
    const float metres = 0.0111320f;
    const float ax = 0.0f;
    const float ay = 0.0f;
    const float bx = (float)(b->lon - a->lon) * metres * lon_scale;
    const float by = (float)(b->lat - a->lat) * metres;
    const float px = (float)(p->lon - a->lon) * metres * lon_scale;
    const float py = (float)(p->lat - a->lat) * metres;
    const float dx = bx - ax;
    const float dy = by - ay;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        return sqrtf(px * px + py * py);
    }
    return fabsf(dy * px - dx * py) / length;
}

/*
 * Keeps the vertices that hold a run within tolerance of where it really
 * goes and drops the rest, ends always kept. A tier drawn no closer than a
 * few metres a pixel has no use for vertices closer than that, and they are
 * most of what a street or a shore weighs.
 */
static size_t osm_simplify(osm_build_t *build, osm_point_t *points, size_t count,
                           float tolerance_m)
{
    if (tolerance_m <= 0.0f || count < 3U) {
        return count;
    }
    uint8_t *keep = solar_os_memory_calloc(count, 1U,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.osm");
    uint32_t *stack = solar_os_memory_calloc(count * 2U, sizeof(uint32_t),
                                             SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                             "service.map.osm");
    if (keep == NULL || stack == NULL) {
        solar_os_memory_free(keep);
        solar_os_memory_free(stack);
        build->failed = true;
        return count;
    }
    const float lon_scale =
        cosf((float)points[0].lat * (3.14159265f / 180.0f / 10000000.0f));
    keep[0] = 1U;
    keep[count - 1U] = 1U;
    size_t top = 0U;
    stack[top++] = 0U;
    stack[top++] = (uint32_t)(count - 1U);
    while (top >= 2U) {
        const uint32_t last = stack[--top];
        const uint32_t first = stack[--top];
        float worst = 0.0f;
        uint32_t at = 0U;
        for (uint32_t i = first + 1U; i < last; i++) {
            const float off = osm_offset_m(&points[first], &points[last],
                                           &points[i], lon_scale);
            if (off > worst) {
                worst = off;
                at = i;
            }
        }
        if (worst > tolerance_m && at != 0U) {
            keep[at] = 1U;
            stack[top++] = first;
            stack[top++] = at;
            stack[top++] = at;
            stack[top++] = last;
        }
    }
    size_t kept = 0U;
    for (size_t i = 0U; i < count; i++) {
        if (keep[i] != 0U) {
            points[kept++] = points[i];
        }
    }
    solar_os_memory_free(keep);
    solar_os_memory_free(stack);
    return kept;
}

/* --- one run, once its element has said what it is ---------------------- */

static void osm_finish_run(osm_build_t *build,
                           osm_point_t *points,
                           size_t count,
                           bool open,
                           solar_os_map_class_t klass,
                           bool area)
{
    const solar_os_map_osm_cell_t *cell = build->cell;
    bool cut_start = false;
    bool cut_end = false;
    if (cell != NULL) {
        /* Positions past the first outside are already dropped by the
         * server; what is left outside at either end is cut back to the
         * border, so neighbouring cells meet rather than overlap. */
        size_t first = 0U;
        while (first + 1U < count && !osm_inside(cell, &points[first]) &&
               !osm_inside(cell, &points[first + 1U])) {
            first++;
        }
        size_t last = count;
        while (last > first + 1U && !osm_inside(cell, &points[last - 1U]) &&
               !osm_inside(cell, &points[last - 2U])) {
            last--;
        }
        points += first;
        count = last - first;
        if (count < 2U ||
            (!osm_inside(cell, &points[0]) && !osm_inside(cell, &points[1]))) {
            return;
        }
        if (!osm_inside(cell, &points[0])) {
            points[0] = osm_cut(cell, &points[1], &points[0]);
            cut_start = true;
        }
        if (!osm_inside(cell, &points[count - 1U])) {
            points[count - 1U] = osm_cut(cell, &points[count - 2U],
                                         &points[count - 1U]);
            cut_end = true;
        }
        if (cut_start || cut_end) {
            open = true;
        }
    }
    /* A lake cut by the border is a piece of its shore, filled with the
     * other pieces and closed along the border, not an area of its own. */
    if (area && open && klass == SOLAR_OS_MAP_CLASS_WATER &&
        (cut_start || cut_end)) {
        klass = SOLAR_OS_MAP_CLASS_WATER_EDGE;
    }
    if (cell != NULL && klass < SOLAR_OS_MAP_CLASS_COUNT) {
        count = osm_simplify(build, points, count,
                             (float)cell->tolerance_m[klass]);
    }
    emit_begin(build);
    for (size_t i = 0U; i < count; i++) {
        emit_point(build, points[i].lat, points[i].lon);
    }
    emit_end(build, open, klass);
    if (!build->writing && klass == SOLAR_OS_MAP_CLASS_WATER_EDGE) {
        if (cut_start) {
            osm_note_crossing(build, &points[0]);
        }
        if (cut_end) {
            osm_note_crossing(build, &points[count - 1U]);
        }
    }
}

/* --- the scanner's callbacks: gather an element, then finish it ---------- */

static void build_ring_begin(void *user)
{
    osm_build_t *build = user;
    if (!osm_grow(build, (void **)&build->runs, &build->runs_cap,
                  build->runs_len, sizeof(*build->runs))) {
        return;
    }
    build->runs[build->runs_len] = (osm_run_t){
        .start = (uint32_t)build->element_len,
        .count = 0U,
        .open = true,
    };
}

static void build_point(void *user, int32_t latitude_e7, int32_t longitude_e7)
{
    osm_build_t *build = user;
    if (build->failed ||
        !osm_grow(build, (void **)&build->element_points, &build->element_cap,
                  build->element_len, sizeof(*build->element_points))) {
        return;
    }
    build->element_points[build->element_len++] =
        (osm_point_t){latitude_e7, longitude_e7};
    build->runs[build->runs_len].count++;
}

static void build_ring_end(void *user, bool open)
{
    osm_build_t *build = user;
    if (build->failed) {
        return;
    }
    build->runs[build->runs_len].open = open;
    build->runs_len++;
}

/* The class arrives with the tags, after every run the element made. */
static void build_element_end(void *user, solar_os_map_class_t klass, bool area)
{
    osm_build_t *build = user;
    for (size_t run = 0U; run < build->runs_len && !build->failed; run++) {
        const osm_run_t *r = &build->runs[run];
        osm_finish_run(build, &build->element_points[r->start], r->count,
                       r->open, klass, area);
    }
    build->element_len = 0U;
    build->runs_len = 0U;
}

static int osm_compare_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a;
    const int64_t y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/*
 * The border of the cell, wherever it lies in water, joins the shore pieces
 * the cell was cut into back into closed shapes, so a lake whose far side is
 * in another cell still fills on this one. Whether the border starts in
 * water is what the server said about the south-west corner; walking round
 * from there, it changes at every shore that meets it. The other corners
 * check the walk, and a walk they disagree with is left out rather than
 * flooding the cell.
 */
static void osm_emit_seams(osm_build_t *build, const uint8_t corners[4],
                           const int64_t *crossings, size_t count)
{
    const solar_os_map_osm_cell_t *cell = build->cell;
    const int64_t wide = (int64_t)cell->east - cell->west;
    const int64_t tall = (int64_t)cell->north - cell->south;
    const int64_t corner_at[4] = {0, wide, wide + tall, 2 * wide + tall};
    const int64_t perimeter = 2 * (wide + tall);

    /* The check first, so nothing is written for a walk that is wrong. */
    bool wet = corners[0] != 0U;
    size_t next = 0U;
    for (int corner = 1; corner < 4; corner++) {
        while (next < count && crossings[next] < corner_at[corner]) {
            wet = !wet;
            next++;
        }
        if (wet != (corners[corner] != 0U)) {
            return;
        }
    }
    if ((count & 1U) != 0U) {
        return;
    }

    wet = corners[0] != 0U;
    int64_t from = 0;
    for (size_t i = 0U; i <= count; i++) {
        const int64_t to = i < count ? crossings[i] : perimeter;
        if (wet && to > from) {
            emit_begin(build);
            osm_point_t p = osm_border_point(cell, from);
            emit_point(build, p.lat, p.lon);
            for (int corner = 1; corner < 4; corner++) {
                if (corner_at[corner] > from && corner_at[corner] < to) {
                    p = osm_border_point(cell, corner_at[corner]);
                    emit_point(build, p.lat, p.lon);
                }
            }
            p = osm_border_point(cell, to);
            emit_point(build, p.lat, p.lon);
            emit_end(build, true, SOLAR_OS_MAP_CLASS_WATER_SEAM);
        }
        wet = !wet;
        from = to;
    }
}

static void osm_build_free(osm_build_t *build)
{
    solar_os_memory_free(build->element_points);
    solar_os_memory_free(build->runs);
    solar_os_memory_free(build->crossings);
    build->element_points = NULL;
    build->runs = NULL;
    build->crossings = NULL;
}

esp_err_t solar_os_map_osm_read_cell(solar_os_inflate_t *reader,
                                     solar_os_map_class_t klass,
                                     const solar_os_map_osm_cell_t *cell,
                                     uint8_t **out,
                                     size_t *out_size)
{
    if (reader == NULL || out == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = ESP_OK;
    osm_build_t build = {.cell = cell};
    osm_scan_t scan = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .element_end = build_element_end,
        .user = &build,
    };
    error = scan_file(reader, &scan, klass);
    const bool seams = cell != NULL && scan.corner_count == 4U;
    if (error == ESP_OK && build.crossings_len > 1U) {
        qsort(build.crossings, build.crossings_len, sizeof(*build.crossings),
              osm_compare_i64);
    }
    if (error == ESP_OK && seams) {
        osm_emit_seams(&build, scan.corners, build.crossings, build.crossings_len);
    }
    if (error == ESP_OK && build.failed) {
        error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        osm_build_free(&build);
        return error;
    }
    if (build.rings == 0U || build.points == 0U) {
        osm_build_free(&build);
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
        osm_build_free(&build);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t size = SOLAR_OS_MAP_LAYER_HEADER +
                        (size_t)build.rings * 20U + (size_t)build.points * 8U;
    uint8_t *data = solar_os_memory_calloc(1U, size,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "service.map.osm");
    if (data == NULL) {
        osm_build_free(&build);
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
    build.element_len = 0U;
    build.runs_len = 0U;

    osm_scan_t second = {
        .ring_begin = build_ring_begin,
        .point = build_point,
        .ring_end = build_ring_end,
        .element_end = build_element_end,
        .user = &build,
    };
    error = scan_file(reader, &second, klass);
    if (error == ESP_OK && seams) {
        osm_emit_seams(&build, scan.corners, build.crossings, build.crossings_len);
    }
    if (error == ESP_OK &&
        (build.failed || build.ring_index != build.rings ||
         build.point_index != build.points)) {
        error = ESP_FAIL;
    }
    osm_build_free(&build);
    if (error != ESP_OK) {
        solar_os_memory_free(data);
        return error;
    }
    const size_t packed = solar_os_map_geometry_pack(data, size);
    if (packed == 0U) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    uint8_t *shrunk = solar_os_memory_realloc(data,
                                              packed,
                                              SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                              "service.map.osm");
    *out = shrunk != NULL ? shrunk : data;
    *out_size = packed;
    return ESP_OK;
}

esp_err_t solar_os_map_osm_read(solar_os_inflate_t *reader,
                                solar_os_map_class_t klass,
                                uint8_t **out,
                                size_t *out_size)
{
    return solar_os_map_osm_read_cell(reader, klass, NULL, out, out_size);
}

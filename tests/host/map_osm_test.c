#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "solar_os_inflate.h"
#include "solar_os_map_osm.h"
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

/*
 * Overpass shapes every element the same way: geometry first, tags after.
 * Each element here is one class the map draws, plus one the query cannot
 * explain, which must come back as whatever the caller asked to call it.
 */
static const char ANSWER[] =
    "{\"version\":0.6,\"elements\":[\n"
    "{\"type\":\"way\",\"id\":1,\"geometry\":["
    "{\"lat\":43.1,\"lon\":-79.1},{\"lat\":43.2,\"lon\":-79.1},"
    "{\"lat\":43.2,\"lon\":-79.2},{\"lat\":43.1,\"lon\":-79.1}],"
    "\"tags\":{\"natural\":\"water\",\"name\":\"A Pond\"}},\n"
    "{\"type\":\"way\",\"id\":2,\"geometry\":["
    "{\"lat\":44.1,\"lon\":-79.1},{\"lat\":44.2,\"lon\":-79.2}],"
    "\"tags\":{\"waterway\":\"river\"}},\n"
    "{\"type\":\"way\",\"id\":3,\"geometry\":["
    "{\"lat\":45.1,\"lon\":-79.1},{\"lat\":45.2,\"lon\":-79.2}],"
    "\"tags\":{\"railway\":\"rail\",\"name\":\"water street line\"}},\n"
    "{\"type\":\"way\",\"id\":4,\"geometry\":["
    "{\"lat\":46.1,\"lon\":-79.1},{\"lat\":46.2,\"lon\":-79.2}],"
    "\"tags\":{\"highway\":\"motorway\"}},\n"
    "{\"type\":\"way\",\"id\":5,\"geometry\":["
    "{\"lat\":49.1,\"lon\":-79.1},{\"lat\":49.2,\"lon\":-79.2}],"
    "\"tags\":{\"highway\":\"tertiary\",\"name\":\"A Side Road\"}},\n"
    "{\"type\":\"way\",\"id\":9,\"geometry\":["
    "{\"lat\":50.1,\"lon\":-79.1},{\"lat\":50.2,\"lon\":-79.2}],"
    "\"tags\":{\"highway\":\"secondary\"}},\n"
    "{\"type\":\"way\",\"id\":6,\"geometry\":["
    "{\"lat\":47.1,\"lon\":-79.1},{\"lat\":47.2,\"lon\":-79.2}],"
    "\"tags\":{\"name\":\"nothing we asked for\"}},\n"
    "{\"type\":\"relation\",\"id\":6,\"members\":[\n"
    "{\"type\":\"way\",\"ref\":7,\"role\":\"outer\",\"geometry\":["
    "{\"lat\":48.1,\"lon\":-79.1},{\"lat\":48.2,\"lon\":-79.2}]},\n"
    "{\"type\":\"way\",\"ref\":8,\"role\":\"outer\",\"geometry\":["
    "{\"lat\":48.2,\"lon\":-79.2},{\"lat\":48.3,\"lon\":-79.3}]}],\n"
    "\"tags\":{\"natural\":\"water\",\"type\":\"multipolygon\"}}\n"
    "]}\n";

/*
 * A query that gave up: Overpass answers 200 with a remark and no
 * elements, which must not read as a cell nobody has mapped.
 */
static const char TIMED_OUT[] =
    "{\"version\":0.6,\"generator\":\"Overpass API\",\"elements\":[\n\n],\n"
    "\"remark\": \"runtime error: Query timed out in \\\"query\\\" at line 1"
    " after 2 seconds.\"\n}\n";

/* The same emptiness with nothing to explain it really is nothing. */
static const char EMPTY[] =
    "{\"version\":0.6,\"generator\":\"Overpass API\",\"elements\":[]}\n";

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static uint32_t ring_word(const uint8_t *packed, uint32_t ring)
{
    return read_u32(&packed[SOLAR_OS_MAP_LAYER_HEADER + ring * 4U]);
}

static solar_os_map_class_t ring_class(const uint8_t *packed, uint32_t ring)
{
    return (solar_os_map_class_t)((ring_word(packed, ring) >>
                                   SOLAR_OS_MAP_RING_CLASS_SHIFT) &
                                  0x7FU);
}

/* The reader is the caller's now, so the tests open one per read. */
static esp_err_t read_osm(FILE *file,
                          solar_os_map_class_t klass,
                          uint8_t **out,
                          size_t *out_size)
{
    solar_os_inflate_t *reader = NULL;
    const esp_err_t opened = solar_os_inflate_open(file, &reader);
    if (opened != ESP_OK) {
        return opened;
    }
    const esp_err_t error = solar_os_map_osm_read(reader, klass, out,
                                                  out_size);
    solar_os_inflate_close(reader);
    return error;
}

static FILE *write_temp(const void *data, size_t size)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    assert(fwrite(data, 1U, size, file) == size);
    return file;
}

/* The same answer as a gzip member, named and commented so the reader has
 * a header with optional fields in it to step over. */
static FILE *write_gzip(const void *data, size_t size)
{
    uint8_t out[8192];
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    assert(deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED,
                        16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    gz_header header;
    memset(&header, 0, sizeof(header));
    header.name = (Bytef *)"answer.json";
    header.comment = (Bytef *)"from overpass";
    assert(deflateSetHeader(&stream, &header) == Z_OK);
    stream.next_in = (Bytef *)data;
    stream.avail_in = (uInt)size;
    stream.next_out = out;
    stream.avail_out = (uInt)sizeof(out);
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    const size_t packed = sizeof(out) - stream.avail_out;
    assert(deflateEnd(&stream) == Z_OK);
    assert(out[0] == 0x1fU && out[1] == 0x8bU);
    return write_temp(out, packed);
}

static void check_classes(const uint8_t *packed, uint32_t rings)
{
    /* Nine runs: seven ways, then the relation's two members. */
    assert(rings == 9U);
    assert(ring_class(packed, 0U) == SOLAR_OS_MAP_CLASS_WATER);
    assert(ring_class(packed, 1U) == SOLAR_OS_MAP_CLASS_WATER);
    assert(ring_class(packed, 2U) == SOLAR_OS_MAP_CLASS_RAIL);
    /* The three tiers of road: a motorway, a tertiary, a secondary. */
    assert(ring_class(packed, 3U) == SOLAR_OS_MAP_CLASS_HIGHWAY);
    assert(ring_class(packed, 4U) == SOLAR_OS_MAP_CLASS_ROAD_MINOR);
    assert(ring_class(packed, 5U) == SOLAR_OS_MAP_CLASS_ROAD);
    /* Nothing the query explains, so it keeps the name the caller gave. */
    assert(ring_class(packed, 6U) == SOLAR_OS_MAP_CLASS_BUILDING);
    /* Both members of the relation are pieces of the water's edge, which
     * is what a relation's ways are: not water, but where it stops. */
    assert(ring_class(packed, 7U) == SOLAR_OS_MAP_CLASS_WATER_EDGE);
    assert(ring_class(packed, 8U) == SOLAR_OS_MAP_CLASS_WATER_EDGE);

    /* The first run closes, so it is an area and does not repeat its start. */
    assert((ring_word(packed, 0U) & SOLAR_OS_MAP_RING_OPEN) == 0U);
    assert((ring_word(packed, 0U) & SOLAR_OS_MAP_RING_COUNT_MASK) == 3U);
    for (uint32_t ring = 1U; ring < rings; ring++) {
        assert((ring_word(packed, ring) & SOLAR_OS_MAP_RING_OPEN) != 0U);
        assert((ring_word(packed, ring) & SOLAR_OS_MAP_RING_COUNT_MASK) == 2U);
    }
}

static void test_tags_decide_the_class(void)
{
    FILE *file = write_temp(ANSWER, sizeof(ANSWER) - 1U);
    uint8_t *packed = NULL;
    size_t size = 0U;
    assert(read_osm(file, SOLAR_OS_MAP_CLASS_BUILDING, &packed,
                                 &size) == ESP_OK);
    check_classes(packed, read_u32(&packed[8]));
    free(packed);
    fclose(file);
}

/* A deflated answer must read exactly as the same answer in the clear. */
static void test_deflated_reads_the_same(void)
{
    FILE *plain = write_temp(ANSWER, sizeof(ANSWER) - 1U);
    FILE *deflated = write_gzip(ANSWER, sizeof(ANSWER) - 1U);
    uint8_t *from_plain = NULL;
    uint8_t *from_deflated = NULL;
    size_t plain_size = 0U;
    size_t deflated_size = 0U;
    assert(read_osm(plain, SOLAR_OS_MAP_CLASS_BUILDING,
                                 &from_plain, &plain_size) == ESP_OK);
    assert(read_osm(deflated, SOLAR_OS_MAP_CLASS_BUILDING,
                                 &from_deflated, &deflated_size) == ESP_OK);
    assert(plain_size == deflated_size);
    assert(memcmp(from_plain, from_deflated, plain_size) == 0);
    check_classes(from_deflated, read_u32(&from_deflated[8]));
    free(from_plain);
    free(from_deflated);
    fclose(plain);
    fclose(deflated);
}

/* The reader hands back every byte, and hands back the same bytes twice. */
static void test_reader_round_trip(void)
{
    const size_t size = sizeof(ANSWER) - 1U;
    FILE *deflated = write_gzip(ANSWER, size);
    solar_os_inflate_t *reader = NULL;
    assert(solar_os_inflate_open(deflated, &reader) == ESP_OK);
    for (int pass = 0; pass < 2; pass++) {
        assert(solar_os_inflate_rewind(reader) == ESP_OK);
        char out[64];
        size_t total = 0U;
        size_t got = 0U;
        /* Read in pieces that do not divide the whole, to land mid-run. */
        while ((got = solar_os_inflate_read(reader, out, sizeof(out))) > 0U) {
            assert(total + got <= size);
            assert(memcmp(out, &ANSWER[total], got) == 0);
            total += got;
        }
        assert(total == size);
        assert(!solar_os_inflate_failed(reader));
    }
    solar_os_inflate_close(reader);
    fclose(deflated);
}

/* A file that is not deflated is passed through untouched. */
static void test_plain_passes_through(void)
{
    const size_t size = sizeof(ANSWER) - 1U;
    FILE *plain = write_temp(ANSWER, size);
    solar_os_inflate_t *reader = NULL;
    assert(solar_os_inflate_open(plain, &reader) == ESP_OK);
    char out[4096];
    const size_t got = solar_os_inflate_read(reader, out, sizeof(out));
    assert(got == size);
    assert(memcmp(out, ANSWER, size) == 0);
    assert(!solar_os_inflate_failed(reader));
    solar_os_inflate_close(reader);
    fclose(plain);
}

/* A stream that stops early is a failure, not a short answer. */
static void test_truncated_deflate_fails(void)
{
    uint8_t out[8192];
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    assert(deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED,
                        16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    stream.next_in = (Bytef *)ANSWER;
    stream.avail_in = (uInt)(sizeof(ANSWER) - 1U);
    stream.next_out = out;
    stream.avail_out = (uInt)sizeof(out);
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    size_t packed = sizeof(out) - stream.avail_out;
    assert(deflateEnd(&stream) == Z_OK);

    FILE *file = write_temp(out, packed / 2U);
    solar_os_inflate_t *reader = NULL;
    assert(solar_os_inflate_open(file, &reader) == ESP_OK);
    char chunk[512];
    while (solar_os_inflate_read(reader, chunk, sizeof(chunk)) > 0U) {
        /* Read out whatever arrived before the stream ran out. */
    }
    assert(solar_os_inflate_failed(reader));
    solar_os_inflate_close(reader);
    fclose(file);
}

static void test_a_failed_query_is_not_an_empty_one(void)
{
    uint8_t *packed = NULL;
    size_t size = 0U;

    FILE *timed_out = write_temp(TIMED_OUT, sizeof(TIMED_OUT) - 1U);
    assert(read_osm(timed_out, SOLAR_OS_MAP_CLASS_ROAD, &packed,
                                 &size) == ESP_ERR_TIMEOUT);
    assert(packed == NULL);
    fclose(timed_out);

    /* And the same answer deflated, which is how it actually arrives. */
    FILE *deflated = write_gzip(TIMED_OUT, sizeof(TIMED_OUT) - 1U);
    assert(read_osm(deflated, SOLAR_OS_MAP_CLASS_ROAD, &packed,
                                 &size) == ESP_ERR_TIMEOUT);
    fclose(deflated);

    FILE *empty = write_temp(EMPTY, sizeof(EMPTY) - 1U);
    assert(read_osm(empty, SOLAR_OS_MAP_CLASS_ROAD, &packed,
                                 &size) == ESP_ERR_NOT_FOUND);
    fclose(empty);
}

int main(void)
{
    test_tags_decide_the_class();
    test_a_failed_query_is_not_an_empty_one();
    test_deflated_reads_the_same();
    test_reader_round_trip();
    test_plain_passes_through();
    test_truncated_deflate_fails();
    printf("map_osm_test passed\n");
    return 0;
}

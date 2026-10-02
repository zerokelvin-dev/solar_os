#include "solar_os_inflate.h"

#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "solar_os_memory.h"

#define INFLATE_IN 2048U

/*
 * Inflate hands back its output through a thirty-two kilobyte window that
 * doubles as the dictionary a back-reference reads from, so the window is
 * kept whole and every run is served out of it before the next one is asked
 * for. A run is written at dict_pos and never crosses the end of the window,
 * which is what makes it contiguous and so serviceable with one copy.
 */
struct solar_os_inflate {
    FILE *file;
    /* Where the deflate stream begins: past a gzip header, or nowhere. */
    long start;
    bool deflated;
    bool failed;
    bool done;
    tinfl_decompressor decompressor;
    size_t in_pos;
    size_t in_len;
    size_t out_pos;
    size_t out_len;
    size_t dict_pos;
    /* Everything produced since the last rewind, against the cap. */
    size_t produced;
    uint8_t in[INFLATE_IN];
    uint8_t dict[TINFL_LZ_DICT_SIZE];
};

/* Steps over a gzip member header and reports where its deflate begins. */
static esp_err_t gzip_start(FILE *file, long *start)
{
    uint8_t head[10];
    if (fread(head, 1U, sizeof(head), file) != sizeof(head)) {
        return ESP_FAIL;
    }
    if (head[2] != 8U) {
        /* The only compression method gzip ever actually used. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t flags = head[3];
    if ((flags & 0x04U) != 0U) {
        uint8_t extra[2];
        if (fread(extra, 1U, sizeof(extra), file) != sizeof(extra) ||
            fseek(file, (long)extra[0] | ((long)extra[1] << 8), SEEK_CUR) != 0) {
            return ESP_FAIL;
        }
    }
    /* A name and then a comment, each running to a zero byte. */
    for (unsigned bit = 0x08U; bit <= 0x10U; bit <<= 1) {
        if ((flags & bit) == 0U) {
            continue;
        }
        int ch = 0;
        do {
            ch = fgetc(file);
        } while (ch != 0 && ch != EOF);
        if (ch == EOF) {
            return ESP_FAIL;
        }
    }
    if ((flags & 0x02U) != 0U && fseek(file, 2L, SEEK_CUR) != 0) {
        return ESP_FAIL;
    }
    *start = ftell(file);
    return *start < 0L ? ESP_FAIL : ESP_OK;
}

/* Fills the window with the next run. False means there will be no more. */
static bool inflate_run(solar_os_inflate_t *reader)
{
    while (true) {
        if (reader->in_pos == reader->in_len) {
            reader->in_len =
                fread(reader->in, 1U, sizeof(reader->in), reader->file);
            reader->in_pos = 0U;
            if (reader->in_len == 0U) {
                /* Input ran out before the stream said it was finished. */
                reader->failed = true;
                return false;
            }
        }
        size_t in_bytes = reader->in_len - reader->in_pos;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - reader->dict_pos;
        const tinfl_status status =
            tinfl_decompress(&reader->decompressor,
                             &reader->in[reader->in_pos],
                             &in_bytes,
                             reader->dict,
                             &reader->dict[reader->dict_pos],
                             &out_bytes,
                             TINFL_FLAG_HAS_MORE_INPUT);
        reader->in_pos += in_bytes;
        reader->out_len = out_bytes;
        if (in_bytes == 0U && out_bytes == 0U) {
            /* Neither consumed nor produced, so going round again would
             * do the same thing for ever. */
            reader->failed = true;
            return false;
        }
        reader->produced += out_bytes;
        if (reader->produced > SOLAR_OS_INFLATE_MAX_OUT) {
            reader->failed = true;
            return false;
        }
        if (status == TINFL_STATUS_DONE) {
            reader->done = true;
            return out_bytes > 0U;
        }
        if (status < TINFL_STATUS_DONE) {
            reader->failed = true;
            return false;
        }
        if (out_bytes > 0U) {
            return true;
        }
        /* It wants more input and produced nothing, so go round again. */
    }
}

esp_err_t solar_os_inflate_open(FILE *file, solar_os_inflate_t **out_reader)
{
    if (file == NULL || out_reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rewind(file);
    uint8_t magic[2] = {0U, 0U};
    const bool deflated =
        fread(magic, 1U, sizeof(magic), file) == sizeof(magic) &&
        magic[0] == 0x1fU && magic[1] == 0x8bU;

    solar_os_inflate_t *reader =
        solar_os_memory_calloc(1U, sizeof(*reader),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "service.inflate");
    if (reader == NULL) {
        return ESP_ERR_NO_MEM;
    }
    reader->file = file;
    reader->deflated = deflated;
    esp_err_t error = ESP_OK;
    if (deflated) {
        rewind(file);
        error = gzip_start(file, &reader->start);
    }
    if (error == ESP_OK) {
        error = solar_os_inflate_rewind(reader);
    }
    if (error != ESP_OK) {
        solar_os_memory_free(reader);
        return error;
    }
    *out_reader = reader;
    return ESP_OK;
}

esp_err_t solar_os_inflate_rewind(solar_os_inflate_t *reader)
{
    if (reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(reader->file, reader->start, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    reader->failed = false;
    reader->done = false;
    reader->in_pos = 0U;
    reader->in_len = 0U;
    reader->out_pos = 0U;
    reader->out_len = 0U;
    reader->dict_pos = 0U;
    reader->produced = 0U;
    if (reader->deflated) {
        tinfl_init(&reader->decompressor);
    }
    return ESP_OK;
}

size_t solar_os_inflate_read(solar_os_inflate_t *reader,
                             void *out,
                             size_t size)
{
    if (reader == NULL || out == NULL) {
        return 0U;
    }
    if (!reader->deflated) {
        return fread(out, 1U, size, reader->file);
    }

    uint8_t *bytes = out;
    size_t served = 0U;
    while (served < size && !reader->failed) {
        if (reader->out_pos < reader->out_len) {
            size_t run = reader->out_len - reader->out_pos;
            if (run > size - served) {
                run = size - served;
            }
            memcpy(&bytes[served],
                   &reader->dict[reader->dict_pos + reader->out_pos], run);
            reader->out_pos += run;
            served += run;
            continue;
        }
        if (reader->done) {
            break;
        }
        /* The run is spent, so the window moves along to where it ended. */
        reader->dict_pos = (reader->dict_pos + reader->out_len) &
                           (TINFL_LZ_DICT_SIZE - 1U);
        reader->out_pos = 0U;
        reader->out_len = 0U;
        if (!inflate_run(reader)) {
            break;
        }
    }
    return served;
}

bool solar_os_inflate_failed(const solar_os_inflate_t *reader)
{
    return reader == NULL || reader->failed;
}

void solar_os_inflate_close(solar_os_inflate_t *reader)
{
    solar_os_memory_free(reader);
}

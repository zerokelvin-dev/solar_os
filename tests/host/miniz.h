#pragma once

/*
 * On the device miniz lives in ROM. Here its inflate is stood up on zlib,
 * which every host already has, so the reader under test is the real one
 * and only the inflate underneath it is borrowed.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#define TINFL_LZ_DICT_SIZE 32768
#define TINFL_FLAG_HAS_MORE_INPUT 2

typedef enum {
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;

typedef struct {
    z_stream stream;
    int started;
} tinfl_decompressor;

static inline void tinfl_init(tinfl_decompressor *state)
{
    if (state->started) {
        (void)inflateEnd(&state->stream);
    }
    memset(&state->stream, 0, sizeof(state->stream));
    /* A negative window means raw deflate, which is what gzip wraps. */
    (void)inflateInit2(&state->stream, -MAX_WBITS);
    state->started = 1;
}

static inline tinfl_status tinfl_decompress(tinfl_decompressor *state,
                                            const uint8_t *in,
                                            size_t *in_size,
                                            uint8_t *dict,
                                            uint8_t *dict_next,
                                            size_t *out_size,
                                            uint32_t flags)
{
    (void)dict;
    (void)flags;
    state->stream.next_in = (Bytef *)in;
    state->stream.avail_in = (uInt)*in_size;
    state->stream.next_out = dict_next;
    state->stream.avail_out = (uInt)*out_size;
    const int result = inflate(&state->stream, Z_NO_FLUSH);
    *in_size -= state->stream.avail_in;
    *out_size -= state->stream.avail_out;
    if (result == Z_STREAM_END) {
        return TINFL_STATUS_DONE;
    }
    if (result != Z_OK && result != Z_BUF_ERROR) {
        return TINFL_STATUS_FAILED;
    }
    return *out_size > 0U ? TINFL_STATUS_HAS_MORE_OUTPUT
                          : TINFL_STATUS_NEEDS_MORE_INPUT;
}

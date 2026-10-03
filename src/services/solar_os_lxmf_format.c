#include "solar_os_lxmf_format.h"

#include <string.h>

#define MSGPACK_FIXARRAY_4 0x94U
#define MSGPACK_FIXMAP_0 0x80U
#define MSGPACK_NIL 0xC0U
#define MSGPACK_BIN8 0xC4U
#define MSGPACK_BIN16 0xC5U
#define MSGPACK_FLOAT32 0xCAU
#define MSGPACK_FLOAT64 0xCBU
#define MSGPACK_UINT32 0xCEU
#define MSGPACK_UINT64 0xCFU
#define MSGPACK_STR8 0xD9U
#define MSGPACK_STR16 0xDAU
#define MSGPACK_SKIP_DEPTH_MAX 8U

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t position;
} reader_t;

static bool reader_take(reader_t *reader, size_t count, const uint8_t **out)
{
    if (reader->len - reader->position < count) {
        return false;
    }
    if (out != NULL) {
        *out = &reader->data[reader->position];
    }
    reader->position += count;
    return true;
}

static bool reader_byte(reader_t *reader, uint8_t *out)
{
    const uint8_t *bytes = NULL;
    if (!reader_take(reader, 1U, &bytes)) {
        return false;
    }
    *out = bytes[0];
    return true;
}

static uint64_t read_be(const uint8_t *bytes, size_t count)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < count; index++) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

static void write_be(uint8_t *out, uint64_t value, size_t count)
{
    for (size_t index = 0U; index < count; index++) {
        out[index] = (uint8_t)(value >> ((count - 1U - index) * 8U));
    }
}

static bool reader_count(reader_t *reader, size_t width, size_t *count)
{
    const uint8_t *bytes = NULL;
    if (!reader_take(reader, width, &bytes)) {
        return false;
    }
    *count = (size_t)read_be(bytes, width);
    return true;
}

/* Advances past one msgpack value of any type. */
static bool skip_value(reader_t *reader, unsigned depth)
{
    if (depth > MSGPACK_SKIP_DEPTH_MAX) {
        return false;
    }
    uint8_t type = 0U;
    if (!reader_byte(reader, &type)) {
        return false;
    }
    size_t count = 0U;
    if (type <= 0x7FU || type >= 0xE0U || type == 0xC0U || type == 0xC2U ||
        type == 0xC3U) {
        return true;
    }
    if ((type & 0xE0U) == 0xA0U) {
        return reader_take(reader, type & 0x1FU, NULL);
    }
    if ((type & 0xF0U) == 0x90U) {
        count = type & 0x0FU;
    } else if ((type & 0xF0U) == 0x80U) {
        count = (size_t)(type & 0x0FU) * 2U;
    } else {
        switch (type) {
        case 0xC1U:
            return false;
        case MSGPACK_BIN8:
        case MSGPACK_STR8:
            return reader_count(reader, 1U, &count) &&
                   reader_take(reader, count, NULL);
        case MSGPACK_BIN16:
        case MSGPACK_STR16:
            return reader_count(reader, 2U, &count) &&
                   reader_take(reader, count, NULL);
        case 0xC6U:  /* bin32 */
        case 0xDBU:  /* str32 */
            return reader_count(reader, 4U, &count) &&
                   reader_take(reader, count, NULL);
        case 0xC7U:  /* ext8 */
            return reader_count(reader, 1U, &count) &&
                   reader_take(reader, count + 1U, NULL);
        case 0xC8U:  /* ext16 */
            return reader_count(reader, 2U, &count) &&
                   reader_take(reader, count + 1U, NULL);
        case 0xC9U:  /* ext32 */
            return reader_count(reader, 4U, &count) &&
                   reader_take(reader, count + 1U, NULL);
        case MSGPACK_FLOAT32:
        case MSGPACK_UINT32:
        case 0xD2U:  /* int32 */
            return reader_take(reader, 4U, NULL);
        case MSGPACK_FLOAT64:
        case MSGPACK_UINT64:
        case 0xD3U:  /* int64 */
            return reader_take(reader, 8U, NULL);
        case 0xCCU:  /* uint8 */
        case 0xD0U:  /* int8 */
            return reader_take(reader, 1U, NULL);
        case 0xCDU:  /* uint16 */
        case 0xD1U:  /* int16 */
            return reader_take(reader, 2U, NULL);
        case 0xD4U:  /* fixext1 */
            return reader_take(reader, 2U, NULL);
        case 0xD5U:  /* fixext2 */
            return reader_take(reader, 3U, NULL);
        case 0xD6U:  /* fixext4 */
            return reader_take(reader, 5U, NULL);
        case 0xD7U:  /* fixext8 */
            return reader_take(reader, 9U, NULL);
        case 0xD8U:  /* fixext16 */
            return reader_take(reader, 17U, NULL);
        case 0xDCU:  /* array16 */
            if (!reader_count(reader, 2U, &count)) {
                return false;
            }
            break;
        case 0xDDU:  /* array32 */
            if (!reader_count(reader, 4U, &count)) {
                return false;
            }
            break;
        case 0xDEU:  /* map16 */
            if (!reader_count(reader, 2U, &count)) {
                return false;
            }
            count *= 2U;
            break;
        case 0xDFU:  /* map32 */
            if (!reader_count(reader, 4U, &count)) {
                return false;
            }
            count *= 2U;
            break;
        default:
            return false;
        }
    }
    for (size_t index = 0U; index < count; index++) {
        if (!skip_value(reader, depth + 1U)) {
            return false;
        }
    }
    return true;
}

/* Reads a msgpack string or binary blob of any width into text. */
static bool read_text(reader_t *reader, char *text, size_t text_len, bool *truncated)
{
    uint8_t type = 0U;
    if (!reader_byte(reader, &type)) {
        return false;
    }
    size_t length = 0U;
    if ((type & 0xE0U) == 0xA0U) {
        length = type & 0x1FU;
    } else if (type == MSGPACK_BIN8 || type == MSGPACK_STR8) {
        if (!reader_count(reader, 1U, &length)) {
            return false;
        }
    } else if (type == MSGPACK_BIN16 || type == MSGPACK_STR16) {
        if (!reader_count(reader, 2U, &length)) {
            return false;
        }
    } else if (type == MSGPACK_NIL) {
        text[0] = '\0';
        return true;
    } else {
        return false;
    }

    const uint8_t *bytes = NULL;
    if (!reader_take(reader, length, &bytes)) {
        return false;
    }
    size_t copied = length;
    if (copied > text_len - 1U) {
        copied = text_len - 1U;
        if (truncated != NULL) {
            *truncated = true;
        }
    }
    for (size_t index = 0U; index < copied; index++) {
        const uint8_t byte = bytes[index];
        /* Control characters would corrupt the terminal. */
        text[index] = (byte == '\n' || byte >= 0x20U) ? (char)byte : ' ';
    }
    text[copied] = '\0';
    return true;
}

static bool read_timestamp(reader_t *reader, double *seconds)
{
    uint8_t type = 0U;
    if (!reader_byte(reader, &type)) {
        return false;
    }
    const uint8_t *bytes = NULL;
    if (type == MSGPACK_FLOAT64) {
        if (!reader_take(reader, 8U, &bytes)) {
            return false;
        }
        const uint64_t bits = read_be(bytes, 8U);
        memcpy(seconds, &bits, sizeof(*seconds));
        return true;
    }
    if (type == MSGPACK_FLOAT32) {
        if (!reader_take(reader, 4U, &bytes)) {
            return false;
        }
        const uint32_t bits = (uint32_t)read_be(bytes, 4U);
        float value = 0.0F;
        memcpy(&value, &bits, sizeof(value));
        *seconds = (double)value;
        return true;
    }
    if (type <= 0x7FU) {
        *seconds = (double)type;
        return true;
    }
    if (type == 0xCCU || type == 0xCDU || type == MSGPACK_UINT32 ||
        type == MSGPACK_UINT64) {
        const size_t width = type == 0xCCU ? 1U : (type == 0xCDU ? 2U :
                             (type == MSGPACK_UINT32 ? 4U : 8U));
        if (!reader_take(reader, width, &bytes)) {
            return false;
        }
        *seconds = (double)read_be(bytes, width);
        return true;
    }
    return false;
}

static size_t pack_blob(uint8_t *out,
                        size_t out_len,
                        size_t position,
                        const char *text)
{
    const size_t length = text != NULL ? strlen(text) : 0U;
    const size_t header = length < 256U ? 2U : 3U;
    if (position + header + length > out_len) {
        return 0U;
    }
    if (length < 256U) {
        out[position] = MSGPACK_BIN8;
        out[position + 1U] = (uint8_t)length;
    } else {
        out[position] = MSGPACK_BIN16;
        write_be(&out[position + 1U], length, 2U);
    }
    memcpy(&out[position + header], text, length);
    return position + header + length;
}

size_t solar_os_lxmf_pack_payload(double timestamp_s,
                                  const char *title,
                                  const char *content,
                                  uint8_t *out,
                                  size_t out_len)
{
    if (out == NULL || out_len < 11U) {
        return 0U;
    }
    out[0] = MSGPACK_FIXARRAY_4;
    out[1] = MSGPACK_FLOAT64;
    uint64_t bits = 0U;
    memcpy(&bits, &timestamp_s, sizeof(bits));
    write_be(&out[2], bits, 8U);

    size_t position = pack_blob(out, out_len, 10U, title);
    if (position == 0U) {
        return 0U;
    }
    position = pack_blob(out, out_len, position, content);
    if (position == 0U || position + 1U > out_len) {
        return 0U;
    }
    out[position] = MSGPACK_FIXMAP_0;
    return position + 1U;
}

bool solar_os_lxmf_unpack_payload(const uint8_t *data,
                                  size_t len,
                                  solar_os_lxmf_payload_t *payload)
{
    if (data == NULL || payload == NULL || len == 0U) {
        return false;
    }
    memset(payload, 0, sizeof(*payload));
    reader_t reader = {.data = data, .len = len, .position = 0U};
    uint8_t type = 0U;
    if (!reader_byte(&reader, &type)) {
        return false;
    }
    /* Timestamp, title, content and fields, then an optional stamp. */
    if ((type & 0xF0U) != 0x90U) {
        return false;
    }
    const size_t elements = type & 0x0FU;
    if (elements < 4U) {
        return false;
    }
    if (!read_timestamp(&reader, &payload->timestamp_s)) {
        return false;
    }
    if (!read_text(&reader, payload->title, sizeof(payload->title),
                   &payload->truncated)) {
        return false;
    }
    if (!read_text(&reader, payload->content, sizeof(payload->content),
                   &payload->truncated)) {
        return false;
    }
    const size_t fields_start = reader.position;
    if (!skip_value(&reader, 0U)) {
        return false;
    }
    payload->fields = &data[fields_start];
    payload->fields_len = reader.position - fields_start;
    payload->signed_len = reader.position;
    payload->stamped = elements > 4U;
    return true;
}

size_t solar_os_lxmf_signed_payload(const uint8_t *data,
                                    const solar_os_lxmf_payload_t *payload,
                                    uint8_t *out,
                                    size_t out_len)
{
    if (data == NULL || payload == NULL || out == NULL ||
        payload->signed_len == 0U || payload->signed_len > out_len) {
        return 0U;
    }
    memcpy(out, data, payload->signed_len);
    out[0] = MSGPACK_FIXARRAY_4;
    return payload->signed_len;
}

size_t solar_os_lxmf_pack_announce(const char *display_name,
                                   uint8_t *out,
                                   size_t out_len)
{
    if (out == NULL || out_len < 2U) {
        return 0U;
    }
    /* [display name, stamp cost]; a nil cost demands no stamp. */
    out[0] = 0x92U;
    const size_t position = pack_blob(out, out_len, 1U, display_name);
    if (position == 0U || position + 1U > out_len) {
        return 0U;
    }
    out[position] = MSGPACK_NIL;
    return position + 1U;
}

bool solar_os_lxmf_parse_announce(const uint8_t *data,
                                  size_t len,
                                  char *name,
                                  size_t name_len)
{
    if (data == NULL || name == NULL || name_len < 2U || len == 0U) {
        return false;
    }
    name[0] = '\0';
    if ((data[0] & 0xF0U) == 0x90U && (data[0] & 0x0FU) >= 1U) {
        reader_t reader = {.data = data, .len = len, .position = 1U};
        if (read_text(&reader, name, name_len, NULL)) {
            return true;
        }
        name[0] = '\0';
        return false;
    }
    /* Before LXMF 0.5.0 the announce carried the bare display name. */
    size_t copied = len;
    if (copied > name_len - 1U) {
        copied = name_len - 1U;
    }
    for (size_t index = 0U; index < copied; index++) {
        const uint8_t byte = data[index];
        if (byte < 0x20U) {
            return false;
        }
        name[index] = (char)byte;
    }
    name[copied] = '\0';
    return copied > 0U;
}

#include "solar_os_reticulum_lora_frame.h"

#include <string.h>

size_t solar_os_rnode_frame_pack(const uint8_t *packet,
                                 size_t len,
                                 uint8_t sequence,
                                 solar_os_rnode_frame_t frames[2])
{
    if (packet == NULL || frames == NULL || len == 0U ||
        len > SOLAR_OS_RNODE_MTU) {
        return 0U;
    }
    uint8_t header = (uint8_t)((sequence & 0x0FU) << 4);
    if (len > SOLAR_OS_RNODE_FRAME_PAYLOAD) {
        header |= SOLAR_OS_RNODE_FLAG_SPLIT;
    }
    size_t count = 0U;
    size_t offset = 0U;
    while (offset < len) {
        size_t take = len - offset;
        if (take > SOLAR_OS_RNODE_FRAME_PAYLOAD) {
            take = SOLAR_OS_RNODE_FRAME_PAYLOAD;
        }
        frames[count].data[0] = header;
        memcpy(&frames[count].data[1], packet + offset, take);
        frames[count].len = take + 1U;
        offset += take;
        count++;
    }
    return count;
}

void solar_os_rnode_assembler_init(solar_os_rnode_assembler_t *assembler)
{
    if (assembler != NULL) {
        assembler->len = 0U;
        assembler->sequence = SOLAR_OS_RNODE_SEQ_UNSET;
    }
}

static size_t assembler_take(solar_os_rnode_assembler_t *assembler,
                             const uint8_t *payload,
                             size_t len)
{
    if (assembler->len + len > sizeof(assembler->buffer)) {
        assembler->len = 0U;
        assembler->sequence = SOLAR_OS_RNODE_SEQ_UNSET;
        return 0U;
    }
    memcpy(&assembler->buffer[assembler->len], payload, len);
    assembler->len += len;
    return len;
}

size_t solar_os_rnode_assembler_feed(solar_os_rnode_assembler_t *assembler,
                                     const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t *out,
                                     size_t out_capacity)
{
    if (assembler == NULL || frame == NULL || out == NULL || frame_len < 2U ||
        frame_len > SOLAR_OS_RNODE_FRAME_MAX) {
        return 0U;
    }
    const uint8_t header = frame[0];
    const uint8_t sequence = (uint8_t)(header >> 4);
    const uint8_t *payload = frame + 1U;
    const size_t payload_len = frame_len - 1U;

    if ((header & SOLAR_OS_RNODE_FLAG_SPLIT) == 0U) {
        /* Whole in one frame. A half still held belonged to a packet whose
         * other half is not coming. */
        assembler->len = 0U;
        assembler->sequence = SOLAR_OS_RNODE_SEQ_UNSET;
        if (payload_len > out_capacity) {
            return 0U;
        }
        memcpy(out, payload, payload_len);
        return payload_len;
    }

    if (assembler->sequence == sequence) {
        /* The second half of the packet we hold the first of. */
        if (assembler_take(assembler, payload, payload_len) == 0U) {
            return 0U;
        }
        const size_t len = assembler->len;
        assembler->len = 0U;
        assembler->sequence = SOLAR_OS_RNODE_SEQ_UNSET;
        if (len > out_capacity) {
            return 0U;
        }
        memcpy(out, assembler->buffer, len);
        return len;
    }

    /* A first half: of a new packet, or of one that displaces a half whose
     * partner never came. */
    assembler->len = 0U;
    assembler->sequence = sequence;
    (void)assembler_take(assembler, payload, payload_len);
    return 0U;
}

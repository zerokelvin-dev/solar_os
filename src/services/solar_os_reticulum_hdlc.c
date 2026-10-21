#include "solar_os_reticulum_hdlc.h"

size_t solar_os_reticulum_hdlc_encode(const uint8_t *payload,
                                      size_t payload_len,
                                      uint8_t *out,
                                      size_t out_capacity)
{
    if ((payload == NULL && payload_len != 0U) || out == NULL ||
        out_capacity < 2U) {
        return 0U;
    }
    size_t used = 0U;
    out[used++] = SOLAR_OS_RETICULUM_HDLC_FLAG;
    for (size_t index = 0U; index < payload_len; index++) {
        const uint8_t byte = payload[index];
        if (byte == SOLAR_OS_RETICULUM_HDLC_FLAG ||
            byte == SOLAR_OS_RETICULUM_HDLC_ESC) {
            if (used + 3U > out_capacity) {
                return 0U;
            }
            out[used++] = SOLAR_OS_RETICULUM_HDLC_ESC;
            out[used++] = (uint8_t)(byte ^ SOLAR_OS_RETICULUM_HDLC_ESC_MASK);
        } else {
            if (used + 2U > out_capacity) {
                return 0U;
            }
            out[used++] = byte;
        }
    }
    out[used++] = SOLAR_OS_RETICULUM_HDLC_FLAG;
    return used;
}

void solar_os_reticulum_hdlc_decoder_init(
    solar_os_reticulum_hdlc_decoder_t *decoder,
    uint8_t *buffer,
    size_t capacity)
{
    decoder->buffer = buffer;
    decoder->capacity = capacity;
    decoder->length = 0U;
    decoder->in_frame = false;
    decoder->escaped = false;
    decoder->overflow = false;
}

void solar_os_reticulum_hdlc_decoder_feed(
    solar_os_reticulum_hdlc_decoder_t *decoder,
    const uint8_t *data,
    size_t length,
    solar_os_reticulum_hdlc_frame_fn frame_fn,
    void *context)
{
    for (size_t index = 0U; index < length; index++) {
        uint8_t byte = data[index];
        if (byte == SOLAR_OS_RETICULUM_HDLC_FLAG) {
            if (decoder->in_frame && !decoder->overflow &&
                decoder->length >= SOLAR_OS_RETICULUM_HDLC_MIN_FRAME) {
                frame_fn(decoder->buffer, decoder->length, context);
            }
            decoder->in_frame = true;
            decoder->escaped = false;
            decoder->overflow = false;
            decoder->length = 0U;
            continue;
        }
        if (!decoder->in_frame) {
            continue;
        }
        if (decoder->escaped) {
            byte = (uint8_t)(byte ^ SOLAR_OS_RETICULUM_HDLC_ESC_MASK);
            decoder->escaped = false;
        } else if (byte == SOLAR_OS_RETICULUM_HDLC_ESC) {
            decoder->escaped = true;
            continue;
        }
        if (decoder->length < decoder->capacity) {
            decoder->buffer[decoder->length++] = byte;
        } else {
            decoder->overflow = true;
        }
    }
}

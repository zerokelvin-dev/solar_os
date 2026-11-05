#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_RETICULUM_HDLC_FLAG 0x7EU
#define SOLAR_OS_RETICULUM_HDLC_ESC 0x7DU
#define SOLAR_OS_RETICULUM_HDLC_ESC_MASK 0x20U
#define SOLAR_OS_RETICULUM_HDLC_MIN_FRAME 19U

/* Worst case: every byte escaped, plus the two delimiting flags. */
#define SOLAR_OS_RETICULUM_HDLC_ENCODED_MAX(payload) (2U * (payload) + 2U)

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool in_frame;
    bool escaped;
    bool overflow;
} solar_os_reticulum_hdlc_decoder_t;

typedef void (*solar_os_reticulum_hdlc_frame_fn)(const uint8_t *frame,
                                                 size_t length,
                                                 void *context);

/* Returns the encoded length, or 0 when capacity is too small. */
size_t solar_os_reticulum_hdlc_encode(const uint8_t *payload,
                                      size_t payload_len,
                                      uint8_t *out,
                                      size_t out_capacity);

void solar_os_reticulum_hdlc_decoder_init(
    solar_os_reticulum_hdlc_decoder_t *decoder,
    uint8_t *buffer,
    size_t capacity);

/* Calls frame_fn for every complete frame of at least MIN_FRAME bytes.
 * Frames that overflow the buffer are dropped whole. */
void solar_os_reticulum_hdlc_decoder_feed(
    solar_os_reticulum_hdlc_decoder_t *decoder,
    const uint8_t *data,
    size_t length,
    solar_os_reticulum_hdlc_frame_fn frame_fn,
    void *context);

#ifdef __cplusplus
}
#endif

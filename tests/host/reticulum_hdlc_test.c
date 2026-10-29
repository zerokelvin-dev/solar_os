#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_reticulum_hdlc.h"

typedef struct {
    uint8_t frame[2][600];
    size_t length[2];
    size_t count;
} capture_t;

static void capture(const uint8_t *frame, size_t length, void *context)
{
    capture_t *state = (capture_t *)context;
    if (state->count < 2U) {
        memcpy(state->frame[state->count], frame, length);
        state->length[state->count] = length;
    }
    state->count++;
}

int main(void)
{
    uint8_t payload[40];
    for (size_t index = 0U; index < sizeof(payload); index++) {
        payload[index] = (uint8_t)index;
    }
    payload[5] = SOLAR_OS_RETICULUM_HDLC_FLAG;
    payload[6] = SOLAR_OS_RETICULUM_HDLC_ESC;
    payload[7] = SOLAR_OS_RETICULUM_HDLC_ESC;

    uint8_t wire[SOLAR_OS_RETICULUM_HDLC_ENCODED_MAX(sizeof(payload))];
    const size_t wire_len = solar_os_reticulum_hdlc_encode(
        payload, sizeof(payload), wire, sizeof(wire));
    assert(wire_len == sizeof(payload) + 2U + 3U);
    assert(wire[0] == SOLAR_OS_RETICULUM_HDLC_FLAG);
    assert(wire[wire_len - 1U] == SOLAR_OS_RETICULUM_HDLC_FLAG);
    assert(wire[6] == 0x7DU && wire[7] == 0x5EU);
    assert(wire[8] == 0x7DU && wire[9] == 0x5DU);
    assert(solar_os_reticulum_hdlc_encode(
               payload, sizeof(payload), wire, wire_len - 1U) == 0U);
    assert(solar_os_reticulum_hdlc_encode(NULL, 1U, wire, sizeof(wire)) == 0U);

    uint8_t buffer[64];
    solar_os_reticulum_hdlc_decoder_t decoder;
    capture_t state;

    /* One byte at a time, two frames back to back. */
    memset(&state, 0, sizeof(state));
    solar_os_reticulum_hdlc_decoder_init(&decoder, buffer, sizeof(buffer));
    for (int pass = 0; pass < 2; pass++) {
        for (size_t index = 0U; index < wire_len; index++) {
            solar_os_reticulum_hdlc_decoder_feed(
                &decoder, &wire[index], 1U, capture, &state);
        }
    }
    assert(state.count == 2U);
    assert(state.length[0] == sizeof(payload));
    assert(memcmp(state.frame[0], payload, sizeof(payload)) == 0);
    assert(memcmp(state.frame[1], payload, sizeof(payload)) == 0);

    /* Noise before the first flag and undersized frames are ignored. */
    memset(&state, 0, sizeof(state));
    solar_os_reticulum_hdlc_decoder_init(&decoder, buffer, sizeof(buffer));
    const uint8_t noise[] = {1U, 2U, 3U, 0x7EU, 4U, 5U, 0x7EU};
    solar_os_reticulum_hdlc_decoder_feed(
        &decoder, noise, sizeof(noise), capture, &state);
    assert(state.count == 0U);
    solar_os_reticulum_hdlc_decoder_feed(
        &decoder, wire, wire_len, capture, &state);
    assert(state.count == 1U);

    /* Oversized frames are dropped whole and the decoder recovers. */
    memset(&state, 0, sizeof(state));
    uint8_t small[20];
    solar_os_reticulum_hdlc_decoder_init(&decoder, small, sizeof(small));
    solar_os_reticulum_hdlc_decoder_feed(
        &decoder, wire, wire_len, capture, &state);
    assert(state.count == 0U);
    uint8_t short_payload[20];
    memset(short_payload, 0x42, sizeof(short_payload));
    uint8_t short_wire[SOLAR_OS_RETICULUM_HDLC_ENCODED_MAX(20U)];
    const size_t short_len = solar_os_reticulum_hdlc_encode(
        short_payload, sizeof(short_payload), short_wire, sizeof(short_wire));
    solar_os_reticulum_hdlc_decoder_feed(
        &decoder, short_wire, short_len, capture, &state);
    assert(state.count == 1U);
    assert(state.length[0] == 20U);

    puts("reticulum_hdlc_test ok");
    return 0;
}

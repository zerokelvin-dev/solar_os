#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_reticulum_lora_frame.h"

static void fill(uint8_t *data, size_t len, uint8_t seed)
{
    for (size_t i = 0U; i < len; i++) {
        data[i] = (uint8_t)(seed + i * 7U);
    }
}

/* A packet that fits one frame goes as a header and the bytes, unsplit. */
static void test_a_short_packet_is_one_frame(void)
{
    uint8_t packet[100];
    fill(packet, sizeof(packet), 1U);
    solar_os_rnode_frame_t frames[2];
    assert(solar_os_rnode_frame_pack(packet, sizeof(packet), 0x0A, frames) == 1U);
    assert(frames[0].len == 101U);
    assert(frames[0].data[0] == 0xA0U);
    assert(memcmp(&frames[0].data[1], packet, sizeof(packet)) == 0);

    solar_os_rnode_assembler_t assembler;
    solar_os_rnode_assembler_init(&assembler);
    uint8_t out[SOLAR_OS_RNODE_MTU];
    assert(solar_os_rnode_assembler_feed(&assembler, frames[0].data,
                                         frames[0].len, out, sizeof(out)) == 100U);
    assert(memcmp(out, packet, 100U) == 0);
}

/* A long packet is two frames with the same header and the split flag:
 * 254 bytes, then the rest. The assembler puts them back together. */
static void test_a_long_packet_splits_and_reassembles(void)
{
    uint8_t packet[300];
    fill(packet, sizeof(packet), 9U);
    solar_os_rnode_frame_t frames[2];
    assert(solar_os_rnode_frame_pack(packet, sizeof(packet), 0x03, frames) == 2U);
    assert(frames[0].data[0] == 0x31U && frames[1].data[0] == 0x31U);
    assert(frames[0].len == 255U && frames[1].len == 47U);

    solar_os_rnode_assembler_t assembler;
    solar_os_rnode_assembler_init(&assembler);
    uint8_t out[SOLAR_OS_RNODE_MTU];
    assert(solar_os_rnode_assembler_feed(&assembler, frames[0].data,
                                         frames[0].len, out, sizeof(out)) == 0U);
    assert(solar_os_rnode_assembler_feed(&assembler, frames[1].data,
                                         frames[1].len, out, sizeof(out)) == 300U);
    assert(memcmp(out, packet, 300U) == 0);

    /* The largest packet the air carries: two full halves. */
    uint8_t big[SOLAR_OS_RNODE_MTU];
    fill(big, sizeof(big), 5U);
    assert(solar_os_rnode_frame_pack(big, sizeof(big), 0x0F, frames) == 2U);
    assert(frames[0].len == 255U && frames[1].len == 255U);
    assert(solar_os_rnode_assembler_feed(&assembler, frames[0].data,
                                         frames[0].len, out, sizeof(out)) == 0U);
    assert(solar_os_rnode_assembler_feed(&assembler, frames[1].data,
                                         frames[1].len, out, sizeof(out)) ==
           SOLAR_OS_RNODE_MTU);
    assert(memcmp(out, big, sizeof(big)) == 0);
}

/*
 * Halves pair by sequence. A lost second half is forgotten when the next
 * packet arrives, whether that is a new first half (a different sequence)
 * or an unsplit packet; and a second half with nothing held is a stray.
 */
static void test_a_lost_half_never_pairs_with_the_wrong_packet(void)
{
    uint8_t first[300];
    uint8_t second[300];
    uint8_t single[40];
    fill(first, sizeof(first), 11U);
    fill(second, sizeof(second), 23U);
    fill(single, sizeof(single), 31U);
    solar_os_rnode_frame_t a[2];
    solar_os_rnode_frame_t b[2];
    solar_os_rnode_frame_t c[2];
    assert(solar_os_rnode_frame_pack(first, sizeof(first), 0x01, a) == 2U);
    assert(solar_os_rnode_frame_pack(second, sizeof(second), 0x02, b) == 2U);
    assert(solar_os_rnode_frame_pack(single, sizeof(single), 0x01, c) == 1U);

    solar_os_rnode_assembler_t assembler;
    solar_os_rnode_assembler_init(&assembler);
    uint8_t out[SOLAR_OS_RNODE_MTU];

    /* First half of A, then B arrives whole: B is delivered, A's half gone. */
    assert(solar_os_rnode_assembler_feed(&assembler, a[0].data, a[0].len, out,
                                         sizeof(out)) == 0U);
    assert(solar_os_rnode_assembler_feed(&assembler, b[0].data, b[0].len, out,
                                         sizeof(out)) == 0U);
    assert(solar_os_rnode_assembler_feed(&assembler, b[1].data, b[1].len, out,
                                         sizeof(out)) == 300U);
    assert(memcmp(out, second, 300U) == 0);

    /* A's second half now, on its own: a stray, delivered as nothing. */
    assert(solar_os_rnode_assembler_feed(&assembler, a[1].data, a[1].len, out,
                                         sizeof(out)) == 0U);
    /* ...and it did not leave a half behind that a later A could pair with
     * the wrong way round: feeding a[1] again starts a new half, then a[0]
     * completes it, but as second-then-first - which RNode would do too.
     * What matters is the unsplit packet clearing the slate: */
    assert(solar_os_rnode_assembler_feed(&assembler, c[0].data, c[0].len, out,
                                         sizeof(out)) == 40U);
    assert(memcmp(out, single, 40U) == 0);
    assert(assembler.sequence == SOLAR_OS_RNODE_SEQ_UNSET && assembler.len == 0U);

    /* A first half held, then an unsplit packet with the same sequence
     * nibble: it is whole, so it is delivered and the half discarded. */
    assert(solar_os_rnode_assembler_feed(&assembler, a[0].data, a[0].len, out,
                                         sizeof(out)) == 0U);
    assert(solar_os_rnode_assembler_feed(&assembler, c[0].data, c[0].len, out,
                                         sizeof(out)) == 40U);
    assert(solar_os_rnode_assembler_feed(&assembler, a[1].data, a[1].len, out,
                                         sizeof(out)) == 0U);
}

/* Nothing, and more than the air can carry, are refused rather than framed;
 * a frame of just a header carries nothing. */
static void test_the_edges_are_refused(void)
{
    uint8_t packet[SOLAR_OS_RNODE_MTU + 1U];
    fill(packet, sizeof(packet), 2U);
    solar_os_rnode_frame_t frames[2];
    assert(solar_os_rnode_frame_pack(packet, 0U, 0U, frames) == 0U);
    assert(solar_os_rnode_frame_pack(packet, sizeof(packet), 0U, frames) == 0U);

    solar_os_rnode_assembler_t assembler;
    solar_os_rnode_assembler_init(&assembler);
    uint8_t out[SOLAR_OS_RNODE_MTU];
    const uint8_t lone_header[1] = {0x00};
    assert(solar_os_rnode_assembler_feed(&assembler, lone_header, 1U, out,
                                         sizeof(out)) == 0U);
    /* A buffer too small for the packet gets nothing, not a truncation. */
    uint8_t small[10];
    assert(solar_os_rnode_frame_pack(packet, 100U, 0U, frames) == 1U);
    assert(solar_os_rnode_assembler_feed(&assembler, frames[0].data,
                                         frames[0].len, small, sizeof(small)) == 0U);
}

int main(void)
{
    test_a_short_packet_is_one_frame();
    test_a_long_packet_splits_and_reassembles();
    test_a_lost_half_never_pairs_with_the_wrong_packet();
    test_the_edges_are_refused();
    printf("reticulum_lora_frame_test ok\n");
    return 0;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * How an RNode puts a Reticulum packet on the air, so an RNode hears ours
 * and we hear its. Every LoRa frame starts with one header byte: a random
 * sequence number in the high nibble and, in bit 0, whether the packet was
 * too long for one frame. A packet of up to 254 bytes is one frame; a
 * longer one is two frames carrying the same header, 254 bytes in the first
 * and the rest in the second. The receiver pairs halves by sequence number
 * and a frame that is not split discards any half it was holding. There is
 * no timeout: a lost second half is forgotten when the next packet comes.
 *
 * Established from RNode_Firmware (Config.h: MTU 508, SINGLE_MTU 255,
 * HEADER_L 1; Framing.h: FLAG_SPLIT 0x01, SEQ_UNSET 0xFF; transmit() and
 * receive_callback() in RNode_Firmware.ino). See doc/design/reticulum-lora.md.
 */
#define SOLAR_OS_RNODE_MTU 508U
#define SOLAR_OS_RNODE_FRAME_MAX 255U
#define SOLAR_OS_RNODE_FRAME_PAYLOAD (SOLAR_OS_RNODE_FRAME_MAX - 1U)
#define SOLAR_OS_RNODE_FLAG_SPLIT 0x01U
#define SOLAR_OS_RNODE_SEQ_UNSET 0xFFU

typedef struct {
    uint8_t data[SOLAR_OS_RNODE_FRAME_MAX];
    size_t len;
} solar_os_rnode_frame_t;

/* Frames one packet. The sequence is any nibble, chosen fresh per packet so
 * two consecutive split packets cannot be paired across each other. Returns
 * how many frames were written: one, two, or zero for an empty or oversize
 * packet. */
size_t solar_os_rnode_frame_pack(const uint8_t *packet,
                                 size_t len,
                                 uint8_t sequence,
                                 solar_os_rnode_frame_t frames[2]);

typedef struct {
    uint8_t buffer[SOLAR_OS_RNODE_MTU];
    size_t len;
    uint8_t sequence;
} solar_os_rnode_assembler_t;

void solar_os_rnode_assembler_init(solar_os_rnode_assembler_t *assembler);

/* Feeds one received frame. Returns the length of a complete packet copied
 * to out, or zero when the frame was a first half, a stray, or malformed. */
size_t solar_os_rnode_assembler_feed(solar_os_rnode_assembler_t *assembler,
                                     const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t *out,
                                     size_t out_capacity);

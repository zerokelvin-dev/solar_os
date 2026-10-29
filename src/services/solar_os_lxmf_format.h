#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An LXMF message is the sender's destination hash, a signature, and a
 * msgpack payload of timestamp, title, content, and a fields map. The
 * signature covers the recipient hash, the sender hash, and the payload
 * without its optional trailing stamp, followed by the hash of all three.
 */
#define SOLAR_OS_LXMF_HASH_LEN 16U
#define SOLAR_OS_LXMF_SIGNATURE_LEN 64U
#define SOLAR_OS_LXMF_TITLE_MAX 32U
#define SOLAR_OS_LXMF_NAME_MAX 32U
#define SOLAR_OS_LXMF_ANNOUNCE_MAX 40U
/* A single encrypted packet carries this much of a destination's payload. */
#define SOLAR_OS_LXMF_PACKET_MAX 383U
/* A packet over a link carries this much, with the recipient hash in front. */
#define SOLAR_OS_LXMF_LINK_PACKET_MAX 431U
#define SOLAR_OS_LXMF_PAYLOAD_MAX \
    (SOLAR_OS_LXMF_LINK_PACKET_MAX - 2U * SOLAR_OS_LXMF_HASH_LEN - \
     SOLAR_OS_LXMF_SIGNATURE_LEN)

typedef struct {
    double timestamp_s;
    char title[SOLAR_OS_LXMF_TITLE_MAX + 1U];
    /* Whether the title was longer than it is here. */
    bool truncated;
    /* The content as it was sent, borrowed from the caller's buffer and not
     * terminated: a message may be far longer than anything worth copying
     * twice. solar_os_lxmf_copy_text() makes text of it. */
    const uint8_t *content;
    size_t content_len;
    bool stamped;
    /* The fields map, borrowed from the caller's buffer. */
    const uint8_t *fields;
    size_t fields_len;
    /* Payload bytes the signature covers, ignoring any trailing stamp. */
    size_t signed_len;
} solar_os_lxmf_payload_t;

/* Returns the packed length, or zero when the buffer is too small. */
size_t solar_os_lxmf_pack_payload(double timestamp_s,
                                  const char *title,
                                  const char *content,
                                  uint8_t *out,
                                  size_t out_len);

bool solar_os_lxmf_unpack_payload(const uint8_t *data,
                                  size_t len,
                                  solar_os_lxmf_payload_t *payload);

/*
 * Copies out the payload the signature was made over. A stamped payload
 * differs only in its array header, which this rewrites.
 */
size_t solar_os_lxmf_signed_payload(const uint8_t *data,
                                    const solar_os_lxmf_payload_t *payload,
                                    uint8_t *out,
                                    size_t out_len);

/*
 * Copies bytes out as text that is safe to show, terminated, and says
 * whether they did not all fit.
 */
void solar_os_lxmf_copy_text(const uint8_t *bytes,
                             size_t len,
                             char *text,
                             size_t text_len,
                             bool *truncated);

size_t solar_os_lxmf_pack_announce(const char *display_name,
                                   uint8_t *out,
                                   size_t out_len);

/* Accepts the msgpack announce format and bare pre-0.5.0 display names. */
bool solar_os_lxmf_parse_announce(const uint8_t *data,
                                  size_t len,
                                  char *name,
                                  size_t name_len);

/* What a propagation node says of itself in its lxmf.propagation announce. */
typedef struct {
    /* It takes messages; a node can announce that it has stopped. */
    bool active;
    /* The largest upload it takes, in kilobytes. */
    uint32_t transfer_limit_kb;
    /* The stamp cost it asks for, and how far below it it still accepts. */
    uint32_t stamp_cost;
    uint32_t stamp_flexibility;
} solar_os_lxmf_propagation_node_t;

/*
 * Accepts what LXMF's pn_announce_data_is_valid() accepts: [legacy flag,
 * timebase, active, transfer limit, sync limit, [stamp cost, flexibility,
 * peering cost], metadata]. Older nodes announce less and are refused.
 */
bool solar_os_lxmf_parse_propagation_announce(
    const uint8_t *data,
    size_t len,
    solar_os_lxmf_propagation_node_t *node);

/*
 * An upload to a propagation node: [time, [message]], where the message is
 * the recipient hash, the encrypted rest, and the propagation stamp.
 */
size_t solar_os_lxmf_pack_propagation(double timestamp_s,
                                      const uint8_t *message,
                                      size_t message_len,
                                      uint8_t *out,
                                      size_t out_len);

/* What identifies a message held by a propagation node: its SHA-256. */
#define SOLAR_OS_LXMF_TRANSIENT_ID_LEN 32U
/* Errors a propagation node answers "/get" with in place of a list. */
#define SOLAR_OS_LXMF_SYNC_NO_IDENTITY 0xF0U
#define SOLAR_OS_LXMF_SYNC_NO_ACCESS 0xF1U

/*
 * A "/get" request to a propagation node: [wants, haves], and the most the
 * reply may carry when limit_kb is not zero. Each list is count transient
 * IDs end to end, and a NULL list packs nil: both nil asks what the node
 * holds, wants asks for those messages, and haves lets it delete them.
 */
size_t solar_os_lxmf_pack_sync_request(const uint8_t *wants,
                                       size_t want_count,
                                       const uint8_t *haves,
                                       size_t have_count,
                                       uint32_t limit_kb,
                                       uint8_t *out,
                                       size_t out_len);

typedef struct {
    const uint8_t *data;
    size_t len;
} solar_os_lxmf_blob_t;

/*
 * Reads a propagation node's answer to "/get": a list of transient IDs, or
 * of messages, borrowed from data. Fills up to max items and says how many
 * there were. A number is an error, given in *error and with no items. False
 * for anything else.
 */
bool solar_os_lxmf_parse_sync_response(const uint8_t *data,
                                       size_t len,
                                       solar_os_lxmf_blob_t *items,
                                       size_t max,
                                       size_t *count,
                                       uint32_t *error);

/*
 * Writes a destination hash given as hexadecimal in its canonical form:
 * 32 lowercase digits. Surrounding angle brackets, as Reticulum prints a
 * hash, are accepted. False for anything else.
 */
bool solar_os_lxmf_normalize_hash(const char *text, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

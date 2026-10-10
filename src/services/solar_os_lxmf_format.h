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

#ifdef __cplusplus
}
#endif

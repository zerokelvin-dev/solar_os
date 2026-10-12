#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_lxmf_format.h"

/* msgpack.packb([1758000000.5, b"hi", b"there", {}]) from the Python
 * reference implementation, which is what an LXMF peer sends. */
static const uint8_t reference[] = {
    0x94, 0xCB, 0x41, 0xDA, 0x32, 0x3C, 0xE0, 0x20, 0x00, 0x00,
    0xC4, 0x02, 'h',  'i',
    0xC4, 0x05, 't',  'h',  'e',  'r',  'e',
    0x80,
};

/* The content is borrowed, unterminated bytes. */
static bool content_is(const solar_os_lxmf_payload_t *payload, const char *text)
{
    return payload->content_len == strlen(text) &&
           memcmp(payload->content, text, payload->content_len) == 0;
}

static void test_pack(void)
{
    uint8_t packed[SOLAR_OS_LXMF_PAYLOAD_MAX];
    const size_t length =
        solar_os_lxmf_pack_payload(1758000000.5, "hi", "there", packed, sizeof(packed));
    assert(length == sizeof(reference));
    assert(memcmp(packed, reference, length) == 0);

    /* An empty title and content still produce a well-formed payload. */
    const size_t empty =
        solar_os_lxmf_pack_payload(0.0, "", "", packed, sizeof(packed));
    assert(empty == 15U);

    /* A buffer that cannot hold the content is refused rather than truncated. */
    char content[200];
    memset(content, 'x', sizeof(content) - 1U);
    content[sizeof(content) - 1U] = '\0';
    assert(solar_os_lxmf_pack_payload(1.0, "t", content, packed, 64U) == 0U);
    assert(solar_os_lxmf_pack_payload(1.0, "t", content, packed, sizeof(packed)) > 200U);
}

static void test_unpack(void)
{
    solar_os_lxmf_payload_t payload;
    assert(solar_os_lxmf_unpack_payload(reference, sizeof(reference), &payload));
    assert(payload.timestamp_s > 1757999999.0 && payload.timestamp_s < 1758000001.0);
    assert(strcmp(payload.title, "hi") == 0);
    assert(content_is(&payload, "there"));
    assert(!payload.truncated);
    assert(payload.fields_len == 1U && payload.fields[0] == 0x80U);
    assert(payload.signed_len == sizeof(reference));
    assert(!payload.stamped);

    /* Round trip. */
    uint8_t packed[SOLAR_OS_LXMF_PAYLOAD_MAX];
    const size_t length =
        solar_os_lxmf_pack_payload(1.5, "title", "body", packed, sizeof(packed));
    assert(solar_os_lxmf_unpack_payload(packed, length, &payload));
    assert(payload.timestamp_s == 1.5);
    assert(strcmp(payload.title, "title") == 0);
    assert(content_is(&payload, "body"));

    /* Peers that send str rather than bin, and an integer timestamp. */
    const uint8_t as_str[] = {0x94, 0x0A, 0xA2, 'h', 'i', 0xA3, 'y', 'o', 'u', 0x80};
    assert(solar_os_lxmf_unpack_payload(as_str, sizeof(as_str), &payload));
    assert(payload.timestamp_s == 10.0);
    assert(strcmp(payload.title, "hi") == 0);
    assert(content_is(&payload, "you"));

    /* A nil title is an empty title. */
    const uint8_t nil_title[] = {0x94, 0x00, 0xC0, 0xA1, 'x', 0x80};
    assert(solar_os_lxmf_unpack_payload(nil_title, sizeof(nil_title), &payload));
    assert(payload.title[0] == '\0');
    assert(content_is(&payload, "x"));

    /* Control characters are replaced so they cannot corrupt a terminal. */
    const uint8_t control[] = {0x94, 0x00, 0xA1, 'x', 0xA3, 'a', 0x07, 'b', 0x80};
    assert(solar_os_lxmf_unpack_payload(control, sizeof(control), &payload));
    char shown[8];
    solar_os_lxmf_copy_text(payload.content, payload.content_len, shown, sizeof(shown), NULL);
    assert(strcmp(shown, "a b") == 0);

    /* Truncated and malformed payloads are rejected, never read past. */
    for (size_t length_limit = 0U; length_limit < sizeof(reference); length_limit++) {
        assert(!solar_os_lxmf_unpack_payload(reference, length_limit, &payload));
    }
    const uint8_t not_array[] = {0xC4, 0x01, 'x'};
    assert(!solar_os_lxmf_unpack_payload(not_array, sizeof(not_array), &payload));
    const uint8_t short_array[] = {0x92, 0x00, 0xA1, 'x'};
    assert(!solar_os_lxmf_unpack_payload(short_array, sizeof(short_array), &payload));

    /* Content of any length is found where it lies rather than copied, and
     * copied out it is cut to what it is given, never past it. */
    static uint8_t long_content[70000U];
    memset(long_content, 'y', sizeof(long_content));
    long_content[0] = 0x94;
    long_content[1] = 0x00;
    long_content[2] = 0xA0;
    long_content[3] = 0xC5;
    long_content[4] = 0x13;
    long_content[5] = 0x88; /* 5000 bytes */
    long_content[6U + 5000U] = 0x80;
    assert(solar_os_lxmf_unpack_payload(long_content, 6U + 5000U + 1U, &payload));
    assert(payload.content_len == 5000U && payload.content == &long_content[6]);
    assert(!payload.truncated);
    assert(payload.signed_len == 6U + 5000U + 1U);
    char text[65];
    bool cut = false;
    solar_os_lxmf_copy_text(payload.content, payload.content_len, text, sizeof(text), &cut);
    assert(strlen(text) == 64U && cut);
    cut = false;
    solar_os_lxmf_copy_text((const uint8_t *)"a\tb", 3U, text, sizeof(text), &cut);
    assert(strcmp(text, "a b") == 0 && !cut);

    /* A 32-bit length, which a message past 64 KiB of content needs. */
    long_content[3] = 0xC6;
    long_content[4] = 0x00;
    long_content[5] = 0x01;
    long_content[6] = 0x00;
    long_content[7] = 0x10; /* 65552 bytes */
    long_content[8U + 65552U] = 0x80;
    assert(solar_os_lxmf_unpack_payload(long_content, 8U + 65552U + 1U, &payload));
    assert(payload.content_len == 65552U);
    /* One that claims more than there is fails. */
    assert(!solar_os_lxmf_unpack_payload(long_content, 8U + 100U, &payload));
}

/*
 * A stamped message carries a fifth element the signature does not cover,
 * and a fields map the payload must be read past to find it.
 */
static void test_stamp(void)
{
    /* msgpack.packb([0, b"t", b"c", {3: b"\xaa\xbb"}, b"\x01\x02"]) */
    const uint8_t stamped[] = {
        0x95, 0x00, 0xC4, 0x01, 't', 0xC4, 0x01, 'c',
        0x81, 0x03, 0xC4, 0x02, 0xAA, 0xBB,
        0xC4, 0x02, 0x01, 0x02,
    };
    /* msgpack.packb of the same payload without its stamp. */
    const uint8_t pre_stamp[] = {
        0x94, 0x00, 0xC4, 0x01, 't', 0xC4, 0x01, 'c',
        0x81, 0x03, 0xC4, 0x02, 0xAA, 0xBB,
    };
    solar_os_lxmf_payload_t payload;
    assert(solar_os_lxmf_unpack_payload(stamped, sizeof(stamped), &payload));
    assert(payload.stamped);
    assert(strcmp(payload.title, "t") == 0);
    assert(content_is(&payload, "c"));
    assert(payload.fields_len == 6U);
    assert(payload.signed_len == sizeof(pre_stamp));

    uint8_t signed_bytes[SOLAR_OS_LXMF_PAYLOAD_MAX];
    const size_t length = solar_os_lxmf_signed_payload(
        stamped, &payload, signed_bytes, sizeof(signed_bytes));
    assert(length == sizeof(pre_stamp));
    assert(memcmp(signed_bytes, pre_stamp, length) == 0);
    assert(solar_os_lxmf_signed_payload(stamped, &payload, signed_bytes, 4U) == 0U);

    /* Nested containers in the fields map are skipped, not misread. */
    const uint8_t nested[] = {
        0x94, 0x00, 0xA0, 0xA0,
        0x81, 0x01, 0x92, 0x93, 0x01, 0x02, 0x03, 0x81, 0xA1, 'k', 0xCB,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    assert(solar_os_lxmf_unpack_payload(nested, sizeof(nested), &payload));
    assert(payload.signed_len == sizeof(nested));
    assert(payload.fields_len == sizeof(nested) - 4U);

    /* A fields map that runs off the end is rejected. */
    assert(!solar_os_lxmf_unpack_payload(nested, sizeof(nested) - 1U, &payload));
}

static void test_announce(void)
{
    uint8_t packed[SOLAR_OS_LXMF_ANNOUNCE_MAX];
    const size_t length =
        solar_os_lxmf_pack_announce("solaros", packed, sizeof(packed));
    /* msgpack.packb([b"solaros", None, []]): the empty third element is
     * what tells an LXMF sender not to compress a resource for us. */
    const uint8_t expected[] = {0x93, 0xC4, 0x07, 's', 'o', 'l',
                                'a',  'r',  'o',  's', 0xC0, 0x90};
    assert(length == sizeof(expected));
    assert(memcmp(packed, expected, length) == 0);

    char name[SOLAR_OS_LXMF_NAME_MAX + 1U];
    assert(solar_os_lxmf_parse_announce(packed, length, name, sizeof(name)));
    assert(strcmp(name, "solaros") == 0);

    /* A stamp cost in the second slot does not change the name. */
    const uint8_t with_cost[] = {0x92, 0xA4, 'n', 'o', 'd', 'e', 0x08};
    assert(solar_os_lxmf_parse_announce(with_cost, sizeof(with_cost), name, sizeof(name)));
    assert(strcmp(name, "node") == 0);

    /* The pre-0.5.0 format is a bare display name. */
    const uint8_t bare[] = {'o', 'l', 'd'};
    assert(solar_os_lxmf_parse_announce(bare, sizeof(bare), name, sizeof(name)));
    assert(strcmp(name, "old") == 0);

    assert(!solar_os_lxmf_parse_announce(NULL, 3U, name, sizeof(name)));
    assert(!solar_os_lxmf_parse_announce(bare, 0U, name, sizeof(name)));
    const uint8_t binary[] = {0x01, 0x02};
    assert(!solar_os_lxmf_parse_announce(binary, sizeof(binary), name, sizeof(name)));
}

int main(void)
{
    test_pack();
    test_unpack();
    test_stamp();
    test_announce();
    printf("lxmf_format_test ok\n");
    return 0;
}

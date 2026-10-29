/* LXMF stamps against vectors from the Python reference (LXMF.LXStamper). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>

#include "solar_os_lxmf_stamp.h"

/* Host SHA-256 for solar_os_crypto; the firmware uses mbedTLS. */
void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *sha) { memset(sha, 0, sizeof(*sha)); }
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *sha)
{ EVP_MD_CTX_free(sha->ctx.pointer); memset(sha, 0, sizeof(*sha)); }
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *sha)
{
    if (sha->ctx.pointer == NULL) sha->ctx.pointer = EVP_MD_CTX_new();
    sha->started = sha->ctx.pointer && EVP_DigestInit_ex(sha->ctx.pointer, EVP_sha256(), NULL);
    return sha->started ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t solar_os_crypto_sha256_clone(solar_os_crypto_sha256_t *dst, const solar_os_crypto_sha256_t *src)
{
    if (dst->ctx.pointer == NULL) dst->ctx.pointer = EVP_MD_CTX_new();
    dst->started = dst->ctx.pointer && EVP_MD_CTX_copy_ex(dst->ctx.pointer, src->ctx.pointer);
    return dst->started ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *sha, const void *data, size_t size)
{ return size == 0 || EVP_DigestUpdate(sha->ctx.pointer, data, size) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *sha, uint8_t digest[32])
{ sha->started = false; return EVP_DigestFinal_ex(sha->ctx.pointer, digest, NULL) ? ESP_OK : ESP_FAIL; }

static void from_hex(const char *hex, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned byte;
        assert(sscanf(hex + 2 * i, "%2x", &byte) == 1);
        out[i] = (uint8_t)byte;
    }
}

static uint32_t value_of(const solar_os_lxmf_stamp_t *stamp, const char *hex)
{
    uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE];
    uint32_t value = 0;
    from_hex(hex, candidate, sizeof(candidate));
    assert(solar_os_lxmf_stamp_value(stamp, candidate, &value) == ESP_OK);
    return value;
}

static void begin(solar_os_lxmf_stamp_t *stamp, uint32_t rounds)
{
    uint8_t material[32];
    from_hex("1d8689fda2d314b5b0cc34273039db8fdf9343e5911febf31d443d0323460745",
             material, sizeof(material));
    assert(solar_os_lxmf_stamp_begin(stamp, material, sizeof(material), rounds) == ESP_OK);
}

static void check_workblock(const solar_os_lxmf_stamp_t *stamp, const char *expected)
{
    uint8_t digest[32];
    uint8_t want[32];
    solar_os_crypto_sha256_t copy;
    solar_os_crypto_sha256_init(&copy);
    assert(solar_os_crypto_sha256_clone(&copy, &stamp->workblock) == ESP_OK);
    assert(solar_os_crypto_sha256_finish(&copy, digest) == ESP_OK);
    solar_os_crypto_sha256_free(&copy);
    from_hex(expected, want, sizeof(want));
    assert(memcmp(digest, want, sizeof(want)) == 0);
}

static void check_search(const solar_os_lxmf_stamp_t *stamp, uint32_t cost,
                         const char *expected, uint32_t expected_attempts)
{
    uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE] = {0};
    uint8_t want[SOLAR_OS_LXMF_STAMP_SIZE];
    uint32_t attempts = 0;

    /* In slices, as the firmware runs it: each slice resumes the last. */
    uint32_t total = 0;
    esp_err_t err;
    do {
        err = solar_os_lxmf_stamp_search(stamp, cost, candidate, 100, &attempts);
        total += attempts;
    } while (err == ESP_ERR_NOT_FOUND);
    assert(err == ESP_OK);
    from_hex(expected, want, sizeof(want));
    assert(memcmp(candidate, want, sizeof(want)) == 0);
    assert(total == expected_attempts);
}

static void test_peering_rounds(void)
{
    solar_os_lxmf_stamp_t stamp;
    begin(&stamp, SOLAR_OS_LXMF_STAMP_ROUNDS_PEERING);
    check_workblock(&stamp, "6787c7e30b9655c472189aa34fcdbe5469bbc2997dbd751e75951cc450685120");
    assert(value_of(&stamp, "0000000000000000000000000000000000000000000000000000000000000000") == 2);
    assert(value_of(&stamp, "0101010101010101010101010101010101010101010101010101010101010101") == 6);
    assert(value_of(&stamp, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff") == 1);
    assert(value_of(&stamp, "5f9ff88745895837e23fef18a37f9ccdcdc2a808b2f01c5f8503c8756fd381be") == 11);
    check_search(&stamp, 10, "00000000000000000000000000000000000000000000000000000000000000d5", 214);
    solar_os_lxmf_stamp_end(&stamp);
}

static void test_propagation_rounds(void)
{
    solar_os_lxmf_stamp_t stamp;
    begin(&stamp, SOLAR_OS_LXMF_STAMP_ROUNDS_PROPAGATION);
    check_workblock(&stamp, "04c3f3ba5ddc2c04c4a0f15b2bec617d6380ffcee271f15dfd6de9dcacd6e49b");
    assert(value_of(&stamp, "0000000000000000000000000000000000000000000000000000000000000000") == 1);
    assert(value_of(&stamp, "0101010101010101010101010101010101010101010101010101010101010101") == 0);
    assert(value_of(&stamp, "086a67a73c7dbe0f6b46e08b1e849dc1c876e40d629befd073b4392f0283d5d5") == 12);
    check_search(&stamp, 12, "00000000000000000000000000000000000000000000000000000000000001e3", 484);
    solar_os_lxmf_stamp_end(&stamp);
}

static void test_arguments(void)
{
    solar_os_lxmf_stamp_t stamp;
    uint8_t material[1] = {0};
    assert(solar_os_lxmf_stamp_begin(&stamp, material, 0, 1) == ESP_ERR_INVALID_ARG);
    assert(solar_os_lxmf_stamp_begin(&stamp, material, 1, 0) == ESP_ERR_INVALID_ARG);
    begin(&stamp, 1);
    uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE] = {0};
    assert(solar_os_lxmf_stamp_search(&stamp, 257, candidate, 1, NULL) == ESP_ERR_INVALID_ARG);
    /* Cost 0 is met by any candidate, on the first attempt. */
    uint32_t attempts = 0;
    assert(solar_os_lxmf_stamp_search(&stamp, 0, candidate, 1, &attempts) == ESP_OK);
    assert(attempts == 1);
    solar_os_lxmf_stamp_end(&stamp);
}

/* Not an assertion: what a propagation stamp at the hub's cost takes here. */
static void report_propagation_cost(uint32_t cost)
{
    solar_os_lxmf_stamp_t stamp;
    const clock_t start = clock();
    begin(&stamp, SOLAR_OS_LXMF_STAMP_ROUNDS_PROPAGATION);
    const clock_t workblock = clock();
    uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE] = {0x5a};
    uint32_t attempts = 0;
    assert(solar_os_lxmf_stamp_search(&stamp, cost, candidate, UINT32_MAX, &attempts) == ESP_OK);
    const clock_t done = clock();
    printf("lxmf_stamp_test: cost %u propagation stamp, workblock %.0f ms, %u attempts in %.0f ms\n",
           (unsigned)cost,
           (double)(workblock - start) * 1000.0 / CLOCKS_PER_SEC,
           (unsigned)attempts,
           (double)(done - workblock) * 1000.0 / CLOCKS_PER_SEC);
    solar_os_lxmf_stamp_end(&stamp);
}

int main(void)
{
    test_peering_rounds();
    test_propagation_rounds();
    test_arguments();
    report_propagation_cost(16);
    printf("lxmf_stamp_test: ok\n");
    return 0;
}

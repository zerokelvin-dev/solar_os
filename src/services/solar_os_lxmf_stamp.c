#include "solar_os_lxmf_stamp.h"

#include <string.h>

#define HASH_LEN SOLAR_OS_CRYPTO_SHA256_LEN
#define HASH_BLOCK 64U
#define ROUND_LEN 256U

static esp_err_t sha256_parts(const uint8_t *a, size_t a_len,
                              const uint8_t *b, size_t b_len,
                              uint8_t digest[HASH_LEN])
{
    solar_os_crypto_sha256_t sha;
    solar_os_crypto_sha256_init(&sha);
    esp_err_t err = solar_os_crypto_sha256_start(&sha);
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, a, a_len);
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, b, b_len);
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&sha, digest);
    solar_os_crypto_sha256_free(&sha);
    return err;
}

/* HMAC-SHA256 with a key no longer than a block, as every key here is. */
static esp_err_t hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *a, size_t a_len,
                             const uint8_t *b, size_t b_len,
                             uint8_t mac[HASH_LEN])
{
    uint8_t pad[HASH_BLOCK];
    uint8_t inner[HASH_LEN];
    memset(pad, 0x36, sizeof(pad));
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];

    solar_os_crypto_sha256_t sha;
    solar_os_crypto_sha256_init(&sha);
    esp_err_t err = solar_os_crypto_sha256_start(&sha);
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, pad, sizeof(pad));
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, a, a_len);
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, b, b_len);
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&sha, inner);
    solar_os_crypto_sha256_free(&sha);
    if (err != ESP_OK) return err;

    memset(pad, 0x5c, sizeof(pad));
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];
    return sha256_parts(pad, sizeof(pad), inner, sizeof(inner), mac);
}

/* The workblock round index, as msgpack packs an unsigned integer. */
static size_t pack_round(uint32_t n, uint8_t out[5])
{
    if (n < 0x80U) {
        out[0] = (uint8_t)n;
        return 1;
    }
    if (n <= 0xFFU) {
        out[0] = 0xCCU;
        out[1] = (uint8_t)n;
        return 2;
    }
    if (n <= 0xFFFFU) {
        out[0] = 0xCDU;
        out[1] = (uint8_t)(n >> 8);
        out[2] = (uint8_t)n;
        return 3;
    }
    out[0] = 0xCEU;
    out[1] = (uint8_t)(n >> 24);
    out[2] = (uint8_t)(n >> 16);
    out[3] = (uint8_t)(n >> 8);
    out[4] = (uint8_t)n;
    return 5;
}

/*
 * One round of the workblock: HKDF-SHA256 of the material, salted with
 * SHA-256(material || packed round index), expanded to 256 bytes with no
 * context.
 */
static esp_err_t workblock_round(const uint8_t *material, size_t material_len,
                                 uint32_t n, uint8_t out[ROUND_LEN])
{
    uint8_t index[5];
    uint8_t salt[HASH_LEN];
    uint8_t prk[HASH_LEN];
    esp_err_t err = sha256_parts(material, material_len,
                                 index, pack_round(n, index), salt);
    if (err == ESP_OK) {
        err = hmac_sha256(salt, sizeof(salt), material, material_len,
                          NULL, 0, prk);
    }
    for (uint32_t i = 0; err == ESP_OK && i < ROUND_LEN / HASH_LEN; i++) {
        const uint8_t counter = (uint8_t)(i + 1U);
        const uint8_t *previous = i == 0 ? NULL : out + (i - 1U) * HASH_LEN;
        err = hmac_sha256(prk, sizeof(prk), previous, i == 0 ? 0 : HASH_LEN,
                          &counter, 1, out + i * HASH_LEN);
    }
    return err;
}

esp_err_t solar_os_lxmf_stamp_begin(solar_os_lxmf_stamp_t *stamp,
                                    const uint8_t *material,
                                    size_t material_len,
                                    uint32_t rounds)
{
    if (stamp == NULL || material == NULL || material_len == 0 || rounds == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_crypto_sha256_init(&stamp->workblock);
    esp_err_t err = solar_os_crypto_sha256_start(&stamp->workblock);
    uint8_t round[ROUND_LEN];
    for (uint32_t n = 0; err == ESP_OK && n < rounds; n++) {
        err = workblock_round(material, material_len, n, round);
        if (err == ESP_OK) {
            err = solar_os_crypto_sha256_update(&stamp->workblock, round, sizeof(round));
        }
    }
    if (err != ESP_OK) solar_os_lxmf_stamp_end(stamp);
    return err;
}

static uint32_t leading_zero_bits(const uint8_t digest[HASH_LEN])
{
    uint32_t bits = 0;
    for (size_t i = 0; i < HASH_LEN; i++) {
        if (digest[i] == 0) {
            bits += 8;
            continue;
        }
        for (uint8_t mask = 0x80U; (digest[i] & mask) == 0; mask >>= 1) bits++;
        break;
    }
    return bits;
}

esp_err_t solar_os_lxmf_stamp_value(const solar_os_lxmf_stamp_t *stamp,
                                    const uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE],
                                    uint32_t *value)
{
    if (stamp == NULL || candidate == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t digest[HASH_LEN];
    solar_os_crypto_sha256_t sha;
    solar_os_crypto_sha256_init(&sha);
    esp_err_t err = solar_os_crypto_sha256_clone(&sha, &stamp->workblock);
    if (err == ESP_OK) err = solar_os_crypto_sha256_update(&sha, candidate, SOLAR_OS_LXMF_STAMP_SIZE);
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&sha, digest);
    solar_os_crypto_sha256_free(&sha);
    if (err == ESP_OK) *value = leading_zero_bits(digest);
    return err;
}

static void next_candidate(uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE])
{
    for (size_t i = SOLAR_OS_LXMF_STAMP_SIZE; i-- > 0;) {
        if (++candidate[i] != 0) break;
    }
}

esp_err_t solar_os_lxmf_stamp_search(const solar_os_lxmf_stamp_t *stamp,
                                     uint32_t cost,
                                     uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE],
                                     uint32_t max_attempts,
                                     uint32_t *attempts)
{
    if (stamp == NULL || candidate == NULL || cost > HASH_LEN * 8U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t tried = 0;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    while (tried < max_attempts) {
        uint32_t value = 0;
        tried++;
        esp_err_t hashed = solar_os_lxmf_stamp_value(stamp, candidate, &value);
        if (hashed != ESP_OK) {
            err = hashed;
            break;
        }
        if (value >= cost) {
            err = ESP_OK;
            break;
        }
        next_candidate(candidate);
    }
    if (attempts != NULL) *attempts = tried;
    return err;
}

void solar_os_lxmf_stamp_end(solar_os_lxmf_stamp_t *stamp)
{
    if (stamp != NULL) solar_os_crypto_sha256_free(&stamp->workblock);
}

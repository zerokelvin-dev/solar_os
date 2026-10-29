#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LXMF proof-of-work stamps. A stamp is 32 bytes such that
 * SHA-256(workblock || stamp) starts with at least `cost` zero bits, where the
 * workblock is `rounds` HKDF-SHA256 outputs of 256 bytes derived from the
 * stamped material: a message ID, or a propagation transient ID.
 */
#define SOLAR_OS_LXMF_STAMP_SIZE 32U
#define SOLAR_OS_LXMF_STAMP_ROUNDS 3000U
#define SOLAR_OS_LXMF_STAMP_ROUNDS_PROPAGATION 1000U
#define SOLAR_OS_LXMF_STAMP_ROUNDS_PEERING 25U

/*
 * The workblock is the same for every candidate stamp and a whole number of
 * SHA-256 blocks, so it is hashed once and each candidate costs one block.
 * Nothing of the workblock is kept but that hash state.
 */
typedef struct {
    solar_os_crypto_sha256_t workblock;
} solar_os_lxmf_stamp_t;

esp_err_t solar_os_lxmf_stamp_begin(solar_os_lxmf_stamp_t *stamp,
                                    const uint8_t *material,
                                    size_t material_len,
                                    uint32_t rounds);

/* The number of leading zero bits a stamp scores; a stamp is valid for any
 * cost up to it. */
esp_err_t solar_os_lxmf_stamp_value(const solar_os_lxmf_stamp_t *stamp,
                                    const uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE],
                                    uint32_t *value);

/*
 * Tries up to max_attempts candidates, counting up from `candidate`. On
 * ESP_OK `candidate` holds a stamp worth at least `cost`; on
 * ESP_ERR_NOT_FOUND it holds the next one to try, so a search can be run in
 * slices. Start from random bytes.
 */
esp_err_t solar_os_lxmf_stamp_search(const solar_os_lxmf_stamp_t *stamp,
                                     uint32_t cost,
                                     uint8_t candidate[SOLAR_OS_LXMF_STAMP_SIZE],
                                     uint32_t max_attempts,
                                     uint32_t *attempts);

void solar_os_lxmf_stamp_end(solar_os_lxmf_stamp_t *stamp);

#ifdef __cplusplus
}
#endif

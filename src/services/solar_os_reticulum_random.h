#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Random bytes for Reticulum's keys, IVs and tokens, from a CTR-DRBG.
 *
 * The ESP32's hardware RNG is a true random source only while Wi-Fi or
 * Bluetooth is running. When neither is, the generator is seeded with the
 * SAR ADC entropy source switched on (bootloader_random_enable()), with ADC
 * reads held off meanwhile, so keys made in LoRa-only use are not
 * pseudo-random. Every draw also mixes in fresh hardware RNG output.
 */

/* Seeds the generator again from a true random source. Reticulum calls it
 * at each start. */
esp_err_t solar_os_reticulum_random_reseed(void);

/* Fills out with len random bytes, seeding first if needed. */
esp_err_t solar_os_reticulum_random(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

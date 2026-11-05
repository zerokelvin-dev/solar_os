/* What the microReticulum crypto overlay needs from mbedTLS, for building it
 * on the host from the ESP-IDF copy of mbedTLS 3.6. */
#pragma once

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CBC

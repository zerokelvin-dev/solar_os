#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_RETICULUM_WORKER_STACK 12288U
#define SOLAR_OS_RETICULUM_HOST_MAX 63U
#define SOLAR_OS_RETICULUM_DEFAULT_PORT 4242U
#define SOLAR_OS_RETICULUM_HASH_HEX_LEN 33U
#define SOLAR_OS_RETICULUM_PRIVATE_KEY_HEX_LEN 129U
#define SOLAR_OS_RETICULUM_ANNOUNCE_LOG 8U
#define SOLAR_OS_RETICULUM_APP_DATA_MAX 48U
#define SOLAR_OS_RETICULUM_APP_NAME "solaros"
#define SOLAR_OS_RETICULUM_ASPECT "node"
#define SOLAR_OS_RETICULUM_UPSTREAM_COMMIT \
    "40fa628809d57140180c1c833559ab96fec992c1"

typedef struct {
    char destination_hex[SOLAR_OS_RETICULUM_HASH_HEX_LEN];
    uint8_t hops;
    uint32_t age_ms;
    char app_data[SOLAR_OS_RETICULUM_APP_DATA_MAX + 1U];
} solar_os_reticulum_announce_t;

typedef struct {
    bool initialized;
    bool running;
    bool identity_set;
    bool interface_online;
    char identity_hash_hex[SOLAR_OS_RETICULUM_HASH_HEX_LEN];
    char destination_hex[SOLAR_OS_RETICULUM_HASH_HEX_LEN];
    char host[SOLAR_OS_RETICULUM_HOST_MAX + 1U];
    uint16_t port;
    size_t paths;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t connects;
    uint32_t announces_received;
    uint32_t announces_sent;
    uint32_t exceptions;
    uint32_t stack_watermark_bytes;
    esp_err_t last_error;
} solar_os_reticulum_status_t;

esp_err_t solar_os_reticulum_init(void);
esp_err_t solar_os_reticulum_get_status(solar_os_reticulum_status_t *status);

esp_err_t solar_os_reticulum_identity_hash(
    char hash_hex[SOLAR_OS_RETICULUM_HASH_HEX_LEN]);
esp_err_t solar_os_reticulum_identity_generate(bool force);
esp_err_t solar_os_reticulum_identity_import(const char *private_key_hex);
esp_err_t solar_os_reticulum_identity_export_private(
    char private_key_hex[SOLAR_OS_RETICULUM_PRIVATE_KEY_HEX_LEN]);

esp_err_t solar_os_reticulum_start(const char *host, uint16_t port);
esp_err_t solar_os_reticulum_stop(void);
void solar_os_reticulum_loop_once(void);
void solar_os_reticulum_note_stack_watermark(uint32_t bytes);

esp_err_t solar_os_reticulum_announce(void);
size_t solar_os_reticulum_announce_snapshot(
    solar_os_reticulum_announce_t *records,
    size_t max_records);

#ifdef __cplusplus
}
#endif

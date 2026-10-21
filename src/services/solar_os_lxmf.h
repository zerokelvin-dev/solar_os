#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_lxmf_format.h"
#include "solar_os_messaging_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_LXMF_HASH_HEX_LEN 33U
#define SOLAR_OS_LXMF_APP_NAME "lxmf"
#define SOLAR_OS_LXMF_ASPECT "delivery"

typedef struct {
    bool attached;
    char destination_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    char display_name[SOLAR_OS_LXMF_NAME_MAX + 1U];
    uint32_t peers;
    uint32_t links;
    uint32_t received;
    uint32_t rejected;
    uint32_t sent;
    uint32_t delivered;
    uint32_t failed;
    uint32_t announces_sent;
    esp_err_t last_error;
} solar_os_lxmf_status_t;

esp_err_t solar_os_lxmf_get_status(solar_os_lxmf_status_t *status);

/* Queues an announce of the delivery destination for the worker. */
esp_err_t solar_os_lxmf_announce(void);

/* Opens a conversation with a peer named by its delivery destination hash. */
esp_err_t solar_os_lxmf_open(const char *destination_hex,
                             solar_os_conversation_id_t *conversation_id);

#ifdef __cplusplus
}
#endif

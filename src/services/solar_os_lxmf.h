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
#define SOLAR_OS_LXMF_CLASS "lxmf"
#define SOLAR_OS_LXMF_DEFAULT_INSTANCE "lxmf"
/* Each instance announces and holds a message of its own. */
#define SOLAR_OS_LXMF_INSTANCE_MAX 4U
#define SOLAR_OS_LXMF_GROUP_CAPACITY 8U

/* The saved profile of an LXMF instance. An empty display name announces the
 * device hostname. Groups are the delivery hashes of group-server peers whose
 * messages are a room rather than a direct conversation. */
typedef struct {
    uint32_t version;
    bool enabled;
    char display_name[SOLAR_OS_LXMF_NAME_MAX + 1U];
    char groups[SOLAR_OS_LXMF_GROUP_CAPACITY][SOLAR_OS_LXMF_HASH_HEX_LEN];
} solar_os_lxmf_config_t;

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
    /* Taken by a propagation node to hold for the recipient. */
    uint32_t propagated;
    uint32_t failed;
    uint32_t announces_sent;
    esp_err_t last_error;
    /* Not ESP_OK when the instance's identity could not be loaded. */
    esp_err_t identity_error;
    /* The propagation node a message goes to when its recipient cannot be
     * reached: the nearest active one heard. Empty when none has been. */
    char propagation_node_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    uint8_t propagation_node_hops;
    uint32_t propagation_stamp_cost;
} solar_os_lxmf_status_t;

/* Registers the lxmf messaging class and restores its saved instances. */
esp_err_t solar_os_lxmf_init(void);

/* Creates the default instance, SOLAR_OS_LXMF_DEFAULT_INSTANCE, when no LXMF
 * instance exists. Reticulum calls it when it first makes a node identity. */
esp_err_t solar_os_lxmf_create_default(void);

/* Each instance has an identity and delivery destination of its own. */
esp_err_t solar_os_lxmf_get_status(solar_os_messaging_provider_id_t id,
                                   solar_os_lxmf_status_t *status);

/* Announces the instance's delivery destination and waits for the worker
 * to say whether it went. */
esp_err_t solar_os_lxmf_announce(solar_os_messaging_provider_id_t id);

/* Makes a new identity for an instance whose own is missing or unreadable,
 * or with force, replaces it, giving the instance a new address. Refused
 * while the instance is listening. */
esp_err_t solar_os_lxmf_identity_generate(solar_os_messaging_provider_id_t id,
                                         bool force);

/* Opens a conversation with a peer named by its delivery destination hash;
 * messages query on the first LXMF instance. */
esp_err_t solar_os_lxmf_open(const char *destination_hex,
                             solar_os_conversation_id_t *conversation_id);

#ifdef __cplusplus
}
#endif

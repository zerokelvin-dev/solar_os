#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_credentials.h"
#include "solar_os_reticulum.h"
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
#include "solar_os_lxmf.h"
#include "solar_os_messaging.h"
#endif
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const reticulum_commands[] = {
    "status", "identity", "announce", "announces",
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
    "lxmf",
#endif
};

static void reticulum_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  reticulum status");
    solar_os_shell_io_writeln(io, "  reticulum identity show");
    solar_os_shell_io_writeln(io, "  reticulum identity generate [--force]");
    solar_os_shell_io_writeln(
        io, "  reticulum identity import <private-key-hex>");
    solar_os_shell_io_writeln(io, "  reticulum identity export --private");
    solar_os_shell_io_writeln(io, "  reticulum announce");
    solar_os_shell_io_writeln(io, "  reticulum announces");
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
    solar_os_shell_io_writeln(io, "  reticulum lxmf status [instance]");
    solar_os_shell_io_writeln(io, "  reticulum lxmf announce [instance]");
    solar_os_shell_io_writeln(io, "  reticulum lxmf open <destination-hex>");
    solar_os_shell_io_writeln(io, "  reticulum lxmf identity generate <instance> [--force]");
#endif
    solar_os_shell_io_writeln(
        io, "start: job start reticulum <host> [port] [lora <radio> <profile>]");
    solar_os_shell_io_writeln(io, "       job start reticulum lora <radio> <profile>");
}

static void reticulum_error(solar_os_shell_io_t *io,
                            const char *operation,
                            esp_err_t error)
{
    solar_os_shell_io_printf(
        io, "reticulum %s: %s\n", operation, solar_os_shell_error_text(error));
}

static void reticulum_status(solar_os_shell_io_t *io)
{
    solar_os_reticulum_status_t status;
    const esp_err_t error = solar_os_reticulum_get_status(&status);
    if (error != ESP_OK) {
        reticulum_error(io, "status", error);
        return;
    }
    solar_os_shell_io_printf(io, "Reticulum: %s\n",
                             status.running ? "running" : "stopped");
    if (!status.running) {
        return;
    }
    solar_os_shell_io_printf(io, "Identity: %s\n", status.identity_hash_hex);
    solar_os_shell_io_printf(io, "Destination: %s (%s.%s)\n",
                             status.destination_hex,
                             SOLAR_OS_RETICULUM_APP_NAME,
                             SOLAR_OS_RETICULUM_ASPECT);
    if (status.host[0] != '\0') {
        solar_os_shell_io_printf(
            io, "Server: %s:%u, %s, connects %" PRIu32 "\n",
            status.host, (unsigned)status.port,
            status.tcp_online ? "online" : "offline", status.connects);
        solar_os_shell_io_printf(
            io,
            "Frames: rx %" PRIu32 " (%" PRIu32 " B), tx %" PRIu32
            " (%" PRIu32 " B)\n",
            status.rx_frames, status.rx_bytes, status.tx_frames, status.tx_bytes);
    }
    if (status.radio[0] != '\0') {
        solar_os_shell_io_printf(
            io, "LoRa: %s (%s), %s, rx %" PRIu32 " (%" PRIu32 " B), tx %" PRIu32
            " (%" PRIu32 " B)\n",
            status.radio, status.profile,
            status.lora_online ? "online" : "offline",
            status.lora_rx_frames, status.lora_rx_bytes, status.lora_tx_frames,
            status.lora_tx_bytes);
        if (status.lora_heard) {
            solar_os_shell_io_printf(io, "LoRa last heard: %d dBm, SNR %d dB\n",
                                     (int)status.lora_rssi_dbm,
                                     (int)status.lora_snr_db);
        }
        if (status.lora_send_errors != 0U || status.lora_crc_errors != 0U ||
            status.lora_recoveries != 0U) {
            solar_os_shell_io_printf(
                io,
                "LoRa errors: send %" PRIu32 ", crc %" PRIu32
                ", radio recovered %" PRIu32 "\n",
                status.lora_send_errors, status.lora_crc_errors,
                status.lora_recoveries);
        }
    }
    solar_os_shell_io_printf(
        io,
        "Announces: rx %" PRIu32 ", tx %" PRIu32 ", paths %u\n",
        status.announces_received, status.announces_sent,
        (unsigned)status.paths);
    solar_os_shell_io_printf(
        io, "Exceptions: %" PRIu32 ", stack watermark: %" PRIu32 " bytes\n",
        status.exceptions, status.stack_watermark_bytes);
}

static bool reticulum_identity(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "show") == 0) {
        char hash[SOLAR_OS_RETICULUM_HASH_HEX_LEN];
        const esp_err_t error = solar_os_reticulum_identity_hash(hash);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io, "%s\n", hash);
        } else {
            reticulum_error(io, "identity show", error);
            solar_os_shell_io_printf(io, "Expected in %s. Restore that file, or make a new "
                                     "identity with: reticulum identity generate\n",
                                     solar_os_credentials_location());
        }
        return true;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[2], "generate") == 0 &&
        (argc == 3 || strcmp(argv[3], "--force") == 0)) {
        const esp_err_t error = solar_os_reticulum_identity_generate(argc == 4);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "Reticulum identity generated");
        } else {
            reticulum_error(io, "identity generate", error);
        }
        return true;
    }
    if (argc == 4 && strcmp(argv[2], "import") == 0) {
        const esp_err_t error = solar_os_reticulum_identity_import(argv[3]);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "Reticulum identity imported");
        } else {
            reticulum_error(io, "identity import", error);
        }
        return true;
    }
    if (argc == 4 && strcmp(argv[2], "export") == 0 &&
        strcmp(argv[3], "--private") == 0) {
        char key[SOLAR_OS_RETICULUM_PRIVATE_KEY_HEX_LEN];
        const esp_err_t error = solar_os_reticulum_identity_export_private(key);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(
                io, "WARNING: private identity; keep this secret");
            solar_os_shell_io_printf(io, "%s\n", key);
            solar_os_credentials_wipe(key, sizeof(key));
        } else {
            reticulum_error(io, "identity export", error);
        }
        return true;
    }
    return false;
}

static void reticulum_announces(solar_os_shell_io_t *io)
{
    solar_os_reticulum_announce_t records[SOLAR_OS_RETICULUM_ANNOUNCE_LOG];
    const size_t count = solar_os_reticulum_announce_snapshot(
        records, SOLAR_OS_RETICULUM_ANNOUNCE_LOG);
    for (size_t index = 0U; index < count; index++) {
        solar_os_shell_io_printf(
            io, "%s  hops %u  %" PRIu32 "s ago  %s\n",
            records[index].destination_hex,
            (unsigned)records[index].hops,
            records[index].age_ms / 1000U,
            records[index].app_data);
    }
    if (count == 0U) {
        solar_os_shell_io_writeln(io, "No announces received");
    }
}

#if SOLAR_OS_PACKAGE_SERVICE_LXMF
/* Each LXMF instance, or only the one named; false when the name is not one. */
static bool reticulum_lxmf_next(const char *only, solar_os_messaging_provider_id_t *cursor,
                                solar_os_messaging_provider_status_t *status)
{
    while (solar_os_messaging_provider_next(cursor, status)) {
        if (strcmp(status->class_name, SOLAR_OS_LXMF_CLASS) == 0 &&
            (only == NULL || strcmp(status->name, only) == 0)) {
            return true;
        }
    }
    return false;
}

static void reticulum_lxmf_status(solar_os_shell_io_t *io,
                                  const solar_os_messaging_provider_status_t *instance)
{
    solar_os_lxmf_status_t status;
    if (solar_os_lxmf_get_status(instance->id, &status) != ESP_OK) {
        solar_os_shell_io_printf(io, "%s: unavailable\n", instance->name);
        return;
    }
    solar_os_shell_io_printf(io, "%s\n", instance->name);
    if (status.identity_error != ESP_OK) {
        solar_os_shell_io_printf(io, "  Identity: unavailable (%s); expected in %s\n",
                                 solar_os_shell_error_text(status.identity_error),
                                 solar_os_credentials_location());
        solar_os_shell_io_printf(io, "  Restore that file, or make a new identity with: "
                                 "reticulum lxmf identity generate %s\n", instance->name);
    }
    solar_os_shell_io_printf(io, "  Delivery: %s\n",
                             status.attached ? status.destination_hex : "offline");
    solar_os_shell_io_printf(io, "  Name: %s\n", status.display_name);
    solar_os_shell_io_printf(io, "  Peers: %lu\n", (unsigned long)status.peers);
    solar_os_shell_io_printf(io, "  Links: %lu\n", (unsigned long)status.links);
    solar_os_shell_io_printf(io, "  Received: %lu (%lu rejected)\n",
                             (unsigned long)status.received,
                             (unsigned long)status.rejected);
    solar_os_shell_io_printf(io, "  Sent: %lu (%lu delivered, %lu failed)\n",
                             (unsigned long)status.sent,
                             (unsigned long)status.delivered,
                             (unsigned long)status.failed);
    solar_os_shell_io_printf(io, "  Announces sent: %lu\n",
                             (unsigned long)status.announces_sent);
}

static bool reticulum_lxmf(solar_os_shell_io_t *io, int argc, char **argv)
{
    if ((argc == 3 || argc == 4) &&
        (strcmp(argv[2], "status") == 0 || strcmp(argv[2], "announce") == 0)) {
        const char *only = argc == 4 ? argv[3] : NULL;
        const bool announce = strcmp(argv[2], "announce") == 0;
        solar_os_messaging_provider_id_t cursor = 0;
        solar_os_messaging_provider_status_t instance;
        bool any = false;
        while (reticulum_lxmf_next(only, &cursor, &instance)) {
            any = true;
            if (!announce) {
                reticulum_lxmf_status(io, &instance);
                continue;
            }
            const esp_err_t error = solar_os_lxmf_announce(instance.id);
            if (error == ESP_ERR_INVALID_STATE) {
                solar_os_shell_io_printf(io, "%s: offline; start it with: job start reticulum <host> [port],"
                                         " and messages connect %s\n", instance.name, instance.name);
            } else if (error != ESP_OK) {
                solar_os_shell_io_printf(io, "%s: %s\n", instance.name, solar_os_shell_error_text(error));
            } else {
                solar_os_shell_io_printf(io, "%s: LXMF announce sent\n", instance.name);
            }
        }
        if (!any) {
            solar_os_shell_io_writeln(io, only ? "reticulum lxmf: no such LXMF instance" :
                "reticulum lxmf: no LXMF instance; add one with: messages add lxmf <name> [display-name]");
        }
        return true;
    }
    if ((argc == 5 || argc == 6) && strcmp(argv[2], "identity") == 0 &&
        strcmp(argv[3], "generate") == 0 &&
        (argc == 5 || strcmp(argv[5], "--force") == 0)) {
        solar_os_messaging_provider_id_t cursor = 0;
        solar_os_messaging_provider_status_t instance;
        if (!reticulum_lxmf_next(argv[4], &cursor, &instance)) {
            solar_os_shell_io_writeln(io, "reticulum lxmf: no such LXMF instance");
            return true;
        }
        const esp_err_t error = solar_os_lxmf_identity_generate(instance.id, argc == 6);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io, "%s: identity generated; its address has changed\n",
                                     instance.name);
        } else if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(io, "%s: has a working identity, or is online; to replace it, "
                                     "messages disconnect %s, then add --force\n",
                                     instance.name, instance.name);
        } else {
            reticulum_error(io, "lxmf identity generate", error);
        }
        return true;
    }
    if (argc == 4 && strcmp(argv[2], "open") == 0) {
        solar_os_conversation_id_t conversation = 0;
        const esp_err_t error = solar_os_lxmf_open(argv[3], &conversation);
        if (error == ESP_ERR_INVALID_ARG) {
            solar_os_shell_diag_invalid(io,
                                        "reticulum lxmf open",
                                        "destination",
                                        argv[3],
                                        "32 hexadecimal characters",
                                        "reticulum lxmf open <destination-hex>",
                                        false);
        } else if (error != ESP_OK) {
            reticulum_error(io, "lxmf open", error);
        } else {
            solar_os_shell_io_printf(io,
                                     "conversation %lu; send with: "
                                     "messages send %lu <text>\n",
                                     (unsigned long)conversation,
                                     (unsigned long)conversation);
        }
        return true;
    }
    return false;
}
#endif

void solar_os_shell_cmd_reticulum(solar_os_context_t *ctx,
                                  int argc,
                                  char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (argc == 1) {
        reticulum_usage(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        reticulum_status(io);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "identity") == 0 &&
        reticulum_identity(io, argc, argv)) {
        return;
    }
    if (argc == 2 && strcmp(argv[1], "announce") == 0) {
        const esp_err_t error = solar_os_reticulum_announce();
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "Reticulum announce sent");
        } else if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(
                io, "reticulum announce: start it with: job start reticulum <host> [port]");
        } else {
            reticulum_error(io, "announce", error);
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "announces") == 0) {
        reticulum_announces(io);
        return;
    }
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
    if (argc >= 3 && strcmp(argv[1], "lxmf") == 0 &&
        reticulum_lxmf(io, argc, argv)) {
        return;
    }
#endif
    solar_os_shell_diag_subcommand(
        io,
        "reticulum",
        argc,
        argv,
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
        "reticulum status|identity|announce|announces|lxmf",
#else
        "reticulum status|identity|announce|announces",
#endif
        reticulum_commands,
        sizeof(reticulum_commands) / sizeof(reticulum_commands[0]));
}

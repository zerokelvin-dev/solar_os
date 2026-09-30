#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_credentials.h"
#include "solar_os_reticulum.h"
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
#include "solar_os_lxmf.h"
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
    solar_os_shell_io_writeln(io, "  reticulum lxmf status");
    solar_os_shell_io_writeln(io, "  reticulum lxmf announce");
    solar_os_shell_io_writeln(io, "  reticulum lxmf open <destination-hex>");
#endif
    solar_os_shell_io_writeln(io, "start: job start reticulum <host> [port]");
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
    solar_os_shell_io_printf(
        io, "Server: %s:%u, %s, connects %" PRIu32 "\n",
        status.host, (unsigned)status.port,
        status.interface_online ? "online" : "offline", status.connects);
    solar_os_shell_io_printf(
        io,
        "Frames: rx %" PRIu32 " (%" PRIu32 " B), tx %" PRIu32
        " (%" PRIu32 " B)\n",
        status.rx_frames, status.rx_bytes, status.tx_frames, status.tx_bytes);
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
static bool reticulum_lxmf(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "status") == 0) {
        solar_os_lxmf_status_t status;
        if (solar_os_lxmf_get_status(&status) != ESP_OK) {
            reticulum_error(io, "lxmf status", ESP_FAIL);
            return true;
        }
        solar_os_shell_io_printf(io,
                                 "Delivery: %s\n",
                                 status.attached ? status.destination_hex :
                                     "offline");
        solar_os_shell_io_printf(io, "Name: %s\n", status.display_name);
        solar_os_shell_io_printf(io,
                                 "Peers: %lu\n",
                                 (unsigned long)status.peers);
        solar_os_shell_io_printf(io,
                                 "Received: %lu (%lu rejected)\n",
                                 (unsigned long)status.received,
                                 (unsigned long)status.rejected);
        solar_os_shell_io_printf(io,
                                 "Sent: %lu (%lu delivered, %lu failed)\n",
                                 (unsigned long)status.sent,
                                 (unsigned long)status.delivered,
                                 (unsigned long)status.failed);
        solar_os_shell_io_printf(io,
                                 "Announces sent: %lu\n",
                                 (unsigned long)status.announces_sent);
        return true;
    }
    if (argc == 3 && strcmp(argv[2], "announce") == 0) {
        const esp_err_t error = solar_os_lxmf_announce();
        if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(
                io, "reticulum lxmf: start it with: job start reticulum <host> [port]");
        } else if (error != ESP_OK) {
            reticulum_error(io, "lxmf announce", error);
        } else {
            solar_os_shell_io_writeln(io, "LXMF announce queued");
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

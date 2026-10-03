#include "solar_os_shell_commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "solar_os_map_base.h"
#include "solar_os_map_fetch.h"
#include "solar_os_map_geo.h"
#include "solar_os_map_layers.h"
#include "solar_os_map_app.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

/* How many kept maps one listing shows at once. */
#define MAP_SHELL_STORED_MAX 64U

/*
 * What to call each class when listing a layer. Kept here rather than in
 * the service because it is how a listing reads, not what a ring is, and
 * the order follows solar_os_map_class_t exactly.
 */
static const char *const map_shell_class_names[SOLAR_OS_MAP_CLASS_COUNT] = {
    "land", "water", "road", "rail", "building", "boundary", "region",
    "minor road", "water edge", "highway",
};

static const char * const map_commands[] = {
    "status", "layers", "load", "unload", "fetch", "base", "stored",
    "forget", "help",
};

static void map_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  map");
    solar_os_shell_io_writeln(io, "  map status");
    solar_os_shell_io_writeln(io, "  map layers");
    solar_os_shell_io_writeln(io,
        "  map load </path | cell | index | name>  slash is a file, n is a "
        "cell, a digit is its number, anything else is a kept name");
    solar_os_shell_io_writeln(io,
        "  map base                           download the world map, once");
    solar_os_shell_io_writeln(io, "  map stored                         list the maps kept on the card");
    solar_os_shell_io_writeln(io,
        "  map forget <name>                  delete a kept map from the card");
    solar_os_shell_io_writeln(io,
        "  map fetch <cell|latitude longitude>  ask OpenStreetMap for a cell");
    solar_os_shell_io_writeln(io, "  map unload <index|all>");
    solar_os_shell_io_writeln(io, "  map help");
    solar_os_shell_io_writeln(io, "points and paths: places help");
}

static void map_status(solar_os_shell_io_t *io)
{
    solar_os_shell_io_printf(io,
                             "Layers: %u of %u, %u of %u KiB\n",
                             (unsigned)solar_os_map_layer_count(),
                             (unsigned)SOLAR_OS_MAP_LAYER_MAX,
                             (unsigned)(solar_os_map_layer_bytes() / 1024U),
                             (unsigned)(SOLAR_OS_MAP_LAYER_BUDGET_BYTES /
                                        1024U));
}

void solar_os_shell_cmd_map(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }

    if (argc == 1) {
        const esp_err_t err = solar_os_context_request_launch(ctx, &solar_os_map_app, 0, NULL);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io, "map: launch failed: %s\n", solar_os_shell_error_text(err));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        }
        return;
    }
    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        map_status(io);
        return;
    }
    if (strcmp(argv[1], "stored") == 0 && argc == 2) {
        /* However many are kept, not however many can be loaded: a card
         * holds far more maps than the twelve that fit in memory. */
        solar_os_map_stored_t *stored =
            solar_os_memory_calloc(MAP_SHELL_STORED_MAX,
                                   sizeof(*stored),
                                   SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                   "shell.map.stored");
        if (stored == NULL) {
            solar_os_shell_io_writeln(io, "map: out of memory");
            return;
        }
        const size_t count =
            solar_os_map_layer_stored(stored, MAP_SHELL_STORED_MAX);
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "no maps kept");
            solar_os_memory_free(stored);
            return;
        }
        for (size_t index = 0U; index < count; index++) {
            solar_os_shell_io_printf(io, "%2u %-24s %8u bytes%s\n",
                                     (unsigned)index, stored[index].name,
                                     (unsigned)stored[index].size_bytes,
                                     stored[index].loaded ? "  loaded" : "");
        }
        const size_t kept = solar_os_map_layer_stored_count();
        if (kept > count) {
            solar_os_shell_io_printf(io, "... and %u more\n",
                                     (unsigned)(kept - count));
        }
        solar_os_memory_free(stored);
        return;
    }

    /*
     * The world, fetched once and then free for ever. Asking again when it
     * is already kept loads it rather than fetching it, which is what
     * somebody typing this on a device that already has it wants.
     */
    if (strcmp(argv[1], "base") == 0 && argc == 2) {
        if (solar_os_map_base_busy()) {
            solar_os_shell_io_printf(io, "map: %s\n",
                                     solar_os_map_base_status());
            return;
        }
        const esp_err_t err = solar_os_map_base_fetch();
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io, "map: %s\n",
                                     solar_os_map_base_status());
            return;
        }
        (void)solar_os_map_base_load();
        solar_os_shell_io_printf(io, "map: %s, %u of 4 kept\n",
                                 solar_os_map_base_status(),
                                 (unsigned)solar_os_map_base_kept());
        return;
    }

    /* Unloading gives back the memory; forgetting gives back the card. */
    if (strcmp(argv[1], "forget") == 0 && argc == 3) {
        const esp_err_t err = solar_os_map_layer_forget(argv[2]);
        if (err != ESP_OK) {
            solar_os_shell_diag_invalid(io,
                                        "map forget",
                                        "map",
                                        argv[2],
                                        "the name of a kept map from map stored",
                                        "map forget <name>",
                                        false);
            return;
        }
        solar_os_shell_io_printf(io, "%s forgotten\n", argv[2]);
        return;
    }

    if (strcmp(argv[1], "layers") == 0 && argc == 2) {
        for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
            const solar_os_map_geometry_t *layer = solar_os_map_layer(index);
            char surveyed[24] = "spacing unknown";
            if (layer != NULL && layer->resolution_m > 0U) {
                (void)snprintf(surveyed, sizeof(surveyed), "every %um",
                               (unsigned)layer->resolution_m);
            }
            solar_os_shell_io_printf(io,
                                     "%u %-20s %5u rings %7u points  %s\n",
                                     (unsigned)index,
                                     solar_os_map_layer_name(index),
                                     layer != NULL ? (unsigned)layer->ring_count : 0U,
                                     layer != NULL ? (unsigned)layer->point_count : 0U,
                                     surveyed);
            if (layer == NULL) {
                continue;
            }
            /*
             * What the layer is made of, which is what decides its colours
             * and how hard the density cull works on it. Only the classes
             * present are named, so a coastline says "land" and nothing else.
             */
            char kinds[96] = "";
            size_t used = 0U;
            for (unsigned klass = 0U; klass < SOLAR_OS_MAP_CLASS_COUNT;
                 klass++) {
                if (layer->class_rings[klass] == 0U) {
                    continue;
                }
                const int wrote =
                    snprintf(&kinds[used], sizeof(kinds) - used, "%s%s %u",
                             used == 0U ? "" : ", ",
                             map_shell_class_names[klass],
                             (unsigned)layer->class_rings[klass]);
                if (wrote <= 0 || (size_t)wrote >= sizeof(kinds) - used) {
                    break;
                }
                used += (size_t)wrote;
            }
            if (kinds[0] != '\0') {
                solar_os_shell_io_printf(io, "  %s\n", kinds);
            }
        }
        return;
    }
    if (strcmp(argv[1], "fetch") == 0 && (argc == 3 || argc == 4)) {
        int32_t latitude = 0;
        int32_t longitude = 0;
        const bool named = argc == 3
                               ? solar_os_map_cell_parse(argv[2], &latitude,
                                                         &longitude)
                               : (solar_os_map_parse_degrees(argv[2], true,
                                                             &latitude) &&
                                  solar_os_map_parse_degrees(argv[3], false,
                                                             &longitude));
        if (!named) {
            solar_os_shell_diag_invalid(
                io,
                "map fetch",
                "cell",
                argv[2],
                "a cell such as n4350w07950, or a latitude and longitude",
                "map fetch <cell|latitude longitude>",
                false);
            return;
        }
        if (solar_os_map_fetch_busy()) {
            solar_os_shell_io_writeln(io, "map fetch: already asking");
            return;
        }
        const esp_err_t err = solar_os_map_fetch_cell(latitude, longitude);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io, "map fetch: %s\n",
                                     solar_os_map_fetch_status());
            return;
        }
        /* It runs elsewhere; wait here so the reader is told what happened
         * rather than left to ask again - but not forever, since the fetch
         * has its own timeout and a shell that never returns is worse. */
        for (unsigned waited = 0U; solar_os_map_fetch_busy(); waited++) {
            if (waited >= 4U * 240U) {
                solar_os_shell_io_writeln(
                    io, "still fetching; map status says when it is done");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        solar_os_shell_io_printf(io, "%s\n", solar_os_map_fetch_status());
        return;
    }

    if (strcmp(argv[1], "load") == 0 && argc == 3) {
        const esp_err_t err = solar_os_map_layer_load_named(argv[2]);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "map load: %s\n",
                                     solar_os_shell_error_text(err));
            return;
        }
        const size_t index = solar_os_map_layer_count() - 1U;
        const solar_os_map_geometry_t *layer = solar_os_map_layer(index);
        solar_os_shell_io_printf(io,
                                 "layer %u: %u rings, %u points\n",
                                 (unsigned)index,
                                 layer != NULL ? (unsigned)layer->ring_count : 0U,
                                 layer != NULL ? (unsigned)layer->point_count : 0U);
        return;
    }
    if (strcmp(argv[1], "unload") == 0 && argc == 3) {
        if (strcmp(argv[2], "all") == 0) {
            solar_os_map_layer_clear();
            solar_os_shell_io_writeln(io, "all layers unloaded");
            return;
        }
        char *end = NULL;
        errno = 0;
        const unsigned long index = strtoul(argv[2], &end, 10);
        const esp_err_t err = (errno != 0 || end == argv[2] || *end != '\0')
                                  ? ESP_ERR_INVALID_ARG
                                  : solar_os_map_layer_remove((size_t)index);
        if (err != ESP_OK) {
            solar_os_shell_diag_invalid(io,
                                        "map unload",
                                        "layer",
                                        argv[2],
                                        "a loaded layer index from map layers, or all",
                                        "map unload <index|all>",
                                        false);
            return;
        }
        solar_os_shell_io_writeln(io, "layer removed");
        return;
    }
    if (argc == 2 && strcmp(argv[1], "help") == 0) {
        map_usage(io);
        return;
    }

    solar_os_shell_diag_subcommand(io,
                                   "map",
                                   argc,
                                   argv,
                                   "map status|layers|load|stored|fetch|base|"
                                   "forget|unload|help",
                                   map_commands,
                                   sizeof(map_commands) / sizeof(map_commands[0]));
}

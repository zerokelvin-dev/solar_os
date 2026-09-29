#include "solar_os_shell_commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_map.h"
#include "solar_os_map_layers.h"
#include "solar_os_map_app.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#define MAP_SHELL_USER_SOURCE "user"

static const char * const map_commands[] = {
    "status", "list", "add", "remove", "clear", "fix", "layers", "load",
    "unload",
};

static void map_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  map");
    solar_os_shell_io_writeln(io, "  map status");
    solar_os_shell_io_writeln(io, "  map list");
    solar_os_shell_io_writeln(io, "  map add <label> <latitude> <longitude>");
    solar_os_shell_io_writeln(io, "  map remove <id>");
    solar_os_shell_io_writeln(io, "  map clear [source]");
    solar_os_shell_io_writeln(io, "  map fix");
    solar_os_shell_io_writeln(io, "  map layers");
    solar_os_shell_io_writeln(io, "  map load <path>");
    solar_os_shell_io_writeln(io, "  map unload <index|all>");
}

static void map_status(solar_os_shell_io_t *io)
{
    solar_os_map_status_t status;
    const esp_err_t err = solar_os_map_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "map: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io,
                             "Points: %u/%u (%u evicted)\n",
                             (unsigned)status.count,
                             (unsigned)status.capacity,
                             (unsigned)status.evicted);
    solar_os_shell_io_printf(io,
                             "Layers: %u of %u\n",
                             (unsigned)solar_os_map_layer_count(),
                             (unsigned)SOLAR_OS_MAP_LAYER_MAX + 1U);
}

static void map_list(solar_os_shell_io_t *io)
{
    solar_os_map_point_t *points = solar_os_memory_calloc(SOLAR_OS_MAP_CAPACITY,
                                                          sizeof(*points),
                                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                          "shell.map");
    if (points == NULL) {
        solar_os_shell_io_writeln(io, "map: out of memory");
        return;
    }
    size_t total = 0U;
    const size_t count = solar_os_map_snapshot(points, SOLAR_OS_MAP_CAPACITY, &total);
    if (count == 0U) {
        solar_os_shell_io_writeln(io, "no points");
    }
    for (size_t i = 0U; i < count; i++) {
        char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
        solar_os_map_format_coord(points[i].latitude_e7, points[i].longitude_e7, coord);
        solar_os_shell_io_printf(io,
                                 "%4u %-8s %-10s %-20s %s\n",
                                 (unsigned)points[i].id,
                                 solar_os_map_kind_name(points[i].kind),
                                 points[i].source,
                                 points[i].label,
                                 coord);
    }
    solar_os_memory_free(points);
}

static void map_add(solar_os_shell_io_t *io, char **argv)
{
    int32_t lat = 0;
    int32_t lon = 0;
    if (!solar_os_map_parse_degrees(argv[3], true, &lat)) {
        solar_os_shell_diag_invalid(io,
                                    "map add",
                                    "latitude",
                                    argv[3],
                                    "degrees from -90 to 90",
                                    "map add <label> <latitude> <longitude>",
                                    false);
        return;
    }
    if (!solar_os_map_parse_degrees(argv[4], false, &lon)) {
        solar_os_shell_diag_invalid(io,
                                    "map add",
                                    "longitude",
                                    argv[4],
                                    "degrees from -180 to 180",
                                    "map add <label> <latitude> <longitude>",
                                    false);
        return;
    }
    const solar_os_map_publish_t point = {
        .source = MAP_SHELL_USER_SOURCE,
        .key = argv[2],
        .label = argv[2],
        .kind = SOLAR_OS_MAP_KIND_WAYPOINT,
        .latitude_e7 = lat,
        .longitude_e7 = lon,
    };
    uint32_t id = 0U;
    const esp_err_t err = solar_os_map_publish(&point, &id);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "map add: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io, "waypoint %u\n", (unsigned)id);
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
    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        map_list(io);
        return;
    }
    if (strcmp(argv[1], "add") == 0 && argc == 5) {
        map_add(io, argv);
        return;
    }
    if (strcmp(argv[1], "remove") == 0 && argc == 3) {
        char *end = NULL;
        errno = 0;
        const unsigned long id = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || id == 0UL || id > UINT32_MAX) {
            solar_os_shell_diag_invalid(io,
                                        "map remove",
                                        "point ID",
                                        argv[2],
                                        "a decimal point ID from map list",
                                        "map remove <id>",
                                        false);
            return;
        }
        const esp_err_t err = solar_os_map_remove((uint32_t)id);
        solar_os_shell_io_printf(io,
                                 "map: %s\n",
                                 err == ESP_OK ? "removed" : solar_os_shell_error_text(err));
        return;
    }
    if (strcmp(argv[1], "clear") == 0 && argc <= 3) {
        if (argc == 3) {
            size_t removed = 0U;
            const esp_err_t err = solar_os_map_remove_source(argv[2], &removed);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(io, "map: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_printf(io, "removed %u points\n", (unsigned)removed);
            }
            return;
        }
        solar_os_map_status_t status = {0};
        (void)solar_os_map_get_status(&status);
        (void)solar_os_map_clear();
        solar_os_shell_io_printf(io, "removed %u points\n", (unsigned)status.count);
        return;
    }
    if (strcmp(argv[1], "fix") == 0 && argc == 2) {
        const esp_err_t err = solar_os_map_update_self();
        solar_os_shell_io_printf(io,
                                 "map: %s\n",
                                 err == ESP_OK ? "position updated"
                                               : "no GNSS fix available");
        return;
    }
    if (strcmp(argv[1], "layers") == 0 && argc == 2) {
        for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
            const solar_os_map_geometry_t *layer = solar_os_map_layer(index);
            solar_os_shell_io_printf(io,
                                     "%u %-20s %5u rings %7u points\n",
                                     (unsigned)index,
                                     solar_os_map_layer_name(index),
                                     layer != NULL ? (unsigned)layer->ring_count : 0U,
                                     layer != NULL ? (unsigned)layer->point_count : 0U);
        }
        return;
    }
    if (strcmp(argv[1], "load") == 0 && argc == 3) {
        const esp_err_t err = solar_os_map_layer_load(argv[2]);
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
            solar_os_shell_io_writeln(io, "only the built-in world remains");
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
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        map_usage(io);
        return;
    }

    solar_os_shell_diag_subcommand(io,
                                   "map",
                                   argc,
                                   argv,
                                   "map status|list|add|remove|clear|fix|layers|load|unload",
                                   map_commands,
                                   sizeof(map_commands) / sizeof(map_commands[0]));
}

#include "solar_os_shell_commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_map_geo.h"
#include "solar_os_memory.h"
#include "solar_os_places.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const places_commands[] = {
    "status", "list", "add", "remove", "clear", "fix", "path", "help",
};

static const char * const places_path_commands[] = {
    "add", "list", "remove", "clear",
};

static void places_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  places status");
    solar_os_shell_io_writeln(io, "  places list");
    solar_os_shell_io_writeln(io, "  places add <label> <latitude> <longitude>");
    solar_os_shell_io_writeln(io, "  places remove <id>");
    solar_os_shell_io_writeln(io, "  places clear [source]");
    solar_os_shell_io_writeln(io, "  places fix                         publish the GNSS position as your own");
    solar_os_shell_io_writeln(io, "  places path add <from-id> <to-id> [label]");
    solar_os_shell_io_writeln(io, "  places path list");
    solar_os_shell_io_writeln(io, "  places path remove <id>");
    solar_os_shell_io_writeln(io, "  places path clear <source>");
    solar_os_shell_io_writeln(io, "  places help");
}

static void places_status(solar_os_shell_io_t *io)
{
    solar_os_places_status_t status;
    const esp_err_t err = solar_os_places_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "places: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io,
                             "Points: %u/%u (%u evicted)\n",
                             (unsigned)status.count,
                             (unsigned)status.capacity,
                             (unsigned)status.evicted);
    solar_os_shell_io_printf(io,
                             "Paths: %u/%u\n",
                             (unsigned)status.path_count,
                             (unsigned)status.path_capacity);
}

static void places_list(solar_os_shell_io_t *io)
{
    solar_os_places_point_t *points = solar_os_memory_calloc(SOLAR_OS_PLACES_CAPACITY,
                                                          sizeof(*points),
                                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                          "shell.places");
    if (points == NULL) {
        solar_os_shell_io_writeln(io, "places: out of memory");
        return;
    }
    size_t total = 0U;
    const size_t count = solar_os_places_snapshot(points, SOLAR_OS_PLACES_CAPACITY, &total);
    if (count == 0U) {
        solar_os_shell_io_writeln(io, "no points");
    }
    for (size_t i = 0U; i < count; i++) {
        char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
        solar_os_map_format_coord(points[i].latitude_e7, points[i].longitude_e7, coord);
        solar_os_shell_io_printf(io,
                                 "%4u %-8s %-10s %-20s %s\n",
                                 (unsigned)points[i].id,
                                 solar_os_places_kind_name(points[i].kind),
                                 points[i].source,
                                 points[i].label,
                                 coord);
    }
    solar_os_memory_free(points);
}

static bool places_parse_id(const char *text, uint32_t *id)
{
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL ||
        value > UINT32_MAX) {
        return false;
    }
    *id = (uint32_t)value;
    return true;
}

static void places_path(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc >= 5 && strcmp(argv[2], "add") == 0 && argc <= 6) {
        uint32_t from = 0U;
        uint32_t to = 0U;
        if (!places_parse_id(argv[3], &from) || !places_parse_id(argv[4], &to)) {
            solar_os_shell_diag_invalid(io,
                                        "places path add",
                                        "point ID",
                                        argv[3],
                                        "two decimal point IDs from places list",
                                        "places path add <from-id> <to-id> [label]",
                                        false);
            return;
        }
        char key[SOLAR_OS_PLACES_KEY_MAX];
        snprintf(key, sizeof(key), "%lu-%lu", (unsigned long)from,
                 (unsigned long)to);
        const solar_os_places_path_publish_t path = {
            .source = SOLAR_OS_PLACES_SOURCE_USER,
            .key = key,
            .label = argc == 6 ? argv[5] : key,
            .from_id = from,
            .to_id = to,
        };
        uint32_t id = 0U;
        const esp_err_t err = solar_os_places_path_publish(&path, &id);
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(io, "places path: no such point");
        } else if (err != ESP_OK) {
            solar_os_shell_io_printf(io, "places path: %s\n",
                                     solar_os_shell_error_text(err));
        } else {
            solar_os_shell_io_printf(io, "path %u\n", (unsigned)id);
        }
        return;
    }
    if (argc == 3 && strcmp(argv[2], "list") == 0) {
        solar_os_places_path_t paths[SOLAR_OS_PLACES_PATH_CAPACITY];
        size_t total = 0U;
        const size_t count =
            solar_os_places_path_snapshot(paths, SOLAR_OS_PLACES_PATH_CAPACITY, &total);
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "no paths");
        }
        for (size_t i = 0U; i < count; i++) {
            solar_os_shell_io_printf(io,
                                     "%4u %-10s %-20s %u -> %u\n",
                                     (unsigned)paths[i].id,
                                     paths[i].source,
                                     paths[i].label,
                                     (unsigned)paths[i].from_id,
                                     (unsigned)paths[i].to_id);
        }
        return;
    }
    if (argc == 4 && strcmp(argv[2], "remove") == 0) {
        uint32_t id = 0U;
        if (!places_parse_id(argv[3], &id)) {
            solar_os_shell_diag_invalid(io,
                                        "places path remove",
                                        "path ID",
                                        argv[3],
                                        "a decimal path ID from places path list",
                                        "places path remove <id>",
                                        false);
            return;
        }
        const esp_err_t err = solar_os_places_path_remove(id);
        solar_os_shell_io_printf(io,
                                 "places path: %s\n",
                                 err == ESP_OK ? "removed"
                                               : solar_os_shell_error_text(err));
        return;
    }
    if (argc == 4 && strcmp(argv[2], "clear") == 0) {
        size_t removed = 0U;
        (void)solar_os_places_path_remove_source(argv[3], &removed);
        solar_os_shell_io_printf(io, "removed %u paths\n", (unsigned)removed);
        return;
    }
    solar_os_shell_diag_subcommand(
        io,
        "places path",
        argc - 1,
        &argv[1],
        "places path add|list|remove|clear",
        places_path_commands,
        sizeof(places_path_commands) / sizeof(places_path_commands[0]));
}

static void places_add(solar_os_shell_io_t *io, char **argv)
{
    int32_t lat = 0;
    int32_t lon = 0;
    if (!solar_os_map_parse_degrees(argv[3], true, &lat)) {
        solar_os_shell_diag_invalid(io,
                                    "places add",
                                    "latitude",
                                    argv[3],
                                    "degrees from -90 to 90",
                                    "places add <label> <latitude> <longitude>",
                                    false);
        return;
    }
    if (!solar_os_map_parse_degrees(argv[4], false, &lon)) {
        solar_os_shell_diag_invalid(io,
                                    "places add",
                                    "longitude",
                                    argv[4],
                                    "degrees from -180 to 180",
                                    "places add <label> <latitude> <longitude>",
                                    false);
        return;
    }
    const solar_os_places_publish_t point = {
        .source = SOLAR_OS_PLACES_SOURCE_USER,
        .key = argv[2],
        .label = argv[2],
        .kind = SOLAR_OS_PLACES_KIND_WAYPOINT,
        .latitude_e7 = lat,
        .longitude_e7 = lon,
    };
    uint32_t id = 0U;
    const esp_err_t err = solar_os_places_publish(&point, &id);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "places add: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io, "waypoint %u\n", (unsigned)id);
}

void solar_os_shell_cmd_places(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "help") == 0)) {
        places_usage(io);
        return;
    }
    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        places_status(io);
        return;
    }
    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        places_list(io);
        return;
    }
    if (strcmp(argv[1], "add") == 0 && argc == 5) {
        places_add(io, argv);
        return;
    }
    if (strcmp(argv[1], "remove") == 0 && argc == 3) {
        char *end = NULL;
        errno = 0;
        const unsigned long id = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || id == 0UL || id > UINT32_MAX) {
            solar_os_shell_diag_invalid(io,
                                        "places remove",
                                        "point ID",
                                        argv[2],
                                        "a decimal point ID from places list",
                                        "places remove <id>",
                                        false);
            return;
        }
        const esp_err_t err = solar_os_places_remove((uint32_t)id);
        solar_os_shell_io_printf(io,
                                 "places: %s\n",
                                 err == ESP_OK ? "removed" : solar_os_shell_error_text(err));
        return;
    }
    if (strcmp(argv[1], "clear") == 0 && argc <= 3) {
        if (argc == 3) {
            size_t removed = 0U;
            const esp_err_t err = solar_os_places_remove_source(argv[2], &removed);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(io, "places: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_printf(io, "removed %u points\n", (unsigned)removed);
            }
            return;
        }
        solar_os_places_status_t status = {0};
        (void)solar_os_places_get_status(&status);
        (void)solar_os_places_clear();
        solar_os_shell_io_printf(io, "removed %u points\n", (unsigned)status.count);
        return;
    }
    if (strcmp(argv[1], "fix") == 0 && argc == 2) {
        /*
         * A reading that was taken and then had nowhere to go is not the
         * same as no reading, and saying so sends the reader to the GNSS
         * when the answer is that the store is full of waypoints.
         */
        const esp_err_t err = solar_os_places_update_self();
        const char *text = "no GNSS fix available";
        if (err == ESP_OK) {
            text = "position updated";
        } else if (err == ESP_ERR_NO_MEM) {
            text = "places full: remove a waypoint";
        }
        solar_os_shell_io_printf(io, "places: %s\n", text);
        return;
    }
    if (strcmp(argv[1], "path") == 0 && argc >= 2) {
        places_path(io, argc, argv);
        return;
    }

    solar_os_shell_diag_subcommand(io,
                                   "places",
                                   argc,
                                   argv,
                                   "places status|list|add|remove|clear|fix|"
                                   "path|help",
                                   places_commands,
                                   sizeof(places_commands) / sizeof(places_commands[0]));
}

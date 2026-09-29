#include "solar_os_map_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_keys.h"
#include "solar_os_map.h"
#include "solar_os_memory.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define MAP_APP_SELF_POLL_MS 5000U
#define MAP_APP_DEFAULT_SCALE_M 100U
#define MAP_APP_LABEL_COLS 10U

typedef struct {
    solar_os_tui_t tui;
    solar_os_map_point_t *points;
    size_t count;
    size_t total;
    uint32_t generation;
    uint32_t selected_id;
    uint32_t last_self_poll_ms;
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    size_t scale;
    bool follow_self;
    bool centered;
    char feedback[64];
} map_app_state_t;

static void *map_app_state;
#define map_app (*(map_app_state_t *)map_app_state)

static uint32_t map_app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const solar_os_map_point_t *map_app_self(void)
{
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].kind == SOLAR_OS_MAP_KIND_SELF) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static const solar_os_map_point_t *map_app_selected(void)
{
    for (size_t i = 0; map_app.selected_id != 0 && i < map_app.count; i++) {
        if (map_app.points[i].id == map_app.selected_id) {
            return &map_app.points[i];
        }
    }
    return NULL;
}

static size_t map_app_map_rows(void)
{
    const size_t rows = solar_os_tui_screen_content_rows(&map_app.tui, 1U, 1U);
    return rows > 1U ? rows - 1U : 0U;
}

static void map_app_fit(void)
{
    int32_t lat[SOLAR_OS_MAP_CAPACITY];
    int32_t lon[SOLAR_OS_MAP_CAPACITY];
    for (size_t i = 0; i < map_app.count; i++) {
        lat[i] = map_app.points[i].latitude_e7;
        lon[i] = map_app.points[i].longitude_e7;
    }
    map_app.scale = solar_os_map_fit_scale(lat,
                                           lon,
                                           map_app.count,
                                           map_app.center_lat_e7,
                                           map_app.center_lon_e7,
                                           solar_os_tui_cols(&map_app.tui),
                                           map_app_map_rows());
}

static void map_app_center_on(const solar_os_map_point_t *point)
{
    map_app.center_lat_e7 = point->latitude_e7;
    map_app.center_lon_e7 = point->longitude_e7;
    map_app.centered = true;
}

static void map_app_refresh(void)
{
    map_app.count = solar_os_map_snapshot(map_app.points,
                                          SOLAR_OS_MAP_CAPACITY,
                                          &map_app.total);
    solar_os_map_status_t status;
    if (solar_os_map_get_status(&status) == ESP_OK) {
        map_app.generation = status.generation;
    }
    if (map_app.selected_id != 0 && map_app_selected() == NULL) {
        map_app.selected_id = 0;
    }

    const solar_os_map_point_t *self = map_app_self();
    if (self != NULL && map_app.follow_self) {
        map_app_center_on(self);
    } else if (!map_app.centered && map_app.count > 0) {
        map_app_center_on(self != NULL ? self : &map_app.points[0]);
        map_app.follow_self = self != NULL;
        map_app_fit();
    }
}

static void map_app_poll_self(void)
{
    map_app.last_self_poll_ms = map_app_now_ms();
    (void)solar_os_map_update_self();
}

static void map_app_format_age(uint32_t updated_ms, char *text, size_t text_len)
{
    const uint32_t seconds = (map_app_now_ms() - updated_ms) / 1000U;
    if (seconds < 90U) {
        snprintf(text, text_len, "%us", (unsigned)seconds);
    } else if (seconds < 5400U) {
        snprintf(text, text_len, "%um", (unsigned)((seconds + 30U) / 60U));
    } else {
        snprintf(text, text_len, "%uh", (unsigned)((seconds + 1800U) / 3600U));
    }
}

static char map_app_glyph(const solar_os_map_point_t *point)
{
    switch (point->kind) {
    case SOLAR_OS_MAP_KIND_SELF:
        return '@';
    case SOLAR_OS_MAP_KIND_WAYPOINT:
        return '+';
    default:
        return 'o';
    }
}

static void map_app_draw_scale_bar(size_t map_rows, size_t cols)
{
    const uint32_t meters = solar_os_map_scale_meters_per_col(map_app.scale);
    const size_t bar = cols > 24U ? 10U : 5U;
    char distance[16];
    char line[48];
    solar_os_map_format_distance((uint32_t)(meters * bar), distance, sizeof(distance));
    snprintf(line, sizeof(line), "|%.*s| %s", (int)(bar - 2U), "----------", distance);
    solar_os_tui_addstr(&map_app.tui, map_rows, 0U, line, SOLAR_OS_TUI_ATTR_NORMAL);
}

static void map_app_render_info(size_t row, size_t cols)
{
    char line[96];
    const solar_os_map_point_t *point = map_app_selected();
    if (point == NULL) {
        snprintf(line,
                 sizeof(line),
                 map_app.count == 0 ? "No positions yet (map add, or wait for a fix)"
                                    : "%u points  Tab selects",
                 (unsigned)map_app.count);
        solar_os_tui_write_cell(&map_app.tui, row, 0U, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }

    char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
    char age[8];
    char away[32] = "";
    solar_os_map_format_coord(point->latitude_e7, point->longitude_e7, coord);
    map_app_format_age(point->updated_ms, age, sizeof(age));
    const solar_os_map_point_t *self = map_app_self();
    if (self != NULL && self->id != point->id) {
        char distance[16];
        solar_os_map_format_distance(solar_os_map_distance_m(self->latitude_e7,
                                                             self->longitude_e7,
                                                             point->latitude_e7,
                                                             point->longitude_e7),
                                     distance,
                                     sizeof(distance));
        snprintf(away,
                 sizeof(away),
                 " %s %03u",
                 distance,
                 (unsigned)solar_os_map_bearing_deg(self->latitude_e7,
                                                    self->longitude_e7,
                                                    point->latitude_e7,
                                                    point->longitude_e7));
    }
    snprintf(line,
             sizeof(line),
             "%c %s [%s] %s%s %s",
             map_app_glyph(point),
             point->label,
             point->source,
             coord,
             away,
             age);
    solar_os_tui_write_cell(&map_app.tui, row, 0U, cols, line, SOLAR_OS_TUI_ATTR_INVERSE);
}

static void map_app_render(void)
{
    const size_t cols = solar_os_tui_cols(&map_app.tui);
    const size_t map_rows = map_app_map_rows();
    char line[SOLAR_OS_TERMINAL_MAX_COLS + 1U];
    char distance[16];

    solar_os_map_format_distance(solar_os_map_scale_meters_per_col(map_app.scale),
                                 distance,
                                 sizeof(distance));
    snprintf(line,
             sizeof(line),
             "Map %u points  %s/col%s",
             (unsigned)map_app.total,
             distance,
             map_app.follow_self ? "  following" : "");
    solar_os_tui_draw_title(&map_app.tui, line, NULL);

    for (size_t row = 0; row < map_rows; row++) {
        solar_os_tui_write_cell(&map_app.tui, row + 1U, 0U, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (map_rows > 0U && cols > 0U) {
        const solar_os_map_view_t view = {
            .center_lat_e7 = map_app.center_lat_e7,
            .center_lon_e7 = map_app.center_lon_e7,
            .meters_per_col = solar_os_map_scale_meters_per_col(map_app.scale),
            .cols = cols,
            .rows = map_rows,
        };
        solar_os_tui_putch(&map_app.tui,
                           1U + view.rows / 2U,
                           view.cols / 2U,
                           '.',
                           SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_putch(&map_app.tui, 1U, cols - 1U, 'N', SOLAR_OS_TUI_ATTR_BOLD);

        const solar_os_map_point_t *selected = map_app_selected();
        /* Oldest first so the newest position wins a shared cell. */
        for (size_t i = map_app.count; i-- > 0;) {
            const solar_os_map_point_t *point = &map_app.points[i];
            size_t col = 0;
            size_t row = 0;
            if (!solar_os_map_project(&view,
                                      point->latitude_e7,
                                      point->longitude_e7,
                                      &col,
                                      &row)) {
                continue;
            }
            const bool chosen = selected != NULL && selected->id == point->id;
            solar_os_tui_putch(&map_app.tui,
                               1U + row,
                               col,
                               (uint32_t)(unsigned char)map_app_glyph(point),
                               chosen ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_BOLD);
        }
        if (selected != NULL) {
            size_t col = 0;
            size_t row = 0;
            if (solar_os_map_project(&view,
                                     selected->latitude_e7,
                                     selected->longitude_e7,
                                     &col,
                                     &row) &&
                col + 2U < cols) {
                char label[MAP_APP_LABEL_COLS + 1U];
                strlcpy(label, selected->label, sizeof(label));
                if (col + 2U + strlen(label) > cols) {
                    label[cols - col - 2U] = '\0';
                }
                solar_os_tui_addstr(&map_app.tui, 1U + row, col + 2U, label, SOLAR_OS_TUI_ATTR_NORMAL);
            }
        }
        map_app_draw_scale_bar(map_rows, cols);
    }

    map_app_render_info(1U + map_rows, cols);
    solar_os_tui_draw_footer(&map_app.tui,
                             map_app.feedback,
                             "arrows pan  +/- zoom  Tab select  c centre  f fit  d delete  q quit");
    map_app.feedback[0] = '\0';
}

static void map_app_select_next(bool forward)
{
    if (map_app.count == 0) {
        map_app.selected_id = 0;
        return;
    }
    size_t index = map_app.count;
    for (size_t i = 0; i < map_app.count; i++) {
        if (map_app.points[i].id == map_app.selected_id) {
            index = i;
            break;
        }
    }
    if (index == map_app.count) {
        index = forward ? 0U : map_app.count - 1U;
    } else if (forward) {
        index = (index + 1U) % map_app.count;
    } else {
        index = (index + map_app.count - 1U) % map_app.count;
    }
    map_app.selected_id = map_app.points[index].id;
}

static void map_app_pan(int columns, int rows)
{
    const int64_t east = (int64_t)columns * solar_os_map_scale_meters_per_col(map_app.scale) *
                         (int64_t)(solar_os_tui_cols(&map_app.tui) / 4U);
    const int64_t north = (int64_t)rows * solar_os_map_scale_meters_per_col(map_app.scale) * 2 *
                          (int64_t)(map_app_map_rows() / 4U);
    solar_os_map_offset(map_app.center_lat_e7,
                        map_app.center_lon_e7,
                        east,
                        north,
                        &map_app.center_lat_e7,
                        &map_app.center_lon_e7);
    map_app.follow_self = false;
    map_app.centered = true;
}

static void map_app_zoom(int direction)
{
    const size_t count = solar_os_map_scale_count();
    if (direction > 0 && map_app.scale + 1U < count) {
        map_app.scale++;
    } else if (direction < 0 && map_app.scale > 0U) {
        map_app.scale--;
    }
}

static void map_app_center_selected(void)
{
    const solar_os_map_point_t *point = map_app_selected();
    if (point == NULL) {
        point = map_app_self();
    }
    if (point == NULL) {
        strlcpy(map_app.feedback, "nothing to centre on", sizeof(map_app.feedback));
        return;
    }
    map_app_center_on(point);
    map_app.follow_self = point->kind == SOLAR_OS_MAP_KIND_SELF;
}

static void map_app_delete_selected(void)
{
    const solar_os_map_point_t *point = map_app_selected();
    if (point == NULL) {
        strlcpy(map_app.feedback, "no point selected", sizeof(map_app.feedback));
        return;
    }
    if (solar_os_map_remove(point->id) == ESP_OK) {
        strlcpy(map_app.feedback, "point removed until it is next heard", sizeof(map_app.feedback));
    }
    map_app.selected_id = 0;
    map_app_refresh();
}

static esp_err_t map_app_start(solar_os_context_t *ctx)
{
    memset(&map_app, 0, sizeof(map_app));
    esp_err_t err = solar_os_map_init();
    if (err != ESP_OK) {
        return err;
    }
    map_app.points = solar_os_memory_calloc(SOLAR_OS_MAP_CAPACITY,
                                            sizeof(*map_app.points),
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "app.map");
    if (map_app.points == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = solar_os_tui_screen_begin(&map_app.tui, ctx);
    if (err != ESP_OK) {
        solar_os_memory_free(map_app.points);
        memset(&map_app, 0, sizeof(map_app));
        return err;
    }
    map_app.scale = solar_os_map_scale_index(MAP_APP_DEFAULT_SCALE_M);
    map_app_poll_self();
    map_app_refresh();
    map_app_render();
    return ESP_OK;
}

static void map_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&map_app.tui, true);
    solar_os_tui_refresh(&map_app.tui);
    solar_os_tui_end(&map_app.tui);
    solar_os_memory_free(map_app.points);
    memset(&map_app, 0, sizeof(map_app));
}

static void map_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    map_app_refresh();
    map_app_render();
}

static void map_app_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "map", buffer_len);
    }
}

static bool map_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        bool redraw = false;
        if (map_app_now_ms() - map_app.last_self_poll_ms >= MAP_APP_SELF_POLL_MS) {
            map_app_poll_self();
        }
        solar_os_map_status_t status;
        if (solar_os_map_get_status(&status) == ESP_OK &&
            status.generation != map_app.generation) {
            map_app_refresh();
            redraw = true;
        }
        if (redraw) {
            map_app_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        map_app_pan(0, -1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        map_app_pan(0, 1);
        break;
    case SOLAR_OS_KEY_LEFT:
    case 'h':
        map_app_pan(-1, 0);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case 'l':
        map_app_pan(1, 0);
        break;
    case '+':
    case '=':
        map_app_zoom(-1);
        break;
    case '-':
    case '_':
        map_app_zoom(1);
        break;
    case '\t':
    case 'n':
        map_app_select_next(true);
        break;
    case 'p':
        map_app_select_next(false);
        break;
    case 'c':
    case 'C':
        map_app_center_selected();
        break;
    case 'f':
    case 'F':
        map_app_fit();
        break;
    case 'd':
    case 'D':
        map_app_delete_selected();
        break;
    case 'r':
    case 'R':
        map_app_poll_self();
        map_app_refresh();
        break;
    default:
        return true;
    }

    map_app_render();
    return true;
}

const solar_os_app_t solar_os_map_app = {
    .name = "map",
    .summary = "plot positions from GNSS, radios and networks",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = map_app_start,
    .resume = map_app_resume,
    .stop = map_app_stop,
    .event = map_app_event,
    .title = map_app_title,
    .state_slot = &map_app_state,
    .state_size = sizeof(map_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};

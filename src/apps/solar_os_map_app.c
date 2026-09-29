#include "solar_os_map_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_map.h"
#include "solar_os_map_layers.h"
#include "solar_os_memory.h"

#define MAP_APP_SELF_POLL_MS 5000U
/* Panning moves this fraction of the view, small enough that the shapes
 * stay readable from one step to the next. */
#define MAP_APP_PAN_DIVISOR 8
/* The system status bar owns the top of a graphical session and draws over
 * whatever is under it, so the map starts below it and claims no title row
 * of its own. */
#define MAP_APP_HEADER_H 14
#define MAP_APP_INFO_H 12

typedef struct {
    solar_os_map_point_t *points;
    solar_os_gfx_point_t *scratch;
    size_t scratch_max;
    size_t count;
    size_t total;
    uint32_t generation;
    uint32_t selected_id;
    uint32_t last_self_poll_ms;
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    uint32_t meters_per_px;
    bool follow_self;
    bool centered;
    char feedback[48];
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

static int map_app_area_height(const solar_os_gfx_t *gfx)
{
    const int height = (int)solar_os_gfx_height(gfx) - MAP_APP_HEADER_H -
                       MAP_APP_INFO_H;
    return height > 0 ? height : 0;
}

static solar_os_map_view_t map_app_view(const solar_os_gfx_t *gfx)
{
    const solar_os_map_view_t view = {
        .center_lat_e7 = map_app.center_lat_e7,
        .center_lon_e7 = map_app.center_lon_e7,
        .meters_per_col = map_app.meters_per_px,
        .cols = solar_os_gfx_width(gfx),
        .rows = (size_t)map_app_area_height(gfx),
    };
    return view;
}

static void map_app_fit(const solar_os_gfx_t *gfx)
{
    if (gfx == NULL || map_app.count == 0U) {
        return;
    }
    int32_t lat[SOLAR_OS_MAP_CAPACITY];
    int32_t lon[SOLAR_OS_MAP_CAPACITY];
    for (size_t i = 0; i < map_app.count; i++) {
        lat[i] = map_app.points[i].latitude_e7;
        lon[i] = map_app.points[i].longitude_e7;
    }
    map_app.meters_per_px = solar_os_map_scale_meters_per_col(
        solar_os_map_fit_scale(lat,
                               lon,
                               map_app.count,
                               map_app.center_lat_e7,
                               map_app.center_lon_e7,
                               solar_os_gfx_width(gfx),
                               (size_t)map_app_area_height(gfx)));
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

/*
 * Layers can be loaded while the app is open, so the scratch buffer is
 * sized against whatever is loaded now rather than against what was loaded
 * when the app started. Without this a layer added later would have its
 * longest rings silently dropped.
 */
static void map_app_size_scratch(void)
{
    const size_t longest = solar_os_map_layer_longest_ring();
    if (longest == 0U || longest <= map_app.scratch_max) {
        return;
    }
    solar_os_gfx_point_t *grown =
        solar_os_memory_calloc(longest,
                               sizeof(*grown),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "app.map.rings");
    if (grown == NULL) {
        return;
    }
    solar_os_memory_free(map_app.scratch);
    map_app.scratch = grown;
    map_app.scratch_max = longest;
}

/* Geometry is drawn from rings of coordinates, so it needs no tiles. */
static void map_app_draw_geometry(solar_os_gfx_t *gfx,
                                  const solar_os_map_view_t *view,
                                  const solar_os_map_geometry_t *geometry)
{
    if (geometry == NULL || map_app.scratch == NULL) {
        return;
    }
    solar_os_map_ring_cursor_t cursor = {0};
    solar_os_map_ring_t ring;
    while (solar_os_map_geometry_next(geometry, &cursor, &ring)) {
        if (ring.point_count > map_app.scratch_max) {
            continue;
        }
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;
        for (size_t point = 0U; point < ring.point_count; point++) {
            int x = 0;
            int y = 0;
            solar_os_map_project_raw(view,
                                     ring.coordinates[point * 2U],
                                     ring.coordinates[point * 2U + 1U],
                                     &x,
                                     &y);
            map_app.scratch[point].x = x;
            map_app.scratch[point].y = y + MAP_APP_HEADER_H;
            if (point == 0U) {
                left = right = x;
                top = bottom = y;
            } else {
                if (x < left) {
                    left = x;
                }
                if (x > right) {
                    right = x;
                }
                if (y < top) {
                    top = y;
                }
                if (y > bottom) {
                    bottom = y;
                }
            }
        }
        if (right < 0 || bottom < 0 || left >= (int)view->cols ||
            top >= (int)view->rows) {
            continue;
        }
        /*
         * Coastlines are drawn as outlines rather than filled areas: a ring
         * runs to hundreds of vertices, and a scanline fill of one costs
         * more per frame than the whole map is worth on a small display.
         *
         * A segment wider than the view is one whose ends were clamped, or
         * one crossing the meridian opposite the view centre, where
         * longitude wraps. Either way it would draw a line straight across
         * the map, so it is left out.
         */
        const int span = (int)view->cols;
        const size_t segments =
            ring.open ? ring.point_count - 1U : ring.point_count;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        for (size_t point = 0U; point < segments; point++) {
            const size_t next = (point + 1U) % ring.point_count;
            const int64_t d_lon =
                (int64_t)solar_os_map_relative_lon(
                    view, ring.coordinates[next * 2U + 1U]) -
                (int64_t)solar_os_map_relative_lon(
                    view, ring.coordinates[point * 2U + 1U]);
            if (d_lon > SOLAR_OS_MAP_LON_MAX_E7 ||
                d_lon < -SOLAR_OS_MAP_LON_MAX_E7) {
                continue;
            }
            const solar_os_gfx_point_t *a = &map_app.scratch[point];
            const solar_os_gfx_point_t *b = &map_app.scratch[next];
            if (b->x - a->x > span || a->x - b->x > span) {
                continue;
            }
            solar_os_gfx_line(gfx, a->x, a->y, b->x, b->y);
        }
    }
}

static void map_app_draw_layers(solar_os_gfx_t *gfx,
                                const solar_os_map_view_t *view)
{
    map_app_size_scratch();
    for (size_t index = 0U; index < solar_os_map_layer_count(); index++) {
        map_app_draw_geometry(gfx, view, solar_os_map_layer(index));
    }
}

static void map_app_draw_point(solar_os_gfx_t *gfx,
                               const solar_os_map_point_t *point,
                               int x,
                               int y,
                               bool chosen)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_circle(gfx, x, y, 4);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    switch (point->kind) {
    case SOLAR_OS_MAP_KIND_SELF:
        solar_os_gfx_fill_circle(gfx, x, y, 3);
        solar_os_gfx_circle(gfx, x, y, 5);
        break;
    case SOLAR_OS_MAP_KIND_WAYPOINT:
        solar_os_gfx_line(gfx, x - 3, y, x + 3, y);
        solar_os_gfx_line(gfx, x, y - 3, x, y + 3);
        break;
    default:
        solar_os_gfx_circle(gfx, x, y, 3);
        break;
    }
    if (chosen) {
        solar_os_gfx_rect(gfx, x - 6, y - 6, 13, 13);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, x + 8, y + 4, point->label);
    }
}

static void map_app_draw_scale_bar(solar_os_gfx_t *gfx, int bottom)
{
    const solar_os_map_view_t view = map_app_view(gfx);
    const uint32_t meters = solar_os_map_view_resolution(&view);
    const int bar = 50;
    char distance[16];
    solar_os_map_format_distance(meters * (uint32_t)bar, distance, sizeof(distance));
    const int y = bottom - 6;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 6, y, 6 + bar, y);
    solar_os_gfx_line(gfx, 6, y - 3, 6, y + 3);
    solar_os_gfx_line(gfx, 6 + bar, y - 3, 6 + bar, y + 3);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6 + bar + 5, y + 3, distance);
}

static void map_app_draw_info(solar_os_gfx_t *gfx, int top, int width)
{
    char line[96];
    const solar_os_map_point_t *point = map_app_selected();
    char distance[16];
    const solar_os_map_view_t scale_view = map_app_view(gfx);
    solar_os_map_format_distance(solar_os_map_view_resolution(&scale_view),
                                 distance,
                                 sizeof(distance));
    if (map_app.feedback[0] != '\0') {
        strlcpy(line, map_app.feedback, sizeof(line));
    } else if (point == NULL) {
        snprintf(line,
                 sizeof(line),
                 "%u points  %s/px%s",
                 (unsigned)map_app.total,
                 distance,
                 map_app.follow_self ? "  following" : "");
    } else {
        char coord[SOLAR_OS_MAP_COORD_TEXT_MAX];
        char age[8];
        char away[32] = "";
        char reach[16];
        solar_os_map_format_coord(point->latitude_e7, point->longitude_e7, coord);
        map_app_format_age(point->updated_ms, age, sizeof(age));
        const solar_os_map_point_t *self = map_app_self();
        if (self != NULL && self->id != point->id) {
            solar_os_map_format_distance(
                solar_os_map_distance_m(self->latitude_e7,
                                        self->longitude_e7,
                                        point->latitude_e7,
                                        point->longitude_e7),
                reach,
                sizeof(reach));
            snprintf(away,
                     sizeof(away),
                     " %s %03u",
                     reach,
                     (unsigned)solar_os_map_bearing_deg(self->latitude_e7,
                                                        self->longitude_e7,
                                                        point->latitude_e7,
                                                        point->longitude_e7));
        }
        snprintf(line, sizeof(line), "%s %s%s %s",
                 point->label, coord, away, age);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, top, width, MAP_APP_INFO_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 3, top + MAP_APP_INFO_H - 3, line);
}

static void map_app_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int area = map_app_area_height(gfx);
    const solar_os_map_view_t view = map_app_view(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    map_app_draw_layers(gfx, &view);

    const solar_os_map_point_t *selected = map_app_selected();
    /* Oldest first so the newest position draws on top. */
    for (size_t i = map_app.count; i-- > 0;) {
        const solar_os_map_point_t *point = &map_app.points[i];
        size_t col = 0U;
        size_t row = 0U;
        if (!solar_os_map_project(&view,
                                  point->latitude_e7,
                                  point->longitude_e7,
                                  &col,
                                  &row)) {
            continue;
        }
        map_app_draw_point(gfx,
                           point,
                           (int)col,
                           (int)row + MAP_APP_HEADER_H,
                           selected != NULL && selected->id == point->id);
    }

    map_app_draw_scale_bar(gfx, MAP_APP_HEADER_H + area);
    map_app_draw_info(gfx, height - MAP_APP_INFO_H, width);
    map_app.feedback[0] = '\0';
    solar_os_gfx_present(gfx);
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

static void map_app_pan(solar_os_context_t *ctx, int columns, int rows)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const solar_os_map_view_t view = map_app_view(gfx);
    solar_os_map_pan(&view,
                     columns * (int)(view.cols / MAP_APP_PAN_DIVISOR),
                     -rows * (int)(view.rows / MAP_APP_PAN_DIVISOR),
                     &map_app.center_lat_e7,
                     &map_app.center_lon_e7);
    map_app.follow_self = false;
    map_app.centered = true;
}

static void map_app_zoom(int direction)
{
    map_app.meters_per_px =
        solar_os_map_scale_step(map_app.meters_per_px, direction);
}

/* The whole world, centred, filling as much of the screen as it fits. */
static void map_app_world_view(const solar_os_gfx_t *gfx)
{
    map_app.center_lat_e7 = 0;
    map_app.center_lon_e7 = 0;
    map_app.meters_per_px = solar_os_map_world_scale(
        gfx != NULL ? solar_os_gfx_width(gfx) : 0U,
        gfx != NULL ? (size_t)map_app_area_height(gfx) : 0U);
    map_app.centered = false;
    map_app.follow_self = false;
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
        strlcpy(map_app.feedback, "point removed", sizeof(map_app.feedback));
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
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    map_app.points = solar_os_memory_calloc(SOLAR_OS_MAP_CAPACITY,
                                            sizeof(*map_app.points),
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "app.map");
    if (map_app.points == NULL) {
        return ESP_ERR_NO_MEM;
    }
    map_app_size_scratch();
    map_app_world_view(solar_os_context_gfx(ctx));
    map_app_poll_self();
    map_app_refresh();
    map_app_render(ctx);
    return ESP_OK;
}

static void map_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_memory_free(map_app.points);
    solar_os_memory_free(map_app.scratch);
    memset(&map_app, 0, sizeof(map_app));
}

static void map_app_resume(solar_os_context_t *ctx)
{
    /* A resume onto state that was never started has no scale yet. */
    if (map_app.meters_per_px == 0U) {
        map_app_world_view(solar_os_context_gfx(ctx));
    }
    map_app_refresh();
    map_app_render(ctx);
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
        if (map_app_now_ms() - map_app.last_self_poll_ms >= MAP_APP_SELF_POLL_MS) {
            map_app_poll_self();
        }
        solar_os_map_status_t status;
        if (solar_os_map_get_status(&status) == ESP_OK &&
            status.generation != map_app.generation) {
            map_app_refresh();
        }
        /*
         * Redrawn every tick, not only when a point moves. Anything else
         * that paints the display leaves the map holding pixels it no
         * longer owns, and there is no event that says so, so the only way
         * to stay on screen is to keep putting it back.
         */
        map_app_render(ctx);
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
        map_app_pan(ctx, 0, 1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        map_app_pan(ctx, 0, -1);
        break;
    case SOLAR_OS_KEY_LEFT:
    case 'h':
        map_app_pan(ctx, -1, 0);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case 'l':
        map_app_pan(ctx, 1, 0);
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
        map_app_fit(solar_os_context_gfx(ctx));
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

    map_app_render(ctx);
    return true;
}

const solar_os_app_t solar_os_map_app = {
    .name = "map",
    .summary = "plot positions from GNSS, radios and networks",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = map_app_start,
    .resume = map_app_resume,
    .stop = map_app_stop,
    .event = map_app_event,
    .title = map_app_title,
    .state_slot = &map_app_state,
    .state_size = sizeof(map_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 1000U,
    .tick_deadline_ms = 150U,
};

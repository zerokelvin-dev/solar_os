#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Fetches the map for one cell from OpenStreetMap, through the Overpass
 * API, and keeps it where a layer is kept.
 *
 * Overpass is a free service run by volunteers with a stated limit of
 * around ten thousand requests and a gigabyte a day, which asks callers not
 * to stitch boxes together to scrape wide areas. One cell is asked for at a
 * time, only when somebody asks for it, and an answer that comes back too
 * large is refused rather than stored.
 */
#define SOLAR_OS_MAP_FETCH_MAX_BYTES (8U * 1024U * 1024U)

/*
 * A cell is fetched a tier at a time, coarsest first, each its own request
 * and its own file: the base is water, rivers, rail and the roads down to
 * secondary; then the tertiary roads; then the streets. A tier is only
 * worth asking for at a scale where it is drawn, so nothing is fetched
 * that the reader could not see.
 */
typedef enum {
    SOLAR_OS_MAP_TIER_BASE = 0,
    SOLAR_OS_MAP_TIER_MINOR,
    SOLAR_OS_MAP_TIER_STREETS,
    SOLAR_OS_MAP_TIER_COUNT,
} solar_os_map_tier_t;

/* The coarsest scale, in metres per pixel, at which a tier is drawn. */
uint32_t solar_os_map_tier_max_mpp(solar_os_map_tier_t tier);

/* How large a tier's cells are, in 1e7 degrees. */
int32_t solar_os_map_tier_cell_e7(solar_os_map_tier_t tier);

/* The layer name a tier of a cell is kept under, without `.bin`. */
void solar_os_map_tier_name(int32_t latitude_e7,
                            int32_t longitude_e7,
                            solar_os_map_tier_t tier,
                            char *out,
                            size_t out_len);

/* Whether a fetch is running, so a second press does not start another. */
bool solar_os_map_fetch_busy(void);

/*
 * Asks for one tier of the cell holding a position and writes it to the
 * card.
 *
 * It returns as soon as the asking has started, because the answer takes
 * tens of seconds and the caller is a key press: waiting for it there stops
 * the application redrawing, reporting progress, or being left. Watch
 * solar_os_map_fetch_busy for the end and solar_os_map_fetch_status for
 * what happened.
 */
esp_err_t solar_os_map_fetch_cell(int32_t latitude_e7,
                                  int32_t longitude_e7,
                                  solar_os_map_tier_t tier);

/* Counts up each time a fetch finishes, so a caller can notice one did. */
uint32_t solar_os_map_fetch_generation(void);

/* What the last fetch did, for a reader to be told. */
const char *solar_os_map_fetch_status(void);

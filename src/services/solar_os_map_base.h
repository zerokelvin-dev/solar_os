#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The world a map starts with: a coastline, its lakes, the borders between
 * countries and the borders within them. Four layers rather than one,
 * because a layer is the unit the map loads and orders, and because a
 * reader who wants a different world replaces one file instead of all of
 * them.
 *
 * Nothing is compiled into the firmware and nothing ships on the card. The
 * layers are fetched once from Natural Earth, which is public domain and
 * asks for no key and no account, and packed on the way in. A world map
 * that arrives this way costs about a quarter of a megabyte over the wire
 * and is then free for ever, which is a better trade than carrying it in
 * every build for the readers who never open the map.
 */

/* Fetches whatever is missing, on a task of its own. Returns at once. */
esp_err_t solar_os_map_base_fetch(void);
/* True while a fetch is running; one runs at a time. */
bool solar_os_map_base_busy(void);
/* The last thing that happened, for a shell or a status line to print. */
const char *solar_os_map_base_status(void);
/* Bumped when a fetch finishes, so a drawing app knows to look again. */
uint32_t solar_os_map_base_generation(void);
/* How many of the four are already on the card. */
size_t solar_os_map_base_kept(void);
/* Loads whichever of the four are kept, in drawing order. */
esp_err_t solar_os_map_base_load(void);

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "solar_os_inflate.h"
#include "solar_os_map_layers.h"

/*
 * Reads the JSON an Overpass query answers with into the packed layer
 * format, scanned twice by a character state machine rather than parsed
 * into a document, so an answer of megabytes costs only its geometry.
 *
 * Each element's own tags decide its class, so one answer can carry water,
 * railways and roads and have each drawn as itself. The tags arrive after
 * the geometry, so the class is settled when the element closes and applied
 * to every ring it produced. klass is the fallback, for an element whose
 * tags say nothing this reader knows.
 *
 * Returns ESP_ERR_TIMEOUT when the answer carried a remark and no geometry,
 * which is how Overpass reports a query that gave up inside a 200, and
 * ESP_ERR_NOT_FOUND when it simply held nothing.
 */
esp_err_t solar_os_map_osm_read(solar_os_inflate_t *reader,
                                solar_os_map_class_t klass,
                                uint8_t **out,
                                size_t *out_size);

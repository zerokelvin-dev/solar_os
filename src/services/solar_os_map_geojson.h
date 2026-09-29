#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

/*
 * Reads GeoJSON into the packed layer format. The file is scanned twice by
 * a character state machine rather than parsed into a document, so a large
 * file costs only its geometry in memory.
 */
esp_err_t solar_os_map_geojson_read(FILE *file, uint8_t **out, size_t *out_size);

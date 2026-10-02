#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "solar_os_inflate.h"
#include "solar_os_map_layers.h"

/*
 * Reads GeoJSON into the packed layer format. The input is scanned twice by
 * a character state machine rather than parsed into a document, so a large
 * file costs only its geometry in memory. It arrives through a reader, which
 * is what lets the same code read a file that was kept deflated.
 */
esp_err_t solar_os_map_geojson_read(solar_os_inflate_t *reader,
                                    solar_os_map_class_t klass,
                                    uint8_t **out,
                                    size_t *out_size);

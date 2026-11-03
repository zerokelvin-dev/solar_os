#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

/*
 * Reads a file that may or may not be deflated. A gzip member is inflated as
 * it is read; anything else is handed back byte for byte, so one caller reads
 * both without asking which it was given.
 *
 * A reader can be wound back to the start. A caller that must scan its input
 * twice therefore pays to inflate twice rather than making the card hold a
 * whole plain copy, which is the cheaper of the two by a wide margin.
 */
/*
 * The most one reader will produce before it calls the stream a lie. Deflate
 * expands by up to a thousandfold, so a small file can ask for gigabytes and
 * a reader that scans twice would ask for them twice. Generous for any map a
 * cell can hold, and far below what would take all day to inflate.
 */
#define SOLAR_OS_INFLATE_MAX_OUT (32U * 1024U * 1024U)

typedef struct solar_os_inflate solar_os_inflate_t;

esp_err_t solar_os_inflate_open(FILE *file, solar_os_inflate_t **out_reader);
size_t solar_os_inflate_read(solar_os_inflate_t *reader,
                             void *out,
                             size_t size);
esp_err_t solar_os_inflate_rewind(solar_os_inflate_t *reader);
/* True once the stream ended early or held something deflate cannot read. */
bool solar_os_inflate_failed(const solar_os_inflate_t *reader);
void solar_os_inflate_close(solar_os_inflate_t *reader);

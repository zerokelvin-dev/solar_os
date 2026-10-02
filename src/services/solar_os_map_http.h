#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "solar_os_http_client.h"
#include "solar_os_inflate.h"
#include "solar_os_map_layers.h"

/*
 * Getting a map from a server and keeping it. Every step of that is the
 * same whichever server it is and whatever format the answer is in: ask,
 * write the answer to the card as it arrives rather than holding megabytes
 * of it, read it back through a reader so a deflated answer needs no
 * saying, pack it, keep it, and throw the answer away.
 *
 * What differs is what to ask for, how to read the reply and what to call
 * the result, so those are the caller's. This is mechanism; the caller
 * keeps the policy, including every word said to a reader about failure.
 * Written twice, one copy grew a fix the other did not.
 */

/* An answer being written to the card as it arrives. */
typedef struct {
    FILE *file;
    size_t written;
    size_t limit;
    bool too_large;
    int status;
} solar_os_map_answer_t;

/* Pass as the request's event_handler with an answer as its user_data. */
esp_err_t solar_os_map_answer_event(const solar_os_http_event_t *event,
                                    void *user);

/* Turns an answer into packed geometry: the OSM or the GeoJSON reader. */
typedef esp_err_t (*solar_os_map_pack_fn)(solar_os_inflate_t *reader,
                                          solar_os_map_class_t klass,
                                          uint8_t **out,
                                          size_t *out_size);

typedef struct {
    /* What the kept map is called, and what the progress line says. */
    const char *name;
    const char *url;
    solar_os_http_method_t method;
    const void *body;
    size_t body_len;
    /* Wire bytes past which the answer is refused rather than stored. */
    size_t limit;
    uint32_t timeout_ms;
    /* Whether the server may be asked to follow a redirect. */
    bool follow_redirects;
    /* The class a packer falls back to for anything it cannot name. */
    solar_os_map_class_t klass;
    solar_os_map_pack_fn pack;
    /* Where to write which phase the work has reached, if anywhere. */
    char *progress;
    size_t progress_capacity;
} solar_os_map_download_t;

/*
 * Asks, packs and keeps. Returns ESP_OK, or ESP_ERR_TIMEOUT when the
 * server said to come back later, ESP_ERR_NOT_FOUND when the answer held
 * no geometry, ESP_ERR_INVALID_SIZE when it was larger than the limit, and
 * ESP_FAIL otherwise. The status and the byte count go to the log, because
 * a number is no use to somebody holding the device.
 */
esp_err_t solar_os_map_download(const solar_os_map_download_t *what);

#include "solar_os_map_http.h"

#include <string.h>

#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

static const char *TAG = "map_http";

esp_err_t solar_os_map_answer_event(const solar_os_http_event_t *event,
                                    void *user)
{
    solar_os_map_answer_t *answer = user;
    if (event == NULL || answer == NULL) {
        return ESP_OK;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_RESPONSE) {
        answer->status = event->status_code;
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA || event->data == NULL) {
        return ESP_OK;
    }
    if (answer->limit != 0U &&
        answer->written + event->data_len > answer->limit) {
        answer->too_large = true;
        return ESP_FAIL;
    }
    if (fwrite(event->data, 1U, event->data_len, answer->file) !=
        event->data_len) {
        return ESP_FAIL;
    }
    answer->written += event->data_len;
    return ESP_OK;
}

static void say(const solar_os_map_download_t *what, const char *doing)
{
    if (what->progress != NULL && what->progress_capacity > 0U) {
        (void)snprintf(what->progress, what->progress_capacity, "%s %s",
                       doing, what->name);
    }
}

/* Every answer lands here on its way past, whatever asked for it. */
static esp_err_t answer_path(char *path, size_t capacity)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_storage_default_path(SOLAR_OS_MAP_DIR "/answer.body",
                                         path, capacity);
}

esp_err_t solar_os_map_download(const solar_os_map_download_t *what)
{
    if (what == NULL || what->name == NULL || what->url == NULL ||
        what->pack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char temp[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = answer_path(temp, sizeof(temp));
    if (error != ESP_OK) {
        return error;
    }
    solar_os_map_answer_t answer = {.file = fopen(temp, "w+b"),
                                    .limit = what->limit};
    if (answer.file == NULL) {
        return ESP_FAIL;
    }

    /*
     * Deflated, because it is several times smaller on the wire and that
     * is the difference between a download that finishes inside its
     * deadline on a slow link and one that does not.
     */
    static const solar_os_http_header_t headers[] = {
        {.name = "Accept-Encoding", .value = "gzip"},
    };
    const solar_os_http_request_options_t options = {
        .url = what->url,
        .method = what->method,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .body = what->body,
        .body_len = what->body_len,
        .follow_redirects = what->follow_redirects,
        .timeout_ms = what->timeout_ms,
        .deadline_ms = what->timeout_ms,
        .event_handler = solar_os_map_answer_event,
        .user_data = &answer,
    };
    say(what, "asking for");
    solar_os_http_request_t *request = NULL;
    solar_os_http_response_t response = {0};
    error = solar_os_http_request_create(&options, &request);
    if (error == ESP_OK) {
        error = solar_os_http_request_perform(request, &response);
        if (answer.status == 0) {
            answer.status = response.status_code;
        }
        (void)solar_os_http_request_destroy(request);
    }
    SOLAR_OS_LOGI(TAG, "%s: http %d, %u bytes, %s", what->name, answer.status,
                  (unsigned)answer.written, esp_err_to_name(error));

    if (answer.too_large) {
        error = ESP_ERR_INVALID_SIZE;
    } else if (error == ESP_OK &&
               (answer.status >= 400 || answer.written == 0U)) {
        /* 429 and anything from the server's own side mean come back. */
        error = (answer.status == 429 || answer.status >= 500)
                    ? ESP_ERR_TIMEOUT
                    : ESP_FAIL;
    }

    uint8_t *packed = NULL;
    size_t packed_size = 0U;
    if (error == ESP_OK) {
        say(what, "packing");
        solar_os_inflate_t *reader = NULL;
        error = solar_os_inflate_open(answer.file, &reader);
        if (error == ESP_OK) {
            error = what->pack(reader, what->klass, &packed, &packed_size);
            solar_os_inflate_close(reader);
        }
        SOLAR_OS_LOGI(TAG, "%s: %u bytes read into %u packed, %s", what->name,
                      (unsigned)answer.written, (unsigned)packed_size,
                      esp_err_to_name(error));
    }
    (void)fclose(answer.file);
    (void)solar_os_storage_remove(temp);
    if (error != ESP_OK) {
        return error;
    }

    error = solar_os_map_layer_keep(what->name, packed, packed_size);
    solar_os_memory_free(packed);
    if (error != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "could not keep %s: %s", what->name,
                      esp_err_to_name(error));
    }
    return error;
}

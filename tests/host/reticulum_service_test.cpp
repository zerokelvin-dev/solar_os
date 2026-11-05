/*
 * The Reticulum service and job on the real microReticulum, with a scripted
 * radio: starts that fail part way and are retried, restarts, and a stop
 * while the worker is inside a slow radio call.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <microReticulum.h>

extern "C" {
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reticulum_stubs/host_platform.h"
#include "solar_os_credentials.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_radio.h"
#include "solar_os_reticulum.h"
#include "solar_os_reticulum_job.h"
#include "solar_os_reticulum_lora_frame.h"
#include "solar_os_reticulum_random.h"
#include "solar_os_time.h"
}

namespace {

/* ---- Platform ---------------------------------------------------------- */

void random_bytes(void *out, size_t length)
{
    static int fd = -1;
    if (fd < 0) {
        fd = open("/dev/urandom", O_RDONLY);
        assert(fd >= 0);
    }
    uint8_t *bytes = static_cast<uint8_t *>(out);
    while (length > 0U) {
        const ssize_t got = read(fd, bytes, length);
        assert(got > 0);
        bytes += got;
        length -= static_cast<size_t>(got);
    }
}

/* ---- The radio --------------------------------------------------------- */

struct Radio {
    esp_err_t claim_result = ESP_OK;
    esp_err_t profile_result = ESP_OK;
    esp_err_t listen_result = ESP_OK;
    bool claimed = false;
    unsigned claims = 0;
    unsigned releases = 0;
    solar_os_radio_state_t state = SOLAR_OS_RADIO_STATE_SLEEP;
    /* While set, a receive waits for it to clear, as a radio call that
     * takes seconds holds the worker inside its pass. */
    std::atomic<int> block_receive{0};
    std::atomic<int> receive_blocked{0};
} radio;

solar_os_radio_config_t lora_config()
{
    solar_os_radio_config_t config{};
    config.frequency_hz = 914875000U;
    config.modulation = SOLAR_OS_RADIO_MODULATION_LORA;
    config.rx_bandwidth_hz = 125000U;
    config.spreading_factor = 10U;
    config.coding_rate_denominator = 8U;
    config.preamble_len = 18U;
    config.tx_power_dbm = 14;
    config.crc_enabled = true;
    config.variable_length = true;
    return config;
}

bool valid(const solar_os_radio_handle_t *handle)
{
    return handle != nullptr && handle->index == 0 && radio.claimed;
}

/* ---- Credentials and Messaging ----------------------------------------- */

std::map<std::string, std::vector<uint8_t>> secrets;
solar_os_messaging_provider_id_t created_instance = 0;

std::string secret_key(solar_os_messaging_provider_id_t provider,
                       solar_os_credential_kind_t kind, const char *label)
{
    return std::to_string(provider) + "/" + std::to_string(kind) + "/" + label;
}

}  // namespace

extern "C" {

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

uint32_t esp_random(void)
{
    uint32_t value;
    random_bytes(&value, sizeof(value));
    return value;
}

void esp_fill_random(void *buffer, size_t length)
{
    random_bytes(buffer, length);
}

esp_err_t solar_os_reticulum_random(uint8_t *out, size_t len)
{
    random_bytes(out, len);
    return ESP_OK;
}

esp_err_t solar_os_reticulum_random_reseed(void)
{
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "error";
    }
}


esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag,
                             const char *fmt, ...)
{
    if (getenv("RETICULUM_TEST_LOG") != nullptr) {
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[%d %s] ", static_cast<int>(level), tag);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
    return ESP_OK;
}

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t memory_class,
                            const char *tag)
{
    (void)memory_class;
    (void)tag;
    return malloc(size);
}

void solar_os_memory_free(void *ptr)
{
    free(ptr);
}

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    *epoch_ms = static_cast<uint64_t>(time(nullptr)) * 1000U;
    return ESP_OK;
}

esp_err_t solar_os_jobs_note_resource(const char *name,
                                      solar_os_job_resource_type_t type,
                                      const char *resource, const char *detail)
{
    (void)name;
    (void)type;
    (void)resource;
    (void)detail;
    return ESP_OK;
}

esp_err_t solar_os_credentials_init(void)
{
    return ESP_OK;
}

const char *solar_os_credentials_location(void)
{
    return "/sdcard/.settings/credentials.bin";
}

void solar_os_credentials_wipe(void *buffer, size_t length)
{
    memset(buffer, 0, length);
}

esp_err_t solar_os_credentials_find(solar_os_messaging_provider_id_t provider,
                                    solar_os_credential_kind_t kind,
                                    const char *label,
                                    solar_os_credential_info_t *record)
{
    const std::string key = secret_key(provider, kind, label);
    if (secrets.find(key) == secrets.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(record, 0, sizeof(*record));
    record->id = 1U;
    record->provider = provider;
    record->kind = kind;
    strlcpy(record->label, label, sizeof(record->label));
    return ESP_OK;
}

esp_err_t solar_os_credentials_put(solar_os_messaging_provider_id_t provider,
                                   solar_os_credential_kind_t kind,
                                   const char *label, const void *secret,
                                   size_t secret_len, bool replace,
                                   solar_os_credential_id_t *record_id)
{
    const std::string key = secret_key(provider, kind, label);
    if (!replace && secrets.find(key) != secrets.end()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(secret);
    secrets[key] = std::vector<uint8_t>(bytes, bytes + secret_len);
    if (record_id != nullptr) {
        *record_id = 1U;
    }
    return ESP_OK;
}

/* One record: the node identity. */
esp_err_t solar_os_credentials_read_secret(solar_os_credential_id_t record_id,
                                           void *secret,
                                           size_t secret_capacity,
                                           size_t *secret_len)
{
    if (record_id != 1U || secrets.size() != 1U) {
        return ESP_ERR_NOT_FOUND;
    }
    const std::vector<uint8_t> &stored = secrets.begin()->second;
    if (stored.size() > secret_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(secret, stored.data(), stored.size());
    *secret_len = stored.size();
    return ESP_OK;
}

bool solar_os_messaging_provider_next(solar_os_messaging_provider_id_t *cursor,
                                      solar_os_messaging_provider_status_t *status)
{
    if (created_instance == 0 || *cursor >= created_instance) {
        return false;
    }
    *cursor = created_instance;
    memset(status, 0, sizeof(*status));
    status->id = created_instance;
    strlcpy(status->class_name, "reticulum", sizeof(status->class_name));
    return true;
}

esp_err_t solar_os_messaging_instance_create(const char *class_name,
                                             const char *name,
                                             const void *config, size_t size,
                                             solar_os_messaging_provider_id_t *id)
{
    (void)name;
    (void)config;
    (void)size;
    assert(strcmp(class_name, "reticulum") == 0 && created_instance == 0);
    created_instance = 7;
    *id = created_instance;
    return ESP_OK;
}

esp_err_t solar_os_messaging_provider_set_status(
    solar_os_messaging_provider_id_t provider, bool running, bool connected,
    esp_err_t error, const char *detail)
{
    (void)provider;
    (void)running;
    (void)connected;
    (void)error;
    (void)detail;
    return ESP_OK;
}

esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *info)
{
    if (strcmp(name, "radio0") != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(info, 0, sizeof(*info));
    strlcpy(info->name, name, sizeof(info->name));
    info->modulations = SOLAR_OS_RADIO_MODULATION_LORA;
    info->features = SOLAR_OS_RADIO_FEATURE_PACKET | SOLAR_OS_RADIO_FEATURE_CRC |
                     SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH;
    info->max_packet_len = 255U;
    info->claimed = radio.claimed || radio.claim_result != ESP_OK;
    if (radio.claim_result != ESP_OK) {
        strlcpy(info->owner, "meshtastic", sizeof(info->owner));
    }
    return ESP_OK;
}

esp_err_t solar_os_radio_claim(const char *name, const char *owner,
                               solar_os_radio_handle_t *handle)
{
    assert(strcmp(name, "radio0") == 0 && strcmp(owner, "reticulum") == 0);
    assert(!radio.claimed);
    if (radio.claim_result != ESP_OK) {
        return radio.claim_result;
    }
    radio.claimed = true;
    radio.claims++;
    handle->index = 0;
    handle->token = radio.claims;
    return ESP_OK;
}

esp_err_t solar_os_radio_release(solar_os_radio_handle_t *handle)
{
    assert(valid(handle));
    radio.claimed = false;
    radio.releases++;
    return ESP_OK;
}

bool solar_os_radio_handle_valid(const solar_os_radio_handle_t *handle)
{
    return valid(handle);
}

esp_err_t solar_os_radio_handle_get_status(const solar_os_radio_handle_t *handle,
                                           solar_os_radio_status_t *status)
{
    assert(valid(handle));
    memset(status, 0, sizeof(*status));
    status->state = radio.state;
    status->config = lora_config();
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_profile_apply(const solar_os_radio_handle_t *handle,
                                              const char *profile_name)
{
    assert(valid(handle) && profile_name != nullptr);
    return radio.profile_result;
}

esp_err_t solar_os_radio_handle_configure(const solar_os_radio_handle_t *handle,
                                          const solar_os_radio_config_t *config)
{
    assert(valid(handle) && config != nullptr);
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_set_state(const solar_os_radio_handle_t *handle,
                                          solar_os_radio_state_t state)
{
    assert(valid(handle));
    if (state == SOLAR_OS_RADIO_STATE_RX && radio.listen_result != ESP_OK) {
        return radio.listen_result;
    }
    radio.state = state;
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_send(const solar_os_radio_handle_t *handle,
                                     const solar_os_radio_packet_t *packet,
                                     uint32_t timeout_ms)
{
    assert(valid(handle) && packet != nullptr);
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t solar_os_radio_handle_receive(const solar_os_radio_handle_t *handle,
                                        solar_os_radio_packet_t *packet,
                                        uint32_t timeout_ms)
{
    assert(valid(handle) && packet != nullptr);
    (void)timeout_ms;
    while (radio.block_receive) {
        radio.receive_blocked = 1;
        vTaskDelay(1);
    }
    radio.receive_blocked = 0;
    return ESP_ERR_TIMEOUT;
}

}  // extern "C"

namespace {

const solar_os_reticulum_config_t kLora = {nullptr, 0U, "radio0", "rns-sf10-cr8"};

void reset_radio()
{
    radio.claim_result = ESP_OK;
    radio.profile_result = ESP_OK;
    radio.listen_result = ESP_OK;
    radio.block_receive = 0;
}

void assert_stopped()
{
    solar_os_reticulum_status_t status{};
    assert(solar_os_reticulum_get_status(&status) == ESP_OK);
    assert(!status.running);
    assert(!radio.claimed);
    assert(radio.claims == radio.releases);
}

void assert_started()
{
    solar_os_reticulum_status_t status{};
    assert(solar_os_reticulum_get_status(&status) == ESP_OK);
    assert(status.running && status.identity_set);
    assert(strlen(status.destination_hex) == SOLAR_OS_RETICULUM_HASH_HEX_LEN - 1U);
    assert(radio.claimed && radio.state == SOLAR_OS_RADIO_STATE_RX);
}

std::string start_error()
{
    char text[128];
    solar_os_reticulum_start_error(text, sizeof(text));
    return text;
}

/* Each way a LoRa start can fail after the node destination exists, then
 * the same start once the radio allows it. */
void test_failed_starts_are_retried()
{
    reset_radio();
    radio.claim_result = ESP_ERR_INVALID_STATE;
    assert(solar_os_reticulum_start(&kLora) == ESP_ERR_INVALID_STATE);
    assert(start_error() == "radio radio0 is in use by meshtastic");
    assert_stopped();

    reset_radio();
    radio.profile_result = ESP_ERR_NOT_FOUND;
    assert(solar_os_reticulum_start(&kLora) == ESP_ERR_NOT_FOUND);
    assert(start_error().find("radio profile rns-sf10-cr8") == 0U);
    assert_stopped();

    reset_radio();
    radio.listen_result = ESP_FAIL;
    assert(solar_os_reticulum_start(&kLora) == ESP_FAIL);
    assert(start_error() == "radio radio0 did not start listening");
    assert_stopped();

    reset_radio();
    assert(solar_os_reticulum_start(&kLora) == ESP_OK);
    assert(start_error().empty());
    assert_started();
    assert(solar_os_reticulum_stop() == ESP_OK);
    assert_stopped();
    assert(radio.state == SOLAR_OS_RADIO_STATE_SLEEP);
}

void test_restarts_keep_the_identity()
{
    reset_radio();
    char first[SOLAR_OS_RETICULUM_HASH_HEX_LEN];
    for (int round = 0; round < 3; round++) {
        assert(solar_os_reticulum_start(&kLora) == ESP_OK);
        assert_started();
        solar_os_reticulum_status_t status{};
        assert(solar_os_reticulum_get_status(&status) == ESP_OK);
        if (round == 0) {
            memcpy(first, status.identity_hash_hex, sizeof(first));
        }
        assert(strcmp(first, status.identity_hash_hex) == 0);
        solar_os_reticulum_loop_once();
        assert(solar_os_reticulum_stop() == ESP_OK);
        assert_stopped();
    }
}

char arg_name[] = "reticulum";
char arg_lora[] = "lora";
char arg_radio[] = "radio0";
char arg_profile[] = "rns-sf10-cr8";
char *lora_args[] = {arg_name, arg_lora, arg_radio, arg_profile};

esp_err_t job_start()
{
    return solar_os_reticulum_job.start(nullptr, 4, lora_args);
}

void wait_until(const std::atomic<int> &flag, int value)
{
    for (int tries = 0; flag != value; tries++) {
        assert(tries < 200000);
        vTaskDelay(1);
    }
}

bool running()
{
    solar_os_reticulum_status_t status{};
    assert(solar_os_reticulum_get_status(&status) == ESP_OK);
    return status.running;
}

/* The job: a refused start, then one that runs; a stop while the worker is
 * held inside a radio call for longer than the stop waits; a start while
 * that worker is still finishing; and a start once it has. */
void test_job_stop_during_radio_call()
{
    reset_radio();
    radio.claim_result = ESP_ERR_INVALID_STATE;
    assert(job_start() == ESP_ERR_INVALID_STATE);
    assert_stopped();

    reset_radio();
    assert(job_start() == ESP_OK);
    assert(running());
    radio.block_receive = 1;
    wait_until(radio.receive_blocked, 1);

    solar_os_reticulum_job.stop(nullptr);
    assert(host_task_deletes_of_other_tasks() == 0U);
    assert(radio.receive_blocked == 1 && radio.claimed);
    assert(job_start() == ESP_ERR_INVALID_STATE);

    radio.block_receive = 0;
    for (int tries = 0; running() || radio.claimed; tries++) {
        assert(tries < 200000);
        vTaskDelay(1);
    }
    assert_stopped();

    /* The worker sets its done flag just after the service stops; until then
     * a start is still refused. */
    esp_err_t restarted;
    for (int tries = 0; (restarted = job_start()) == ESP_ERR_INVALID_STATE; tries++) {
        assert(tries < 200000);
        vTaskDelay(1);
    }
    assert(restarted == ESP_OK);
    assert(running());
    solar_os_reticulum_job.stop(nullptr);
    assert(host_task_deletes_of_other_tasks() == 0U);
    assert_stopped();
}

}  // namespace

// A resource advertisement past the build's segment limit is refused before
// anything is allocated for it. Checked through Resource.cpp, so the library
// itself was built with the limit.
void test_resource_size_limit()
{
    assert(RNS::Resource::transfer_size_acceptable(128U * 1024U));
    assert(!RNS::Resource::transfer_size_acceptable(128U * 1024U + 1U));
}

int main()
{
    assert(solar_os_reticulum_init() == ESP_OK);
    assert(solar_os_reticulum_instance() != 0);

    test_failed_starts_are_retried();
    test_restarts_keep_the_identity();
    test_job_stop_during_radio_call();
    test_resource_size_limit();
    puts("reticulum service tests passed");
    return 0;
}

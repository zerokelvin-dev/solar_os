#include "solar_os_reticulum.h"

#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <exception>
#include <memory>

#include "solar_os_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <microReticulum.h>
#include <microStore/Adapters/UniversalFileSystem.h>

extern "C" {
#include "solar_os_credentials.h"
#include "solar_os_log.h"
#include "solar_os_radio.h"
#include "solar_os_time.h"
}
#include "solar_os_reticulum_lora.h"
#include "solar_os_reticulum_tcp.h"
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
#include "solar_os_lxmf.h"
#include "solar_os_lxmf_internal.h"
#endif

namespace {

constexpr const char *TAG = "reticulum";
constexpr const char *kStorageRoot = "/sdcard/reticulum";
constexpr const char *kIdentityLabel = "identity";
constexpr size_t kPrivateKeySize = 64U;
constexpr time_t kMinimumSaneTime = 1577836800;  // 2020-01-01

struct AnnounceRecord {
    RNS::Bytes destination;
    uint8_t hops;
    int64_t received_us;
    char app_data[SOLAR_OS_RETICULUM_APP_DATA_MAX + 1U];
};

class AnnounceLog : public RNS::AnnounceHandler {
public:
    void received_announce(const RNS::Bytes &destination_hash,
                           const RNS::Identity &,
                           const RNS::Bytes &app_data) override;
};

SemaphoreHandle_t lock = nullptr;
bool initialized = false;
bool running = false;
bool instance_created = false;
bool identity_set = false;
bool interface_was_online = false;
solar_os_reticulum_status_t counters = {};
char host[SOLAR_OS_RETICULUM_HOST_MAX + 1U] = {};
uint16_t port = 0;
char radio_name[SOLAR_OS_RADIO_NAME_MAX] = {};
char profile_name[SOLAR_OS_RADIO_PROFILE_NAME_MAX] = {};

RNS::Reticulum reticulum({RNS::Type::NONE});
RNS::Identity identity({RNS::Type::NONE});
RNS::Destination destination({RNS::Type::NONE});
RNS::Interface tcp_interface({RNS::Type::NONE});
solar_os::ReticulumTcpInterface *tcp_impl = nullptr;
RNS::Interface lora_interface({RNS::Type::NONE});
solar_os::ReticulumLoraInterface *lora_impl = nullptr;
solar_os_radio_handle_t radio_handle = SOLAR_OS_RADIO_HANDLE_INIT;
solar_os_radio_status_t radio_saved = {};
RNS::HAnnounceHandler announce_handler;
std::unique_ptr<microStore::FileSystem> filesystem;

AnnounceRecord announces[SOLAR_OS_RETICULUM_ANNOUNCE_LOG];
size_t announce_next = 0U;
size_t announce_count = 0U;

class Guard {
public:
    Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(lock); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
};

void log_bridge(const char *message, RNS::LogLevel level)
{
    if (message == nullptr || message[0] == '\0') {
        return;
    }
    switch (level) {
    case RNS::LOG_CRITICAL:
    case RNS::LOG_ERROR:
        SOLAR_OS_LOGE(TAG, "%s", message);
        break;
    case RNS::LOG_WARNING:
        SOLAR_OS_LOGW(TAG, "%s", message);
        break;
    case RNS::LOG_NOTICE:
    case RNS::LOG_INFO:
        SOLAR_OS_LOGI(TAG, "%s", message);
        break;
    default:
        SOLAR_OS_LOGD(TAG, "%s", message);
        break;
    }
}

void AnnounceLog::received_announce(const RNS::Bytes &destination_hash,
                                    const RNS::Identity &,
                                    const RNS::Bytes &app_data)
{
    AnnounceRecord &record = announces[announce_next];
    record.destination = destination_hash;
    record.hops = RNS::Transport::hops_to(destination_hash);
    record.received_us = esp_timer_get_time();
    size_t length = app_data.size();
    if (length > SOLAR_OS_RETICULUM_APP_DATA_MAX) {
        length = SOLAR_OS_RETICULUM_APP_DATA_MAX;
    }
    for (size_t index = 0U; index < length; index++) {
        const unsigned char byte = app_data.data()[index];
        record.app_data[index] = isprint(byte) ? static_cast<char>(byte) : '.';
    }
    record.app_data[length] = '\0';
    announce_next = (announce_next + 1U) % SOLAR_OS_RETICULUM_ANNOUNCE_LOG;
    if (announce_count < SOLAR_OS_RETICULUM_ANNOUNCE_LOG) {
        announce_count++;
    }
    counters.announces_received++;
}

esp_err_t identity_read(RNS::Bytes &private_key)
{
    solar_os_credential_info_t record = {};
    esp_err_t error = solar_os_credentials_find(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
        SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY,
        kIdentityLabel,
        &record);
    if (error != ESP_OK) {
        return error;
    }
    uint8_t secret[kPrivateKeySize];
    size_t length = 0U;
    error = solar_os_credentials_read_secret(
        record.id, secret, sizeof(secret), &length);
    if (error == ESP_OK && length != kPrivateKeySize) {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK) {
        private_key = RNS::Bytes(secret, sizeof(secret));
    }
    solar_os_credentials_wipe(secret, sizeof(secret));
    return error;
}

esp_err_t identity_store(const RNS::Bytes &private_key, bool replace)
{
    if (private_key.size() != kPrivateKeySize) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_credentials_put(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
        SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY,
        kIdentityLabel,
        private_key.data(),
        private_key.size(),
        replace,
        nullptr);
}

esp_err_t identity_load_locked()
{
    RNS::Bytes private_key;
    const esp_err_t error = identity_read(private_key);
    if (error != ESP_OK) {
        return error;
    }
    identity = RNS::Identity(false);
    if (!identity.load_private_key(private_key)) {
        return ESP_ERR_INVALID_STATE;
    }
    identity_set = true;
    return ESP_OK;
}

void text_hash(const RNS::Bytes &hash, char *out)
{
    const std::string hex = hash.toHex();
    strncpy(out, hex.c_str(), SOLAR_OS_RETICULUM_HASH_HEX_LEN - 1U);
    out[SOLAR_OS_RETICULUM_HASH_HEX_LEN - 1U] = '\0';
}

void adopt_provider_clock()
{
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    if (now.tv_sec >= kMinimumSaneTime) {
        return;
    }
    uint64_t epoch_ms = 0;
    if (solar_os_time_get_utc_epoch_ms(&epoch_ms) == ESP_OK &&
        static_cast<time_t>(epoch_ms / 1000ULL) >= kMinimumSaneTime) {
        now.tv_sec = static_cast<time_t>(epoch_ms / 1000ULL);
        now.tv_usec = 0;
        if (settimeofday(&now, nullptr) == 0) {
            return;
        }
    }
    SOLAR_OS_LOGW(TAG, "system clock is not set; announces carry a "
                       "1970 timestamp until time is synchronised");
}

bool any_interface_online()
{
    return (tcp_interface && tcp_interface.online()) ||
           (lora_interface && lora_interface.online());
}

/* Gives the radio back as it was found, whoever had set it up. */
void radio_release_locked()
{
    if (!solar_os_radio_handle_valid(&radio_handle)) {
        return;
    }
    (void)solar_os_radio_handle_configure(&radio_handle, &radio_saved.config);
    if (radio_saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
        (void)solar_os_radio_handle_set_state(&radio_handle, radio_saved.state);
    }
    (void)solar_os_radio_release(&radio_handle);
    radio_handle = SOLAR_OS_RADIO_HANDLE_INIT;
}

esp_err_t radio_claim_locked(const char *radio, const char *profile)
{
    solar_os_radio_info_t info{};
    esp_err_t error = solar_os_radio_get_info(radio, &info);
    if (error != ESP_OK) {
        return error;
    }
    const solar_os_radio_features_t needed =
        SOLAR_OS_RADIO_FEATURE_PACKET | SOLAR_OS_RADIO_FEATURE_CRC |
        SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH;
    if ((info.features & needed) != needed ||
        (info.modulations & SOLAR_OS_RADIO_MODULATION_LORA) == 0U ||
        info.max_packet_len < SOLAR_OS_RNODE_FRAME_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    error = solar_os_radio_claim(radio, "reticulum", &radio_handle);
    if (error != ESP_OK) {
        return error;
    }
    solar_os_radio_status_t configured{};
    error = solar_os_radio_handle_get_status(&radio_handle, &radio_saved);
    if (error == ESP_OK) {
        error = solar_os_radio_handle_profile_apply(&radio_handle, profile);
    }
    if (error == ESP_OK) {
        error = solar_os_radio_handle_get_status(&radio_handle, &configured);
    }
    if (error != ESP_OK) {
        radio_release_locked();
        return error;
    }
    lora_impl = new solar_os::ReticulumLoraInterface(radio_handle,
                                                     configured.config);
    lora_interface = lora_impl;
    lora_interface.mode(RNS::Type::Interface::MODE_FULL);
    RNS::Transport::register_interface(lora_interface);
    if (!lora_interface.start()) {
        RNS::Transport::deregister_interface(lora_interface);
        lora_interface.clear();
        lora_impl = nullptr;
        radio_release_locked();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t create_instance_locked()
{
    mkdir(kStorageRoot, 0775);
    filesystem.reset(new microStore::FileSystem(
        microStore::Adapters::UniversalFileSystem(kStorageRoot)));
    if (!filesystem->init()) {
        filesystem.reset();
        return ESP_FAIL;
    }
    RNS::Utilities::OS::register_filesystem(*filesystem);
    RNS::Reticulum::storagepath("");
    reticulum = RNS::Reticulum();
    reticulum.transport_enabled(false);
    reticulum.probe_destination_enabled(false);
    reticulum.start();
    announce_handler = std::make_shared<AnnounceLog>();
    RNS::Transport::register_announce_handler(announce_handler);
    instance_created = true;
    return ESP_OK;
}

}  // namespace

esp_err_t solar_os_reticulum_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    lock = xSemaphoreCreateMutex();
    if (lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }
    RNS::set_log_callback(log_bridge);
    RNS::loglevel(RNS::LOG_NOTICE);
    initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_reticulum_identity_hash(
    char hash_hex[SOLAR_OS_RETICULUM_HASH_HEX_LEN])
{
    if (hash_hex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = solar_os_reticulum_init();
    if (error != ESP_OK) {
        return error;
    }
    Guard guard;
    RNS::Bytes key;
    const esp_err_t read_error = identity_read(key);
    if (read_error != ESP_OK) {
        return read_error;
    }
    try {
        RNS::Identity loaded(false);
        if (!loaded.load_private_key(key)) {
            return ESP_ERR_INVALID_STATE;
        }
        text_hash(loaded.hash(), hash_hex);
    } catch (const std::exception &) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t solar_os_reticulum_identity_generate(bool force)
{
    esp_err_t error = solar_os_reticulum_init();
    if (error != ESP_OK) {
        return error;
    }
    Guard guard;
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }
    RNS::Bytes existing;
    if (!force && identity_read(existing) == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    try {
        RNS::Identity fresh;
        error = identity_store(fresh.get_private_key(), true);
    } catch (const std::exception &) {
        return ESP_FAIL;
    }
    identity_set = false;
    return error;
}

esp_err_t solar_os_reticulum_identity_import(const char *private_key_hex)
{
    esp_err_t error = solar_os_reticulum_init();
    if (error != ESP_OK) {
        return error;
    }
    if (private_key_hex == nullptr ||
        strlen(private_key_hex) != kPrivateKeySize * 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    Guard guard;
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }
    RNS::Bytes key;
    key.assignHex(private_key_hex);
    if (key.size() != kPrivateKeySize) {
        return ESP_ERR_INVALID_ARG;
    }
    error = identity_store(key, true);
    identity_set = false;
    return error;
}

esp_err_t solar_os_reticulum_identity_export_private(
    char private_key_hex[SOLAR_OS_RETICULUM_PRIVATE_KEY_HEX_LEN])
{
    if (private_key_hex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_reticulum_init();
    if (error != ESP_OK) {
        return error;
    }
    Guard guard;
    RNS::Bytes key;
    error = identity_read(key);
    if (error != ESP_OK) {
        return error;
    }
    const std::string hex = key.toHex();
    memcpy(private_key_hex, hex.c_str(), SOLAR_OS_RETICULUM_PRIVATE_KEY_HEX_LEN);
    return ESP_OK;
}

esp_err_t solar_os_reticulum_start(const solar_os_reticulum_config_t *config)
{
    esp_err_t error = solar_os_reticulum_init();
    if (error != ESP_OK) {
        return error;
    }
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *server = config->host;
    const uint16_t server_port = config->port;
    const bool want_tcp = server != nullptr && server[0] != '\0';
    const bool want_lora = config->radio != nullptr && config->radio[0] != '\0';
    if ((!want_tcp && !want_lora) ||
        (want_tcp && (strlen(server) > SOLAR_OS_RETICULUM_HOST_MAX ||
                      server_port == 0U)) ||
        (want_lora && (config->profile == nullptr || config->profile[0] == '\0' ||
                       strlen(config->radio) >= sizeof(radio_name) ||
                       strlen(config->profile) >= sizeof(profile_name)))) {
        return ESP_ERR_INVALID_ARG;
    }
    Guard guard;
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }
    adopt_provider_clock();
    try {
        if (!instance_created) {
            error = create_instance_locked();
            if (error != ESP_OK) {
                return error;
            }
        }
        if (identity_load_locked() != ESP_OK) {
            RNS::Identity fresh;
            error = identity_store(fresh.get_private_key(), false);
            if (error != ESP_OK) {
                return error;
            }
            error = identity_load_locked();
            if (error != ESP_OK) {
                return error;
            }
        }
        destination = RNS::Destination(
            identity,
            RNS::Type::Destination::IN,
            RNS::Type::Destination::SINGLE,
            SOLAR_OS_RETICULUM_APP_NAME,
            SOLAR_OS_RETICULUM_ASPECT);

        /* The radio first: it is the one that can refuse. */
        if (want_lora) {
            error = radio_claim_locked(config->radio, config->profile);
            if (error != ESP_OK) {
                return error;
            }
        }
        if (want_tcp) {
            tcp_impl = new solar_os::ReticulumTcpInterface(server, server_port);
            tcp_interface = tcp_impl;
            tcp_interface.mode(RNS::Type::Interface::MODE_FULL);
            RNS::Transport::register_interface(tcp_interface);
            tcp_interface.start();
        }
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
        (void)solar_os::lxmf::attach(identity);
        interface_was_online = false;
#endif
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "start failed: %s", failure.what());
        counters.exceptions++;
        return ESP_FAIL;
    }
    memset(host, 0, sizeof(host));
    memset(radio_name, 0, sizeof(radio_name));
    memset(profile_name, 0, sizeof(profile_name));
    port = 0;
    if (want_tcp) {
        strncpy(host, server, sizeof(host) - 1U);
        port = server_port;
    }
    if (want_lora) {
        strncpy(radio_name, config->radio, sizeof(radio_name) - 1U);
        strncpy(profile_name, config->profile, sizeof(profile_name) - 1U);
    }
    running = true;
    counters.last_error = ESP_OK;
    SOLAR_OS_LOGI(TAG, "started, server=%s:%u radio=%s profile=%s",
                  want_tcp ? host : "-", static_cast<unsigned>(port),
                  want_lora ? radio_name : "-", want_lora ? profile_name : "-");
    return ESP_OK;
}

esp_err_t solar_os_reticulum_stop(void)
{
    if (!initialized) {
        return ESP_OK;
    }
    Guard guard;
    if (!running) {
        return ESP_OK;
    }
    running = false;
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
    solar_os::lxmf::detach();
#endif
    try {
        RNS::Transport::persist_data();
        if (tcp_interface) {
            tcp_interface.stop();
            RNS::Transport::deregister_interface(tcp_interface);
        }
        if (lora_interface) {
            lora_interface.stop();
            RNS::Transport::deregister_interface(lora_interface);
        }
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "stop failed: %s", failure.what());
        counters.exceptions++;
    }
    tcp_interface.clear();
    tcp_impl = nullptr;
    lora_interface.clear();
    lora_impl = nullptr;
    radio_release_locked();
    destination = RNS::Destination({RNS::Type::NONE});
    SOLAR_OS_LOGI(TAG, "stopped");
    return ESP_OK;
}

void solar_os_reticulum_loop_once(void)
{
    if (!initialized) {
        return;
    }
    Guard guard;
    if (!running) {
        return;
    }
    try {
        reticulum.loop();
#if SOLAR_OS_PACKAGE_SERVICE_LXMF
        /* An announce before an interface is up has nowhere to go. */
        const bool online = any_interface_online();
        if (online && !interface_was_online) {
            (void)solar_os_lxmf_announce();
        }
        interface_was_online = online;
        solar_os::lxmf::tick();
#endif
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "loop exception: %s", failure.what());
        counters.exceptions++;
    }
}

void solar_os_reticulum_note_stack_watermark(uint32_t bytes)
{
    if (counters.stack_watermark_bytes == 0U ||
        bytes < counters.stack_watermark_bytes) {
        counters.stack_watermark_bytes = bytes;
    }
}

esp_err_t solar_os_reticulum_announce(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    Guard guard;
    if (!running || !destination) {
        return ESP_ERR_INVALID_STATE;
    }
    try {
        destination.announce(RNS::bytesFromString("SolarOS"));
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "announce failed: %s", failure.what());
        counters.exceptions++;
        return ESP_FAIL;
    }
    counters.announces_sent++;
    return ESP_OK;
}

size_t solar_os_reticulum_announce_snapshot(
    solar_os_reticulum_announce_t *records, size_t max_records)
{
    if (!initialized || records == nullptr) {
        return 0U;
    }
    Guard guard;
    const int64_t now = esp_timer_get_time();
    size_t written = 0U;
    for (size_t index = 0U; index < announce_count && written < max_records;
         index++) {
        const size_t slot = (announce_next + SOLAR_OS_RETICULUM_ANNOUNCE_LOG -
                             1U - index) % SOLAR_OS_RETICULUM_ANNOUNCE_LOG;
        const AnnounceRecord &record = announces[slot];
        solar_os_reticulum_announce_t &out = records[written++];
        text_hash(record.destination, out.destination_hex);
        out.hops = record.hops;
        out.age_ms = static_cast<uint32_t>((now - record.received_us) / 1000);
        memcpy(out.app_data, record.app_data, sizeof(out.app_data));
    }
    return written;
}

esp_err_t solar_os_reticulum_get_status(solar_os_reticulum_status_t *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        memset(status, 0, sizeof(*status));
        return ESP_OK;
    }
    Guard guard;
    *status = counters;
    status->initialized = initialized;
    status->running = running;
    status->identity_set = identity_set;
    strncpy(status->host, host, sizeof(status->host) - 1U);
    status->port = port;
    if (identity_set) {
        text_hash(identity.hash(), status->identity_hash_hex);
    }
    strncpy(status->radio, radio_name, sizeof(status->radio) - 1U);
    strncpy(status->profile, profile_name, sizeof(status->profile) - 1U);
    if (running && destination) {
        text_hash(destination.hash(), status->destination_hex);
        status->interface_online = any_interface_online();
        if (tcp_interface) {
            status->tcp_online = tcp_interface.online();
            status->rx_frames = static_cast<uint32_t>(tcp_interface.rx());
            status->tx_frames = static_cast<uint32_t>(tcp_interface.tx());
            status->rx_bytes = static_cast<uint32_t>(tcp_interface.rxbytes());
            status->tx_bytes = static_cast<uint32_t>(tcp_interface.txbytes());
            status->connects = tcp_impl != nullptr ? tcp_impl->connects() : 0U;
        }
        if (lora_interface && lora_impl != nullptr) {
            status->lora_online = lora_interface.online();
            status->lora_heard = lora_impl->heard();
            status->lora_rx_frames = static_cast<uint32_t>(lora_interface.rx());
            status->lora_tx_frames = static_cast<uint32_t>(lora_interface.tx());
            status->lora_rx_bytes = static_cast<uint32_t>(lora_interface.rxbytes());
            status->lora_tx_bytes = static_cast<uint32_t>(lora_interface.txbytes());
            status->lora_send_errors = lora_impl->send_errors();
            status->lora_crc_errors = lora_impl->crc_errors();
            status->lora_rssi_dbm = lora_impl->last_rssi_dbm();
            status->lora_snr_db = lora_impl->last_snr_db();
        }
        status->paths = RNS::Transport::new_path_table().size();
    }
    return ESP_OK;
}

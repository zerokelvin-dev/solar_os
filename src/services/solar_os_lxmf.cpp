#include "solar_os_lxmf.h"

#include <string.h>

#include <exception>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_lxmf_internal.h"

extern "C" {
#include "solar_os_contacts.h"
#include "solar_os_identity.h"
#include "solar_os_log.h"
#include "solar_os_messaging.h"
#include "solar_os_time.h"
}

namespace {

constexpr const char *TAG = "lxmf";
/* A peer has this long to answer a path request before the send fails. */
constexpr int64_t kPathTimeoutUs = 20LL * 1000000LL;
constexpr int16_t kProofTimeoutSeconds = 30;
constexpr size_t kOverhead =
    SOLAR_OS_LXMF_HASH_LEN + SOLAR_OS_LXMF_SIGNATURE_LEN;

struct Pending {
    bool active;
    bool awaiting_path;
    bool concluded;
    uint32_t request_id;
    int64_t started_us;
    RNS::Bytes destination;
    char body[SOLAR_OS_LXMF_CONTENT_MAX + 1U];
};

class DeliveryAnnounces : public RNS::AnnounceHandler {
public:
    DeliveryAnnounces()
        : RNS::AnnounceHandler(SOLAR_OS_LXMF_APP_NAME "." SOLAR_OS_LXMF_ASPECT)
    {
    }
    void received_announce(const RNS::Bytes &destination_hash,
                           const RNS::Identity &announced_identity,
                           const RNS::Bytes &app_data) override;
};

SemaphoreHandle_t lock = nullptr;
StaticSemaphore_t lock_storage;
bool attached = false;
bool announce_requested = false;
solar_os_lxmf_status_t counters = {};
Pending pending = {};

RNS::Destination inbound({RNS::Type::NONE});
RNS::HAnnounceHandler announce_handler;

class Guard {
public:
    Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(lock); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
};

void ensure_lock()
{
    if (lock == nullptr) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    }
}

void display_name(char *out, size_t out_len)
{
    solar_os_identity_get_hostname(out, out_len);
    if (out[0] == '\0') {
        strlcpy(out, "SolarOS", out_len);
    }
}

void hash_hex(const RNS::Bytes &hash, char *out, size_t out_len)
{
    const std::string hex = hash.toHex();
    strlcpy(out, hex.c_str(), out_len);
}

bool hex_to_hash(const char *hex, RNS::Bytes &hash)
{
    if (hex == nullptr || strlen(hex) != SOLAR_OS_LXMF_HASH_LEN * 2U) {
        return false;
    }
    hash.assignHex(hex);
    return hash.size() == SOLAR_OS_LXMF_HASH_LEN;
}

uint64_t now_ms()
{
    uint64_t epoch_ms = 0U;
    if (solar_os_time_get_utc_epoch_ms(&epoch_ms) == ESP_OK) {
        return epoch_ms;
    }
    return 0U;
}

/* Records the peer so the chat and contacts apps can address it. */
esp_err_t remember_peer(const RNS::Bytes &destination_hash,
                        const char *name,
                        solar_os_contact_id_t *contact_id,
                        solar_os_endpoint_id_t *endpoint_id)
{
    char fallback[SOLAR_OS_LXMF_HASH_HEX_LEN];
    hash_hex(destination_hash, fallback, sizeof(fallback));
    return solar_os_contacts_upsert_discovered(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
        destination_hash.data(),
        destination_hash.size(),
        (name != nullptr && name[0] != '\0') ? name : fallback,
        SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ACK,
        now_ms(),
        nullptr,
        0U,
        contact_id,
        endpoint_id);
}

void DeliveryAnnounces::received_announce(const RNS::Bytes &destination_hash,
                                          const RNS::Identity &,
                                          const RNS::Bytes &app_data)
{
    char name[SOLAR_OS_LXMF_NAME_MAX + 1U] = {};
    if (!solar_os_lxmf_parse_announce(
            app_data.data(), app_data.size(), name, sizeof(name))) {
        name[0] = '\0';
    }
    if (remember_peer(destination_hash, name, nullptr, nullptr) == ESP_OK) {
        counters.peers++;
    }
}

void receive(const RNS::Bytes &data, const RNS::Packet &)
{
    if (data.size() <= kOverhead) {
        counters.rejected++;
        return;
    }
    const RNS::Bytes source_hash = data.mid(0U, SOLAR_OS_LXMF_HASH_LEN);
    const RNS::Bytes signature =
        data.mid(SOLAR_OS_LXMF_HASH_LEN, SOLAR_OS_LXMF_SIGNATURE_LEN);
    const uint8_t *payload_bytes = data.data() + kOverhead;
    const size_t payload_len = data.size() - kOverhead;

    solar_os_lxmf_payload_t payload = {};
    if (!solar_os_lxmf_unpack_payload(payload_bytes, payload_len, &payload)) {
        counters.rejected++;
        SOLAR_OS_LOGW(TAG, "dropped a malformed message");
        return;
    }

    RNS::Identity source = RNS::Identity::recall(source_hash);
    if (!source) {
        counters.rejected++;
        SOLAR_OS_LOGW(TAG, "dropped a message from an unannounced sender");
        return;
    }

    uint8_t signed_payload[SOLAR_OS_LXMF_PAYLOAD_MAX];
    const size_t signed_len = solar_os_lxmf_signed_payload(
        payload_bytes, &payload, signed_payload, sizeof(signed_payload));
    if (signed_len == 0U) {
        counters.rejected++;
        return;
    }
    RNS::Bytes hashed_part(inbound.hash());
    hashed_part.append(source_hash);
    hashed_part.append(RNS::Bytes(signed_payload, signed_len));
    RNS::Bytes signed_part(hashed_part);
    signed_part.append(RNS::Identity::full_hash(hashed_part));
    if (!source.validate(signature, signed_part)) {
        counters.rejected++;
        SOLAR_OS_LOGW(TAG, "dropped a message with an invalid signature");
        return;
    }

    char peer_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    hash_hex(source_hash, peer_hex, sizeof(peer_hex));
    solar_os_contact_id_t contact_id = 0;
    solar_os_endpoint_id_t endpoint_id = 0;
    solar_os_endpoint_t endpoint = {};
    const char *name = peer_hex;
    if (solar_os_contacts_find_endpoint(SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
                                        source_hash.data(),
                                        source_hash.size(),
                                        &endpoint) == ESP_OK) {
        contact_id = endpoint.contact_id;
        endpoint_id = endpoint.id;
    }
    solar_os_contact_t contact = {};
    if (contact_id != 0 && solar_os_contacts_get(contact_id, &contact) == ESP_OK &&
        contact.display_name[0] != '\0') {
        name = contact.display_name;
    } else {
        (void)remember_peer(source_hash, nullptr, &contact_id, &endpoint_id);
    }

    char body[SOLAR_OS_LXMF_TITLE_MAX + SOLAR_OS_LXMF_CONTENT_MAX + 2U];
    if (payload.title[0] != '\0') {
        snprintf(body, sizeof(body), "%s\n%s", payload.title, payload.content);
    } else {
        strlcpy(body, payload.content, sizeof(body));
    }

    const solar_os_messaging_inbound_t message = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
        .conversation_key = peer_hex,
        .conversation_kind = SOLAR_OS_CONVERSATION_DIRECT,
        .conversation_title = name,
        .contact_id = contact_id,
        .endpoint_id = endpoint_id,
        .group_ref = 0U,
        .provider_message_key = 0U,
        .timestamp_ms = (uint64_t)(payload.timestamp_s * 1000.0),
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
                          SOLAR_OS_SECURITY_PEER_KEY_KNOWN |
                          SOLAR_OS_SECURITY_TRANSPORT_SECURED,
        .sender = name,
        .body = body,
        .truncated = payload.truncated,
    };
    bool inserted = false;
    if (solar_os_messaging_publish_inbound(&message, &inserted, nullptr) ==
            ESP_OK &&
        inserted) {
        counters.received++;
    }
}

void conclude(uint32_t request_id, solar_os_delivery_state_t state,
              const char *error)
{
    if (!pending.active || pending.request_id != request_id) {
        return;
    }
    pending.active = false;
    pending.concluded = true;
    (void)solar_os_messaging_outbox_update(request_id, state, error);
    if (state == SOLAR_OS_DELIVERY_DELIVERED) {
        counters.delivered++;
    } else if (state == SOLAR_OS_DELIVERY_FAILED) {
        counters.failed++;
    }
}

void fail(uint32_t request_id, const char *reason)
{
    conclude(request_id, SOLAR_OS_DELIVERY_FAILED, reason);
}

bool transmit()
{
    RNS::Identity peer = RNS::Identity::recall(pending.destination);
    if (!peer) {
        fail(pending.request_id, "Reticulum peer identity unknown");
        return false;
    }
    char name[SOLAR_OS_LXMF_NAME_MAX + 1U];
    display_name(name, sizeof(name));

    uint8_t payload[SOLAR_OS_LXMF_PAYLOAD_MAX];
    const uint64_t timestamp_ms = now_ms();
    const size_t payload_len = solar_os_lxmf_pack_payload(
        (double)timestamp_ms / 1000.0, "", pending.body, payload,
        sizeof(payload));
    if (payload_len == 0U ||
        payload_len + kOverhead > SOLAR_OS_LXMF_PACKET_MAX) {
        fail(pending.request_id, "Reticulum message exceeds one packet");
        return false;
    }

    RNS::Destination out(peer,
                         RNS::Type::Destination::OUT,
                         RNS::Type::Destination::SINGLE,
                         SOLAR_OS_LXMF_APP_NAME,
                         SOLAR_OS_LXMF_ASPECT);

    RNS::Bytes hashed_part(pending.destination);
    hashed_part.append(inbound.hash());
    hashed_part.append(RNS::Bytes(payload, payload_len));
    RNS::Bytes signed_part(hashed_part);
    signed_part.append(RNS::Identity::full_hash(hashed_part));

    RNS::Bytes wire(inbound.hash());
    wire.append(inbound.identity().sign(signed_part));
    wire.append(RNS::Bytes(payload, payload_len));

    const uint32_t request_id = pending.request_id;
    RNS::Packet packet(out, wire);
    RNS::PacketReceipt receipt = packet.receipt_send();
    if (!receipt) {
        fail(request_id, "Reticulum packet could not be sent");
        return false;
    }
    counters.sent++;
    (void)solar_os_messaging_outbox_update(
        request_id, SOLAR_OS_DELIVERY_SENT, nullptr);
    receipt.set_timeout(kProofTimeoutSeconds);
    receipt.set_delivery_handler([request_id](const RNS::PacketReceipt &) {
        conclude(request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
    });
    receipt.set_timeout_handler([request_id](const RNS::PacketReceipt &) {
        /* The message went out; only the delivery proof never came back. */
        conclude(request_id, SOLAR_OS_DELIVERY_SENT, nullptr);
    });
    return true;
}

void start_next()
{
    solar_os_messaging_outbound_t outbound = {};
    if (solar_os_messaging_outbox_peek(SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
                                       &outbound) != ESP_OK) {
        return;
    }
    memset(&pending, 0, sizeof(pending));
    pending.active = true;
    pending.request_id = outbound.id;
    pending.started_us = esp_timer_get_time();
    strlcpy(pending.body, outbound.body, sizeof(pending.body));

    solar_os_messaging_conversation_t conversation = {};
    if (solar_os_messaging_conversation_get(outbound.conversation_id,
                                            &conversation) != ESP_OK ||
        !hex_to_hash(conversation.provider_key, pending.destination)) {
        fail(outbound.id, "Reticulum conversation has no destination");
        return;
    }
    if (strnlen(outbound.body, sizeof(outbound.body)) >
        SOLAR_OS_LXMF_CONTENT_MAX) {
        fail(outbound.id, "Reticulum message exceeds 255 bytes");
        return;
    }
    (void)solar_os_messaging_outbox_update(
        outbound.id, SOLAR_OS_DELIVERY_SENDING, nullptr);

    if (!RNS::Transport::has_path(pending.destination)) {
        RNS::Transport::request_path(pending.destination);
        pending.awaiting_path = true;
        return;
    }
    (void)transmit();
}

}  // namespace

namespace solar_os {
namespace lxmf {

esp_err_t attach(const RNS::Identity &identity)
{
    ensure_lock();
    if (attached) {
        return ESP_OK;
    }
    try {
        inbound = RNS::Destination(identity,
                                   RNS::Type::Destination::IN,
                                   RNS::Type::Destination::SINGLE,
                                   SOLAR_OS_LXMF_APP_NAME,
                                   SOLAR_OS_LXMF_ASPECT);
        inbound.set_packet_callback(receive);
        inbound.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
        announce_handler = std::make_shared<DeliveryAnnounces>();
        RNS::Transport::register_announce_handler(announce_handler);
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "attach failed: %s", failure.what());
        counters.last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    attached = true;
    memset(&pending, 0, sizeof(pending));
    (void)solar_os_messaging_init();
    (void)solar_os_messaging_provider_register(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM, "Reticulum");
    (void)solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM, true, true, ESP_OK, nullptr);
    announce_requested = true;
    return ESP_OK;
}

void detach()
{
    if (!attached) {
        return;
    }
    attached = false;
    if (pending.active) {
        fail(pending.request_id, "Reticulum stopped");
    }
    try {
        RNS::Transport::deregister_announce_handler(announce_handler);
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "detach failed: %s", failure.what());
    }
    announce_handler.reset();
    inbound = RNS::Destination({RNS::Type::NONE});
    (void)solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM, false, false, ESP_OK, nullptr);
}

void tick()
{
    if (!attached) {
        return;
    }
    bool announce_now = false;
    {
        Guard guard;
        announce_now = announce_requested;
        announce_requested = false;
    }
    try {
        if (announce_now) {
            char name[SOLAR_OS_LXMF_NAME_MAX + 1U];
            display_name(name, sizeof(name));
            uint8_t app_data[SOLAR_OS_LXMF_ANNOUNCE_MAX];
            const size_t length = solar_os_lxmf_pack_announce(
                name, app_data, sizeof(app_data));
            if (length > 0U) {
                inbound.announce(RNS::Bytes(app_data, length));
                counters.announces_sent++;
            }
        }
        if (pending.active && pending.awaiting_path) {
            if (RNS::Transport::has_path(pending.destination)) {
                pending.awaiting_path = false;
                (void)transmit();
            } else if (esp_timer_get_time() - pending.started_us >
                       kPathTimeoutUs) {
                fail(pending.request_id, "no Reticulum path to the peer");
            }
        }
        if (!pending.active) {
            start_next();
        }
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "tick failed: %s", failure.what());
        counters.last_error = ESP_FAIL;
        if (pending.active) {
            fail(pending.request_id, "Reticulum send failed");
        }
    }
}

}  // namespace lxmf
}  // namespace solar_os

esp_err_t solar_os_lxmf_get_status(solar_os_lxmf_status_t *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    Guard guard;
    *status = counters;
    status->attached = attached;
    display_name(status->display_name, sizeof(status->display_name));
    if (attached && inbound) {
        hash_hex(inbound.hash(), status->destination_hex,
                 sizeof(status->destination_hex));
    }
    return ESP_OK;
}

esp_err_t solar_os_lxmf_announce(void)
{
    ensure_lock();
    Guard guard;
    if (!attached) {
        return ESP_ERR_INVALID_STATE;
    }
    announce_requested = true;
    return ESP_OK;
}

esp_err_t solar_os_lxmf_open(const char *destination_hex,
                             solar_os_conversation_id_t *conversation_id)
{
    RNS::Bytes hash;
    if (!hex_to_hash(destination_hex, hash)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_contact_id_t contact_id = 0;
    solar_os_endpoint_id_t endpoint_id = 0;
    const esp_err_t error =
        remember_peer(hash, nullptr, &contact_id, &endpoint_id);
    if (error != ESP_OK) {
        return error;
    }
    char peer_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    hash_hex(hash, peer_hex, sizeof(peer_hex));
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
        .provider_key = peer_hex,
        .kind = SOLAR_OS_CONVERSATION_DIRECT,
        .title = peer_hex,
        .contact_id = contact_id,
        .endpoint_id = endpoint_id,
        .group_ref = 0U,
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED,
    };
    return solar_os_messaging_conversation_upsert(&request, conversation_id);
}

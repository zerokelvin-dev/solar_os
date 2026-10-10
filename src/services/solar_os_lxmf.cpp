#include "solar_os_lxmf.h"

#include <string.h>

#include <exception>

#include "esp_attr.h"
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
/*
 * Delivery follows the same shape as the LXMF router's outbound job, so
 * that what a peer observes, and what someone reading this after reading
 * LXMF expects, are the same thing. The retries are driven by whether
 * Reticulum has a path rather than by a clock alone: after a try without
 * one, ask for a path and wait; if a path exists and delivery still fails,
 * treat the path as stale, drop it and ask again.
 *
 * LXMF ships five attempts. Three is enough on a link this slow, and the
 * difference between giving up after two minutes and after four is not
 * worth the airtime.
 */
constexpr uint8_t kMaxDeliveryAttempts = 3;
constexpr uint8_t kMaxPathlessTries = 1;
constexpr int64_t kProcessingIntervalUs = 4LL * 1000000LL;
constexpr int64_t kDeliveryRetryWaitUs = 10LL * 1000000LL;
constexpr int64_t kPathRequestWaitUs = 7LL * 1000000LL;
constexpr int16_t kProofTimeoutSeconds = 30;
/* A link nothing has used for this long is closed, as the LXMF router
 * closes its own: an open link costs keepalives on a slow channel. */
constexpr double kLinkIdleSeconds = 600.0;
constexpr size_t kOverhead =
    SOLAR_OS_LXMF_HASH_LEN + SOLAR_OS_LXMF_SIGNATURE_LEN;
/*
 * The longest message taken, as it is carried. LXMF and Reticulum set no
 * limit that matters here; this one is airtime, about three minutes of it
 * at the slowest setting this runs on. What is kept of a message is what the
 * messaging service holds, and a longer one is marked as cut.
 */
constexpr size_t kMessageMax = 64U * 1024U;

struct Pending {
    bool active;
    bool concluded;
    uint32_t request_id;
    uint8_t attempts;
    /* On the link and not yet answered. */
    bool sending;
    /* The link did not carry it. A link that fails a message is taken for
     * dead, so the next attempt opens another and counts as a try. */
    bool link_failed;
    /* The tries for a link ran out and it is going as a single packet. */
    bool opportunistic;
    int64_t next_attempt_us;
    uint64_t timestamp_ms;
    RNS::Bytes destination;
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
int64_t last_process_us = 0;
solar_os_lxmf_status_t counters = {};
Pending pending = {};
/* The message being sent and the one being taken in, at the size the
 * messaging service holds. Too large for the stack they are used from. */
EXT_RAM_BSS_ATTR char pending_body[SOLAR_OS_MESSAGING_BODY_MAX];
EXT_RAM_BSS_ATTR char inbound_body[SOLAR_OS_MESSAGING_BODY_MAX];

RNS::Destination inbound({RNS::Type::NONE});
/* The one link this sends on, and whose it is. Kept after a message so the
 * next to the same peer does not pay for another. */
RNS::Link direct({RNS::Type::NONE});
RNS::Bytes direct_peer;
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

/* A message from its sender hash on: the recipient hash is implied. */
void receive_message(const RNS::Bytes &data)
{
    if (data.size() <= kOverhead || data.size() > kMessageMax) {
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

    /* The signature covers the payload without any stamp, which differs from
     * the payload as sent only in its first byte. */
    RNS::Bytes signed_payload;
    uint8_t *signed_bytes = signed_payload.writable(payload.signed_len);
    if (signed_bytes == nullptr ||
        solar_os_lxmf_signed_payload(payload_bytes, &payload, signed_bytes,
                                     payload.signed_len) == 0U) {
        counters.rejected++;
        return;
    }
    RNS::Bytes hashed_part(inbound.hash());
    hashed_part.append(source_hash);
    hashed_part.append(signed_payload);
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

    char *body = inbound_body;
    size_t used = 0U;
    if (payload.title[0] != '\0') {
        used = (size_t)snprintf(body, sizeof(inbound_body), "%s\n", payload.title);
    }
    bool truncated = payload.truncated;
    solar_os_lxmf_copy_text(payload.content, payload.content_len, &body[used],
                            sizeof(inbound_body) - used, &truncated);

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
        .truncated = truncated,
    };
    bool inserted = false;
    const esp_err_t error =
        solar_os_messaging_publish_inbound(&message, &inserted, nullptr);
    if (error != ESP_OK) {
        counters.last_error = error;
        SOLAR_OS_LOGW(TAG, "message not stored: %s", esp_err_to_name(error));
        return;
    }
    if (inserted) {
        counters.received++;
    }
}

/* An opportunistic packet leaves the recipient hash out, since it is the
 * destination the packet was addressed to. */
void receive(const RNS::Bytes &data, const RNS::Packet &)
{
    receive_message(data);
}

/*
 * Over a link the whole message arrives, recipient hash first. This is how
 * an LXMF client sends by default, so it has to be taken; the link proves
 * the packet, because the delivery destination proves everything.
 */
void receive_whole(const RNS::Bytes &data)
{
    if (data.size() <= SOLAR_OS_LXMF_HASH_LEN ||
        memcmp(data.data(), inbound.hash().data(), SOLAR_OS_LXMF_HASH_LEN) != 0) {
        counters.rejected++;
        return;
    }
    receive_message(data.mid(SOLAR_OS_LXMF_HASH_LEN));
}

void receive_over_link(const RNS::Bytes &data, const RNS::Packet &)
{
    receive_whole(data);
}

/*
 * A message larger than one link packet comes as a resource, advertised
 * first. One past the limit is told so, which is what stops its sender
 * trying again: the link takes every advertisement whatever is said here,
 * and dropping the transfer on this side alone looks to the sender like a
 * transfer that stalled.
 */
void resource_advertised(const RNS::ResourceAdvertisement &advertisement)
{
    if (advertisement.get_data_size() <= kMessageMax) {
        return;
    }
    counters.rejected++;
    SOLAR_OS_LOGW(TAG, "refused a %u byte message",
                  (unsigned)advertisement.get_data_size());
    try {
        RNS::Packet reject(advertisement.get_link(), advertisement.get_hash());
        reject.context(RNS::Type::Packet::RESOURCE_RCL);
        reject.send();
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGW(TAG, "refusal not sent: %s", failure.what());
    }
}

/* The refused one is taken up by the link all the same, and stopped here. */
void resource_started(const RNS::Resource &resource)
{
    if (resource.total_size() > kMessageMax) {
        RNS::Resource(resource).cancel();
    }
}

/* Whole, it is the same bytes a link packet would have carried. */
void resource_concluded(const RNS::Resource &resource)
{
    if (resource.status() != RNS::Type::Resource::COMPLETE) {
        return;
    }
    receive_whole(resource.data());
}

void link_established(RNS::Link &link)
{
    link.set_packet_callback(receive_over_link);
    link.set_resource_strategy(RNS::Type::Link::ACCEPT_APP);
    link.set_resource_callback(resource_advertised);
    link.set_resource_started_callback(resource_started);
    link.set_resource_concluded_callback(resource_concluded);
    counters.links++;
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

/*
 * The message as its sender signs it: sender hash, signature, payload. An
 * opportunistic packet carries exactly this, and a link carries it with the
 * recipient hash in front. Built from the time the message was queued, so
 * that every attempt is the same message to whoever receives two of them.
 */
bool build_message(RNS::Bytes &message)
{
    const size_t room = strlen(pending_body) + 32U;
    RNS::Bytes payload;
    uint8_t *packed = payload.writable(room);
    if (packed == nullptr) {
        return false;
    }
    const size_t payload_len = solar_os_lxmf_pack_payload(
        (double)pending.timestamp_ms / 1000.0, "", pending_body, packed, room);
    if (payload_len == 0U) {
        return false;
    }
    payload.resize(payload_len);

    RNS::Bytes hashed_part(pending.destination);
    hashed_part.append(inbound.hash());
    hashed_part.append(payload);
    RNS::Bytes signed_part(hashed_part);
    signed_part.append(RNS::Identity::full_hash(hashed_part));

    message = inbound.hash();
    message.append(inbound.identity().sign(signed_part));
    message.append(payload);
    return true;
}

RNS::Destination delivery_destination(const RNS::Identity &peer)
{
    return RNS::Destination(peer,
                            RNS::Type::Destination::OUT,
                            RNS::Type::Destination::SINGLE,
                            SOLAR_OS_LXMF_APP_NAME,
                            SOLAR_OS_LXMF_ASPECT);
}

/*
 * Returns false when this attempt could not be made. That is not a failure:
 * the peer's identity arrives with its announce and its path arrives with a
 * path request, so an attempt that finds neither is simply early. Only the
 * attempt count ends a message.
 */
bool transmit()
{
    RNS::Identity peer = RNS::Identity::recall(pending.destination);
    if (!peer) {
        RNS::Transport::request_path(pending.destination);
        return false;
    }
    RNS::Bytes wire;
    if (!build_message(wire)) {
        fail(pending.request_id, "Reticulum message could not be packed");
        return false;
    }
    RNS::Destination out = delivery_destination(peer);

    const uint32_t request_id = pending.request_id;
    RNS::Packet packet(out, wire);
    RNS::PacketReceipt receipt = packet.receipt_send();
    if (!receipt) {
        /* No interface would take it; there may be one by the next try. */
        return false;
    }
    counters.sent++;
    receipt.set_timeout(kProofTimeoutSeconds);
    receipt.set_delivery_handler([request_id](const RNS::PacketReceipt &) {
        conclude(request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
    });
    /*
     * No timeout handler: an opportunistic packet that draws no proof is
     * not finished, it is unanswered, and the next attempt is the answer.
     * Claiming it was sent would put a message the peer never saw next to
     * one it did.
     */
    return true;
}

/*
 * One step of the outbound job without a link. Mirrors the opportunistic
 * branch of the LXMF router: try, then ask for a path, then distrust the
 * path you have, then give up.
 */
void process_opportunistic()
{
    const int64_t now = esp_timer_get_time();
    if (pending.attempts > kMaxDeliveryAttempts) {
        fail(pending.request_id, "no answer from the Reticulum peer");
        return;
    }
    const bool has_path = RNS::Transport::has_path(pending.destination);
    if (pending.attempts >= kMaxPathlessTries && !has_path) {
        pending.attempts++;
        RNS::Transport::request_path(pending.destination);
        pending.next_attempt_us = now + kPathRequestWaitUs;
        return;
    }
    if (pending.attempts == kMaxPathlessTries + 1U && has_path) {
        /* Delivery keeps failing with a path in hand, so the path is the
         * thing to doubt. */
        pending.attempts++;
        (void)RNS::Transport::expire_path(pending.destination);
        RNS::Transport::request_path(pending.destination);
        pending.next_attempt_us = now + kPathRequestWaitUs;
        return;
    }
    if (now > pending.next_attempt_us) {
        pending.attempts++;
        pending.next_attempt_us = now + kDeliveryRetryWaitUs;
        (void)transmit();
    }
}

void drop_link()
{
    if (direct) {
        try {
            direct.teardown();
        } catch (const std::exception &failure) {
            SOLAR_OS_LOGW(TAG, "link not closed: %s", failure.what());
        }
    }
    direct = RNS::Link({RNS::Type::NONE});
    direct_peer = RNS::Bytes();
}

/* The outbound job waits out its interval; a link that has come up is
 * what it was waiting for. */
void link_ready(RNS::Link &)
{
    counters.links++;
    last_process_us = 0;
}

/* How a message sent as a resource ended. */
void resource_sent(const RNS::Resource &resource)
{
    if (!pending.active || !pending.sending) {
        return;
    }
    if (resource.status() == RNS::Type::Resource::COMPLETE) {
        conclude(pending.request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
        return;
    }
    pending.sending = false;
    pending.link_failed = true;
    pending.next_attempt_us = esp_timer_get_time() + kDeliveryRetryWaitUs;
}

/*
 * Sends the message on the link that is up: in one packet if it fits, as a
 * resource if it does not. Not compressed, since the only compression a peer
 * would undo is one this does not have.
 */
void send_over_link()
{
    RNS::Bytes message;
    if (!build_message(message)) {
        fail(pending.request_id, "Reticulum message could not be packed");
        return;
    }
    RNS::Bytes wire(pending.destination);
    wire.append(message);
    if (wire.size() > kMessageMax) {
        /* Permanent: no number of attempts shortens a message. */
        fail(pending.request_id, "Reticulum message is too long");
        return;
    }
    const uint32_t request_id = pending.request_id;
    if (wire.size() <= SOLAR_OS_LXMF_LINK_PACKET_MAX) {
        RNS::Packet packet(direct, wire);
        RNS::PacketReceipt receipt = packet.receipt_send();
        if (!receipt) {
            return;
        }
        receipt.set_timeout(kProofTimeoutSeconds);
        receipt.set_delivery_handler([request_id](const RNS::PacketReceipt &) {
            conclude(request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
        });
        receipt.set_timeout_handler([request_id](const RNS::PacketReceipt &) {
            if (pending.active && pending.request_id == request_id) {
                pending.sending = false;
                pending.link_failed = true;
            }
        });
    } else {
        RNS::Resource resource(wire, direct, true, false, resource_sent);
        if (!resource) {
            return;
        }
    }
    counters.sent++;
    pending.sending = true;
}

/*
 * One step of the outbound job. Mirrors the direct branch of the LXMF
 * router: use the link to the peer if there is one, open one if there is a
 * path, ask for a path if there is not, and count the tries. A message that
 * fits one packet is sent without a link once the tries for one run out,
 * which is what reaches a peer that can hear a packet and not hold a link.
 */
void process_outbound()
{
    if (pending.opportunistic) {
        process_opportunistic();
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (direct && (pending.link_failed ||
                   direct_peer != pending.destination)) {
        drop_link();
    }
    pending.link_failed = false;
    if (direct) {
        switch (direct.status()) {
        case RNS::Type::Link::ACTIVE:
            if (!pending.sending && now > pending.next_attempt_us) {
                send_over_link();
            }
            return;
        case RNS::Type::Link::CLOSED:
            drop_link();
            pending.sending = false;
            pending.next_attempt_us = now + kDeliveryRetryWaitUs;
            return;
        default:
            return; /* still being established */
        }
    }
    if (now <= pending.next_attempt_us) {
        return;
    }
    if (pending.attempts >= kMaxDeliveryAttempts) {
        if (strlen(pending_body) + kOverhead + 16U <= SOLAR_OS_LXMF_PACKET_MAX) {
            pending.opportunistic = true;
            pending.attempts = 0U;
            pending.next_attempt_us = 0;
            process_opportunistic();
        } else {
            fail(pending.request_id, "no link to the Reticulum peer");
        }
        return;
    }
    pending.attempts++;
    pending.next_attempt_us = now + kDeliveryRetryWaitUs;
    RNS::Identity peer = RNS::Identity::recall(pending.destination);
    if (!peer || !RNS::Transport::has_path(pending.destination)) {
        RNS::Transport::request_path(pending.destination);
        pending.next_attempt_us = now + kPathRequestWaitUs;
        return;
    }
    direct = RNS::Link(delivery_destination(peer), link_ready);
    direct_peer = pending.destination;
}

void start_next()
{
    solar_os_messaging_outbound_t outbound = {};
    if (solar_os_messaging_outbox_peek(SOLAR_OS_MESSAGING_PROVIDER_RETICULUM,
                                       &outbound) != ESP_OK) {
        return;
    }
    pending = Pending{};
    pending.active = true;
    pending.request_id = outbound.id;
    pending.timestamp_ms = now_ms();
    strlcpy(pending_body, outbound.body, sizeof(pending_body));

    solar_os_messaging_conversation_t conversation = {};
    if (solar_os_messaging_conversation_get(outbound.conversation_id,
                                            &conversation) != ESP_OK ||
        !hex_to_hash(conversation.provider_key, pending.destination)) {
        fail(outbound.id, "Reticulum conversation has no destination");
        return;
    }
    (void)solar_os_messaging_outbox_update(
        outbound.id, SOLAR_OS_DELIVERY_SENDING, nullptr);
    process_outbound();
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
        inbound.set_link_established_callback(link_established);
        inbound.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
        announce_handler = std::make_shared<DeliveryAnnounces>();
        RNS::Transport::register_announce_handler(announce_handler);
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "attach failed: %s", failure.what());
        counters.last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    attached = true;
    pending = Pending{};
    (void)solar_os_messaging_init();
    (void)solar_os_messaging_provider_register(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM, "Reticulum");
    (void)solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_RETICULUM, true, true, ESP_OK, nullptr);
    return ESP_OK;
}

void detach()
{
    if (!attached) {
        return;
    }
    attached = false;
    if (pending.active) {
        /* Stopping is not a delivery failure. The message goes back on the
         * queue so it is tried again when Reticulum returns. */
        (void)solar_os_messaging_outbox_update(
            pending.request_id, SOLAR_OS_DELIVERY_QUEUED, nullptr);
        pending = Pending{};
    }
    drop_link();
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
        /* The outbound job runs on its own interval, as the LXMF router's
         * does, rather than on every pass of the Reticulum loop. */
        const int64_t now = esp_timer_get_time();
        if (now - last_process_us >= kProcessingIntervalUs) {
            last_process_us = now;
            if (pending.active) {
                process_outbound();
            }
            if (!pending.active) {
                start_next();
            }
            if (!pending.active && direct &&
                (direct.status() == RNS::Type::Link::CLOSED ||
                 direct.inactive_for() > kLinkIdleSeconds)) {
                drop_link();
            }
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

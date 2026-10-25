#include "solar_os_lxmf.h"

#include <string.h>

#include <exception>
#include <new>
#include <vector>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "solar_os_lxmf_internal.h"
#include "solar_os_lxmf_stamp.h"

extern "C" {
#include "solar_os_contacts.h"
#include "solar_os_credentials.h"
#include "solar_os_identity.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
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
/* Longer than the slowest LoRa profile takes to put an announce on the air. */
constexpr int64_t kAnnounceWaitUs = 30LL * 1000000LL;
/* How long removal waits for the worker to withdraw a destination. */
constexpr int64_t kReleaseWaitUs = 5LL * 1000000LL;
/* A link that has carried no message for this long is closed, as the LXMF
 * router closes its own: an open link costs keepalives on a slow channel. */
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
/*
 * Stamp candidates tried per pass of the Reticulum loop, about 40 ms on an
 * ESP32-S3, so a stamp that takes seconds does not hold up the radio.
 */
constexpr uint32_t kStampSliceAttempts = 2048U;
constexpr const char *kPropagationAspect = "propagation";
constexpr const char *kIdentityLabel = "identity";
constexpr size_t kPrivateKeySize = 64U;

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
    /* The tries for a link ran out and it is going to a propagation node,
     * to be held there for the recipient. */
    bool propagated;
    /* The recipient hash and the encrypted rest, then the stamp once it is
     * found: what the propagation node is given. */
    RNS::Bytes propagation_message;
    bool stamped;
    int64_t next_attempt_us;
    uint64_t timestamp_ms;
    RNS::Bytes destination;
};

/*
 * One LXMF instance: an identity of its own, its delivery destination, and
 * the one message it is sending. Slots are allocated once and never freed,
 * so a callback or receipt that outlives an instance finds an empty slot or
 * a different ID, never freed memory.
 */
struct Instance {
    /* Guarded by lock. */
    bool in_use;
    bool removing;
    solar_os_messaging_provider_id_t id;
    solar_os_lxmf_config_t config;
    bool announce_requested;
    uint32_t announces_done;
    esp_err_t announce_result;
    bool listening;
    /* Why the identity could not be loaded; the worker waits on it until
     * a new one is made rather than retrying. */
    esp_err_t identity_error;
    char destination_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    solar_os_lxmf_status_t counters;
    /* Worker thread only. */
    RNS::Identity identity{RNS::Type::NONE};
    RNS::Destination inbound{RNS::Type::NONE};
    Pending pending;
    /* The message being sent, at the size the messaging service holds. */
    char *pending_body;
    /* The one link this sends on, and whose it is. Kept after a message so
     * the next to the same peer does not pay for another. */
    RNS::Link direct{RNS::Type::NONE};
    RNS::Bytes direct_peer;
    RNS::Bytes sending_resource;
    /* The link to the propagation node, kept like the direct one. */
    RNS::Link propagation_link{RNS::Type::NONE};
    RNS::Bytes propagation_peer;
    /* The stamp being searched for, a slice per pass. */
    solar_os_lxmf_stamp_t stamp;
    bool stamp_open;
    uint8_t stamp_candidate[SOLAR_OS_LXMF_STAMP_SIZE];
    uint32_t stamp_cost;
    int64_t last_process_us;
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

class PropagationAnnounces : public RNS::AnnounceHandler {
public:
    PropagationAnnounces()
        : RNS::AnnounceHandler(SOLAR_OS_LXMF_APP_NAME ".propagation")
    {
    }
    void received_announce(const RNS::Bytes &destination_hash,
                           const RNS::Identity &announced_identity,
                           const RNS::Bytes &app_data) override;
};

/*
 * The propagation node messages go to: the nearest active one heard, as the
 * LXMF router picks one when none is set. Worker thread only, but for the
 * copy in propagation_status, which is guarded by lock.
 */
struct PropagationNode {
    RNS::Bytes destination;
    solar_os_lxmf_propagation_node_t info;
    uint8_t hops;
};

SemaphoreHandle_t lock = nullptr;
StaticSemaphore_t lock_storage;
/* Serialises profile edits, which write files, without holding lock. */
SemaphoreHandle_t edit_lock = nullptr;
StaticSemaphore_t edit_lock_storage;
bool class_registered = false;
Instance *slots = nullptr;
/* The Reticulum job is running; each instance listens while it is enabled. */
bool job_attached = false;
RNS::HAnnounceHandler announce_handler;
RNS::HAnnounceHandler propagation_handler;
PropagationNode propagation_node;
struct {
    char hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    uint8_t hops;
    uint32_t stamp_cost;
} propagation_status;
/* The message being taken in. Receiving is one at a time on the worker. */
EXT_RAM_BSS_ATTR char inbound_body[SOLAR_OS_MESSAGING_BODY_MAX];

class Guard {
public:
    Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(lock); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
};

class EditGuard {
public:
    EditGuard() { xSemaphoreTake(edit_lock, portMAX_DELAY); }
    ~EditGuard() { xSemaphoreGive(edit_lock); }
    EditGuard(const EditGuard &) = delete;
    EditGuard &operator=(const EditGuard &) = delete;
};

esp_err_t ensure_slots()
{
    if (lock == nullptr) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
        edit_lock = xSemaphoreCreateMutexStatic(&edit_lock_storage);
    }
    if (slots != nullptr) {
        return ESP_OK;
    }
    void *memory = solar_os_memory_calloc(SOLAR_OS_LXMF_INSTANCE_MAX,
                                          sizeof(Instance),
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "lxmf.instances");
    char *bodies = static_cast<char *>(solar_os_memory_calloc(
        SOLAR_OS_LXMF_INSTANCE_MAX, SOLAR_OS_MESSAGING_BODY_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "lxmf.pending"));
    if (memory == nullptr || bodies == nullptr) {
        solar_os_memory_free(memory);
        solar_os_memory_free(bodies);
        return ESP_ERR_NO_MEM;
    }
    Instance *made = static_cast<Instance *>(memory);
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        new (&made[i]) Instance();
        made[i].pending_body = &bodies[i * SOLAR_OS_MESSAGING_BODY_MAX];
    }
    slots = made;
    return ESP_OK;
}

/* Callers hold lock. */
Instance *find_locked(solar_os_messaging_provider_id_t id)
{
    for (size_t i = 0; slots != nullptr && i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        if (slots[i].in_use && slots[i].id == id) {
            return &slots[i];
        }
    }
    return nullptr;
}

/* Worker thread only: the instance whose delivery destination this is. */
Instance *find_by_destination(const RNS::Bytes &hash)
{
    for (size_t i = 0; slots != nullptr && i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        Instance &in = slots[i];
        if (in.in_use && !in.removing && in.inbound && in.inbound.hash() == hash) {
            return &in;
        }
    }
    return nullptr;
}

void display_name(const Instance &in, char *out, size_t out_len)
{
    {
        Guard guard;
        if (in.config.display_name[0] != '\0') {
            strlcpy(out, in.config.display_name, out_len);
            return;
        }
    }
    solar_os_identity_get_hostname(out, out_len);
    if (out[0] == '\0') {
        strlcpy(out, "SolarOS", out_len);
    }
}

RNS::Bytes announce_data(const Instance &in)
{
    char name[SOLAR_OS_LXMF_NAME_MAX + 1U];
    display_name(in, name, sizeof(name));
    uint8_t app_data[SOLAR_OS_LXMF_ANNOUNCE_MAX];
    const size_t length =
        solar_os_lxmf_pack_announce(name, app_data, sizeof(app_data));
    return length > 0U ? RNS::Bytes(app_data, length) : RNS::Bytes();
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

bool is_group(const Instance &in, const char *peer_hex)
{
    Guard guard;
    for (size_t i = 0; i < SOLAR_OS_LXMF_GROUP_CAPACITY; i++) {
        if (strcmp(in.config.groups[i], peer_hex) == 0) {
            return true;
        }
    }
    return false;
}

/* Records the peer so the chat and contacts apps can address it. */
esp_err_t remember_peer(solar_os_messaging_provider_id_t id,
                        const RNS::Bytes &destination_hash,
                        const char *name,
                        solar_os_contact_id_t *contact_id,
                        solar_os_endpoint_id_t *endpoint_id)
{
    char fallback[SOLAR_OS_LXMF_HASH_HEX_LEN];
    hash_hex(destination_hash, fallback, sizeof(fallback));
    return solar_os_contacts_upsert_discovered(
        id,
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

/* Every listening instance hears the same network, so each learns the peer. */
void DeliveryAnnounces::received_announce(const RNS::Bytes &destination_hash,
                                          const RNS::Identity &,
                                          const RNS::Bytes &app_data)
{
    char name[SOLAR_OS_LXMF_NAME_MAX + 1U] = {};
    if (!solar_os_lxmf_parse_announce(
            app_data.data(), app_data.size(), name, sizeof(name))) {
        name[0] = '\0';
    }
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        Instance &in = slots[i];
        if (!in.in_use || in.removing || !in.inbound ||
            in.inbound.hash() == destination_hash) {
            continue;
        }
        if (remember_peer(in.id, destination_hash, name, nullptr, nullptr) ==
            ESP_OK) {
            Guard guard;
            in.counters.peers++;
        }
    }
}

void PropagationAnnounces::received_announce(const RNS::Bytes &destination_hash,
                                             const RNS::Identity &,
                                             const RNS::Bytes &app_data)
{
    solar_os_lxmf_propagation_node_t info;
    if (!solar_os_lxmf_parse_propagation_announce(app_data.data(), app_data.size(),
                                                  &info)) {
        return;
    }
    const uint8_t hops = RNS::Transport::hops_to(destination_hash);
    const bool current = propagation_node.destination == destination_hash;
    if (!info.active) {
        if (!current) {
            return;
        }
        propagation_node = PropagationNode{};
    } else if (current || !propagation_node.destination ||
               hops < propagation_node.hops) {
        propagation_node.destination = destination_hash;
        propagation_node.info = info;
        propagation_node.hops = hops;
    } else {
        return;
    }
    Guard guard;
    if (propagation_node.destination) {
        hash_hex(propagation_node.destination, propagation_status.hex,
                 sizeof(propagation_status.hex));
    } else {
        propagation_status.hex[0] = '\0';
    }
    propagation_status.hops = propagation_node.hops;
    propagation_status.stamp_cost = propagation_node.info.stamp_cost;
}

void count_rejected(Instance &in)
{
    Guard guard;
    in.counters.rejected++;
}

/* A message from its sender hash on: the recipient hash is implied. */
void receive_message(Instance &in, const RNS::Bytes &data)
{
    if (data.size() <= kOverhead || data.size() > kMessageMax) {
        count_rejected(in);
        return;
    }
    const RNS::Bytes source_hash = data.mid(0U, SOLAR_OS_LXMF_HASH_LEN);
    const RNS::Bytes signature =
        data.mid(SOLAR_OS_LXMF_HASH_LEN, SOLAR_OS_LXMF_SIGNATURE_LEN);
    const uint8_t *payload_bytes = data.data() + kOverhead;
    const size_t payload_len = data.size() - kOverhead;

    solar_os_lxmf_payload_t payload = {};
    if (!solar_os_lxmf_unpack_payload(payload_bytes, payload_len, &payload)) {
        count_rejected(in);
        SOLAR_OS_LOGW(TAG, "dropped a malformed message");
        return;
    }

    RNS::Identity source = RNS::Identity::recall(source_hash);
    if (!source) {
        count_rejected(in);
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
        count_rejected(in);
        return;
    }
    RNS::Bytes hashed_part(in.inbound.hash());
    hashed_part.append(source_hash);
    hashed_part.append(signed_payload);
    RNS::Bytes signed_part(hashed_part);
    signed_part.append(RNS::Identity::full_hash(hashed_part));
    if (!source.validate(signature, signed_part)) {
        count_rejected(in);
        SOLAR_OS_LOGW(TAG, "dropped a message with an invalid signature");
        return;
    }

    char peer_hex[SOLAR_OS_LXMF_HASH_HEX_LEN];
    hash_hex(source_hash, peer_hex, sizeof(peer_hex));
    const bool room = is_group(in, peer_hex);
    solar_os_contact_id_t contact_id = 0;
    solar_os_endpoint_id_t endpoint_id = 0;
    solar_os_endpoint_t endpoint = {};
    const char *name = peer_hex;
    if (solar_os_contacts_find_endpoint(in.id,
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
        (void)remember_peer(in.id, source_hash, nullptr, &contact_id,
                            &endpoint_id);
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
        .provider = in.id,
        .conversation_key = peer_hex,
        .conversation_kind = room ? SOLAR_OS_CONVERSATION_ROOM :
                                    SOLAR_OS_CONVERSATION_DIRECT,
        .conversation_title = name,
        .contact_id = contact_id,
        .endpoint_id = endpoint_id,
        .group_ref = 0U,
        .provider_message_key = 0U,
        .timestamp_ms = (uint64_t)(payload.timestamp_s * 1000.0),
        /* A group server signs what it relays; who wrote it first is the
         * server's word, not something this can check. */
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
                          SOLAR_OS_SECURITY_PEER_KEY_KNOWN |
                          SOLAR_OS_SECURITY_TRANSPORT_SECURED |
                          (room ? SOLAR_OS_SECURITY_SENDER_UNVERIFIED : 0U),
        .sender = name,
        .body = body,
        .truncated = truncated,
    };
    bool inserted = false;
    const esp_err_t error =
        solar_os_messaging_publish_inbound(&message, &inserted, nullptr);
    Guard guard;
    if (error != ESP_OK) {
        in.counters.last_error = error;
        SOLAR_OS_LOGW(TAG, "message not stored: %s", esp_err_to_name(error));
        return;
    }
    if (inserted) {
        in.counters.received++;
    }
}

/* An opportunistic packet leaves the recipient hash out, since it is the
 * destination the packet was addressed to. */
void receive(const RNS::Bytes &data, const RNS::Packet &packet)
{
    Instance *in = find_by_destination(packet.destination_hash());
    if (in != nullptr) {
        receive_message(*in, data);
    }
}

/*
 * Over a link the whole message arrives, recipient hash first. This is how
 * an LXMF client sends by default, so it has to be taken; the link proves
 * the packet, because the delivery destination proves everything. The
 * recipient hash also says which instance it is for.
 */
void receive_whole(const RNS::Bytes &data)
{
    if (data.size() <= SOLAR_OS_LXMF_HASH_LEN) {
        return;
    }
    Instance *in = find_by_destination(data.mid(0U, SOLAR_OS_LXMF_HASH_LEN));
    if (in == nullptr) {
        return;
    }
    receive_message(*in, data.mid(SOLAR_OS_LXMF_HASH_LEN));
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
    Instance *in = find_by_destination(
        advertisement.get_link().destination().hash());
    if (in != nullptr) {
        count_rejected(*in);
    }
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
    Instance *in = find_by_destination(link.destination().hash());
    if (in != nullptr) {
        Guard guard;
        in->counters.links++;
    }
}

void end_stamp(Instance &in)
{
    if (in.stamp_open) {
        solar_os_lxmf_stamp_end(&in.stamp);
        in.stamp_open = false;
    }
}

/* Worker thread only. A receipt that outlives its instance finds no match. */
void conclude(solar_os_messaging_provider_id_t id, uint32_t request_id,
              solar_os_delivery_state_t state, const char *error)
{
    Instance *in = nullptr;
    {
        Guard guard;
        in = find_locked(id);
    }
    if (in == nullptr || in->removing || !in->pending.active ||
        in->pending.request_id != request_id) {
        return;
    }
    in->pending.active = false;
    in->pending.concluded = true;
    end_stamp(*in);
    (void)solar_os_messaging_outbox_update(request_id, state, error);
    Guard guard;
    if (state == SOLAR_OS_DELIVERY_DELIVERED) {
        in->counters.delivered++;
    } else if (state == SOLAR_OS_DELIVERY_SENT) {
        in->counters.propagated++;
    } else if (state == SOLAR_OS_DELIVERY_FAILED) {
        in->counters.failed++;
    }
}

void fail(Instance &in, uint32_t request_id, const char *reason)
{
    conclude(in.id, request_id, SOLAR_OS_DELIVERY_FAILED, reason);
}

void count_sent(Instance &in)
{
    Guard guard;
    in.counters.sent++;
}

/*
 * The message as its sender signs it: sender hash, signature, payload. An
 * opportunistic packet carries exactly this, and a link carries it with the
 * recipient hash in front. Built from the time the message was queued, so
 * that every attempt is the same message to whoever receives two of them.
 */
bool build_message(Instance &in, RNS::Bytes &message)
{
    const size_t room = strlen(in.pending_body) + 32U;
    RNS::Bytes payload;
    uint8_t *packed = payload.writable(room);
    if (packed == nullptr) {
        return false;
    }
    const size_t payload_len = solar_os_lxmf_pack_payload(
        (double)in.pending.timestamp_ms / 1000.0, "", in.pending_body, packed,
        room);
    if (payload_len == 0U) {
        return false;
    }
    payload.resize(payload_len);

    RNS::Bytes hashed_part(in.pending.destination);
    hashed_part.append(in.inbound.hash());
    hashed_part.append(payload);
    RNS::Bytes signed_part(hashed_part);
    signed_part.append(RNS::Identity::full_hash(hashed_part));

    message = in.inbound.hash();
    message.append(in.inbound.identity().sign(signed_part));
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
bool transmit(Instance &in)
{
    RNS::Identity peer = RNS::Identity::recall(in.pending.destination);
    if (!peer) {
        RNS::Transport::request_path(in.pending.destination);
        return false;
    }
    RNS::Bytes wire;
    if (!build_message(in, wire)) {
        fail(in, in.pending.request_id, "Reticulum message could not be packed");
        return false;
    }
    RNS::Destination out = delivery_destination(peer);

    const solar_os_messaging_provider_id_t id = in.id;
    const uint32_t request_id = in.pending.request_id;
    RNS::Packet packet(out, wire);
    RNS::PacketReceipt receipt = packet.receipt_send();
    if (!receipt) {
        /* No interface would take it; there may be one by the next try. */
        return false;
    }
    count_sent(in);
    receipt.set_timeout(kProofTimeoutSeconds);
    receipt.set_delivery_handler([id, request_id](const RNS::PacketReceipt &) {
        conclude(id, request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
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
void process_opportunistic(Instance &in)
{
    Pending &pending = in.pending;
    const int64_t now = esp_timer_get_time();
    if (pending.attempts > kMaxDeliveryAttempts) {
        fail(in, pending.request_id, "no answer from the Reticulum peer");
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
        (void)transmit(in);
    }
}

void drop_link(Instance &in)
{
    if (in.direct) {
        try {
            in.direct.teardown();
        } catch (const std::exception &failure) {
            SOLAR_OS_LOGW(TAG, "link not closed: %s", failure.what());
        }
    }
    in.direct = RNS::Link({RNS::Type::NONE});
    in.direct_peer = RNS::Bytes();
    in.sending_resource = RNS::Bytes();
}

/* The outbound job waits out its interval; a link that has come up is
 * what it was waiting for. */
void link_ready(RNS::Link &link)
{
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        Instance &in = slots[i];
        if (in.in_use && ((in.direct && in.direct.hash() == link.hash()) ||
                          (in.propagation_link &&
                           in.propagation_link.hash() == link.hash()))) {
            Guard guard;
            in.counters.links++;
            in.last_process_us = 0;
            return;
        }
    }
}

/* How a message sent as a resource ended. */
void resource_sent(const RNS::Resource &resource)
{
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        Instance &in = slots[i];
        if (!in.in_use || in.removing || !in.pending.active ||
            !in.pending.sending || in.sending_resource != resource.hash()) {
            continue;
        }
        in.sending_resource = RNS::Bytes();
        if (resource.status() == RNS::Type::Resource::COMPLETE) {
            /* A propagation node holds it; only the recipient delivers. */
            conclude(in.id, in.pending.request_id,
                     in.pending.propagated ? SOLAR_OS_DELIVERY_SENT :
                                             SOLAR_OS_DELIVERY_DELIVERED,
                     nullptr);
            return;
        }
        in.pending.sending = false;
        in.pending.link_failed = true;
        in.pending.next_attempt_us = esp_timer_get_time() + kDeliveryRetryWaitUs;
        return;
    }
}

/*
 * Sends the message on the link that is up: in one packet if it fits, as a
 * resource if it does not. Not compressed, since the only compression a peer
 * would undo is one this does not have.
 */
void send_over_link(Instance &in)
{
    RNS::Bytes message;
    if (!build_message(in, message)) {
        fail(in, in.pending.request_id, "Reticulum message could not be packed");
        return;
    }
    RNS::Bytes wire(in.pending.destination);
    wire.append(message);
    if (wire.size() > kMessageMax) {
        /* Permanent: no number of attempts shortens a message. */
        fail(in, in.pending.request_id, "Reticulum message is too long");
        return;
    }
    const solar_os_messaging_provider_id_t id = in.id;
    const uint32_t request_id = in.pending.request_id;
    if (wire.size() <= SOLAR_OS_LXMF_LINK_PACKET_MAX) {
        RNS::Packet packet(in.direct, wire);
        RNS::PacketReceipt receipt = packet.receipt_send();
        if (!receipt) {
            return;
        }
        receipt.set_timeout(kProofTimeoutSeconds);
        receipt.set_delivery_handler([id, request_id](const RNS::PacketReceipt &) {
            conclude(id, request_id, SOLAR_OS_DELIVERY_DELIVERED, nullptr);
        });
        receipt.set_timeout_handler([id, request_id](const RNS::PacketReceipt &) {
            Instance *owner = nullptr;
            {
                Guard guard;
                owner = find_locked(id);
            }
            if (owner != nullptr && owner->pending.active &&
                owner->pending.request_id == request_id) {
                owner->pending.sending = false;
                owner->pending.link_failed = true;
            }
        });
    } else {
        RNS::Resource resource(wire, in.direct, true, false, resource_sent);
        if (!resource) {
            return;
        }
        in.sending_resource = resource.hash();
    }
    count_sent(in);
    in.pending.sending = true;
}

void drop_propagation_link(Instance &in)
{
    if (in.propagation_link) {
        try {
            in.propagation_link.teardown();
        } catch (const std::exception &failure) {
            SOLAR_OS_LOGW(TAG, "link not closed: %s", failure.what());
        }
    }
    in.propagation_link = RNS::Link({RNS::Type::NONE});
    in.propagation_peer = RNS::Bytes();
    in.sending_resource = RNS::Bytes();
}

RNS::Destination propagation_destination(const RNS::Identity &node)
{
    return RNS::Destination(node,
                            RNS::Type::Destination::OUT,
                            RNS::Type::Destination::SINGLE,
                            SOLAR_OS_LXMF_APP_NAME,
                            kPropagationAspect);
}

/*
 * Encrypts the message for its recipient, as only the recipient may read
 * what a propagation node holds, and starts the search for its stamp. False
 * while the recipient's identity, which the encryption needs, is asked for.
 */
bool prepare_propagation(Instance &in)
{
    RNS::Identity peer = RNS::Identity::recall(in.pending.destination);
    if (!peer) {
        RNS::Transport::request_path(in.pending.destination);
        return false;
    }
    RNS::Bytes message;
    if (!build_message(in, message)) {
        fail(in, in.pending.request_id, "Reticulum message could not be packed");
        return false;
    }
    RNS::Bytes lxmf_data(in.pending.destination);
    lxmf_data.append(delivery_destination(peer).encrypt(message));
    const RNS::Bytes transient_id = RNS::Identity::full_hash(lxmf_data);
    end_stamp(in);
    if (solar_os_lxmf_stamp_begin(&in.stamp, transient_id.data(),
                                  transient_id.size(),
                                  SOLAR_OS_LXMF_STAMP_ROUNDS_PROPAGATION) != ESP_OK ||
        solar_os_crypto_random(in.stamp_candidate,
                               sizeof(in.stamp_candidate)) != ESP_OK) {
        end_stamp(in);
        fail(in, in.pending.request_id, "propagation stamp could not be made");
        return false;
    }
    in.stamp_open = true;
    in.stamp_cost = propagation_node.info.stamp_cost;
    in.pending.propagation_message = lxmf_data;
    in.pending.attempts = 0U;
    return true;
}

/* One slice of the stamp search. Runs on every pass of the Reticulum loop,
 * not on the outbound job's interval. */
void stamp_step(Instance &in)
{
    uint32_t attempts = 0;
    const esp_err_t error = solar_os_lxmf_stamp_search(
        &in.stamp, in.stamp_cost, in.stamp_candidate, kStampSliceAttempts,
        &attempts);
    if (error == ESP_ERR_NOT_FOUND) {
        return;
    }
    end_stamp(in);
    if (error != ESP_OK) {
        fail(in, in.pending.request_id, "propagation stamp could not be made");
        return;
    }
    in.pending.propagation_message.append(in.stamp_candidate,
                                          sizeof(in.stamp_candidate));
    in.pending.stamped = true;
    in.last_process_us = 0;
}

/* Gives the stamped message to the propagation node: in one packet if it
 * fits, as a resource if it does not. */
void send_to_propagation_node(Instance &in)
{
    const RNS::Bytes &message = in.pending.propagation_message;
    const size_t room = message.size() + 16U;
    RNS::Bytes wire;
    uint8_t *out = wire.writable(room);
    const size_t length = out == nullptr ? 0U :
        solar_os_lxmf_pack_propagation((double)now_ms() / 1000.0,
                                       message.data(), message.size(), out,
                                       room);
    if (length == 0U) {
        fail(in, in.pending.request_id, "Reticulum message could not be packed");
        return;
    }
    wire.resize(length);
    const solar_os_messaging_provider_id_t id = in.id;
    const uint32_t request_id = in.pending.request_id;
    if (wire.size() <= SOLAR_OS_LXMF_LINK_PACKET_MAX) {
        RNS::Packet packet(in.propagation_link, wire);
        RNS::PacketReceipt receipt = packet.receipt_send();
        if (!receipt) {
            return;
        }
        receipt.set_timeout(kProofTimeoutSeconds);
        receipt.set_delivery_handler([id, request_id](const RNS::PacketReceipt &) {
            conclude(id, request_id, SOLAR_OS_DELIVERY_SENT, nullptr);
        });
        receipt.set_timeout_handler([id, request_id](const RNS::PacketReceipt &) {
            Instance *owner = nullptr;
            {
                Guard guard;
                owner = find_locked(id);
            }
            if (owner != nullptr && owner->pending.active &&
                owner->pending.request_id == request_id) {
                owner->pending.sending = false;
                owner->pending.link_failed = true;
            }
        });
    } else {
        RNS::Resource resource(wire, in.propagation_link, true, false, resource_sent);
        if (!resource) {
            return;
        }
        in.sending_resource = resource.hash();
    }
    count_sent(in);
    in.pending.sending = true;
}

/*
 * One step of a message going to a propagation node, as the LXMF router
 * sends one: encrypted for the recipient and stamped, then given to the node
 * over a link. A node that turns the stamp down closes the link, which this
 * counts as a failed try. Taken is all a sender learns: the node holds the
 * message until the recipient asks for it.
 */
void process_propagated(Instance &in)
{
    Pending &pending = in.pending;
    const int64_t now = esp_timer_get_time();
    if (!pending.propagation_message) {
        if (now <= pending.next_attempt_us) {
            return;
        }
        if (pending.attempts >= kMaxDeliveryAttempts) {
            fail(in, pending.request_id, "the Reticulum peer is unknown");
            return;
        }
        pending.attempts++;
        pending.next_attempt_us = now + kPathRequestWaitUs;
        (void)prepare_propagation(in);
        return;
    }
    if (!pending.stamped) {
        return;
    }
    if (in.propagation_link &&
        (pending.link_failed || in.propagation_peer != propagation_node.destination)) {
        drop_propagation_link(in);
    }
    pending.link_failed = false;
    if (in.propagation_link) {
        switch (in.propagation_link.status()) {
        case RNS::Type::Link::ACTIVE:
            if (!pending.sending && now > pending.next_attempt_us) {
                send_to_propagation_node(in);
            }
            return;
        case RNS::Type::Link::CLOSED:
            drop_propagation_link(in);
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
    if (!propagation_node.destination) {
        fail(in, pending.request_id, "no propagation node to hold the message");
        return;
    }
    if (pending.propagation_message.size() + 32U >
        (size_t)propagation_node.info.transfer_limit_kb * 1000U) {
        fail(in, pending.request_id, "too long for the propagation node");
        return;
    }
    if (pending.attempts >= kMaxDeliveryAttempts) {
        fail(in, pending.request_id, "the propagation node did not take the message");
        return;
    }
    pending.attempts++;
    pending.next_attempt_us = now + kDeliveryRetryWaitUs;
    RNS::Identity node = RNS::Identity::recall(propagation_node.destination);
    if (!node || !RNS::Transport::has_path(propagation_node.destination)) {
        RNS::Transport::request_path(propagation_node.destination);
        pending.next_attempt_us = now + kPathRequestWaitUs;
        return;
    }
    in.propagation_link = RNS::Link(propagation_destination(node), link_ready);
    in.propagation_peer = propagation_node.destination;
}

/*
 * One step of the outbound job. Mirrors the direct branch of the LXMF
 * router: use the link to the peer if there is one, open one if there is a
 * path, ask for a path if there is not, and count the tries. A message that
 * fits one packet is sent without a link once the tries for one run out,
 * which is what reaches a peer that can hear a packet and not hold a link.
 */
void process_outbound(Instance &in)
{
    Pending &pending = in.pending;
    if (pending.propagated) {
        process_propagated(in);
        return;
    }
    if (pending.opportunistic) {
        process_opportunistic(in);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (in.direct && (pending.link_failed ||
                      in.direct_peer != pending.destination)) {
        drop_link(in);
    }
    pending.link_failed = false;
    if (in.direct) {
        switch (in.direct.status()) {
        case RNS::Type::Link::ACTIVE:
            if (!pending.sending && now > pending.next_attempt_us) {
                send_over_link(in);
            }
            return;
        case RNS::Type::Link::CLOSED:
            drop_link(in);
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
        /* A recipient that holds no link is likely away: a propagation node
         * keeps the message for it, where a lone packet would be lost. */
        if (propagation_node.destination) {
            pending.propagated = true;
            pending.attempts = 0U;
            pending.next_attempt_us = 0;
            process_propagated(in);
            return;
        }
        if (strlen(in.pending_body) + kOverhead + 16U <= SOLAR_OS_LXMF_PACKET_MAX) {
            pending.opportunistic = true;
            pending.attempts = 0U;
            pending.next_attempt_us = 0;
            process_opportunistic(in);
        } else {
            fail(in, pending.request_id, "no link to the Reticulum peer");
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
    in.direct = RNS::Link(delivery_destination(peer), link_ready);
    in.direct_peer = pending.destination;
}

void start_next(Instance &in)
{
    solar_os_messaging_outbound_t outbound = {};
    if (solar_os_messaging_outbox_peek(in.id, &outbound) != ESP_OK) {
        return;
    }
    in.pending = Pending{};
    in.pending.active = true;
    in.pending.request_id = outbound.id;
    in.pending.timestamp_ms = now_ms();
    strlcpy(in.pending_body, outbound.body, SOLAR_OS_MESSAGING_BODY_MAX);

    solar_os_messaging_conversation_t conversation = {};
    if (solar_os_messaging_conversation_get(outbound.conversation_id,
                                            &conversation) != ESP_OK ||
        !hex_to_hash(conversation.provider_key, in.pending.destination)) {
        fail(in, outbound.id, "Reticulum conversation has no destination");
        return;
    }
    (void)solar_os_messaging_outbox_update(
        outbound.id, SOLAR_OS_DELIVERY_SENDING, nullptr);
    process_outbound(in);
}

/* Stores a new identity for the instance; replace says whether one there
 * already may be overwritten. */
esp_err_t make_identity(solar_os_messaging_provider_id_t id, bool replace)
{
    try {
        RNS::Identity fresh;
        return solar_os_credentials_put(
            id, SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY, kIdentityLabel,
            fresh.get_private_key().data(), fresh.get_private_key().size(),
            replace, nullptr);
    } catch (const std::exception &) {
        return ESP_FAIL;
    }
}

/* An instance's own identity, made when the instance was. */
esp_err_t read_identity(solar_os_messaging_provider_id_t id, RNS::Identity &out)
{
    solar_os_credential_info_t record = {};
    esp_err_t error = solar_os_credentials_find(
        id, SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY, kIdentityLabel, &record);
    if (error != ESP_OK) {
        return error;
    }
    uint8_t secret[kPrivateKeySize];
    size_t length = 0U;
    error = solar_os_credentials_read_secret(record.id, secret, sizeof(secret),
                                             &length);
    if (error == ESP_OK && length != kPrivateKeySize) {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK) {
        RNS::Identity loaded(false);
        if (loaded.load_private_key(RNS::Bytes(secret, sizeof(secret)))) {
            out = loaded;
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
    }
    solar_os_credentials_wipe(secret, sizeof(secret));
    return error;
}

bool any_listening()
{
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        if (slots[i].in_use && slots[i].inbound) {
            return true;
        }
    }
    return false;
}

/* Registers the delivery destination. Worker thread only. */
esp_err_t listen(Instance &in)
{
    if (!in.identity) {
        const esp_err_t error = read_identity(in.id, in.identity);
        if (error != ESP_OK) {
            solar_os_messaging_provider_status_t status = {};
            (void)solar_os_messaging_provider_get_status(in.id, &status);
            const char *name = status.name;
            SOLAR_OS_LOGE(TAG, "%s: identity not in %s (%s)", name,
                          solar_os_credentials_location(),
                          esp_err_to_name(error));
            SOLAR_OS_LOGE(TAG, "%s: restore that file, or: "
                          "reticulum lxmf identity generate %s", name, name);
            (void)solar_os_messaging_provider_set_status(
                in.id, false, false, error,
                "identity missing; see: reticulum lxmf status");
            Guard guard;
            in.identity_error = error;
            return error;
        }
    }
    try {
        if (!announce_handler) {
            announce_handler = std::make_shared<DeliveryAnnounces>();
            RNS::Transport::register_announce_handler(announce_handler);
        }
        if (!propagation_handler) {
            propagation_handler = std::make_shared<PropagationAnnounces>();
            RNS::Transport::register_announce_handler(propagation_handler);
        }
        in.inbound = RNS::Destination(in.identity,
                                      RNS::Type::Destination::IN,
                                      RNS::Type::Destination::SINGLE,
                                      SOLAR_OS_LXMF_APP_NAME,
                                      SOLAR_OS_LXMF_ASPECT);
        in.inbound.set_packet_callback(receive);
        in.inbound.set_link_established_callback(link_established);
        in.inbound.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
        in.inbound.set_default_app_data(announce_data(in));
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "attach failed: %s", failure.what());
        in.inbound = RNS::Destination({RNS::Type::NONE});
        Guard guard;
        in.counters.last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    in.pending = Pending{};
    {
        Guard guard;
        in.listening = true;
        /* A destination nobody has heard of is unreachable: announce it. */
        in.announce_requested = true;
        hash_hex(in.inbound.hash(), in.destination_hex,
                 sizeof(in.destination_hex));
    }
    (void)solar_os_messaging_provider_set_status(in.id, true, true, ESP_OK,
                                                 nullptr);
    return ESP_OK;
}

/* Worker thread only. */
void stop_listening(Instance &in, const char *detail)
{
    if (!in.inbound) {
        return;
    }
    if (in.pending.active) {
        /* Stopping is not a delivery failure. The message goes back on the
         * queue so it is tried again when the instance returns. */
        (void)solar_os_messaging_outbox_update(
            in.pending.request_id, SOLAR_OS_DELIVERY_QUEUED, nullptr);
        in.pending = Pending{};
    }
    drop_link(in);
    drop_propagation_link(in);
    end_stamp(in);
    try {
        RNS::Transport::deregister_destination(in.inbound);
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "detach failed: %s", failure.what());
    }
    in.inbound = RNS::Destination({RNS::Type::NONE});
    /* Read again on the next listen, in case it has been replaced. */
    in.identity = RNS::Identity({RNS::Type::NONE});
    {
        Guard guard;
        in.listening = false;
        in.destination_hex[0] = '\0';
    }
    if (!any_listening() && announce_handler) {
        try {
            RNS::Transport::deregister_announce_handler(announce_handler);
        } catch (const std::exception &failure) {
            SOLAR_OS_LOGE(TAG, "detach failed: %s", failure.what());
        }
        announce_handler.reset();
    }
    if (!any_listening() && propagation_handler) {
        try {
            RNS::Transport::deregister_announce_handler(propagation_handler);
        } catch (const std::exception &failure) {
            SOLAR_OS_LOGE(TAG, "detach failed: %s", failure.what());
        }
        propagation_handler.reset();
    }
    if (!in.removing) {
        (void)solar_os_messaging_provider_set_status(in.id, false, false,
                                                     ESP_OK, detail);
    }
}

void announce(Instance &in)
{
    const RNS::Bytes app_data = announce_data(in);
    esp_err_t result = ESP_FAIL;
    try {
        if (!app_data.empty()) {
            /* Path responses carry the default; keep it the latest name. */
            in.inbound.set_default_app_data(app_data);
            in.inbound.announce(app_data);
            result = ESP_OK;
        }
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "announce failed: %s", failure.what());
    }
    Guard guard;
    if (result == ESP_OK) {
        in.counters.announces_sent++;
    }
    in.announce_result = result;
    in.announces_done++;
}

void tick_instance(Instance &in)
{
    bool want = false;
    bool announce_now = false;
    {
        Guard guard;
        want = !in.removing && in.config.enabled;
        announce_now = in.announce_requested && in.inbound;
        if (announce_now) {
            in.announce_requested = false;
        }
    }
    /* Connect, disconnect and removal only change the profile; the worker
     * owns the destination and follows it here. */
    if (want && !in.inbound) {
        {
            Guard guard;
            if (in.identity_error != ESP_OK) {
                return;
            }
        }
        if (listen(in) != ESP_OK) {
            return;
        }
        Guard guard;
        announce_now = in.announce_requested;
        in.announce_requested = false;
    } else if (!want && in.inbound) {
        stop_listening(in, "disconnected");
    }
    if (!in.inbound) {
        return;
    }
    try {
        if (announce_now) {
            announce(in);
        }
        if (in.pending.active && in.pending.propagated && in.stamp_open) {
            stamp_step(in);
        }
        /* The outbound job runs on its own interval, as the LXMF router's
         * does, rather than on every pass of the Reticulum loop. */
        const int64_t now = esp_timer_get_time();
        if (now - in.last_process_us >= kProcessingIntervalUs) {
            in.last_process_us = now;
            if (in.pending.active) {
                process_outbound(in);
            }
            if (!in.pending.active) {
                start_next(in);
            }
            if (!in.pending.active && in.direct &&
                (in.direct.status() == RNS::Type::Link::CLOSED ||
                 in.direct.no_data_for() > kLinkIdleSeconds)) {
                drop_link(in);
            }
            if (!in.pending.active && in.propagation_link &&
                (in.propagation_link.status() == RNS::Type::Link::CLOSED ||
                 in.propagation_link.no_data_for() > kLinkIdleSeconds)) {
                drop_propagation_link(in);
            }
        }
    } catch (const std::exception &failure) {
        SOLAR_OS_LOGE(TAG, "tick failed: %s", failure.what());
        {
            Guard guard;
            in.counters.last_error = ESP_FAIL;
        }
        if (in.pending.active) {
            fail(in, in.pending.request_id, "Reticulum send failed");
        }
    }
}

}  // namespace

namespace solar_os {
namespace lxmf {

esp_err_t attach(const RNS::Identity &)
{
    const esp_err_t error = ensure_slots();
    if (error != ESP_OK) {
        return error;
    }
    job_attached = true;
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        Guard guard;
        if (slots[i].in_use && !slots[i].config.enabled) {
            (void)solar_os_messaging_provider_set_status(
                slots[i].id, false, false, ESP_OK, "disconnected");
        }
    }
    return ESP_OK;
}

void detach()
{
    if (!job_attached || slots == nullptr) {
        return;
    }
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        if (slots[i].in_use) {
            stop_listening(slots[i], nullptr);
        }
    }
    job_attached = false;
}

void request_announce()
{
    if (slots == nullptr) {
        return;
    }
    Guard guard;
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        if (slots[i].in_use) {
            slots[i].announce_requested = true;
        }
    }
}

void tick()
{
    if (!job_attached || slots == nullptr) {
        return;
    }
    for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
        bool active = false;
        {
            Guard guard;
            active = slots[i].in_use;
        }
        if (active) {
            tick_instance(slots[i]);
        } else if (slots[i].inbound) {
            stop_listening(slots[i], nullptr);
        }
    }
}

}  // namespace lxmf
}  // namespace solar_os

namespace {

bool text_valid(const char *text, size_t size)
{
    if (strnlen(text, size) >= size) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 0x20U || *p == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool config_valid(const solar_os_lxmf_config_t &config)
{
    if (config.version != 1U ||
        !text_valid(config.display_name, sizeof(config.display_name))) {
        return false;
    }
    for (size_t i = 0; i < SOLAR_OS_LXMF_GROUP_CAPACITY; i++) {
        char canonical[SOLAR_OS_LXMF_HASH_HEX_LEN];
        if (config.groups[i][0] != '\0' &&
            (!solar_os_lxmf_normalize_hash(config.groups[i], canonical,
                                           sizeof(canonical)) ||
             strcmp(canonical, config.groups[i]) != 0)) {
            return false;
        }
    }
    return true;
}

solar_os_lxmf_config_t current_config(const Instance &in)
{
    Guard guard;
    return in.config;
}

/* Caller holds edit_lock. The file is written before the profile changes,
 * so a failed save leaves both as they were. */
esp_err_t save_config(Instance &in, const solar_os_lxmf_config_t &next)
{
    const esp_err_t error =
        solar_os_messaging_profile_save(in.id, &next, sizeof(next));
    if (error == ESP_OK) {
        Guard guard;
        in.config = next;
    }
    return error;
}

/* Caller holds edit_lock. */
esp_err_t attach_instance(solar_os_messaging_provider_id_t id,
                          const char *name,
                          const solar_os_lxmf_config_t &config)
{
    Instance *in = nullptr;
    {
        Guard guard;
        if (find_locked(id) != nullptr) {
            return ESP_OK;
        }
        for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX && in == nullptr; i++) {
            /* A slot the worker has not yet released is not free. */
            if (!slots[i].in_use && !slots[i].listening) {
                in = &slots[i];
            }
        }
        if (in == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_err_t error = solar_os_messaging_instance_register(
        id, SOLAR_OS_LXMF_CLASS, name, in);
    if (error != ESP_OK) {
        return error;
    }
    {
        Guard guard;
        in->id = id;
        in->config = config;
        in->removing = false;
        in->listening = false;
        in->identity_error = ESP_OK;
        in->announce_requested = false;
        in->announces_done = 0U;
        in->announce_result = ESP_OK;
        in->destination_hex[0] = '\0';
        in->counters = solar_os_lxmf_status_t{};
        in->in_use = true;
    }
    /* Worker-owned, but the slot is not in use until now. */
    in->identity = RNS::Identity({RNS::Type::NONE});
    in->pending = Pending{};
    in->last_process_us = 0;
    for (size_t i = 0; i < SOLAR_OS_LXMF_GROUP_CAPACITY; i++) {
        if (config.groups[i][0] == '\0') {
            continue;
        }
        const solar_os_messaging_conversation_upsert_t request = {
            .provider = id,
            .provider_key = config.groups[i],
            .kind = SOLAR_OS_CONVERSATION_ROOM,
            .title = config.groups[i],
            .contact_id = 0U,
            .endpoint_id = 0U,
            .group_ref = 0U,
            .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
                              SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
        };
        (void)solar_os_messaging_conversation_upsert(&request, nullptr);
        (void)solar_os_messaging_join_update(
            id, config.groups[i], SOLAR_OS_MESSAGING_JOIN_JOINED, nullptr);
    }
    return ESP_OK;
}

esp_err_t create_instance(const char *name, const char *display,
                          solar_os_messaging_provider_id_t *id)
{
    solar_os_lxmf_config_t config = {};
    config.version = 1U;
    config.enabled = true;
    if (display != nullptr &&
        strlcpy(config.display_name, display, sizeof(config.display_name)) >=
            sizeof(config.display_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    EditGuard edit;
    {
        Guard guard;
        size_t used = 0;
        for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
            if (slots[i].in_use || slots[i].listening) {
                used++;
            }
        }
        if (used >= SOLAR_OS_LXMF_INSTANCE_MAX) {
            return ESP_ERR_NO_MEM;
        }
    }
    solar_os_messaging_provider_id_t created = 0;
    esp_err_t error = solar_os_messaging_instance_create(
        SOLAR_OS_LXMF_CLASS, name, &config, sizeof(config), &created);
    if (error != ESP_OK) {
        return error;
    }
    /* The identity is made with the instance and only then, so one that
     * goes missing later is reported rather than replaced. */
    error = make_identity(created, false);
    if (error != ESP_OK) {
        (void)solar_os_messaging_instance_remove(created);
        return error;
    }
    error = attach_instance(created, name, config);
    if (error == ESP_OK && id != nullptr) {
        *id = created;
    }
    return error;
}

esp_err_t class_create(const char *name, int argc, const char *const *argv,
                       solar_os_messaging_provider_id_t *id)
{
    if (argc > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = ensure_slots();
    if (error != ESP_OK) {
        return error;
    }
    return create_instance(name, argc == 1 ? argv[0] : nullptr, id);
}

/* configure NAME DISPLAY_NAME, or hostname to announce the device's own. */
esp_err_t class_configure(void *context, int argc, const char *const *argv)
{
    Instance &in = *static_cast<Instance *>(context);
    if (argc != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    EditGuard edit;
    solar_os_lxmf_config_t next = current_config(in);
    const char *display = strcmp(argv[0], "hostname") == 0 ? "" : argv[0];
    if (strlcpy(next.display_name, display, sizeof(next.display_name)) >=
            sizeof(next.display_name) ||
        !config_valid(next)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = save_config(in, next);
    if (error == ESP_OK) {
        /* Peers learn a name from the announce, so say it again. */
        Guard guard;
        in.announce_requested = true;
    }
    return error;
}

esp_err_t class_connect(void *context, bool enabled)
{
    Instance &in = *static_cast<Instance *>(context);
    EditGuard edit;
    solar_os_lxmf_config_t next = current_config(in);
    next.enabled = enabled;
    return save_config(in, next);
}

esp_err_t class_normalize(void *, const char *target, char *out, size_t size)
{
    return solar_os_lxmf_normalize_hash(target, out, size) ? ESP_OK :
                                                             ESP_ERR_INVALID_ARG;
}

/* A known peer's name, or false. */
bool peer_name(solar_os_messaging_provider_id_t id, const char *peer_hex,
               char *name, size_t size)
{
    RNS::Bytes hash;
    if (!hex_to_hash(peer_hex, hash)) {
        return false;
    }
    solar_os_endpoint_t endpoint = {};
    if (solar_os_contacts_find_endpoint(id, hash.data(), hash.size(),
                                        &endpoint) != ESP_OK) {
        return false;
    }
    solar_os_contact_t contact = {};
    if (solar_os_contacts_get(endpoint.contact_id, &contact) != ESP_OK) {
        return false;
    }
    strlcpy(name, contact.display_name, size);
    return name[0] != '\0';
}

/*
 * A group is a group-server peer: an LXMF destination that relays what it is
 * sent to its members. Joining makes its messages a room here and keeps it
 * across restarts; whether the server counts this node as a member is its
 * own rule, often a join command sent to it as a message.
 */
esp_err_t class_join(void *context, const char *target)
{
    Instance &in = *static_cast<Instance *>(context);
    EditGuard edit;
    solar_os_lxmf_config_t next = current_config(in);
    int empty = -1;
    for (size_t i = 0; i < SOLAR_OS_LXMF_GROUP_CAPACITY; i++) {
        if (strcmp(next.groups[i], target) == 0) {
            empty = -2;
            break;
        }
        if (next.groups[i][0] == '\0' && empty < 0) {
            empty = (int)i;
        }
    }
    if (empty == -1) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ESP_OK;
    if (empty >= 0) {
        strlcpy(next.groups[empty], target, sizeof(next.groups[empty]));
        error = save_config(in, next);
    }
    if (error != ESP_OK) {
        return error;
    }
    char name[SOLAR_OS_MESSAGING_TITLE_MAX];
    if (peer_name(in.id, target, name, sizeof(name))) {
        const solar_os_messaging_conversation_upsert_t request = {
            .provider = in.id,
            .provider_key = target,
            .kind = SOLAR_OS_CONVERSATION_ROOM,
            .title = name,
            .contact_id = 0U,
            .endpoint_id = 0U,
            .group_ref = 0U,
            .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
                              SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
        };
        (void)solar_os_messaging_conversation_upsert(&request, nullptr);
    }
    return solar_os_messaging_join_update(
        in.id, target, SOLAR_OS_MESSAGING_JOIN_JOINED, nullptr);
}

esp_err_t class_leave(void *context, const char *target)
{
    Instance &in = *static_cast<Instance *>(context);
    EditGuard edit;
    solar_os_lxmf_config_t next = current_config(in);
    bool found = false;
    for (size_t i = 0; i < SOLAR_OS_LXMF_GROUP_CAPACITY; i++) {
        if (strcmp(next.groups[i], target) == 0) {
            next.groups[i][0] = '\0';
            found = true;
        }
    }
    const esp_err_t error = found ? save_config(in, next) : ESP_OK;
    if (error != ESP_OK) {
        return error;
    }
    return solar_os_messaging_join_update(
        in.id, target, SOLAR_OS_MESSAGING_JOIN_LEFT, nullptr);
}

esp_err_t class_direct(void *context, const char *peer_hex,
                       solar_os_conversation_id_t *conversation_id)
{
    Instance &in = *static_cast<Instance *>(context);
    RNS::Bytes hash;
    if (!hex_to_hash(peer_hex, hash)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_contact_id_t contact_id = 0;
    solar_os_endpoint_id_t endpoint_id = 0;
    const esp_err_t error =
        remember_peer(in.id, hash, nullptr, &contact_id, &endpoint_id);
    if (error != ESP_OK) {
        return error;
    }
    /* A conversation that exists is returned as it is: an upsert would put
     * the bare hash back over the name and security it has earned since. */
    std::vector<solar_os_messaging_conversation_t> known(
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
    const size_t count =
        solar_os_messaging_conversation_snapshot(known.data(), known.size());
    for (size_t i = 0; i < count; i++) {
        if (known[i].provider == in.id &&
            strcmp(known[i].provider_key, peer_hex) == 0) {
            if (conversation_id != nullptr) {
                *conversation_id = known[i].id;
            }
            return ESP_OK;
        }
    }
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = in.id,
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

/*
 * Removal withdraws the destination first. The worker does that, so this
 * marks the instance and waits for it; with the job stopped there is
 * nothing to withdraw. Messaging then deletes the profile, history and
 * identity, and finish frees the slot.
 */
bool listening(const Instance &in)
{
    Guard guard;
    return in.listening;
}

esp_err_t class_remove_prepare(void *context)
{
    Instance &in = *static_cast<Instance *>(context);
    xSemaphoreTake(edit_lock, portMAX_DELAY);
    {
        Guard guard;
        in.removing = true;
    }
    const int64_t deadline = esp_timer_get_time() + kReleaseWaitUs;
    while (job_attached && listening(in)) {
        if (esp_timer_get_time() > deadline) {
            Guard guard;
            in.removing = false;
            xSemaphoreGive(edit_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

void class_remove_finish(void *context, bool removed)
{
    Instance &in = *static_cast<Instance *>(context);
    {
        Guard guard;
        if (removed) {
            in.in_use = false;
            in.id = 0;
            in.config = solar_os_lxmf_config_t{};
        }
        in.removing = false;
    }
    xSemaphoreGive(edit_lock);
}

const solar_os_messaging_provider_class_t lxmf_class = {
    .name = SOLAR_OS_LXMF_CLASS,
    .capabilities = SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ROOM |
                    SOLAR_OS_ENDPOINT_CAP_ACK,
    .connection_oriented = false,
    .join = class_join,
    .join_open = nullptr,
    .leave = class_leave,
    .direct_open = class_direct,
    .connect = class_connect,
    .create = class_create,
    .configure = class_configure,
    .normalize_target = class_normalize,
    .remove_prepare = class_remove_prepare,
    .remove_finish = class_remove_finish,
};

}  // namespace

esp_err_t solar_os_lxmf_init(void)
{
    esp_err_t error = ensure_slots();
    if (error != ESP_OK) {
        return error;
    }
    if (class_registered) {
        return ESP_OK;
    }
    error = solar_os_messaging_class_register(&lxmf_class);
    if (error != ESP_OK) {
        return error;
    }
    class_registered = true;
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status = {};
    while (solar_os_messaging_provider_next(&cursor, &status)) {
        if (strcmp(status.class_name, SOLAR_OS_LXMF_CLASS) != 0) {
            continue;
        }
        solar_os_lxmf_config_t config = {};
        error = solar_os_messaging_profile_load(cursor, &config, sizeof(config));
        if (error == ESP_OK && !config_valid(config)) {
            error = ESP_ERR_INVALID_ARG;
        }
        if (error == ESP_OK) {
            EditGuard edit;
            error = attach_instance(cursor, status.name, config);
        }
        if (error != ESP_OK) {
            (void)solar_os_messaging_provider_set_status(
                cursor, false, false, error, "LXMF profile unavailable or invalid");
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_lxmf_create_default(void)
{
    esp_err_t error = solar_os_lxmf_init();
    if (error != ESP_OK) {
        return error;
    }
    {
        Guard guard;
        for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX; i++) {
            if (slots[i].in_use) {
                return ESP_OK;
            }
        }
    }
    return create_instance(SOLAR_OS_LXMF_DEFAULT_INSTANCE, nullptr, nullptr);
}

esp_err_t solar_os_lxmf_identity_generate(solar_os_messaging_provider_id_t id,
                                         bool force)
{
    if (solar_os_lxmf_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    EditGuard edit;
    Instance *in = nullptr;
    {
        Guard guard;
        in = find_locked(id);
        if (in == nullptr) {
            return ESP_ERR_NOT_FOUND;
        }
        /* The worker owns a destination made from the old one. */
        if (in->listening) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    RNS::Identity existing{RNS::Type::NONE};
    if (!force && read_identity(id, existing) == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = make_identity(id, true);
    if (error == ESP_OK) {
        Guard guard;
        in->identity_error = ESP_OK;
    }
    return error;
}

esp_err_t solar_os_lxmf_open(const char *destination_hex,
                             solar_os_conversation_id_t *conversation_id)
{
    if (solar_os_lxmf_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_messaging_provider_id_t id = 0;
    {
        Guard guard;
        for (size_t i = 0; i < SOLAR_OS_LXMF_INSTANCE_MAX && id == 0; i++) {
            if (slots[i].in_use) {
                id = slots[i].id;
            }
        }
    }
    if (id == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_messaging_open_direct(id, destination_hex, conversation_id);
}

esp_err_t solar_os_lxmf_get_status(solar_os_messaging_provider_id_t id,
                                   solar_os_lxmf_status_t *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_lxmf_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    Instance *in = nullptr;
    {
        Guard guard;
        in = find_locked(id);
    }
    if (in == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    char name[sizeof(status->display_name)];
    display_name(*in, name, sizeof(name));
    Guard guard;
    *status = in->counters;
    status->attached = in->listening;
    status->identity_error = in->identity_error;
    strlcpy(status->display_name, name, sizeof(status->display_name));
    strlcpy(status->destination_hex, in->destination_hex,
            sizeof(status->destination_hex));
    strlcpy(status->propagation_node_hex, propagation_status.hex,
            sizeof(status->propagation_node_hex));
    status->propagation_node_hops = propagation_status.hops;
    status->propagation_stamp_cost = propagation_status.stamp_cost;
    return ESP_OK;
}

esp_err_t solar_os_lxmf_announce(solar_os_messaging_provider_id_t id)
{
    if (solar_os_lxmf_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    Instance *in = nullptr;
    uint32_t before = 0;
    {
        Guard guard;
        in = find_locked(id);
        if (in == nullptr) {
            return ESP_ERR_NOT_FOUND;
        }
        if (!in->listening) {
            return ESP_ERR_INVALID_STATE;
        }
        before = in->announces_done;
        in->announce_requested = true;
    }
    /* The worker makes the announce and the radio takes its time over it, so
     * this waits to answer whether it went, as the node's own announce does. */
    const int64_t deadline = esp_timer_get_time() + kAnnounceWaitUs;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
        Guard guard;
        if (!in->in_use || in->id != id || !in->listening) {
            return ESP_ERR_INVALID_STATE;
        }
        if (in->announces_done != before) {
            return in->announce_result;
        }
    }
    return ESP_ERR_TIMEOUT;
}

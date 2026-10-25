/*
 * Collecting from a propagation node, on the real LXMF service and
 * microReticulum: which collected messages the node is told to delete when
 * the message store refuses some of them.
 */
#include "../../src/services/solar_os_lxmf.cpp"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

namespace {

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

/* The message store: what it took, and a body it refuses, as a full or
 * missing card refuses every write. */
std::vector<std::string> stored;
std::string refuse_body;
std::string blocked_body;
bool refuse_all = false;
std::vector<uint32_t> stored_flags;

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

void esp_fill_random(void *buffer, size_t length) { random_bytes(buffer, length); }

esp_err_t solar_os_reticulum_random(uint8_t *out, size_t len)
{
    random_bytes(out, len);
    return ESP_OK;
}

esp_err_t solar_os_reticulum_random_reseed(void) { return ESP_OK; }

/* Stamps are for sending; collection never makes one. */
esp_err_t solar_os_lxmf_stamp_start(solar_os_lxmf_stamp_t *, const uint8_t *, size_t,
                                    uint32_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_lxmf_stamp_build(solar_os_lxmf_stamp_t *, uint32_t, bool *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_lxmf_stamp_search(const solar_os_lxmf_stamp_t *, uint32_t,
                                     uint8_t[SOLAR_OS_LXMF_STAMP_SIZE], uint32_t,
                                     uint32_t *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
void solar_os_lxmf_stamp_end(solar_os_lxmf_stamp_t *) {}
void solar_os_lxmf_settings_get(solar_os_lxmf_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
}

esp_err_t solar_os_crypto_random(uint8_t *out, size_t len)
{
    random_bytes(out, len);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "error";
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag,
                             const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
    return ESP_OK;
}

void *solar_os_memory_calloc(size_t count, size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    return calloc(count, size);
}

void solar_os_memory_free(void *ptr) { free(ptr); }

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    *epoch_ms = static_cast<uint64_t>(time(nullptr)) * 1000U;
    return ESP_OK;
}

void solar_os_identity_get_hostname(char *buffer, size_t len)
{
    strlcpy(buffer, "solaros", len);
}

esp_err_t solar_os_messaging_publish_inbound(
    const solar_os_messaging_inbound_t *request, bool *inserted,
    solar_os_message_key_t *message_key)
{
    (void)message_key;
    if (refuse_all || request->body == refuse_body) {
        return ESP_ERR_NO_MEM;
    }
    if (request->body == blocked_body) {
        return ESP_ERR_INVALID_STATE; /* as Messaging answers a blocked sender */
    }
    stored.push_back(request->body);
    stored_flags.push_back(request->security_flags);
    *inserted = true;
    return ESP_OK;
}

esp_err_t solar_os_contacts_find_endpoint(solar_os_messaging_provider_id_t provider,
                                          const uint8_t *address,
                                          size_t address_len,
                                          solar_os_endpoint_t *endpoint)
{
    (void)provider;
    (void)address;
    (void)address_len;
    (void)endpoint;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_contacts_get(solar_os_contact_id_t id, solar_os_contact_t *contact)
{
    (void)id;
    (void)contact;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_contacts_upsert_discovered(
    solar_os_messaging_provider_id_t provider, const uint8_t *address,
    size_t address_len, const char *display_name, uint32_t capabilities,
    uint64_t last_seen_ms, const void *provider_metadata,
    size_t provider_metadata_len, solar_os_contact_id_t *contact_id,
    solar_os_endpoint_id_t *endpoint_id)
{
    (void)provider;
    (void)address;
    (void)address_len;
    (void)display_name;
    (void)capabilities;
    (void)last_seen_ms;
    (void)provider_metadata;
    (void)provider_metadata_len;
    *contact_id = 1U;
    *endpoint_id = 1U;
    return ESP_OK;
}

esp_err_t solar_os_contacts_import_discovered(
    solar_os_messaging_provider_id_t, const uint8_t *, size_t, const char *,
    uint32_t, uint64_t, const void *, size_t, solar_os_contact_id_t *,
    solar_os_endpoint_id_t *)
{
    return ESP_OK;
}

esp_err_t solar_os_contacts_flush(void)
{
    return ESP_OK;
}

/* Not reached by collection; present so the service links. */
esp_err_t solar_os_messaging_provider_set_status(solar_os_messaging_provider_id_t,
                                                 bool, bool, esp_err_t, const char *)
{
    return ESP_OK;
}
esp_err_t solar_os_messaging_outbox_update(uint32_t, solar_os_delivery_state_t,
                                           const char *)
{
    return ESP_OK;
}
esp_err_t solar_os_messaging_join_update(solar_os_messaging_provider_id_t, const char *,
                                         solar_os_messaging_join_state_t, const char *)
{
    return ESP_OK;
}
esp_err_t solar_os_messaging_conversation_upsert(
    const solar_os_messaging_conversation_upsert_t *, solar_os_conversation_id_t *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
bool solar_os_messaging_provider_next(solar_os_messaging_provider_id_t *,
                                      solar_os_messaging_provider_status_t *)
{
    return false;
}
esp_err_t solar_os_messaging_provider_get_status(solar_os_messaging_provider_id_t,
                                                 solar_os_messaging_provider_status_t *)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_messaging_profile_save(solar_os_messaging_provider_id_t,
                                          const void *, size_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_messaging_profile_load(solar_os_messaging_provider_id_t, void *,
                                          size_t)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_messaging_outbox_peek(solar_os_messaging_provider_id_t,
                                         solar_os_messaging_outbound_t *)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_messaging_open_direct(solar_os_messaging_provider_id_t, const char *,
                                         solar_os_conversation_id_t *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_messaging_instance_remove(solar_os_messaging_provider_id_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_messaging_instance_register(solar_os_messaging_provider_id_t,
                                               const char *, const char *, void *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_messaging_instance_create(const char *, const char *, const void *,
                                             size_t, solar_os_messaging_provider_id_t *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
size_t solar_os_messaging_conversation_snapshot(solar_os_messaging_conversation_t *,
                                                size_t)
{
    return 0U;
}
esp_err_t solar_os_messaging_conversation_get(solar_os_conversation_id_t,
                                              solar_os_messaging_conversation_t *)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_messaging_class_register(const solar_os_messaging_provider_class_t *)
{
    return ESP_OK;
}
void solar_os_credentials_wipe(void *buffer, size_t length) { memset(buffer, 0, length); }
esp_err_t solar_os_credentials_read_secret(solar_os_credential_id_t, void *, size_t,
                                           size_t *)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_credentials_put(solar_os_messaging_provider_id_t,
                                   solar_os_credential_kind_t, const char *,
                                   const void *, size_t, bool,
                                   solar_os_credential_id_t *)
{
    return ESP_ERR_NOT_SUPPORTED;
}
const char *solar_os_credentials_location(void) { return "credentials"; }
esp_err_t solar_os_credentials_find(solar_os_messaging_provider_id_t,
                                    solar_os_credential_kind_t, const char *,
                                    solar_os_credential_info_t *)
{
    return ESP_ERR_NOT_FOUND;
}

}  // extern "C"

namespace {

Instance &make_instance(size_t slot, solar_os_messaging_provider_id_t id)
{
    assert(ensure_slots() == ESP_OK);
    Instance &in = slots[slot];
    in.in_use = true;
    in.id = id;
    in.identity = RNS::Identity();
    in.inbound = RNS::Destination(in.identity, RNS::Type::Destination::IN,
                                  RNS::Type::Destination::SINGLE,
                                  SOLAR_OS_LXMF_APP_NAME, SOLAR_OS_LXMF_ASPECT);
    return in;
}

/* Heard: its identity can be recalled from its delivery hash. */
void heard(const Instance &in)
{
    RNS::Bytes packet_hash;
    random_bytes(packet_hash.writable(32U), 32U);
    RNS::Identity::remember(packet_hash, in.inbound.hash(),
                            in.identity.get_public_key());
}

/* As a propagation node holds it: recipient hash, then encrypted for the
 * recipient. Built by the sending side of the same service. */
RNS::Bytes held_message(Instance &from, const Instance &to, const char *body)
{
    from.pending.destination = to.inbound.hash();
    from.pending.timestamp_ms = 1767225600000ULL;
    strlcpy(from.pending_body, body, SOLAR_OS_MESSAGING_BODY_MAX);
    RNS::Bytes message;
    assert(build_message(from, message));
    RNS::Bytes lxmf_data(to.inbound.hash());
    lxmf_data.append(delivery_destination(to.identity).encrypt(message));
    return lxmf_data;
}

std::string transient_id(const RNS::Bytes &lxmf_data)
{
    return RNS::Identity::full_hash(lxmf_data)
        .left(SOLAR_OS_LXMF_TRANSIENT_ID_LEN).toHex();
}

/* The IDs the node is told to delete. */
std::set<std::string> take(Instance &in, const std::vector<RNS::Bytes> &batch)
{
    std::vector<solar_os_lxmf_blob_t> items(batch.size());
    for (size_t i = 0; i < batch.size(); i++) {
        items[i].data = batch[i].data();
        items[i].len = batch[i].size();
    }
    uint8_t haves[kSyncBatch * SOLAR_OS_LXMF_TRANSIENT_ID_LEN];
    const size_t count = take_collected(in, items.data(), items.size(), haves);
    std::set<std::string> ids;
    for (size_t i = 0; i < count; i++) {
        ids.insert(RNS::Bytes(&haves[i * SOLAR_OS_LXMF_TRANSIENT_ID_LEN],
                              SOLAR_OS_LXMF_TRANSIENT_ID_LEN).toHex());
    }
    return ids;
}

void reset_store()
{
    stored.clear();
    stored_flags.clear();
    refuse_body.clear();
    blocked_body.clear();
    refuse_all = false;
}

/* A store that refuses everything: nothing is deleted from the node, and
 * the same messages are taken once it accepts them. */
void test_store_failure_leaves_messages_on_the_node(Instance &me, Instance &peer)
{
    reset_store();
    const RNS::Bytes first = held_message(peer, me, "first");
    const RNS::Bytes second = held_message(peer, me, "second");

    refuse_all = true;
    assert(take(me, {first, second}).empty());
    assert(stored.empty() && me.counters.collected == 0U);

    refuse_all = false;
    const std::set<std::string> ids = take(me, {first, second});
    assert(ids == std::set<std::string>({transient_id(first), transient_id(second)}));
    assert(stored == std::vector<std::string>({"first", "second"}));
    assert(me.counters.collected == 2U);
}

/* One batch of each kind: stored, refused by a full store, from a sender
 * not yet heard, from a blocked sender, undecryptable, and for someone
 * else. Only the one the store could not take stays on the node: anything
 * else left there would be offered first at every collection, and enough
 * of them would hide every message behind. */
void test_mixed_batch(Instance &me, Instance &peer, Instance &stranger)
{
    reset_store();
    me.counters.collected = 0U;
    const RNS::Bytes kept = held_message(peer, me, "kept");
    const RNS::Bytes refused = held_message(peer, me, "card full");
    const RNS::Bytes unknown = held_message(stranger, me, "who");
    const RNS::Bytes blocked = held_message(peer, me, "from a blocked peer");
    RNS::Bytes garbled(me.inbound.hash());
    garbled.append(RNS::Bytes("not encrypted for anyone", 24U));
    const RNS::Bytes elsewhere = held_message(me, peer, "for peer");

    refuse_body = "card full";
    blocked_body = "from a blocked peer";
    const std::set<std::string> ids =
        take(me, {kept, refused, unknown, blocked, garbled, elsewhere});
    assert(ids == std::set<std::string>(
                      {transient_id(kept), transient_id(unknown),
                       transient_id(blocked), transient_id(garbled),
                       transient_id(elsewhere)}));
    assert(stored == std::vector<std::string>({"kept", "who"}));
    /* The unannounced sender's is kept, but not as verified. */
    assert((stored_flags[0] & SOLAR_OS_SECURITY_SENDER_UNVERIFIED) == 0U);
    assert((stored_flags[1] & SOLAR_OS_SECURITY_SENDER_UNVERIFIED) != 0U);
    assert((stored_flags[1] & SOLAR_OS_SECURITY_PEER_KEY_KNOWN) == 0U);
    assert(me.counters.collected == 2U);

    /* Offered again, they are taken once. */
    reset_store();
    const std::set<std::string> again = take(me, {kept, unknown});
    assert(again.size() == 2U && stored.empty());
}

}  // namespace

int main()
{
    RNS::loglevel(RNS::LOG_NONE);
    Instance &me = make_instance(0U, 11U);
    Instance &peer = make_instance(1U, 12U);
    Instance &stranger = make_instance(2U, 13U);
    heard(peer);
    /* A local destination is known to Transport; a stranger's is not. */
    RNS::Transport::deregister_destination(stranger.inbound);

    test_store_failure_leaves_messages_on_the_node(me, peer);
    test_mixed_batch(me, peer, stranger);
    puts("lxmf sync tests passed");
    return 0;
}

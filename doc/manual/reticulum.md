+++
id = "reticulum"
title = "Reticulum network stack"
section = "service"
summary = "Join a Reticulum network as a non-transport node over a TCP connection"
aliases = ["reticulum.job"]
keywords = "reticulum rns tcp announce destination identity mesh microreticulum hdlc"
packages_any = ["service_reticulum", "job_reticulum"]
+++
# Reticulum network stack

SolarOS embeds the microReticulum implementation of the Reticulum protocol
and connects it to the network through a TCP client interface to an existing
node, a LoRa packet radio framed the way an RNode frames the air, or both.
The node is a leaf: it does not forward traffic for other nodes. It carries
identity, announces, path discovery, and LXMF messaging.

## Quick start

Join Wi-Fi, ensure the clock is set, and start the job with the address of a
Reticulum `TCPServerInterface`:

```text
job start reticulum HOST 4242
reticulum status
reticulum announce
reticulum announces
```

The clock must be set (RTC or NTP) because announces carry timestamps that
other nodes validate. The TCP connection is reconnected with a backoff from
two to sixty seconds. Frames use the standard Reticulum HDLC framing.

## Over LoRa

A packet radio the board has - `radio0` on a board with one - carries
Reticulum directly, with no other node in between:

```text
job start reticulum lora radio0 reticulum-us915
job start reticulum HOST 4242 lora radio0 reticulum-us915
reticulum status
```

The job claims the radio, applies the profile, and listens; `job stop`
gives the radio back as it was found. The profile is mandatory because a
region is never chosen silently. `reticulum-us915` is 915 MHz, 125 kHz,
SF8, coding rate 4/5, an 18-symbol preamble, sync word `0x12`, CRC, 14 dBm:
a common RNode setting for the 902-928 MHz band. Any profile the radio
accepts will do, as long as the peer uses the same numbers.

The air framing is the RNode's, so a stock RNode is a peer: one header byte
carrying a sequence number and a split flag, a packet of up to 254 bytes in
one LoRa frame and a longer one in two. An RNode attached to a desktop
Reticulum node, configured to the same frequency, bandwidth, spreading
factor and coding rate, hears this node's announces and this node hears
its. `reticulum status` shows the radio, whether it is online, frames and
bytes each way, and the RSSI and SNR of the last frame heard.

## Identity and storage

The first start generates an identity from the SolarOS RNG and stores its
64-byte private key as an opaque Credentials record. Protocol state (known
destinations and paths) is stored under `/sdcard/reticulum`.

```text
reticulum identity show
reticulum identity generate --force
reticulum identity import PRIVATE_KEY_HEX
reticulum identity export --private
```

Generating with `--force` or importing is rejected while the job runs. Private
export prints a warning; treat the output as a password.

## Destination

SolarOS announces a `solaros.node` single destination, and an
`lxmf.delivery` destination for messaging. `reticulum status` shows the
node destination hash and the count of known paths, frames, announces, and
caught exceptions.

## LXMF messaging

LXMF messages appear in the ordinary SolarOS messaging surfaces: the `chat`
app, `messages`, and the universal inbox, alongside MeshCore and gateway
conversations. The provider is `reticulum`.

```text
reticulum lxmf status
reticulum lxmf announce
reticulum lxmf open DESTINATION_HEX
messages send CONVERSATION_ID TEXT
```

A peer becomes reachable once its `lxmf.delivery` announce has been heard,
which records it as a discovered contact under its announced display name.
`reticulum lxmf open` starts a conversation with a peer named by the hash of
its delivery destination, whether or not it has announced; SolarOS then
requests a path and sends once one arrives.

The display name SolarOS announces is the device hostname.

Sending is direct: SolarOS opens a link to the peer and sends the message
over it, signed with the node identity. A message that fits one link packet,
about 330 bytes, goes as a packet and shows as delivered once the peer's
proof comes back. A longer one goes as a resource, in as many packets as it
takes, and shows as delivered once the peer has confirmed the whole of it.
The link is kept for the next message to the same peer and closed after ten
minutes without use.

Receiving takes every way an LXMF client sends directly: an opportunistic
packet, a packet over a link, and a resource over a link. A message of more
than 64 KB is refused when the peer offers it, before any of it is sent.
A message longer than the message store keeps is stored cut short and marked
as such. `reticulum lxmf status` counts the links opened in either direction.

SolarOS has no bz2, which is how Reticulum compresses a resource. Its
announce says so, in the field LXMF uses for this, and a peer running LXMF
1.2 or later then sends uncompressed. A compressed resource is refused.

Delivery follows the same shape as the LXMF router's outbound job, so a peer
sees the behaviour it would see from any other LXMF sender. Attempts are
driven by whether Reticulum has a path rather than by a clock alone: a try
without a path is followed by a path request, and a try with one opens a
link. A link that does not carry the message is closed, and the next try
opens another. Three tries, ten seconds apart. After them a message that fits
a single packet of 255 bytes is sent opportunistically, without a link, which
reaches a peer that can hear a packet and not hold a link; a longer one is
marked failed.

A message queued while the job is stopped stays queued, and is sent when the
job starts. The outbox is rebuilt at boot from the stored messages, so
queued mail survives a restart, and a message whose provider is not running
says so rather than sitting at queued with no explanation.

There is no store-and-forward through a propagation node, and no
attachments or stamps. A message to a peer that never becomes reachable
fails rather than waiting for it.

Inbound messages are dropped unless their signature verifies against an
identity SolarOS already knows, so a message from a peer that has never
announced is refused rather than shown unverified.

## Limits

- One TCP interface and one LoRa radio; no serial or local-network interfaces yet.
- No transport (forwarding) mode.
- LXMF sends and receives directly, up to 64 KB a message; no propagation nodes, attachments, or compressed resources.
- The link MTU stays at the Reticulum default of 500 bytes.

## Quick reference

```text
reticulum status
reticulum identity show
reticulum identity generate [--force]
reticulum identity import <private-key-hex>
reticulum identity export --private
reticulum announce
reticulum announces
reticulum lxmf status
reticulum lxmf announce
reticulum lxmf open <destination-hex>
job start reticulum <host> [port]
job start reticulum lora <radio> <profile>
job start reticulum <host> [port] lora <radio> <profile>
job status reticulum
job stop reticulum
```

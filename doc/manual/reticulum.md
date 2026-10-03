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

Sending is opportunistic: each message is one encrypted packet, signed with
the node identity, addressed directly to the peer. That caps a message at 255
bytes. A message shows as delivered once the peer's proof comes back.

Receiving takes both ways an LXMF client sends directly: an opportunistic
packet, and a message over a link, which is what NomadNet and most clients
use by default. A link message must fit one link packet, about 330 bytes of
message; a longer one would arrive as a resource, which is refused.
`reticulum lxmf status` counts the links peers have opened.

Delivery follows the same shape as the LXMF router's outbound job, so a peer
sees the behaviour it would see from any other LXMF sender. Attempts are
driven by whether Reticulum has a path rather than by a clock alone: a try
without a path is followed by a path request, and a try that fails with a
path in hand treats that path as stale, drops it and asks again. Three
attempts, ten seconds apart, and then the message is marked failed. LXMF
itself allows five; three is enough on a link this slow.

A message queued while the job is stopped stays queued, and is sent when the
job starts. The outbox is rebuilt at boot from the stored messages, so
queued mail survives a restart, and a message whose provider is not running
says so rather than sitting at queued with no explanation.

There is no store-and-forward through a propagation node, no sending over
links, and no resources, attachments, or stamps. A message to a peer that never becomes
reachable fails rather than waiting for it.

Inbound messages are dropped unless their signature verifies against an
identity SolarOS already knows, so a message from a peer that has never
announced is refused rather than shown unverified.

## Limits

- One TCP interface and one LoRa radio; no serial or local-network interfaces yet.
- No transport (forwarding) mode.
- LXMF sends opportunistically and receives single-packet messages, opportunistic or over a link; no propagation nodes, resources, or attachments.
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

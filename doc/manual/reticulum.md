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
and connects it to an existing Reticulum node through a TCP client interface.
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

Delivery is opportunistic: each message is one encrypted packet, signed with
the node identity, addressed directly to the peer. That caps a message at 255
bytes and means the peer must be reachable when it is sent. A message shows
as delivered once the peer's proof comes back, and as sent if the proof never
arrives. There is no store-and-forward through a propagation node, and no
links, resources, attachments, or stamps.

Inbound messages are dropped unless their signature verifies against an
identity SolarOS already knows, so a message from a peer that has never
announced is refused rather than shown unverified.

## Limits

- One TCP interface; no LoRa, serial, or local-network interfaces yet.
- No transport (forwarding) mode.
- LXMF is opportunistic only: no propagation nodes, links, or attachments.
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
job status reticulum
job stop reticulum
```

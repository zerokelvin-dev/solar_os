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
The node is a leaf: it does not forward traffic for other nodes. This first
stage provides identity, announces, and path discovery; LXMF messaging is not
yet available.

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

SolarOS announces one `solaros.node` single destination. `reticulum status`
shows its hash and the count of known paths, frames, announces, and caught
exceptions.

## Limits

- One TCP interface; no LoRa, serial, or local-network interfaces yet.
- No transport (forwarding) mode and no LXMF messaging yet.
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
job start reticulum <host> [port]
job status reticulum
job stop reticulum
```

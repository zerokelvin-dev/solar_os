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
identity, announces, and path discovery.

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

LoRa's low-data-rate optimize must be set alike at both ends, and the
interface sets it as RNode firmware does rather than as the radio driver
would by itself. The two differ where a symbol lasts a fraction over 16 ms,
at SF11 on 125 kHz and SF12 on 250 kHz among others: an RNode leaves it off
there, and so does this.

## Identity and storage

The first start generates an identity from the SolarOS RNG and stores its
64-byte private key as an opaque Credentials record, in
`.settings/credentials.bin` on the default storage volume. Protocol state
(known destinations and paths) is stored under `/sdcard/reticulum`.

```text
reticulum identity show
reticulum identity generate [--force]
reticulum identity import PRIVATE_KEY_HEX
reticulum identity export --private
```

An identity is made only once, with the instance that holds it. If it is
later missing, because the credentials file was lost or the storage holding it
did not mount, the job refuses to start and names the file it expected,
rather than quietly becoming a new node with a new address. Restore the file,
or accept a new identity with `reticulum identity generate`. Replacing one
that works takes `--force`. Generating or importing is rejected while the job
runs. Private export prints a warning; treat the output as a password.

## Destination

SolarOS announces a `solaros.node` single destination. `reticulum status` shows the
node destination hash and the count of known paths, frames, announces, and
caught exceptions.

## Limits

- One TCP interface and one LoRa radio; no serial or local-network interfaces yet.
- No transport (forwarding) mode.
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
job start reticulum lora <radio> <profile>
job start reticulum <host> [port] lora <radio> <profile>
job status reticulum
job stop reticulum
```

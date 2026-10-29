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

SolarOS announces a `solaros.node` single destination, and an
`lxmf.delivery` destination for messaging. `reticulum status` shows the
node destination hash and the count of known paths, frames, announces, and
caught exceptions.

## LXMF messaging

LXMF messages appear in the ordinary SolarOS messaging surfaces: the `chat`
app, `messages`, and the universal inbox, alongside IRC, MeshCore and gateway
conversations. LXMF is a messaging class, `lxmf`, set up the way IRC servers
are: each instance is an identity of its own, with its own delivery address,
display name, conversations, and joined groups. Up to four run at once.

```text
reticulum lxmf status [INSTANCE]
reticulum lxmf announce [INSTANCE]
reticulum lxmf sync [INSTANCE]
reticulum lxmf propagation [node DESTINATION_HEX|auto]
reticulum lxmf propagation interval MINUTES|off
reticulum lxmf identity generate INSTANCE [--force]
messages add lxmf NAME [DISPLAY_NAME]
messages query NAME DESTINATION_HEX
messages join NAME GROUP_HEX
messages leave CONVERSATION_ID
messages configure NAME "Display name"
messages configure NAME hostname
messages disconnect NAME
messages connect NAME
messages remove NAME
messages send CONVERSATION_ID TEXT
```

The first start, when the node identity is made, also creates an instance
named `lxmf`, enabled, announcing the device hostname. `messages add lxmf`
creates another, with an identity generated then and kept as that
instance's Credentials record. An instance whose identity has gone missing
stays offline, and `reticulum lxmf status` says so and where it was expected;
`reticulum lxmf identity generate NAME` gives it a new one, and a new address.
Replacing a working identity takes `messages disconnect NAME` and `--force`. Each instance announces its own
`lxmf.delivery` destination, so a peer sees each as a separate address.
Profiles are saved in `.chat/profiles` with every other instance's.

The node's own identity, the one `reticulum identity` shows and
`solaros.node` announces, is separate. It belongs to an instance named
`reticulum` that carries no conversations, so removing an LXMF instance
never changes the node.

Chat takes the same commands: `/add lxmf NAME`, `/query NAME DESTINATION_HEX`,
`/join NAME GROUP_HEX`, `/leave`, `/configure`, `/connect`, `/disconnect`
and `/remove`. `messages leave` takes the conversation number from
`messages conversations`; Chat's `/leave` leaves the open conversation.
Hashes are 32 hexadecimal digits, with or without the angle brackets
Reticulum prints around them. `reticulum lxmf open DESTINATION_HEX` is
`messages query` on the first LXMF instance.

`disconnect` saves the instance as disabled: the job keeps running for
other instances and other nodes' traffic, but that instance's
`lxmf.delivery` destination is withdrawn, so it announces, receives and
sends nothing until `connect`. Its queued messages wait. `remove` withdraws
the destination, then deletes the instance's history and identity; the
same name added again is a new address.

A peer becomes reachable once its `lxmf.delivery` announce has been heard,
which records it as a discovered contact under its announced display name.
`messages query` starts a conversation with a peer named by the hash of its
delivery destination, whether or not it has announced; SolarOS then requests
a path and sends once one arrives.

### Groups

LXMF has no group chat of its own; a group is a group-server peer, an LXMF
destination that relays what it is sent to its members. `messages join`
makes such a peer a room: its messages are shown as the room's, what is sent
there goes to the server, and the room is kept across restarts, up to eight
of them. Whether the server counts this node as a member is its own rule;
many want a join command sent to them as an ordinary message first. A room's
messages are signed by the server, so its members' names are the server's
word: they are marked sender-unverified. `messages leave` stops treating the
peer as a group; its history stays.

Sending is direct: SolarOS opens a link to the peer and sends the message
over it, signed with the identity of the instance sending it. A message
that fits one link packet, about 330 bytes, goes as a packet and shows as
delivered once the peer's proof comes back. A longer one goes as a resource,
in as many packets as it takes, and shows as delivered once the peer has
confirmed the whole of it.
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
opens another. Three tries, ten seconds apart. After them the message goes to
a propagation node, if one has been heard, to be held there until the
recipient collects it. Without one, a message that fits a single packet of
255 bytes is sent opportunistically, without a link, which reaches a peer
that can hear a packet and not hold a link; a longer one is marked failed.

A message queued while the job is stopped stays queued, and is sent when the
job starts; it says the provider is not running rather than sitting at queued
with no explanation. The outbox does not survive a restart: a message still
queued or sending then is marked failed, as for every provider.

### Propagation nodes

A propagation node holds messages for recipients that are away; on a
Reticulum network it is usually `lxmd -p`. SolarOS uses the node set, or
else the nearest active node it has heard announce, as `reticulum lxmf
status` shows, and sends
through it only after direct delivery has failed. The message is encrypted
for its recipient, so the node cannot read it, and carries the proof-of-work
stamp the node asks for in its announce; on an ESP32-S3 that takes about two
seconds at a node's usual cost of 16. Once the node takes it the message is
shown as sent: the node holds it, and only the recipient's client can say it
arrived. A node that refuses the stamp, or cannot be reached in three tries,
fails the message.

Each instance also collects what the node holds for it: once a node is
heard, then every six hours, as NomadNet does by default, and whenever
`reticulum lxmf sync` asks. `reticulum lxmf propagation interval MINUTES`
changes the six hours, and `off` leaves collecting to `reticulum lxmf sync`. It identifies to the node, asks what it holds,
fetches up to sixteen messages at a time, and tells the node which arrived so
it deletes them. A message whose sender SolarOS has not heard announce is
left on the node while the sender's announce is asked for, and collection
looks again a minute later. `reticulum lxmf status` counts what was collected
and says how the last collection went.

Without a node set, one is chosen from the announces heard, which `lxmd`
sends at start and every six hours by default; its `announce_interval` sets
that. `reticulum lxmf propagation node DESTINATION_HEX` sets the node every
instance uses, and the others heard are ignored. A node that is set does not
need to be heard announcing: SolarOS asks for a path to it, which brings its
announce data back, and asks again every five minutes until it answers.
`reticulum lxmf propagation node auto` goes back to the nearest heard.

```text
reticulum lxmf propagation
reticulum lxmf propagation node DESTINATION_HEX|auto
reticulum lxmf propagation interval MINUTES|off
```

These settings are for the device, not an instance, and are kept in
`.settings/lxmf.bin`.

There are no attachments. A message to a peer that never becomes reachable,
with no propagation node heard, fails rather than waiting for it.

Inbound messages are dropped unless their signature verifies against an
identity SolarOS already knows, so a message from a peer that has never
announced is refused rather than shown unverified.

## Limits

- One TCP interface and one LoRa radio; no serial or local-network interfaces yet.
- No transport (forwarding) mode.
- LXMF sends and receives directly, up to 64 KB a message, and through a propagation node; no attachments or compressed resources.
- Up to four LXMF instances, each with its own identity; groups are group-server peers.
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
reticulum lxmf status [instance]
reticulum lxmf announce [instance]
reticulum lxmf sync [instance]
reticulum lxmf propagation [node <destination-hex>|auto]
reticulum lxmf propagation interval <minutes>|off
reticulum lxmf open <destination-hex>
reticulum lxmf identity generate <instance> [--force]
messages add lxmf <name> [display-name]
messages query <instance> <destination-hex>
messages join <instance> <group-hex>
job start reticulum <host> [port]
job start reticulum lora <radio> <profile>
job start reticulum <host> [port] lora <radio> <profile>
job status reticulum
job stop reticulum
```

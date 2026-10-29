# Reticulum over LoRa: the air framing

How a SolarOS radio puts a Reticulum packet on the air so that a stock RNode
is a peer, established from the RNode firmware and the Python RNode interface
and written down here before it was implemented. The protocol is a fact about
bytes; the sources were read, not copied.

## Facts

- **One header byte, always.** Every LoRa frame is a header byte followed by
  packet bytes. The high nibble is a sequence number chosen at random per
  packet (`random(256) & 0xF0`); bit 0 is `FLAG_SPLIT` (0x01). Nothing else is
  in the header. (`RNode_Firmware.ino: transmit()`, `Framing.h: FLAG_SPLIT`,
  `Config.h: HEADER_L 1`.)
- **Up to 254 bytes go in one frame.** A LoRa frame is at most 255 bytes
  (`SINGLE_MTU`), one of which is the header. A packet longer than 254 is
  sent as two frames carrying the same header with the split flag set: the
  first with 254 bytes, the second with the rest. The most a packet can be
  is 508 (`MTU`); Reticulum's own MTU is 500. (`transmit()`: the frame is
  ended at `written == 255` and a new one begun with the same header.)
- **Halves pair by sequence number.** The receiver holds at most one half.
  A split frame with no half held, or with a different sequence than the
  held half, starts a new half; a split frame with the same sequence
  completes it. A frame without the split flag is a whole packet and
  discards any half held. There is no timeout. (`receive_callback()`,
  `Utilities.h: isSplitPacket()`, `packetSequence()`.)
- **The link.** Explicit LoRa header (implicit only in promiscuous mode),
  CRC on (`LoRa->enableCrc()` in `setup()`), the LoRa library's default sync
  word 0x12 (the firmware never sets one), preamble length chosen by the
  firmware from the symbol time (at least 18 symbols, about 24 ms). The
  Python `RNodeInterface` sends the raw Reticulum packet bytes to the
  firmware (`process_outgoing`: KISS `CMD_DATA` around `data`) and declares
  `HW_MTU = 508`.

## What SolarOS does with them

- `solar_os_reticulum_lora_frame.{h,c}` packs a packet into one or two
  frames and reassembles frames into packets by exactly the rules above;
  host-tested, including a lost half and a stray second half.
- `ReticulumLoraInterface` drives a claimed `solar_os_radio` handle: RX by
  default, one blocking send per frame, back to RX; CRC-failed frames are
  counted and dropped; RSSI and SNR of the last frame are kept for status.
- The `reticulum-us915` profile: 915 MHz, 125 kHz, SF8, CR 4/5, 18-symbol
  preamble, sync 0x12, CRC, 14 dBm. The peer's RNode is configured with the
  same numbers; the band is the only thing the profile commits to.

## Not done

- Promiscuous and implicit-header modes.
- Airtime limiting and channel statistics the RNode reports; the radio
  service's own limits apply.

## Bench, 2026-10-01

ThinkNode M9 (LR1110, `reticulum-us915`) against a Heltec LoRa32 v4 running
stock RNode firmware 1.86 on a Mac, both on one desk about a metre apart.

Mac side: `rnodeconf --autoinstall` (Heltec LoRa32 v4, 915 MHz band); a
private `rnsd` config with one `RNodeInterface` at 914875000 Hz, 125000 Hz,
SF8, CR5, 7 dBm, **`share_instance = Yes` on non-default ports** - with
`share_instance = No` every `rnstatus`/`rnpath` starts its own Reticulum and
fights `rnsd` for the serial port, and the interface shows Down. The LXMF peer
is `scripts/rns_lora_bench_peer.py <configdir> <M9 delivery hash>`: it
announces as `mac`, sends one opportunistic message, and prints what it
receives and when its proof comes back.

| Step | Result |
| --- | --- |
| M9 announce -> Mac | every one of 5 received; `rnpath -t` shows the M9 one hop away via the RNode |
| Mac announce -> M9 | every one of 5 received after the driver fix below; `reticulum announces` lists them, 1 hop, -35 dBm, SNR 15 |
| Mac LXMF message -> M9 | received, in `messages`/`inbox`; delivery proof back at the Mac 2 s later |
| M9 `messages send ... --allow-untrusted` -> Mac | received by the Mac's LXMF router; M9 shows `Sent: 1 (1 delivered)` |
| `rnprobe` to `solaros.node` | times out: the M9 receives it (rx counter moves) and sends no proof, which is that destination's proof strategy, not the link |

Found on the bench: the LR1110's packet-parameter payload length is also its
maximum receive length, and a transmit re-set it to the outgoing size. After
its own 174-byte announce the M9 took a 131-byte probe and dropped every
~193-byte announce. Entering receive now restores the maximum
(`lr11xx_set_packet_length`, pinned by a driver test).

A reply to a peer discovered only from the air needs `--allow-untrusted`;
without it the shell reports "resource is busy or not ready", which is the
generic text for `ESP_ERR_INVALID_STATE` and does not say why. Worth a
clearer message.

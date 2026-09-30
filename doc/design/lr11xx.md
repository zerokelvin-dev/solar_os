# LR11xx Radio Driver

SolarOS has one command-set radio driver, `sx1262`. Two separate needs land on
a second one: the Elecrow ThinkNode M9 carries an onboard LR1110 whose pins the
board manifest reserves with no driver behind them, and the LilyGO T-LoRa-Pager
ships an interchangeable LR1121 radio module alongside the SX1262 one that port
already supports.

The LR1110 and the LR1121 are not two parts. They are one part with different
front ends, they share an opcode set, and the silicon says which one it is.
This is therefore an LR11xx driver that reads the variant off the chip, not an
LR1110 driver with an LR1121 mode bolted on.

## The family

| | LR1110 | LR1120 | LR1121 |
| --- | --- | --- | --- |
| Device type byte | `0x01` | `0x02` | `0x03` |
| Sub-GHz LoRa and (G)FSK, 150–960 MHz | yes | yes | yes |
| S-band, 1.9–2.2 GHz | no | yes | yes |
| 2.4 GHz ISM LoRa and FSK | no | yes | yes |
| GNSS scanning | yes | yes | no |
| Wi-Fi passive scanning | yes | yes | no |
| RTToF ranging | yes | yes | no |

A part sitting in its bootloader reports type `0xDF` instead, and only
firmware-update commands work in that state.

All three answer the same system, register/memory and radio command groups,
byte for byte. The only visible differences are which bands the front end
reaches and whether the Wi-Fi (`0x03xx`) and GNSS (`0x04xx`) groups exist —
and an LR1121 rejects those cleanly, by returning `CMD_PERR` in `stat1`.
Nothing in the transport or the packet engine changes, which is why one driver
covers all three and why the variant is a runtime fact rather than a build
flag.

Only three things could ever need to branch on the part: band validation, PA
selection above 1 GHz, and gating GNSS/Wi-Fi/ranging. A driver that does none
of those three needs no branching at all.

**One trap worth recording.** The LR1110 datasheet's feature list claims a
"High frequency PA path +13 dBm (pin RFIO_HF) for 2.4 GHz ISM band", but the
LR1110 has no 2.4 GHz LoRa or (G)FSK transceiver. Its specification tables
cover sub-GHz receive, the GNSS scanner, the Wi-Fi scanner, and a sub-GHz
transmit path — there is no 2.4 GHz TX/RX table at all, where the LR1120 and
LR1121 datasheets both add one. The LR1110's real 2.4 GHz capability is Wi-Fi
passive scanning plus BLE-beacon-compatible transmit, nothing more. That
feature bullet is boilerplate shared across the family's datasheets, and a
driver that trusted it would let an LR1110 be configured for 2.4 GHz LoRa.
RadioLib's own range checks agree: it caps the LR1110 at 960 MHz and only
opens 1.9–2.2 and 2.4–2.5 GHz on the LR112x.

The LR1110's GNSS and Wi-Fi scanning are genuinely unusual — the part can take
a GNSS snapshot or sniff Wi-Fi MAC addresses and hand back a payload for
server-side position solving, without a GPS receiver. They are also out of
scope here; see *Not covered*.

## Transport, and how it differs from the SX1262's

The SX126x driver in `src/drivers/sx1262.c` treats every exchange as one
chip-select window: a one-byte opcode, parameter bytes, and — for a command
that returns data — extra NOP bytes appended to the same window, whose
clocked-out bytes are the response. BUSY gates each window.

The LR11xx keeps the BUSY gate and changes everything else.

**Opcodes are two bytes**, a group byte and a command byte. The groups this
driver uses are `0x01` (system and register/memory) and `0x02` (radio); `0x03`
is Wi-Fi scanning, `0x04` GNSS, `0x05` cryptography.

**A command that returns data needs two chip-select windows.** The first
carries the opcode and its parameters. BUSY then rises while the part prepares
its answer. The second window, with no opcode at all, clocks out one discarded
byte and then the response. Semtech's own HAL documents this exactly: *"a
two-step radio read operation… writing the command, releasing then re-asserting
the NSS line, then reading a discarded dummy byte followed by data_length
bytes of response data."* The SX126x trick of appending NOPs to one window
returns nothing here.

**Status is the exception, and it is a bare read.** The part answers *any*
read, with no command in front of it, with `stat1`, `stat2`, and the 32-bit
interrupt word. Semtech fetches status with a `direct_read` of six bytes.
There is no `GetIrqStatus` opcode on this family. Sending `GetStatus` (0x0100)
and then performing a two-window read would shift the interrupt word by one
byte and make TxDone unreadable — this driver had that bug before the transport
was checked against the reference HAL, and `tests/host/lr11xx_test.c` now pins
the correct behaviour.

**MOSI must be held at zero for the whole read.** Semtech's HAL is explicit
that only NOP bytes may be written while the response is clocked out: *"Some
hardware SPI implementations write arbitrary values on the MOSI line while
reading. If this is done on the LR11XX, non-zero values may be interpreted as
commands."* `LR11XX_NOP` is `0x00`. Every read window in this driver sends a
zeroed transmit buffer, and the host test asserts it.

`stat1` carries a command status in bits [3:1] — `FAIL` 0, `PERR` 1, `OK` 2,
`DATA` 3 — and an interrupt-active flag in bit 0. `stat2` carries a
running-from-flash flag in bit 0, the chip mode in bits [3:1] (sleep 0,
standby RC 1, standby XOSC 2, FS 3, RX 4, TX 5, Wi-Fi/GNSS 6) and a reset
cause in bits [7:4] (cleared 0, analog/POR 1, NRESET pin 2, system 3, watchdog
4, chip-select wake-up 5, RTC restart 6). This driver reads the status byte
and ignores it; see *Not covered*.

**Frequency is plain hertz**, four bytes big-endian. The SX126x's
`freq × 2²⁵ / F_XTAL` PLL-step arithmetic has no counterpart. Encoding 915 MHz
the SX126x way would send `0x39300000` and land the part in a different band
entirely, which is why the host test asserts the exact four bytes.

**Image calibration takes a band, not a magic number.** `CalibImage` is given
two bounds in 4 MHz steps, so a single arithmetic expression covers every
regional plan. The SX126x needs a per-band lookup table of datasheet
constants, which `sx1262.c` carries.

**The LoRa sync word is one command of one byte.** The SX126x stores it split
across two nibble-mapped registers.

**The interrupt mask is 32 bits**, so `SetDioIrqParams` is eight parameter
bytes (one mask per interrupt DIO) and `ClearIrq` is four.

**`WriteBuffer8` takes no offset.** It fills the transmit buffer from zero.
`ReadBuffer8` does take an offset and a length. The SX126x's `WriteBuffer` has
a leading offset byte; sending one here transmits it as payload and truncates
the last byte.

**Timeouts count in 1/32768 s ticks** (~30.52 µs), three bytes wide, for both
`SetTx` and `SetRx`. For receive, `0x000000` means single-shot and `0xFFFFFF`
means continuous; for transmit, `0` means no hardware timeout. The same tick
period applies to the TCXO start-up delay and the sleep duration.

## Verified command and parameter reference

Everything in this section was read out of Semtech's published LR11xx driver,
SWDR001 (see *Sources*). Command lengths are quoted as Semtech's own
`*_CMD_LENGTH` constants, which include the two opcode bytes.

### System group

| Command | Opcode | Length |
| --- | --- | --- |
| GetStatus | `0x0100` | not sent — status is a 6-byte bare read |
| GetVersion | `0x0101` | 2, response 4 |
| GetErrors | `0x010D` | 2 |
| ClearErrors | `0x010E` | 2 |
| Calibrate | `0x010F` | 2 + 1 |
| SetRegMode | `0x0110` | 2 + 1 |
| CalibImage | `0x0111` | 2 + 2 |
| SetDioAsRfSwitch | `0x0112` | 2 + 8 |
| SetDioIrqParams | `0x0113` | 2 + 8 |
| ClearIrq | `0x0114` | 2 + 4 |
| ConfigLfClock | `0x0116` | 2 + 1 |
| SetTcxoMode | `0x0117` | 2 + 4 |
| Reboot | `0x0118` | 2 + 1 |
| SetSleep | `0x011B` | 2 + 5 |
| SetStandby | `0x011C` | 2 + 1 |
| SetFs | `0x011D` | 2 |
| ReadUid (chip EUI) | `0x0125` | 2, response 8 |
| EnableSpiCrc | `0x0128` | 2 + 1 |

`SetSleep`'s config byte is bit 0 for configuration retention and bit 1 for the
RTC wake-up; the four-byte duration that follows is only read when bit 1 is
set, in the same 1/32768 s ticks as everything else.

`GetErrors` returns two big-endian bytes: LF RC calibration `0x0001`, HF RC
`0x0002`, ADC `0x0004`, PLL `0x0008`, image `0x0010`, HF crystal start
`0x0020`, LF crystal start `0x0040`, PLL lock `0x0080`. Note this ordering
is **not** the `Calibrate` mask's — `Calibrate` puts PLL at bit 2 and ADC at
bit 3, `GetErrors` the other way round.

### Register/memory group

| Command | Opcode | Layout |
| --- | --- | --- |
| WriteRegMem32 | `0x0105` | |
| ReadRegMem32 | `0x0106` | |
| WriteBuffer8 | `0x0109` | opcode then data, **no offset** |
| ReadBuffer8 | `0x010A` | opcode, offset, length; response is `length` bytes |
| ClearRxBuffer | `0x010B` | opcode only |
| WriteRegMem32Mask | `0x010C` | 4-byte address, 4-byte mask, 4-byte value |

### Radio group

| Command | Opcode | Length |
| --- | --- | --- |
| GetRxBufferStatus | `0x0203` | 2, response 2 (payload length, start pointer) |
| GetPacketStatus | `0x0204` | 2, response 3 for LoRa |
| SetRx | `0x0209` | 2 + 3 |
| SetTx | `0x020A` | 2 + 3 |
| SetRfFrequency | `0x020B` | 2 + 4 |
| SetPacketType | `0x020E` | 2 + 1 |
| SetModulationParams | `0x020F` | 2 + 4 for LoRa |
| SetPacketParams | `0x0210` | 2 + 6 for LoRa |
| SetTxParams | `0x0211` | 2 + 2 |
| SetPaConfig | `0x0215` | 2 + 4 |
| SetLoRaSyncWord | `0x022B` | 2 + 1 |

### Enumerations

Packet type: none `0x00`, GFSK `0x01`, LoRa `0x02`, BPSK `0x03`, LR-FHSS
`0x04`, RTToF `0x05`.

LoRa bandwidth: 10.4 kHz `0x08`, 15.6 `0x01`, 20.8 `0x09`, 31.25 `0x02`,
41.7 `0x0A`, 62.5 `0x03`, 125 `0x04`, 250 `0x05`, 500 `0x06`, and — 2.4 GHz
only — 203 `0x0D`, 406 `0x0E`, 812 `0x0F`. The sub-GHz codes coincide with the
SX126x's, but the family has **no 7.8 kHz step** and adds three wide ones, so
the driver writes the table out rather than sharing it and refuses a bandwidth
it cannot reach instead of rounding into a wrong code.

LoRa coding rate: `4/5` `0x01` through `4/8` `0x04`, then long-interleaved
variants `0x05`–`0x07`. Header: explicit `0x00`, implicit `0x01`. CRC: off
`0x00`, on `0x01`. IQ: standard `0x00`, inverted `0x01`.

Ramp time: `0x00` is 16 µs and each step adds 16 µs up to `0x0F`, so `0x04` is
80 µs. The SX126x's `0x04` is 200 µs — the number does not carry over.

PA selection: low power `0x00`, high power `0x01`, high frequency `0x02`.
PA regulator supply: VREG `0x00`, VBAT `0x01`. Usable ranges are −17 to
+14 dBm on the low-power PA, −9 to +22 on the high-power PA, and −18 to +13 on
the high-frequency PA. `paDutyCycle` sets the duty as `0.2 + 0.04 × value`
(0–7 on the low-power PA, 0–4 on the high-power one) and `paHpSel` sets the
slice count as `value + 1`.

`SetPaConfig`'s four values are not derivable from the requested power. This
driver carries Semtech's reference table for the sub-GHz amplifiers, indexed
by requested dBm from −17 to +22, crossing from the low-power to the
high-power amplifier at 16 dBm and switching that path to the VBAT supply.
The value handed to `SetTxParams` is part of the table and is generally *not*
the requested power — asking for 16 dBm sends 22 to `SetTxParams` with a duty
cycle of 3 and 4 slices. The table is deliberately non-monotonic because it is
measured board tuning, so it is transcribed and not computed. It is tuning for
Semtech's own evaluation shield; a board with a different matching network may
need its own, which is why it sits in one table rather than scattered through
the configure path.

Regulator mode: LDO `0x00`, DC-DC `0x01`. Standby: RC `0x00`, XOSC `0x01`.

TCXO supply: 1.6 V `0x00`, 1.7 `0x01`, 1.8 `0x02`, 2.2 `0x03`, 2.4 `0x04`,
2.7 `0x05`, 3.0 `0x06`, 3.3 `0x07`.

Calibration bits: LF RC `0x01`, HF RC `0x02`, PLL `0x04`, ADC `0x08`, image
`0x10`, PLL TX `0x20`. `0x3F` is all six.

Interrupts (32-bit): TxDone `0x00000004`, RxDone `0x00000008`, preamble
detected `0x00000010`, sync-word/header valid `0x00000020`, header error
`0x00000040`, CRC error `0x00000080`, CAD done `0x00000100`, CAD detected
`0x00000200`, timeout `0x00000400`, LR-FHSS intra-packet hop `0x00000800`,
GNSS scan done `0x00080000`, Wi-Fi scan done `0x00100000`, end of life
`0x00200000`, command error `0x00400000`, error `0x00800000`, FSK length error
`0x01000000`, FSK address error `0x02000000`, LoRa RX timestamp `0x08000000`.

### GFSK, layout verified and codes not

The GFSK modulation block is bitrate (4 bytes, bit/s), pulse shape (1),
receive bandwidth code (1), deviation (4 bytes, Hz) — plain units, another
simplification over the SX126x. The packet block is preamble length in bits
(2), preamble detector (1), sync-word length in bits (1), address filtering
(1), header type (1), payload length (1), CRC type (1), DC-free (1).

The field orders are verified. The enumerated **code values** for pulse shape,
receive bandwidth, preamble detector, CRC type and DC-free are not. The CRC
field is an enum here rather than the SX126x's byte count, so the driver's
`0x02` for a two-byte CRC is a guess carried over from that part and may select
the wrong polynomial or none. GFSK is advertised because the radio service asks
for it; treat it as untested.

## Initialisation and calibration sequence

`lr11xx_init()` configures BUSY as input, IRQ as input, RESET as output, pulses
NRESET low, and waits for BUSY to fall. NRESET must be held low for at least
100 µs; the driver uses 1 ms, as Semtech's reference HAL does.

Readiness afterwards is BUSY-based, not delay-based: the part holds BUSY high
for its whole start-up and drops it on reaching standby RC. That takes about
180 ms on an LR1110 and 237 ms on an LR1121 — typical figures, with no
published maximum, so a fixed delay would be a guess. The driver's BUSY poll
has a one-second timeout, with margin over both. (RadioLib's code comments
quote 273 ms; no datasheet supports that number and it looks like a
transposition of 237.)

`lr11xx_probe()` then reads GetVersion and maps the device-type byte to the
part; an all-zero, all-ones, or unrecognised type byte is `ESP_ERR_NOT_FOUND`
rather than a working radio. The response is hardware revision, device type,
firmware major, firmware minor. The hardware-revision byte is deliberately not
interpreted: Semtech documents the field but publishes no value list, and
neither its driver nor RadioLib maps it.

`lr11xx_configure()` runs, in order:

1. Wake, if the part is asleep.
2. `SetStandby(RC)`.
3. `SetRegMode(DC-DC)`.
4. If the board declares a TCXO: `SetTcxoMode(voltage, 5 ms)`, then
   `Calibrate(0x3F)`, then `ClearErrors`. Changing the clock source
   invalidates the factory calibration, so recalibration is not optional.
   A board with a plain crystal must not see this command at all — telling a
   part to power a TCXO that is not there stalls its clock start-up.
5. If the board declares an antenna-switch table: `SetDioAsRfSwitch`. Semtech
   documents this as standby-RC only, which is where step 2 left the part.
6. `SetPacketType`.
7. `SetRfFrequency`.
8. `CalibImage` over a 4 MHz window around the chosen frequency.
9. `SetModulationParams`, then `SetPacketParams`.
10. `SetPaConfig`, then `SetTxParams`.
11. `SetLoRaSyncWord`, for LoRa.
12. `SetDioIrqParams(mask, 0)` — only IRQ1 is armed, because every board
    carrying this part wires a single interrupt line.
13. `ClearIrq(all)`.

Step 4's ordering is the one that bites. If a TCXO is fitted in place of the
32 MHz crystal, the part still boots but *skips every power-on calibration*,
and the datasheet puts the burden on the host: program the TCXO configuration,
then re-launch the calibrations. Doing it the other way round leaves a part
that answers commands normally and transmits badly.

## Transmit and receive

Transmit re-sends the packet parameters for this payload's length (the length
is one of those parameters), clears interrupts, writes the payload with
`WriteBuffer8`, issues `SetTx` with no hardware timeout, polls status until
TxDone or timeout, then drops to standby.

Receive follows the poll-not-session contract `sx1262_receive()` established,
because the meshcore job calls it repeatedly with `timeout_ms == 0` meaning
"check now". The receiver is armed once with `SetRx(0xFFFFFF)` and left in
continuous mode; re-issuing `SetRx` on every poll would leave the part
restarting its receiver instead of listening, and dropping to standby when
nothing arrived would do the same. On RxDone the driver reads
`GetRxBufferStatus` for the length and start pointer, `ReadBuffer8` from that
pointer, and `GetPacketStatus` for RSSI and SNR.

RSSI is `-(int8_t)(raw >> 1)` — the byte shifts as unsigned and is then
negated, giving the same −raw/2 dBm scaling as the SX126x. SNR is **not** the
SX126x's plain `raw/4`: the byte is signed and the conversion is
`(((int8_t)raw) + 2) >> 2`, rounding to nearest rather than truncating.
Dropping the bias term costs a decibel of reported link margin and produces no
other symptom, which is exactly why it is worth naming.

## Sleep, wake, and the adjacent-channel erratum

Sleep uses **warm start**, retaining the configuration. That is not an
optimisation: a cold sleep loses the whole configuration, and nothing in this
driver reconfigures on wake, so a cold sleep would come back as a silently
unconfigured radio.

Waking needs a falling edge on the part's own chip select, held low for at
least 100 µs, then released, then a BUSY wait. Semtech's reference code drives
NSS directly to do that. Here the SPI peripheral owns chip select, so the only
way to hold it low is to keep clocking — and the bytes clocked must be zero,
by the MOSI rule above. The driver therefore sends a run of NOP bytes sized
from the bus clock to span 100 µs (26 bytes at its 2 MHz default) and then
waits on BUSY. Wake from a retained sleep is under 1 ms; from a cold one it is
30 ms on an LR1110 and 52 ms on an LR1121.

Warm start brings an erratum with it, and it has to be handled. Semtech
documents that waking from a retention sleep leaves one internal parameter
misconfigured, and *every subsequent LoRa transmission* then has unexpectedly
high adjacent-channel power, at every bandwidth except 500 and 800 kHz.
Affected firmware is LR1110 0x0303 through 0x0307 and LR1120 0x0101. The
published workaround is to clear bit 30 of register `0x00F30054` after the
wake, which this driver does with a single `WriteRegMem32Mask`. Semtech's own
driver re-applies it before *every* `SetTx` and `SetRx`; doing it once per wake
is the same coverage for the documented cause and one fewer SPI write per
packet, but it is a deviation and worth re-checking on hardware.

The workaround has a documented hole: it cannot cover a part configured to
auto-transmit out of a retention sleep via `AutoTxRx` with a sleep
intermediary mode, because the part transmits before the host can clear the
bit. This driver does not use `AutoTxRx`.

## Interrupts and DIO handling

The IRQ pin is accepted, configured as an input, and otherwise unused: this
driver polls status, as `sx1262.c` does. The polling interval is 4 ms.

On the LR11xx the general-purpose interrupt line is the chip's DIO9, not DIO1.
Both boards' documentation calls the ESP32-side net "LORA_DIO1", which is a
board-level label; do not read it as the chip pad.

`SetDioIrqParams` arms IRQ1 only. Interrupt-driven receive is future work; the
ordering above already puts the mask in place for it.

## Choosing the part variant

At runtime, from the chip. `GetVersion` returns a device-type byte and
`lr11xx_probe()` maps `0x01`/`0x02`/`0x03` to LR1110/LR1120/LR1121. The
registered radio's summary string carries the detected name, so `radio list`
shows what is actually fitted.

There is no build flag and no manifest field for the variant, deliberately.
A board that declares "LR1121" and has an LR1110 soldered on it would then be
wrong in a way nothing could detect; asking the part costs one command. This
also means a T-LoRa-Pager with the interchangeable module swapped needs no
rebuild — the same binary reports whichever module is fitted.

## What a board manifest must declare

```toml
[[devices]]
driver = "lr11xx"
name = "radio0"
bindings = { spi = "spi0", cs = 39, busy = 41, reset = 45, irq = 42, freq = 915, tcxo = 3300 }
```

| Binding | Required | Meaning |
| --- | --- | --- |
| `spi` | yes | named SPI bus |
| `cs` | yes | chip select |
| `busy` | yes | BUSY input |
| `reset` | no | NRESET output |
| `irq` | no | interrupt input |
| `freq` | no | MHz, 150–2500 |
| `tcxo` | no | TCXO supply in mV, or 0 for a crystal |

`freq` exists because this family spans 150 MHz to 2.5 GHz and every board
ships a different SKU. `sx1262.c` hardcodes 915 MHz with a comment admitting
it is one board's SKU; that does not survive a second board. `tcxo` has no safe
default in either direction, so the board states it.

The antenna-switch table does not fit in a binding — it is eight bytes across
seven operating modes — so a board declares it as a `[defines]` entry, the
same idiom the M9 already uses for its fixed key actions:

```toml
SOLAR_OS_BOARD_LR11XX_RF_SWITCH = '''{ \
    .enable = LR11XX_RF_SWITCH_DIO5 | LR11XX_RF_SWITCH_DIO6, \
    .rx = LR11XX_RF_SWITCH_DIO5, \
    .tx = LR11XX_RF_SWITCH_DIO5 | LR11XX_RF_SWITCH_DIO6, \
    .tx_hp = LR11XX_RF_SWITCH_DIO6 }'''
```

This has to come from the board, not the driver. The two boards that carry an
LR11xx use **opposite** polarities for the same modes:

| Mode | ThinkNode M9 | T-LoRa-Pager |
| --- | --- | --- |
| standby | — | — |
| rx | DIO5 | DIO6 |
| tx | DIO5 + DIO6 | DIO5 |
| tx_hp | DIO6 | DIO5 |
| tx_hf | — | — |

A board that declares no table leaves the switch DIOs in high impedance, which
is the part's own default. On a board that has a switch, that means transmit
goes nowhere. Guessing a table is worse: it parks the switch on the wrong port.

The M9's values above are the ones Meshtastic's and MeshCore's ports of that
board ship, bit for bit and independently of each other. The M9 board manifest
now declares them, along with `tcxo = 3300` which both projects also agree on.
Its `freq = 915` is the 915 MHz SKU; the M9 also ships as 868 MHz and nothing
on the board identifies which, so an 868 unit has to change that one number.

## Sharing an SPI bus with the SD card

Both boards put the radio, the SD card, and the display on one physical SPI
bus, distinguished only by chip select. The reporter of the LR1121 request
flagged a known quirk here, and it is worth stating precisely what it is,
because the popular hypotheses are wrong.

The actual failure, from the Meshtastic fix for the T-LoRa-Pager, is a firmware
configuration bug: that port defined `SDCARD_USE_SPI1`, which initialises a
*second* ESP32 SPI peripheral for the SD card on the *same* physical pads that
the first peripheral is already driving for the radio. Two controllers driving
one set of pins is bus contention; inserting a card made RadioLib fault and the
device boot-loop. The fix was to delete the flag so both devices share one
peripheral and chip select does the selecting, as intended.

It is not the SD card failing to release MISO, not an ordering requirement, not
SDIO-versus-SPI, and not a level shifter.

SolarOS is structurally immune to it. `src/drivers/spi_bus.c` initialises one
peripheral per board-declared bus and hands it out by reference count;
`src/drivers/sd_card.c` calls `solar_os_spi_bus_acquire()` rather than
initialising a host of its own, and `solar_os_bus_spi_transfer()` takes the
per-bus mutex for the duration of each transfer. There is no path by which a
second controller can be brought up on the same pins. The driver needs to do
nothing special, which is the point of the bus service existing.

Two real consequences remain, and the driver handles both:

- `solar_os_bus_spi_transfer()` adds and removes an ESP-IDF device per call,
  so chip select falls and rises exactly once per call. That is precisely what
  the LR11xx's two-window reads need, and it means another bus user can
  interleave a transfer *between* a command and its response read. Correctness
  survives that — the LR11xx's chip select stays high throughout the SD card's
  window, so the pending answer is untouched — but two *radio* callers
  interleaving would not. Every command-and-response pair in this driver runs
  under the per-device mutex for that reason.
- A sleeping LR11xx wakes when its own chip select falls. The SD card's chip
  select is a different pin, so SD traffic cannot wake it, and the driver's own
  wake is an explicit one-byte transfer before the first command.

The T-LoRa-Pager has a second, unrelated trap worth recording for whoever ports
that board: its SD socket has no supply until XL9555 expander line 12 is driven
high, so the card reads as absent otherwise. It is also worth noting that
Meshtastic clocks that board's SD at 75 MHz while LilyGO's own library and the
MeshCore fork use 4 MHz. Nobody has attributed a failure to the difference, but
a 19× disagreement on a bus shared with a radio deserves suspicion.

## Not covered in this first pass

- **GNSS and Wi-Fi scanning** on the LR1110 and LR1120. These are whole command
  groups (`0x04` and `0x03`) with their own almanac management and result
  formats, and they are only useful with a position-solving service. The M9
  has a separate ATGM336H GNSS receiver on UART that SolarOS already drives, so
  there is no need on the one board that could use it.
- **2.4 GHz and S-band** on the LR1120 and LR1121. `lr11xx_configure()`
  refuses any frequency above 960 MHz with `ESP_ERR_NOT_SUPPORTED`, which is
  also the right answer on an LR1110 at any time. Reaching those bands needs
  the high-frequency PA and a board whose antenna switch routes the HF port,
  and on the one board known to have that port the published switch table
  leaves the sub-GHz switch idle for `TX_HF` — implying the HF output bypasses
  it and goes to its own antenna. Refusing is the honest behaviour: silently
  configuring 2.4 GHz would transmit into the sub-GHz front end.
- **Interrupt-driven receive.** The driver polls, as `sx1262.c` does. The IRQ
  pin is claimed and the mask armed, so this is a change of one loop.
- **`stat1` command-status checking.** The driver reads the status byte and
  ignores it. Acting on `CMD_FAIL`/`CMD_PERR` would turn a rejected command
  into an error instead of a silent no-op, and is the cheapest reliability win
  available. It is also how an LR1121 reports that a Wi-Fi or GNSS command was
  refused, so it becomes necessary the moment those are attempted.
- **LR-FHSS, BPSK, ranging/RTToF, CAD, RX duty cycling, `AutoTxRx`, the crypto
  engine, and the optional SPI CRC.** All present on the part, none needed by
  anything in SolarOS.
- **The GFSK low-bitrate workarounds.** Semtech ships register pokes for
  600 bps and 1200 bps GFSK. They are not applied here, and GFSK is untested
  anyway. Note that Semtech's driver and the LR1121 user manual give these in
  two different register-address conventions with no published mapping between
  them, so transcribing from the manual is not safe.
- **Firmware image update.** Parts ship functional but with old firmware, and
  Semtech advises updating. The current images are LR1110 0x0402, LR1120
  0x0202, LR1121 0x0104; shipping LR1121 units are 0x0101. This driver does
  not enter the bootloader or update anything, but `lr11xx_probe()` reports the
  firmware version it finds, so a stale one is visible. Two things make an
  update worth doing rather than optional: the adjacent-channel erratum above
  covers exactly the older firmware, and Semtech's April 2026 advisory
  SEM-PSA-2026-001 (CVE-2025-14857/-14858/-14859) describes unauthenticated
  firmware load via physical SPI access on every version below the current
  ones. Neither is remotely exploitable and neither blocks LoRa, but a driver
  that reports the version lets someone decide.
- **`SetLoRaSyncWord`'s firmware floor.** The command needs LR1110 firmware
  0x0303 or newer; older parts need the deprecated `SetLoRaPublicNetwork`
  (`0x0208`) instead. No gate is implemented, because the oldest LR1110
  firmware image Semtech publishes *is* 0x0303, so no shipping part falls
  below it. Worth knowing that `0x0208` is also the one place Semtech's own
  driver contradicts itself — it declares a ten-byte command and sends three —
  so if a part ever does need it, check the manual first.
- **The T-LoRa-Pager LR1121 board itself.** This adds the driver, not the
  board variant. `boards/manifests/t_lora_pager.toml` describes the SX1262
  module. A separate LR1121 manifest needs the pins CS 36, BUSY 48, RESET 47,
  IRQ 14, `tcxo = 3000`, the pager's own switch table from the table above,
  and the XL9555 SD supply line handled.

## What needs hardware

Less than there was. Every opcode, command length, parameter layout and
enumeration in the verified reference above is transcribed from Semtech's own
driver and pinned by a host test, so the bytes leaving the bus are right by
construction. What no amount of reading settles:

- **Whether it works at all.** Nothing here has been run against silicon.
  The single most likely class of failure is the transport, and the host test
  covers the framing but not the timing.
- **Actual radiated power.** The PA table is Semtech's evaluation-shield
  tuning. Neither of these boards is that shield.
- **The GFSK code values** — pulse shape, receive bandwidth, preamble
  detector, CRC type, DC-free. The field layouts are verified; these five
  enumerations are not, and the CRC one is a guess carried from the SX126x.
- **The RFSW-to-DIO bit mapping**, assumed DIO5, 6, 7, 8, 10 for RFSW0–4.
  Both boards only use the first two, where every published table agrees, so
  a board with a switch on DIO7 or above should confirm it.
- **Whether once-per-wake is enough** for the adjacent-channel workaround, or
  whether Semtech's per-transmit application is load-bearing for some reason
  the erratum text does not give.
- **Whether the M9 unit in hand is 868 or 915 MHz.** Nothing on the board
  says, and the manifest currently claims 915.
- **The T-LoRa-Pager's 2.4 GHz antenna topology** — whether the HF port goes
  straight to its own connector or through a switch. The published switch
  table implies the former. Its schematic would settle it.

The issue reporter offered hardware-in-the-loop testing on the LR1121 pager,
which is the right way to close most of this. In rough order of what a first
bench session should check: does `GetVersion` answer with a plausible part and
firmware; does a configure sequence leave no error flags in `GetErrors`; does
a transmit raise TxDone; does a receive from a known-good SX1262 peer decode;
do RSSI and SNR track a step attenuator; and does sleep/wake survive without
losing configuration.

## Sources

- Semtech SWDR001, the published LR11xx driver — `lr11xx_system.c`,
  `lr11xx_system.h`, `lr11xx_system_types.h`, `lr11xx_radio.c`,
  `lr11xx_radio.h`, `lr11xx_radio_types.h`, `lr11xx_regmem.c`, `lr11xx_hal.h`,
  and its README for the errata: <https://github.com/Lora-net/SWDR001>. Every
  opcode, command length, parameter layout and enumeration in the verified
  reference above comes from these files, read at driver version 3.0.0.
- Semtech SWSD003, the matching application and evaluation-shield code:
  <https://github.com/Lora-net/SWSD003>. Source of the reference HAL's reset
  and wake sequences, and of the PA operating-point tables
  (`libs/smtc-shields/lr11xx/src/`).
- Semtech's published firmware images and their per-chip READMEs, for the
  version list and the driver pairing:
  <https://github.com/Lora-net/radio_firmware_images>.
- Semtech advisory SEM-PSA-2026-001:
  <https://www.semtech.com/uploads/bulletins/SEM-PSA-2026-001.pdf>.
- RadioLib's LR11x0 module, used only to corroborate — every opcode matched:
  <https://github.com/jgromes/RadioLib>. Several of its constants are wrong
  against Semtech's driver (`STANDBY_XOSC` defined as 0, the sleep wake-up bit
  at the wrong position, a byte-shift typo in the sleep duration), so nothing
  here follows it where the two disagree.
- Meshtastic's board variants, for the two boards' pin maps, TCXO voltages and
  antenna-switch tables:
  `variants/esp32s3/ELECROW-ThinkNode-M9/{variant.h,rfswitch.h}` and
  `variants/esp32s3/tlora-pager/{variant.h,rfswitch.h}`:
  <https://github.com/meshtastic/firmware>.
- Meshtastic pull request 9870, for the shared-SPI root cause and fix:
  <https://github.com/meshtastic/firmware/pull/9870>.
- LilyGO's own library, for the pager's TCXO voltage, switch table and the
  2.4 GHz power cap: <https://github.com/Xinyuan-LilyGO/LilyGoLib>.
- Elecrow's ThinkNode M9 wiki, corroborating the M9 pin map:
  <https://www.elecrow.com/wiki/ThinkNode_M9_Meshtastic_Communication_Terminal_with_Full_Keyboard.html>.
- `CJvanSoest/meshcore-lilygo-pager-LR1121`, a standalone MeshCore build for
  the LR1121 pager, which corroborates both boards' pin maps and the M9 switch
  table. Two of its settings look wrong and were not followed: it sets the
  pager's TCXO to 1.8 V where LilyGO and Meshtastic both say 3.0 V, and its
  pager target never programs the antenna-switch table at all.
- `nilseuropa/solar_os` issue 37, the LR1121 request and the hardware-testing
  offer.

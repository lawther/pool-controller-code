# Connect 10 Pool Controller Protocol Documentation

This document describes the proprietary serial protocol used by the Connect 10 pool controller and has been clean-room developed by sniffing the messages on the RS-232 like bus that is used for communications.

## Table of Contents

- [Message Structure](#message-structure)
  - [Message Format](#message-format)
  - [Checksum Calculation](#checksum-calculation)
  - [Device Addresses](#device-addresses)
- [Command Summary](#command-summary)
- [Commands](#commands)
  - [0x05 — Touchscreen Activation Ack ⚠️](#0x05--touchscreen-activation-ack-️)
  - [0x06 — Lighting Zone Configuration ✅](#0x06--lighting-zone-configuration-)
  - [0x0A — Firmware Version ✅](#0x0a--firmware-version-)
  - [0x0B — Channel Status ✅](#0x0b--channel-status-)
  - [0x0D — Active Channels Bitmask ✅](#0x0d--active-channels-bitmask-)
  - [0x0F — Chlorinator Mode → Touchscreen ⚠️](#0x0f--chlorinator-mode--touchscreen-️)
  - [0x10 — Channel Toggle Command ⚠️](#0x10--channel-toggle-command-️)
  - [0x12 — Device Status ⚠️](#0x12--device-status-️)
  - [0x14 — Mode (Spa/Pool) ✅](#0x14--mode-spapool-)
  - [0x16 — Water Temperature Reading ✅](#0x16--water-temperature-reading-)
  - [0x17 — Temperature Settings ✅](#0x17--temperature-settings-)
  - [0x18 — Chlorinator Cell Mode ⚠️](#0x18--chlorinator-cell-mode-️)
  - [0x19 — Temperature Setpoint Command ✅](#0x19--temperature-setpoint-command-)
  - [0x1B — Pump Button Activity ⚠️](#0x1b--pump-button-activity-️)
  - [0x1D — Chlorinator Setpoint ✅](#0x1d--chlorinator-setpoint-)
  - [0x1F — Chlorinator Reading ✅](#0x1f--chlorinator-reading-)
  - [0x25 — Valve Sync ✅](#0x25--valve-sync-)
  - [0x26 — Configuration ⚠️](#0x26--configuration-️)
  - [0x27 — Valve State Broadcast ✅](#0x27--valve-state-broadcast-)
  - [0x28 — Valve Control Command ✅](#0x28--valve-control-command-)
  - [0x2A — Mode/Favourite Control Command ✅](#0x2a--modefavourite-control-command-)
  - [0x2B — Unknown ⚠️](#0x2b--unknown-️)
  - [0x31 — Water Temperature Reading (alt) ✅](#0x31--water-temperature-reading-alt-)
  - [0x37 — Internet Gateway Info ⚠️](#0x37--internet-gateway-info-️)
  - [0x38 — Register Data ⚠️](#0x38--register-data-️)
  - [0x39 — Register Read Request ✅](#0x39--register-read-request-)
  - [0x3A — Register Write / Control ✅](#0x3a--register-write--control-)
  - [0x3B — Pump Speed Telemetry ✅](#0x3b--pump-speed-)
  - [0xFD — Controller Day/Time/Clock ✅](#0xfd--controller-daytimeclock-)
- [Appendix A: Register Dispatch Table](#appendix-a-register-dispatch-table)
- [Implementation Notes](#implementation-notes)

---

## Message Structure

All messages follow this basic structure:

```
[START] [SRC_HI] [SRC_LO] [DST_HI] [DST_LO] [CTRL_HI] [CTRL_LO] [CMD] [LENGTH] [HEADER_CHECKSUM] [DATA...] [DATA_CHECKSUM] [END]
```

### Message Format

| Offset | Field           | Description                                                       |
| ------ | --------------- | ----------------------------------------------------------------- |
| 0      | START           | Always `0x02`                                                     |
| 1-2    | SOURCE          | Source device address (big endian)                                |
| 3-4    | DEST            | Destination device address (big endian)                           |
| 5-6    | CONTROL         | Control bytes (typically `0x80 0x00`)                             |
| 7      | COMMAND         | Command byte (message type)                                       |
| 8      | LENGTH          | Total message length in bytes (including START and END bytes)     |
| 9      | HEADER_CHECKSUM | Sum of bytes 0–8, masked to 8 bits (`sum(bytes[0..8]) & 0xFF`)    |
| 10+    | DATA            | Payload data (varies by message type)                             |
| N-2    | DATA_CHECKSUM   | Sum of all data bytes (from index 10 to N-3) masked with 0xFF     |
| N-1    | END             | Always `0x03`                                                     |

### Checksum Calculation

There are two checksums in every message:

**Header checksum** (byte 9): Sum of bytes 0–8 masked to 8 bits:

```c
uint8_t header_checksum = 0;
for (int i = 0; i < 9; i++) {
    header_checksum += data[i];
}
// header_checksum &= 0xFF  (implicit for uint8_t)
```

**Data checksum** (second-to-last byte): Sum of payload bytes from index 10 to (length - 3), masked to 8 bits:

```c
uint32_t sum = 0;
for (int i = 10; i < len - 2; i++) {
    sum += data[i];
}
uint8_t data_checksum = sum & 0xFF;
```

### Device Addresses

| Address  | Device            | Description                       |
| -------- | ----------------- | --------------------------------- |
| `0x0050` | Touch Screen      | Touch screen interface            |
| `0x0062` | Connect 8/10      | Main pool controller (Connect 10) |
| `0x006F` | Internal Channels | Internal messages for active channels sent to this address |
| `0x0070` | Genus Heater      | Active i25 Evo electric heater    |
| `0x0072` | Internal Genus? 0x72 | Internal temperature module?   |
| `0x0074` | ICI Gas Heater    | Astral/Fluidra ICI 400B NG gas pool heater |
| `0x007F` | Internal Genus? 0x7F | Internal temperature module?   |
| `0x0081` | VX 11S v3 Salt Chlorinator | Salt chlorinator (VX 11S v3)|
| `0x0084` | Viron Chlorinator | Chemistry/chlorinator module (alternate variant; mutually exclusive with `0x0090`) |
| `0x0090` | RolaChem          | Chemistry/chlorinator module      |
| `0x00A0` | Viron Pump        | Viron XT Variable Speed Pump      |
| `0x00F0` | Internet Gateway  | Internet gateway module           |
| `0xFFFF` | Broadcast         | Broadcast to all devices          |

---

## Command Summary

The command byte (byte 7) identifies the message type. Some commands are universal across sources (same payload layout regardless of who sends — e.g. `0x0A`); others are source-dependent (same CMD byte, different payload per source — e.g. `0x12`, `0x16`, `0x17`).

Click any CMD in the first column to jump to the full section in [Commands](#commands). The **In code?** column distinguishes commands that have an actual handler in `message_decoder.c` from those that are only documented in this file.

| CMD                                                            | Name                                | Direction                                                              | Variants / Notes                                                                            | In code?                |
|----------------------------------------------------------------|-------------------------------------|------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|-------------------------|
| [`0x05`](#0x05--touchscreen-activation-ack-️)                  | Touchscreen Activation Ack          | `0x0050` → Broadcast                                                   | 1-byte payload `0x01`; sent after mode/favourite changes                                    | Yes (log-only)          |
| [`0x06`](#0x06--lighting-zone-configuration-)                  | Lighting Zone Configuration         | `0x0050` → Broadcast                                                   |                                                                                             | Yes                     |
| [`0x0A`](#0x0a--firmware-version-)                             | Firmware Version                    | `0x0050`, `0x0062`, `0x0070`, `0x0081`, `0x0084`, `0x00A0`, `0x00F0` → Broadcast | Same `{major, minor}` payload across all sources; dispatched on CMD byte alone              | Yes (unified handler)   |
| [`0x0B`](#0x0b--channel-status-)                               | Channel Status                      | `0x0050` → Broadcast                                                   |                                                                                             | Yes                     |
| [`0x0D`](#0x0d--active-channels-bitmask-)                      | Active Channels Bitmask             | `0x0050` → `0x006F` Internal Channels                                  | Unicast                                                                                     | Yes                     |
| [`0x0F`](#0x0f--chlorinator-mode--touchscreen-️)               | Chlorinator Mode → Touchscreen      | `0x0084` → `0x0050`                                                    | 2-byte `[01, mode]`; mirrors [0x18](#0x18--chlorinator-cell-mode-️) cell mode               | **No (doc only)**       |
| [`0x10`](#0x10--channel-toggle-command-️)                      | Channel Toggle Command              | `0x00F0` Gateway → Broadcast                                           |                                                                                             | Yes                     |
| [`0x12`](#0x12--device-status-️)                               | Device Status                       | `0x0050`, `0x0062`, `0x0074`, `0x0081`, `0x0084`, `0x0090`, `0x00F0` → Broadcast | Payload layout differs per source                                                           | Yes (per-source)        |
| [`0x14`](#0x14--mode-spapool-)                                 | Mode (Spa/Pool)                     | `0x0050` → Broadcast                                                   |                                                                                             | Yes                     |
| [`0x16`](#0x16--water-temperature-reading-)                    | Water Temperature Reading           | `0x0062` (LEN `0x0E`), `0x0070`/`0x0072`/`0x0074` (LEN `0x0D`) → Broadcast | Payload length differs by source: LEN `0x0E` = `{temp1, temp2}`, LEN `0x0D` = `{temp1}`; dispatched on CMD byte alone | Yes (unified handler)   |
| [`0x17`](#0x17--temperature-settings-)                         | Temperature Settings                | `0x0050` (LEN `0x10`), `0x0070`/`0x0074` (LEN `0x0E`) → Broadcast | Source-dependent payload layout                                                             | Yes (per-source)        |
| [`0x18`](#0x18--pump-speed-command-)                           | Pump Speed Command                  | `0x0050`, `0x0084` → `0x00A0` Viron XT Pump                            | Set pump speed (low/med/high)                                                       | Yes                     |
| [`0x1B`](#0x1b--pump-button-activity-)                         | Pump Button Activity                | `0x00A0` Viron XT Pump → Broadcast                                      | Speed button pressed on pump (Low/Med/High)                                                  | Yes (log-only)          |
| [`0x19`](#0x19--temperature-setpoint-command-)                 | Temperature Setpoint Command        | `0x00F0` Gateway, `0x0050` Touchscreen → Broadcast                     | Sub-dispatched by slot byte (`0x01`/`0x02` Pool/Spa from Gateway, `0x03` heater pair from Touchscreen); dispatched on CMD byte alone | Yes (unified handler)   |
| [`0x1D`](#0x1d--chlorinator-setpoint-)                         | Chlorinator Setpoint                | `0x0090` RolaChem, `0x0084` Viron, `0x0081` VX 11S v3 → Broadcast         | Byte 10: `0x00`=chlorine output level (VX 11S v3 only), `0x01`=pH, `0x02`=ORP; dispatched on CMD byte alone | Yes (unified handler)   |
| [`0x1F`](#0x1f--chlorinator-reading-)                          | Chlorinator Reading                 | `0x0090` RolaChem, `0x0084` Viron → Broadcast                          | Byte 10: `0x01`=pH, `0x02`=ORP; same payload from both sources; dispatched on CMD byte alone | Yes (unified handler)   |
| [`0x25`](#0x25--valve-sync-)                                   | Valve Sync                          | `0x0050` → `0x006F` Internal Channels                                  | Unicast                                                                                     | **No (doc only)**       |
| [`0x26`](#0x26--configuration-️)                               | Configuration                       | `0x0050` → Broadcast                                                   |                                                                                             | Yes                     |
| [`0x27`](#0x27--valve-state-broadcast-)                        | Valve State Broadcast               | `0x0050` → Broadcast                                                   | Two LEN variants: `0x0D` (short) and `0x13` (full)                                          | Yes (both variants)     |
| [`0x28`](#0x28--valve-control-command-)                        | Valve Control Command               | `0x00F0` Gateway → Broadcast                                           |                                                                                             | **No (doc only)**       |
| [`0x2A`](#0x2a--modefavourite-control-command-)                | Mode/Favourite Control Command      | `0x00F0` Gateway → `0x0050` Touchscreen                                | Unicast                                                                                     | Yes                     |
| [`0x2B`](#0x2b--unknown-️)                                     | Unknown                             | `0x0062` Connect 8/10 → `0x0050` Touchscreen                          | Unicast; payload `[02 00]` observed; meaning unknown                                        | **No (doc only)**       |
| [`0x31`](#0x31--water-temperature-reading-alt-)                | Water Temperature Reading (alt)     | `0x0062` → Broadcast                                                   | Same `{temp1, temp2}` field layout as `0x16`; different disconnected encoding (`>= 0xA0` vs `0x00`); shared handler, log-only | Yes (unified handler)   |
| [`0x37`](#0x37--internet-gateway-info-️)                       | Internet Gateway Info               | `0x00F0` → Broadcast                                                   | LEN distinguishes serial (`0x11`), network config (`0x15`), comms status (`0x0F`) variants  | Yes (3 handlers)        |
| [`0x38`](#0x38--register-data-️)                               | Register Data (Response)            | `0x0050` Touchscreen → Broadcast                                       | Universal register system — sub-dispatched by register + slot (see [Appendix A](#appendix-a-register-dispatch-table)); dispatched on CMD byte alone | Yes (unified handler)   |
| [`0x39`](#0x39--register-read-request-)                        | Register Read Request               | `0x00F0` Gateway → Broadcast                                           | Dispatched on CMD byte alone                                                                | Yes (unified handler)   |
| [`0x3A`](#0x3a--register-write--control-)                      | Register Write / Control            | `0x00F0` Gateway → Broadcast                                           | Used for Light Zone Control (`0xC0`–`0xC7`/slot `0x01`), Heater Control (`0xE6`/slot `0x00`), and Heater 2 pool setpoint (`0xEA`/slot `0x00`) | Yes (both)              |
| [`0x3B`](#0x3b--pump-speed-)                                   | Pump Speed Telemetry                | `0x00A0` Viron XT Pump → Broadcast                                      | 2-byte big-endian RPM value; broadcast every ~60 seconds                                    | Yes                     |
| [`0xFD`](#0xfd--controller-daytimeclock-)                      | Controller Day/Time/Clock           | `0x0050` → Broadcast                                                   |                                                                                             | Yes                     |

---

## Commands

The protocol is organised around a single-byte CMD identifier carried in byte 7 of every frame. Each section below documents one CMD, in ascending hex order, covering all known sources, sub-variants (by LENGTH, slot, or source where they differ), payload layout, and any handler notes.

Read this section linearly to learn the protocol bottom-up, or jump in via:

- the [Command Summary](#command-summary) master table for a one-line summary of every CMD plus its source/destination directions and whether it has a handler in `message_decoder.c`;
- the [Table of Contents](#table-of-contents) for direct links;
- [Appendix A](#appendix-a-register-dispatch-table) when you're looking for a specific register inside the universal register message ([0x38](#0x38--register-data-️)).

Status markers in section titles: ✅ = fully decoded (every byte's meaning is known), ⚠️ = unknowns remain (unknown bytes, bits, values, or tentative interpretations). Handler presence in the firmware is conveyed separately via the master table's **In code?** column.

### 0x05 — Touchscreen Activation Ack ⚠️

Single-byte broadcast emitted by the Touchscreen (`0x0050`) immediately after a mode or favourite activation, ahead of the corresponding mode, active-channel, and channel-status broadcasts that announce the resulting state.

**Pattern:** `02 00 50 FF FF 80 00 05 0D E2`

**Example:**

```
02 00 50 FF FF 80 00 05 0D E2 01 01 03
                              ^^ Always 0x00 or 0x01 in observed captures
```

**Data Fields:**

- Byte 10: Acknowledgement value (always `0x00` or `0x01` in observed captures)

**Notes:**

- Decoded in code by `handle_touchscreen_unknown3` — log-only, no `pool_state` update.
- Triggered by [0x2A Mode/Favourite Control Command](#0x2a--modefavourite-control-command-); see that section for the full activation sequence.
- Status ⚠️ because the meaning of the constants `0x00` and `0x01` is unconfirmed — it could be a fixed "ack" sentinel or a single-value-observed flags field.

---

### 0x06 — Lighting Zone Configuration ✅

Indicates which lighting zones are installed and their current on/off state. Broadcast by the Touchscreen (`0x0050`).

**Pattern:** `02 00 50 FF FF 80 00 06 0E E4`

**Example:**

```
02 00 50 FF FF 80 00 06 0E E4 00 00 00 03
                              ^^ Zone index (0-3 for zones 1-4)
                                 ^^ Light status (00 off, 01 on)
```

**Data Fields:**

- Byte 10: Zone index (`0x00` to `0x03` for zones 1-4)
- Byte 11: Light status (`0x00` off, `0x01` on)

---

### 0x0A — Firmware Version ✅

Firmware-version announcement (`{major, minor}` payload) broadcast by multiple devices on the bus. CMD byte (`0x0A`) and payload layout are identical across every observed source — the source address selects which device is announcing its firmware. Dispatched in code by a single source-agnostic handler.

**Common pattern:** `02 00 ?? FF FF 80 00 0A 0E ??` (first `??` is the source LO byte; last `??` is the header checksum)

**Data Fields:**

- Byte 10: Major version number
- Byte 11: Minor version number
- Byte 12: Standard frame data checksum (`major + minor`)

**Known sources and observed samples:**

| Source   | Device                                   | Full prefix (bytes 0–9)                 | Observed payload (bytes 10–12) | Version       |
|----------|------------------------------------------|-----------------------------------------|--------------------------------|---------------|
| `0x0050` | Touchscreen                              | `02 00 50 FF FF 80 00 0A 0E E8`         | `02 08 0A`                     | 2.8           |
| `0x0062` | Connect 8/10 Controller                  | `02 00 62 FF FF 80 00 0A 0E FA`         | `02 06 08`                     | 2.6           |
| `0x0070` | Genus Heater (Active i25 Evo)            | `02 00 70 FF FF 80 00 0A 0E 08`         | _(observed; log-only, no dedicated state field)_ | —    |
| `0x0081` | VX 11S v3 Salt Chlorinator               | `02 00 81 FF FF 80 00 0A 0E 19`         | `05 02 07`                     | 5.2           |
| `0x0084` | Viron Chlorinator                        | `02 00 84 FF FF 80 00 0A 0E 1C`         | `05 07 0C`                     | 5.7           |
| `0x00A0` | Viron XT Pump                            | `02 00 A0 FF FF 80 00 0A 0E 38`         | `01 09 0A`                     | 1.9           |
| `0x00F0` | Internet Gateway                         | `02 00 F0 FF FF 80 00 0A 0E 88`         | `05 01 06` / `05 00 05`        | 5.1 / 5.0     |

**Example (Internet Gateway, v5.1):**

```
02 00 F0 FF FF 80 00 0A 0E 88 05 01 06 03
                              ^^ Major version (5)
                                 ^^ Minor version (1)
                                    → Version 5.1
```

**Notes:**

- Decoded by the source-agnostic `handle_firmware_version` handler (matches on `data[7] == 0x0A` regardless of source); state is stored in per-device fields on `pool_state` (`touchscreen_version_*`, `controller_version_*`, `chlor_version_*`, `gateway_version_*`). Genus Heater (`0x0070`) firmware is logged only — no dedicated state field.
- The same `{major, minor}` pair is also redundantly embedded in the Internet Gateway variant of [0x12 — Device Status](#0x12--device-status-️); firmware-version state population is performed once here.
- Broadcast at device startup; appears alongside other announcement broadcasts (mode, channel status, time).

---

### 0x0B — Channel Status ✅

Detailed status for all configured channels. Broadcast by the Touchscreen (`0x0050`).

**Pattern:** `02 00 50 FF FF 80 00 0B 25 00`

**Example:**

```
02 00 50 FF FF 80 00 0B 25 00 08 01 00 00 02 00 00 FE 00 00 FE 00 00 0B 02 01 09 00 00 FD 00 00 00 00 00 1B 03
                              ^^ Number of channels
                                 ^^  Channel 1: Type=1 (Filter)
                                    ^^ Channel 1: State (00 off, 01, Auto, 02 On)
                                       ^^ Channel 1:  currently active (either on or auto timer)
                                         ^^ Channel 2: Type=2 (Cleaner)
                                            etc
```

**Data Fields:**

- Byte 10: Number of channels
- Bytes 11+: For each channel (3 bytes):
  - Byte 0: Channel type — see lookup table below
  - Byte 1: Channel state (`0x00` Off, `0x01` Auto, `0x02` On)
  - Byte 2: Currently active (e.g. turned on by timer)

**Channel Types:**

- `0x00`: Unused
- `0x01`: Filter
- `0x02`: Cleaning
- `0x03`: Heater Pump
- `0x04`: Booster
- `0x05`: Waterfall
- `0x06`: Fountain
- `0x07`: Spa Pump
- `0x08`: Solar
- `0x09`: Blower
- `0x0A`: Swimjet
- `0x0B`: Jets
- `0x0C`: Spa Jets
- `0x0D`: Overflow
- `0x0E`: Spillway
- `0x0F`: Audio
- `0x11`: Hot Seat
- `0x12`: Heater Power
- `0x13`: Custom Name
- `0xFB`: Secondary Heater
- `0xFD`: Flagged as heater power
- `0xFE`: Flagged as light channel

**Channel States:**

- `0x00`: Off
- `0x01`: Auto
- `0x02`: On
- `0x03`: On — Low Speed
- `0x04`: On — Medium Speed
- `0x05`: On — High Speed

States `0x03`–`0x05` are used by channels driving a multi-speed pump (e.g. a Filter channel paired with the `0x00A0` Viron XT Variable Speed Pump) in place of the plain `0x02` On; simple on/off channels only use `0x00`–`0x02`. When a multi-speed channel enters one of these states, the Touchscreen unicasts the matching speed preset to the pump via [CMD `0x18`](#0x18--pump-speed-command-) (`0x03`→Low, `0x04`→Med, `0x05`→High) within ~130 ms of the channel status broadcast.

---

### 0x0D — Active Channels Bitmask ✅

Reports which channels are currently active. Unicast from the Touchscreen (`0x0050`) to Internal Channels (`0x006F`).

**Pattern:** `02 00 50 00 6F 80 00 0D 0D 5B`

**Example:**

```
02 00 50 00 6F 80 00 0D 0D 5B 10 10 03
                              ^^
                              Bitmask: 0x10 = Channel 5 active
```

**Data Fields:**

- Byte 10: Channel bitmask
  - Bit 7: Channel 8
  - Bit 6: Channel 7
  - Bit 5: Channel 6
  - Bit 4: Channel 5
  - Bit 3: Channel 4
  - Bit 2: Channel 3
  - Bit 1: Channel 2
  - Bit 0: Channel 1

---

### 0x0F — Chlorinator Mode → Touchscreen ⚠️

Inter-device unicast from the Viron Chlorinator (`0x0084`) to the Touchscreen (`0x0050`) reporting the chlorinator's current mode. Counterpart to the `0x18` cell-mode unicast that the chlorinator sends to the Viron XT Pump (`0x00A0`) — the two messages carry the same mode value and may briefly disagree during transitions.

**Pattern (provisional):** `02 00 84 00 50 80 00 0F ?? ??` (LENGTH and HDR_CHK to be confirmed from a capture)

**Data Fields (provisional):**

- Byte 10: Fixed `0x01` in observed captures (purpose unknown)
- Byte 11: Mode value — same encoding as the `0x18` cell-mode broadcast (`0x00`=Off, `0x01`=Auto, `0x02`=On — tentative)

**Notes:**

- ⚠️ Documented only — no handler in `message_decoder.c` yet. The layout came from contemporaneous capture analysis but a definitive sample pair has not been pinned down.
- Mode encoding follows the protocol-wide channel-state convention (see [0x0B](#0x0b--channel-status-)).
- The companion `0x18` cell-mode unicast is documented at [0x18](#0x18--chlorinator-cell-mode-️).

---

### 0x10 — Channel Toggle Command ⚠️

Command from the Internet Gateway (`0x00F0`) to cycle a channel through its available states (Auto → On → Off, or On → Off depending on channel type).

**Pattern:** `02 00 F0 FF FF 80 00 10 0D 8D`

**Examples:**

| Channel    | Index | Command                                   | States        |
| ---------- | ----- | ----------------------------------------- | ------------- |
| Filter     | 0x00  | `02 00 F0 FF FF 80 00 10 0D 8D 00 00 03`  | Auto, On, Off |
| Cleaning   | 0x01  | `02 00 F0 FF FF 80 00 10 0D 8D 01 01 03`  | Auto, On, Off |
| Pool Light | 0x02  | `02 00 F0 FF FF 80 00 10 0D 8D 02 02 03`  | Auto, On, Off |
| Spa Light  | 0x03  | `02 00 F0 FF FF 80 00 10 0D 8D 03 03 03`  | Auto, On, Off |
| Jets       | 0x04  | `02 00 F0 FF FF 80 00 10 0D 8D 04 04 03`  | On, Off       |
| Blower     | 0x05  | `02 00 F0 FF FF 80 00 10 0D 8D 05 05 03`  | On, Off       |

**Data Fields:**

- Byte 10: Channel index (0-based)
- Byte 11: Data checksum (equals channel index, as that is the only data byte)

**Channel Index Mapping:**

- `0x00`: Channel 1 (Filter)
- `0x01`: Channel 2 (Cleaning)
- `0x02`: Channel 3 (Pool Light)
- `0x03`: Channel 4 (Spa Light)
- `0x04`: Channel 5 (Jets)
- `0x05`: Channel 6 (Blower)

**Behaviour:**

- Each send **cycles** the channel to its next state; it does not set a specific state
- Channels with Auto support cycle: Auto → On → Off → Auto → ...
- Channels without Auto cycle: On → Off → On → ...
- The controller broadcasts the new channel state after processing the toggle

**Notes:**

- Sending this command always advances the state — there is no direct way to set a specific state.
- The controller will respond with an updated [Channel Status message (0x0B)](#0x0b--channel-status-).
- Channel index is 0-based and corresponds to the channel's position in the controller configuration.
- ⚠️ Multi-speed pump channels report extended states `0x03`–`0x05` (On at Low/Med/High — see [Channel States](#0x0b--channel-status-)), but how this command cycles through them has not been captured. Status is ⚠️ pending characterisation of how multi-speed channels respond to this command.

---

### 0x12 — Device Status ⚠️

Status broadcast emitted by multiple devices. The CMD byte is shared but the **payload layout differs per source** — there is no unified handler; each variant is dispatched by its own `MSG_TYPE_*` pattern and documented separately below.

**Source variants:**

| Source                          | LENGTH | Payload shape                            | Status | Handler                       |
|---------------------------------|--------|------------------------------------------|--------|-------------------------------|
| `0x0050` Touchscreen            | `0x0E` | 2 bytes — always `01 00` or `05 00` observed        | ⚠️     | `handle_touchscreen_unknown1` |
| `0x0062` Connect 8/10 Controller| `0x0F` | 3 bytes — heater state + unknowns        | ⚠️     | `handle_heater`               |
| `0x0074` ICI Gas Heater         | `0x10` | 4 bytes — `{00, status, 00, 00}`         | ✅     | `handle_ici_heater_status`    |
| `0x0081` VX 11S v3 Salt Chlorinator | `0x0D` | 2 bytes — always `00 00` observed   | ⚠️     | **No (doc only)**             |
| `0x0084` Viron / `0x0090` RolaChem Chlorinator | `0x0D` | 1 byte — operational mode | ⚠️     | `handle_chlor_status`         |
| `0x00F0` Internet Gateway       | `0x0F` | 3 bytes — `{major, minor, checksum}`     | ✅     | `handle_gateway_status`       |

---

#### Touchscreen (`0x0050`) ⚠️

Broadcast consistently after the firmware version message. Currently appears to always carry data `01 00` or `05 00`.

Pattern: `02 00 50 FF FF 80 00 12 0E F0`

Example: `02 00 50 FF FF 80 00 12 0E F0 05 00 05 03`

Data fields:
- Byte 10: Unknown (always `0x01` or `0x05` in observed samples)
- Byte 11: Unknown (always `0x00` in observed samples)

Part of the regular touchscreen status sequence.

---

#### Connect 8/10 Controller (`0x0062`) ⚠️

Broadcast by the main controller (`0x0062`) reporting the inbuilt heater's on/off state.

Pattern: `02 00 62 FF FF 80 00 12 0F 03`

Examples:

```
02 00 62 FF FF 80 00 12 0F 03 00 01 08 09 03   Heater On
02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03   Heater Off
                                 ^^ Heater state (0x00 = Off, 0x01 = On)
                                    ^^ Unknown (always 0x08 observed)
```

Data fields:
- Byte 10: Padding/unused
- Byte 11: Heater state (`0x00` = Off, `0x01` = On)
- Byte 12: Unknown (maybe bitmask or interlock?)

---

#### ICI Gas Heater (`0x0074`) ✅

Pattern: `02 00 74 FF FF 80 00 12 10 16`

Examples:

```
02 00 74 FF FF 80 00 12 10 16 00 00 00 00 00 03   Idle / off
02 00 74 FF FF 80 00 12 10 16 00 01 00 00 01 03   On and Lighting
02 00 74 FF FF 80 00 12 10 16 00 03 00 00 03 03   At Setpoint (on but not heating)
02 00 74 FF FF 80 00 12 10 16 00 07 00 00 07 03   (transitional?)
02 00 74 FF FF 80 00 12 10 16 00 0F 00 00 0F 03   Heater Lit and Running
                                 ^^ Status byte
```

Data fields:
- Byte 10: Always `0x00` in all observed samples
- Byte 11: Status byte — see table below
- Byte 12: Always `0x00` in all observed samples
- Byte 13: Always `0x00` in all observed samples

Observed status values (payload[1]):

| Value  | Meaning |
|--------|---------|
| `0x00` | Idle / off |
| `0x01` | On and Lighting (attempting ignition) |
| `0x03` | At Setpoint — on but not heating |
| `0x07` | Transitional? (observed briefly between `0x01` and `0x0F`) |
| `0x0F` | Heater Lit and Running |

Payload[1] is the only byte that varies; bytes 10, 12, and 13 are always `0x00`. The data checksum (byte 14) equals payload[1] since all other payload bytes are zero.

---

#### VX 11S v3 Salt Chlorinator (`0x0081`) ⚠️

Broadcast by the VX 11S v3 on the same ~60-second cycle as its CMD `0x1D` chlorine output level message. Payload is always `00 00` in all observed captures (normal operation). Meaning unknown — may carry status or warning flags.

Pattern: `02 00 81 FF FF 80 00 12 0D 20`

Example:

```
02 00 81 FF FF 80 00 12 0D 20  00 00  03
                               ^^ ^^
                               byte 10: unknown (always 0x00 observed)
                               byte 11: data checksum (sum of byte 10 = 0x00)
```

Data fields:
- Byte 10: Unknown — always `0x00` in observed captures; suspected status/warning flags (see [CMD 0x1D slot 0x00 notes](#0x1d--chlorinator-setpoint-))
- Byte 11: Data checksum (equals byte 10)

No handler in code — documented only.

---

#### Chlorinator (`0x0084` Viron / `0x0090` RolaChem) ⚠️

Carries the chlorinator's current operating mode. Both chlorinator address variants (mutually exclusive; see [Device Addresses](#device-addresses)) emit this with the same structure — the header checksum differs (`0x23` vs `0x2F`) purely because the source byte changes.

Patterns:
- Variant A (`0x0090` RolaChem): `02 00 90 FF FF 80 00 12 0D 2F`
- Variant B (`0x0084` Viron): `02 00 84 FF FF 80 00 12 0D 23`

Examples:

```
02 00 90 FF FF 80 00 12 0D 2F 01 01 03   RolaChem 0x0090, mode = 0x01 (Auto)
02 00 84 FF FF 80 00 12 0D 23 02 02 03   Viron 0x0084, mode = 0x02 (On)
```

Data fields:
- Byte 10: Mode value
- Byte 11: Data checksum (equals byte 10 — only one data byte)

Observed mode values (tentative; follows the standard channel-state convention from [0x0B](#0x0b--channel-status-)):

| Value  | Meaning |
|--------|---------|
| `0x00` | Off (not yet observed) |
| `0x01` | Auto    |
| `0x02` | On      |

Distinct from the configured *cell* mode at [0x18](#0x18--chlorinator-cell-mode-️) (unicast to the Viron XT Pump at `0x00A0`). The two can hold different values concurrently — e.g. chlorinator overall = On while cell = Auto. ⚠️ Tentative because a single-device Off↔Auto↔On transition has not been captured.

---

#### Internet Gateway (`0x00F0`) ✅

Firmware version broadcast by the Internet Gateway on startup. One byte longer than the other variants because it carries an embedded data-level checksum in addition to the standard frame checksum.

Pattern: `02 00 F0 FF FF 80 00 12 0F 91`

Example:

```
02 00 F0 FF FF 80 00 12 0F 91 05 01 06 0C 03
                              ^^ Major version (5)
                                 ^^ Minor version (1)
                                    ^^ Embedded checksum (major + minor)
                                       → Version 5.1
```

Data fields:
- Byte 10: Major version number
- Byte 11: Minor version number
- Byte 12: Embedded checksum — sum of bytes 10 and 11 (`major + minor`)

Observed samples:

| Sample (bytes 10–12) | Major | Minor | Embedded checksum |
|----------------------|-------|-------|-------------------|
| `05 01 06`           | 5     | 1     | `0x06` (=5+1)     |
| `05 00 05`           | 5     | 0     | `0x05` (=5+0)     |

Carries redundant firmware-version information already announced by [0x0A](#0x0a--firmware-version-); firmware-version state population is left to that handler alone. Broadcast at startup, paired with the gateway's `0x0A` firmware-version announcement.

---

### 0x14 — Mode (Spa/Pool) ✅

Reports the current operating mode — pool or spa. Broadcast by the Touchscreen (`0x0050`).

**Pattern:** `02 00 50 FF FF 80 00 14 0D F1`

**Examples:**

```
02 00 50 FF FF 80 00 14 0D F1 00 00 03   Spa mode
02 00 50 FF FF 80 00 14 0D F1 01 01 03   Pool mode
                              ^^
                              Mode: 0x00 = Spa, 0x01 = Pool
```

**Data Fields:**

- Byte 10: Mode (`0x00` = Spa, `0x01` = Pool)

---

### 0x16 — Water Temperature Reading ✅

Current water temperature broadcast by the device that measures it. Three sources are known: the Connect 8/10 Controller (`0x0062`) and the add-on Genus Heater family (`0x0070`, `0x0072`). All use CMD `0x16` and are dispatched on the CMD byte alone; the payload **length** distinguishes the two layouts.

**Source variants:**

| Source                          | LENGTH | Payload                       | Status |
|---------------------------------|--------|-------------------------------|--------|
| `0x0062` Connect 8/10 Controller| `0x0E` | 2 bytes — `{temp1, temp2}`    | ✅ |
| `0x0070`/`0x0072` Genus Heater  | `0x0D` | 1 byte  — `{temp1}`           | ✅ |
| `0x0074` ICI Gas Heater         | `0x0D` | 1 byte  — `{temp1}`           | ✅ |

All variants are handled by the unified `handle_temp_reading()`, which selects the layout from `payload_len`. The Connect 8/10 also emits a second water-temperature variant under [0x31 — Water Temperature Reading (alt)](#0x31--water-temperature-reading-alt-) — same `{temp1, temp2}` field layout, routed through the same handler, but log-only and with a different disconnected-sensor encoding (`>= 0xA0` rather than `0x00`).

**Invalid-reading sentinel:** any temperature byte with a raw value `>= 0xA0` (≥ 160°C) indicates a disconnected or invalid sensor. The handler logs these as warnings and does not publish them to MQTT.

---

#### Connect 8/10 Controller (`0x0062`) ✅

Pattern: `02 00 62 FF FF 80 00 16 0E 06`

Example:

```
02 00 62 FF FF 80 00 16 0E 06 19 00 19 03
                              ^^ Current water temperature 1 (0x19 = 25°C)
                                 ^^ Current water temperature 2 in °C (0x00 in this sample)
```

Data fields:
- Byte 10: Current water temperature 1 in °C
- Byte 11: Current water temperature 2 in °C — second sensor reading; often `0x00` in installations with only one sensor wired.

---

#### Genus Heater (`0x0070`) / HiNRG Gas Heater (`0x0072`) / ICI Gas Heater (`0x0074`) ✅

When an Active i25 Evo (Genus) / HiNRG Gas Heater or ICI Gas Heater is fitted it broadcasts its own current water-temperature reading on the same CMD but with a shorter LENGTH (`0x0D`) and only one data byte.

Pattern (`0x0070`): `02 00 70 FF FF 80 00 16 0D 13`
Pattern (`0x0072`): `02 00 72 FF FF 80 00 16 0D 15`
Pattern (`0x0074`): `02 00 74 FF FF 80 00 16 0D 17`

Example:

```
02 00 70 FF FF 80 00 16 0D 13 12 12 03
                              ^^ Current water temperature (0x12 = 18°C)
                                 ^^ Data checksum (equals byte 10 — only one data byte)
```

Data fields:
- Byte 10: Current water temperature in °C
- Byte 11: Data checksum (equals byte 10)

This is the Heater's own water-temperature reading; it is independent of the controller's reading and may differ if the two sensors are sited differently in the plumbing. 

Handled by the unified `handle_temp_reading()` via the LEN `0x0D` path.

---

### 0x17 — Temperature Settings ✅

Setpoint broadcast. CMD `0x17` is shared across two sources with different payload layouts: the Touchscreen (`0x0050`) emits spa/pool setpoints in both °C and °F, while the heater devices (`0x0070`/`0x0072`/`0x0074`) emit their spa/pool setpoints in °C only.

**Source variants:**

| Source                | LENGTH | Payload                                | Status | Handler                       |
|-----------------------|--------|----------------------------------------|--------|-------------------------------|
| `0x0050` Touchscreen  | `0x10` | 4 bytes — spa/pool setpoint °C + spa/pool setpoint °F    | ✅     | `handle_temp_setting`         |
| `0x0070` Genus Heater | `0x0E` | 2 bytes — Spa setpoint °C, Pool setpoint °C | ✅     | `handle_genus_heater_temp_setting`|
| `0x0072` HiNRG Heater | `0x0E` | 2 bytes — Spa setpoint °C, Pool setpoint °C | ✅     | `handle_genus_heater_temp_setting`|
| `0x0074` ICI Gas Heater | `0x0E` | 2 bytes — Spa setpoint °C, Pool setpoint °C | ✅     | `handle_ici_heater_temp_setting`  |

The same setpoints are also broadcast individually via the register system — see the [Register-based variant](#register-based-temperature-setpoints) below.

---

#### Touchscreen (`0x0050`) ✅

Pattern: `02 00 50 FF FF 80 00 17 10 F7`

Example:

```
02 00 50 FF FF 80 00 17 10 F7 25 1D 63 54 F9 03
                              ^^ Spa setpoint °C (0x25 = 37°C)
                                 ^^ Pool setpoint °C (0x1D = 29°C)
                                    ^^ Spa setpoint °F (0x63 = 99°F)
                                       ^^ Pool setpoint °F (0x54 = 84°F)
```

Data fields:
- Byte 10: Spa setpoint temperature (°C)
- Byte 11: Pool setpoint temperature (°C)
- Byte 12: Spa setpoint temperature (°F)
- Byte 13: Pool setpoint temperature (°F)

Temperature scale (Celsius vs Fahrenheit) is set by [0x26 Configuration](#0x26--configuration-️).

---

#### Genus Heater (`0x0070`) / HiNRG Gas Heater (`0x0072`) / ICI Gas Heater (`0x0074`) ✅

When an Active i25 Evo (Genus) / HiNRG Gas Heater or ICI Gas Heater is fitted it broadcasts its own setpoints using the same CMD but a shorter LENGTH and a different payload — both heater setpoints in a single frame, °C only.

Pattern (`0x0070`): `02 00 70 FF FF 80 00 17 0E 15` (Genus Heater)
Pattern (`0x0072`): `02 00 72 FF FF 80 00 17 0E 17` (HiNRG Gas Heater)
Pattern (`0x0074`): `02 00 74 FF FF 80 00 17 0E 19` (ICI Gas Heater)

Example:

```
02 00 70 FF FF 80 00 17 0E 15 18 1B 33 03
                              ^^ Spa setpoint °C (0x18 = 24°C)
                                 ^^ Pool setpoint °C (0x1B = 27°C)
                                    ^^ Data checksum (0x18 + 0x1B = 0x33)
```

Data fields:
- Byte 10: Spa setpoint (°C)
- Byte 11: Pool setpoint (°C)
- Byte 12: Data checksum (sum of bytes 10–11)

Both setpoints are carried in a single broadcast; these heaters never send them separately. The actual current water temperature is reported separately via [0x16](#0x16--water-temperature-reading-) (Genus Heater variant).

---

#### Register-based Temperature Setpoints

The controller also broadcasts pool and spa setpoints as individual register messages (one per message, Celsius only) using CMD `0x38` — see [0x38 Register Data](#0x38--register-data-response) and [Appendix A](#appendix-a-register-dispatch-table) for the full register dispatch system.

Pattern: `02 00 50 FF FF 80 00 38 0F 17`

Examples:

```
02 00 50 FF FF 80 00 38 0F 17 E7 00 1D 04 03   Pool setpoint = 29°C (register 0xE7)
02 00 50 FF FF 80 00 38 0F 17 E8 00 25 0D 03   Spa setpoint  = 37°C (register 0xE8)
                              ^^ Register ID
                                 ^^ Slot
                                    ^^ Temperature in °C
```

---

### 0x18 — Pump Speed Command ✅

Inter-device unicast sent by the controller (Touchscreen `0x0050` or Viron Chlorinator `0x0084`) to the Viron XT Pump (`0x00A0`) to set the pump speed. The controller sends this periodically (approx. every 60 seconds) and whenever the speed needs to change (e.g., due to timers or manual mode changes).

**Pattern:** `02 00 50 00 A0 80 00 18 0D 97` (Touchscreen)
**Pattern:** `02 00 84 00 A0 80 00 18 0D CB` (Chlorinator)

**Examples:**
```
02 00 50 00 A0 80 00 18 0D 97 02 02 03   Touchscreen sets Pump to High
                              ^^ Target Speed (0x02 = High)
```

**Data Fields:**

- Byte 10: Speed command value
- Byte 11: Data checksum (equals byte 10 — single data byte)

**Observed values:**

| Value  | Meaning    |
|--------|------------|
| `0x00` | Low Speed  |
| `0x01` | Med Speed  |
| `0x02` | High Speed |

**Notes:**

- The Touchscreen sends this command to implement its timers
- The speed value mirrors the driving channel's extended state in the [Channel Status (0x0B)](#0x0b--channel-status-) broadcast: channel states `0x03`/`0x04`/`0x05` (On at Low/Med/High) map to speed `0x00`/`0x01`/`0x02`, with the `0x18` unicast following the channel broadcast within ~130 ms.
- The `0x0090` RolaChem chlorinator variant has not been observed using this command; the `0x18` traffic appears specific to the `0x0084` Viron / `0x00A0` Viron XT Pump two-module chlorinator topology.

---

### 0x19 — Temperature Setpoint Command ✅

Command from the Internet Gateway (`0x00F0`) to set the pool or spa temperature setpoint. The temperature byte is repeated twice within the payload.

**Pattern:** `02 00 F0 FF FF 80 00 19 0F 98`

**Examples:**

```
02 00 F0 FF FF 80 00 19 0F 98 01 1E 1E 3D 03   Set Pool to 30°C
02 00 F0 FF FF 80 00 19 0F 98 02 25 25 4C 03   Set Spa  to 37°C
                              ^^ Target (0x01 = Pool, 0x02 = Spa)
                                 ^^ Temperature °C
                                    ^^ Temperature °C (repeated)
                                       ^^ Data checksum (sum of bytes 10–12)
```

**Data Fields:**

- Byte 10: Target (`0x01` = Pool, `0x02` = Spa)
- Byte 11: Temperature in °C
- Byte 12: Temperature in °C (repeated)
- Byte 13: Data checksum (sum of bytes 10–12)

**Notes:**

- The temperature value is repeated at bytes 11 and 12 — this is part of the message format, not two separate sends.
- The controller will respond with an updated [Temperature Settings message (0x17)](#0x17--temperature-settings-).

---

### 0x1B — Pump Button Activity ✅

Broadcast by the Viron XT Pump (`0x00A0`) when one of the three preset speed buttons is pressed on the physical pump panel.

**Pattern:** `02 00 A0 FF FF 80 00 1B 0D 48`

**Example:**

```
02 00 A0 FF FF 80 00 1B 0D 48 00 00 03   
                              ^^ Low button pressed
02 00 A0 FF FF 80 00 1B 0D 48 01 01 03   
                              ^^ Med button pressed
02 00 A0 FF FF 80 00 1B 0D 48 02 02 03   
                              ^^ HIGH button pressed
```

**Data Fields:**

- Byte 10: Button identifier
- Byte 11: Data checksum (equals byte 10 — single data byte)

**Observed values:**

| Value  | Button |
|--------|--------|
| `0x00` | Low    |
| `0x01` | Med    |
| `0x02` | High   |

**Notes:**

- Only the preset speed buttons (LOW, MED, HIGH) trigger this message.
- Other buttons on the panel (Power ON/OFF, Menu, Enter, UP/DOWN arrows) do **not** trigger a `0x1B` broadcast.
- The controller's [0x18 Speed Command](#0x18--pump-speed-command-) will override manual button presses during its next scheduled broadcast (every 60s).
- Decoded in code by `handle_pump_buttons` — log-only, no `pool_state` update.

---

### 0x1D — Chlorinator Setpoint ✅

Setpoint broadcasts from chlorinator devices. The slot byte (byte 10) selects which value the message carries. Three sources are known; slots `0x01` and `0x02` come from the chemistry controllers while slot `0x00` comes from the VX 11S v3 Salt Chlorinator.

**Patterns:**

| Source | Pattern |
|--------|---------|
| `0x0090` RolaChem | `02 00 90 FF FF 80 00 1D 0F 3C` |
| `0x0084` Viron    | `02 00 84 FF FF 80 00 1D 0F 30` |
| `0x0081` VX 11S v3 | `02 00 81 FF FF 80 00 1D 0F 2D` |

**Slot variants:**

| Slot | Source | Meaning | Value units |
|------|--------|---------|-------------|
| `0x00` | `0x0081` | Chlorine output level | Integer 1–8 (byte 11); byte 12 = `0x00` |
| `0x01` | `0x0090`, `0x0084` | pH setpoint | pH × 10, little-endian (e.g. `4E 00` = 78 → 7.8) |
| `0x02` | `0x0090`, `0x0084` | ORP setpoint | mV, little-endian (e.g. `8A 02` = 0x028A = 650 mV) |

**Examples:**

```
02 00 90 FF FF 80 00 1D 0F 3C 01 4E 00 4F 03   pH  setpoint = 7.8
02 00 90 FF FF 80 00 1D 0F 3C 02 8A 02 8E 03   ORP setpoint = 650 mV
                              ^^ Slot (0x01 = pH, 0x02 = ORP)
                                 ^^ ^^ Value (little-endian)
                                       ^^ Data checksum

02 00 81 FF FF 80 00 1D 0F 2D 00 03 00 03 03   Chlorine output level = 3
02 00 81 FF FF 80 00 1D 0F 2D 00 02 00 02 03   Chlorine output level = 2
                              ^^ Slot (0x00 - Chlorine Output Level)
                                 ^^ Chlorine Output Level value
                                    ^^ Always 0x00
                                       ^^ Data checksum
```

**Data Fields (RolaChem and Viron Chlorinators):**

- Byte 10: Slot `0x01` = pH, `0x02` = ORP)
- Bytes 11-12: Value (little-endian; pH × 10 or mV depending on slot)

**Data Fields (VX 11S v3 Salt Chlorinator):**

- Byte 10: `0x00` in all observed captures.
- Byte 11: Chlorine output level (integer 1–8; matches the 8 physical LEDs on the device)
- Byte 12: `0x00` in all observed captures (normal operation). This could potentially carry warning flags — the device has two warnings lights (LOW SALT, NO FLOW) that may be encoded here as bits. ⚠️ Unconfirmed: capture a message while a warning is active to verify.

**Notes (VX 11S v3 Salt Chlorinator):**

- Per the VX 11S v3 manual: "This 'chlorine output level' only applies to Pool Mode. When the Chlorinator is in Spa mode, the chlorine output will be at level 1."
- During a Pool→Spa transition the device continued broadcasting the displayed level value (e.g. `03`) unchanged — it seems to broadcast the configured level, not the effective output?
- The chlorine output level value is user-set via the physical buttons. Unsure if it is bus-controllable.
- The device has physical Pool mode, Spa mode, and Safety Backwash buttons with indicator LEDs. Whether pressing these buttons generates bus traffic is unconfirmed.

---

### 0x1F — Chlorinator Reading ✅

Current pH or ORP reading from the RolaChem chlorinator's sensors (`0x0090`). The slot byte (byte 10) selects which value the message carries — same shape as [0x1D Setpoint](#0x1d--chlorinator-setpoint-).

**Pattern:** `02 00 90 FF FF 80 00 1F 0F 3E`

**Slot variants:**

| Slot | Meaning      | Value units                                       |
|------|--------------|---------------------------------------------------|
| `0x01` | pH reading  | pH × 10, little-endian (e.g. `55 00` = 85 → 8.5)  |
| `0x02` | ORP reading | mV, little-endian (e.g. `0A 02` = 0x020A = 522 mV) |

**Examples:**

```
02 00 90 FF FF 80 00 1F 0F 3E 01 55 00 56 03   pH  reading = 8.5
02 00 90 FF FF 80 00 1F 0F 3E 02 0A 02 0E 03   ORP reading = 522 mV
                              ^^ Slot (0x01 = pH, 0x02 = ORP)
                                 ^^ ^^ Value (little-endian)
                                       ^^ Data checksum
```

**Data Fields:**

- Byte 10: Slot (`0x01` = pH, `0x02` = ORP)
- Bytes 11-12: Value (little-endian; pH × 10 or mV depending on slot)

---

### 0x25 — Valve Sync ✅

Unicast from the Touchscreen (`0x0050`) to Internal Channels (`0x006F`) carrying the overall valve-active bitmask. Emitted as part of the regular broadcast cycle. No handler in `message_decoder.c` — documented only.

**Pattern:** `02 00 50 00 6F 80 00 25 0D 73`

**Examples:**

```
02 00 50 00 6F 80 00 25 0D 73 00 00 03   No valves active
02 00 50 00 6F 80 00 25 0D 73 01 01 03   Valve 1 active
                              ^^ Active bitmask
                                 ^^ Data checksum (equals byte 10)
```

**Data Fields:**

- Byte 10: Valve-active bitmask — bit 0 = valve 1, bit 1 = valve 2 (`0x00` = none active)
- Byte 11: Data checksum (equals byte 10)

**Observed values:**

| Value  | Meaning              |
|--------|----------------------|
| `0x00` | No valves active     |
| `0x01` | Valve 1 active only  |
| `0x02` | Valve 2 active only  |
| `0x03` | Both valves active   |

**Notes:**

- Unlike most touchscreen messages, this is addressed specifically to Internal Channels (`0x006F`), not broadcast.
- Mirrors the OR of all `active` flags in [0x27 Valve State Broadcast](#0x27--valve-state-broadcast-), encoded as a bitmask.
- Emitted every broadcast cycle (~60 s); may lag real-time valve state changes by up to one cycle.

---

### 0x26 — Configuration ⚠️

Broadcast from the Touchscreen (`0x0050`) carrying system configuration including temperature scale, heater type, and current heater on/off state.

**Pattern:** `02 00 50 FF FF 80 00 26 0E 04`

**Example — Celsius:**

```
02 00 50 FF FF 80 00 26 0E 04 01 06 07 03
                              ^^ 0x01 - Celsius
                                 ^^ Unknown
```

**Example — Fahrenheit:**

```
02 00 50 FF FF 80 00 26 0E 04 11 06 17 03
                              ^^ 0x11 - Fahrenheit
                                 ^^ Unknown
```

**Data Fields:**

- Byte 10: Configuration bitmask
  - Bit 7:
  - Bit 6:
  - Bit 5:
  - Bit 4: `0` = Celsius, `1` = Fahrenheit
  - Bit 3: `0` = heater Off, `1` = heater currently On (live state, not a config flag)
  - Bit 2: `0` = 1° temperature step, `1` = 2° temperature step
  - Bit 1: `0` = heat, `1` = cooler-only
  - Bit 0: Unknown — always `1` in observed samples
- Byte 11: Unknown (consistently `0x06`)

**Observed Byte 10 values:**

| Value  | Binary       | Meaning                                                   |
|--------|--------------|-----------------------------------------------------------|
| `0x01` | `0000 0001`  | Celsius, heat, heater Off, 1° step                        |
| `0x03` | `0000 0011`  | Celsius, **cooler-only**, heater Off, 1° step             |
| `0x05` | `0000 0101`  | Celsius, heat, heater Off, **2° step**                    |
| `0x09` | `0000 1001`  | Celsius, heat, **heater On**, 1° step                     |
| `0x11` | `0001 0001`  | Fahrenheit, heat, heater Off, 1° step                     |

---

### 0x27 — Valve State Broadcast ✅

Broadcast by the Touchscreen (`0x0050`) to report the configured and active state of all valve zones. Appears in two LENGTH variants.

**Pattern (short form):** `02 00 50 FF FF 80 00 27 0D 04`

Used at startup before valve state is available; always carries a single zero data byte.

**Pattern (long form):** `02 00 50 FF FF 80 00 27 13 0A`

Carries live per-valve state. Each valve occupies 3 bytes (configured flag, state, active flag).

**Example — Short form:**

```
02 00 50 FF FF 80 00 27 0D 04 00 00 03
                              ^^ Data (always 0x00)
```

**Example — Long form, both valves off:**

```
02 00 50 FF FF 80 00 27 13 0A 02 01 00 00 01 00 00 04 03
                              ^^ Slot count (0x02 = 2 slots)
                                 ^^ Valve 1 configured (0x01 = yes)
                                    ^^ Valve 1 state (0x00 = Off)
                                       ^^ Valve 1 active (0x00 = Inactive)
                                          ^^ Valve 2 configured (0x01 = yes)
                                             ^^ Valve 2 state (0x00 = Off)
                                                ^^ Valve 2 active (0x00 = Inactive)
```

**Example — Long form, Valve 1 On and active:**

```
02 00 50 FF FF 80 00 27 13 0A 02 01 02 01 01 00 00 07 03
                                    ^^ Valve 1 state: 0x02 = On
                                       ^^ Valve 1 active: 0x01 = Active
```

**Example — Long form, Valve 2 On and active:**

```
02 00 50 FF FF 80 00 27 13 0A 02 01 00 00 01 02 01 07 03
                                          ^^ Valve 2 state: 0x02 = On
                                             ^^ Valve 2 active: 0x01 = Active
```

**Data Fields (long form):**

- Byte 10: Valve slot count (0x02 = 2 slots)
- Bytes 11–13: Valve 1 entry:
  - Byte 11: Configured (`0x00` = not present, `0x01` = configured)
  - Byte 12: State (`0x00` = Off, `0x01` = Auto, `0x02` = On)
  - Byte 13: Active (`0x00` = Inactive, `0x01` = Active)
- Bytes 14–16: Valve 2 entry (same layout as bytes 11–13)

**State Values:**

- `0x00`: Off
- `0x01`: Auto (only for valves configured with Auto mode)
- `0x02`: On

**Notes:**

- The short form (LENGTH=`0x0D`) appears at startup; the long form (LENGTH=`0x13`) carries live state
- Valves not yet configured appear as `00 00 00` in their slot
- Whether a valve supports Auto mode depends on its configuration; in the observed capture valve 1 was configured without Auto, valve 2 was configured with Auto
- Valve labels are stored via the register system (`0xD0`–`0xD1`, Slot `0x02`); see [Appendix A](#appendix-a-register-dispatch-table)
- State transitions correlate exactly with [0x25 Valve Sync](#0x25--valve-sync-)

---

### 0x28 — Valve Control Command ✅

Sent by the Internet Gateway (`0x00F0`) to set a valve to a specific state directly. Unlike the [Channel Toggle Command (0x10)](#0x10--channel-toggle-command-️) which cycles through states, this sets the target state explicitly. No handler in `message_decoder.c` — documented only.

**Pattern:** `02 00 F0 FF FF 80 00 28 0E A6`

**Examples:**

| Command      | Full message                                |
|--------------|---------------------------------------------|
| Valve 1 Off  | `02 00 F0 FF FF 80 00 28 0E A6 00 00 00 03` |
| Valve 1 Auto | `02 00 F0 FF FF 80 00 28 0E A6 00 01 01 03` |
| Valve 1 On   | `02 00 F0 FF FF 80 00 28 0E A6 00 02 02 03` |
| Valve 2 Off  | `02 00 F0 FF FF 80 00 28 0E A6 01 00 01 03` |
| Valve 2 Auto | `02 00 F0 FF FF 80 00 28 0E A6 01 01 02 03` |
| Valve 2 On   | `02 00 F0 FF FF 80 00 28 0E A6 01 02 03 03` |

**Data Fields:**

- Byte 10: Valve index, 0-based (`0x00`=Valve 1, `0x01`=Valve 2)
- Byte 11: Target state (`0x00`=Off, `0x01`=Auto, `0x02`=On)
- Byte 12: Data checksum = (byte 10 + byte 11) & 0xFF

**Notes:**

- Whether Auto is accepted by the controller depends on the valve's configuration
- The controller responds immediately with an updated Valve State Broadcast ([0x27](#0x27--valve-state-broadcast-))

---

### 0x2A — Mode/Favourite Control Command ✅

Command sent by the Internet Gateway (`0x00F0`) to the Touchscreen (`0x0050`) to switch modes or activate a stored Favourite preset. A single data byte encodes the target mode or favourite index.

**Pattern:** `02 00 F0 00 50 80 00 2A 0D F9`

**Examples:**

```
02 00 F0 00 50 80 00 2A 0D F9 00 00 03   Pool mode (all extras off)
02 00 F0 00 50 80 00 2A 0D F9 01 01 03   Spa mode
02 00 F0 00 50 80 00 2A 0D F9 02 02 03   Activate Favourite 1
02 00 F0 00 50 80 00 2A 0D F9 80 80 03   All Off mode
02 00 F0 00 50 80 00 2A 0D F9 81 81 03   All Auto mode
                              ^^ Mode/favourite byte
                                 ^^ Data checksum (equals the mode byte)
```

**Data Fields:**

- Bytes 1-2: `00 F0` — Source (Internet Gateway = `0x00F0`)
- Bytes 3-4: `00 50` — Destination (Touchscreen = `0x0050`) — **not broadcast**
- Byte 10: Mode/favourite value (see table below)
- Byte 11: Data checksum (equals byte 10 since it is the only data byte)

**Mode/Favourite Values:**

| Value  | Meaning       | Label register (slot `0x03`) | Enable register (slot `0x03`) |
|--------|---------------|------------------------------|-------------------------------|
| `0x00` | Pool mode     | `0x31` — always `"Pool"`     | `0x21` — always `0x01`        |
| `0x01` | Spa mode      | `0x32` — always `"Spa"`      | `0x22` — always `0x01`        |
| `0x02` | Favourite 1   | `0x33` — user-defined label  | `0x23` — `0x01`=enabled, `0x00`=disabled |
| `0x03` | Favourite 2   | `0x34` — user-defined label  | `0x24` — `0x01`=enabled, `0x00`=disabled |
| `0x04` | Favourite 3   | `0x35` — user-defined label  | `0x25` — `0x01`=enabled, `0x00`=disabled |
| `0x05` | Favourite 4   | `0x36` — user-defined label  | `0x26` — `0x01`=enabled, `0x00`=disabled |
| `0x06` | Favourite 5   | `0x37` — user-defined label  | `0x27` — `0x01`=enabled, `0x00`=disabled |
| `0x07` | Favourite 6   | `0x38` — user-defined label  | `0x28` — `0x01`=enabled, `0x00`=disabled |
| `0x80` | All Off mode  | — (no label register)        | — (always available)          |
| `0x81` | All Auto mode | — (no label register)        | — (always available)          |

**Notes:**

- **Destination is Touchscreen (`0x0050`), not broadcast** — addressed specifically to the touchscreen, which holds the stored Favourite presets and applies them
- **Command values are inverted from status values** — in status messages ([0x14 Mode](#0x14--mode-spapool-)), Spa=`0x00` and Pool=`0x01`; in this command, Pool=`0x00` and Spa=`0x01`
- The Touchscreen acknowledges each activation with an immediate [0x05 Touchscreen Activation Ack](#0x05--touchscreen-activation-ack-️) (value `0x01`) followed by the relevant mode, active-channel, and channel-status broadcasts
- Up to 6 user Favourites are supported (`0x02`–`0x07`). The labels for all 8 slots (including the Pool and Spa built-ins) are stored in registers `0x31`–`0x38` (slot `0x03`), readable via the register protocol ([0x38](#0x38--register-data-️))
- Each slot's enabled/disabled state is stored in registers `0x21`–`0x28` (slot `0x03`), with `0x01` = enabled and `0x00` = disabled. Pool (`0x21`) and Spa (`0x22`) are always `0x01`. All Off (`0x80`) and All Auto (`0x81`) have no corresponding enable registers and are always available
- This command requires the sender to impersonate the Internet Gateway (source address `0x00F0`)

---

### 0x2B — Unknown ⚠️

Unicast message sent by the Connect 8/10 Controller (`0x0062`) directly to the Touchscreen (`0x0050`). Purpose is unknown.

**Pattern:** `02 00 62 00 50 80 00 2B 0E 6D`

**Example:**

```
02 00 62 00 50 80 00 2B 0E 6D 02 00 02 03
                              ^^ ^^ Unknown payload bytes
```

**Data Fields:**

- Byte 10: `0x02` — meaning unknown
- Byte 11: `0x00` — meaning unknown

**Notes:**

- Payload `02 00` is the only value observed across all captures.
- Sent approximately every 60 seconds.
- Not decoded in firmware — logged as unhandled.

---

### 0x31 — Water Temperature Reading (alt) ✅

Second water-temperature variant broadcast by the Connect 8/10 Controller (`0x0062`), in parallel to the [0x16](#0x16--water-temperature-reading-) reading. The two CMDs carry the same `{temp1, temp2}` field layout; the only practical difference is the **disconnected-sensor encoding**:

- CMD `0x16` reports a disconnected sensor as `0x00` (indistinguishable from a genuine 0°C reading).
- CMD `0x31` reports a disconnected sensor as `>= 0xA0` (a clean sentinel — observed values include `0xA6`, `0xAD`, `0xAF`).

Confirmed by paired captures: whenever a CMD `0x31` byte reads `>= 0xA0`, the corresponding CMD `0x16` byte in the same broadcast cycle reads `0x00`.

**Pattern:** `02 00 62 FF FF 80 00 31 0E 21`

```
02 00 62 FF FF 80 00 31 0E 21 1E A6 C4 03
                              ^^ Current water temperature 1 (0x1E = 30°C)
                                 ^^ Current water temperature 2 (0xA6 = disconnected sensor)
```

**Data Fields:**

- Byte 10: Current water temperature 1 in °C (`>= 0xA0` = disconnected)
- Byte 11: Current water temperature 2 in °C (`>= 0xA0` = disconnected)

**Notes:**

- Both `0x16` and `0x31` are routed through the same `handle_temp_reading()` (dispatched on the CMD byte). `0x16` is the canonical source — it updates `pool_state->current_temp` and publishes to MQTT. `0x31` is log-only to avoid dual MQTT updates for the same reading (the Connect 8/10 broadcasts them ~70 ms apart).
- Byte 10 has been observed decreasing as pool water cools (30→25°C), confirming it as the current temperature.

---

### 0x37 — Internet Gateway Info ⚠️

Broadcast by the Internet Gateway (`0x00F0`) reporting gateway-level information. Three variants share the same CMD `0x37` and source, distinguished by the LENGTH byte.

| LENGTH | Variant                                                           | Purpose                       |
|--------|-------------------------------------------------------------------|-------------------------------|
| `0x11` | [Serial Number](#serial-number-len-0x11-)                         | Gateway module serial         |
| `0x15` | [Network Config](#network-config-len-0x15-)                       | IP address + WiFi signal      |
| `0x0F` | [Communications Status](#communications-status-len-0x0f-)         | Internet connection state     |

---

#### Serial Number (LEN `0x11`) ⚠️

Serial number of the internet gateway module.

**Pattern:** `02 00 F0 FF FF 80 00 37 11 B8`

**Example:**

```
02 00 F0 FF FF 80 00 37 11 B8 04 A3 15 21 00 DD 03
                              ^^ Unknown
                                 ^^ ^^ ^^ ^^ Serial number (little endian)
                                               0x002115A3 = 2168227
```

**Data Fields:**

- Byte 10: Unknown (maybe a type `0x04`)
- Bytes 11-14: Serial number (32-bit little endian)

---

#### Network Config (LEN `0x15`) ⚠️

IP address and signal strength of the gateway.

**Pattern:** `02 00 F0 FF FF 80 00 37 15 BC`

**Example — On startup (no connection):**

```
02 00 F0 FF FF 80 00 37 15 BC 01 01 01 03 00 00 00 00 00 06 03
```

**Example — With IP address (wifi connected):**

```
02 00 F0 FF FF 80 00 37 15 BC 01 01 01 07 C0 A8 00 17 2B B4 03
                              ^^ Unknown
                                 ^^ Unknown
                                    ^^ Unknown
                                       ^^ Unknown
                                          ^^ ^^ ^^ ^^ IP address (192.168.1.23)
                                                      ^^ Signal level (43)
```

**Data Fields:**

- Byte 10: Unknown
- Byte 11: Unknown
- Byte 12: Unknown
- Byte 13: Unknown
- Bytes 14-17: IP address (4 bytes, standard order)
- Byte 18: WiFi signal level (0-100)

---

#### Communications Status (LEN `0x0F`) ⚠️

Status of the gateway's internet connection.

**Pattern:** `02 00 F0 FF FF 80 00 37 0F B6`

**Example — Communicating with server:**

```
02 00 F0 FF FF 80 00 37 0F B6 02 01 80 83 03
                              ^^ Unknown
                                 ^^ ^^ Status code (little endian)
                                          0x8001 = 32769: Communicating with server
```

**Data Fields:**

- Byte 10: Unknown (observed as always `0x02`)
- Bytes 11-12: Communications status code (little endian)

**Status Codes:**

- `0x0000`: `0` Idle
- `0x0100`: `256` No suitable interfaces ready
- `0x0201`: `513` DNS resolve error
- `0x0301`: `769` Internal error creating local socket
- `0x0400`: `1024` Connecting to server
- `0x0401`: `1025` Failed to connect
- `0x8000`: `32768` Connection open
- `0x8001`: `32769` Communicating with server
- `0xF000`: `61440` Connection closed
- `0xF001`: `61441` Communication error with server
- `0xF002`: `61442` Communication error with server
- `0xF003`: `61443` Communication error with server
- `0xF004`: `61444` Communication error with server

---

### 0x38 — Register Data ⚠️

The controller uses a unified register-based system for configuration and state. Broadcast by the Touchscreen (`0x0050`). All register messages share the same base pattern `02 00 50 FF FF 80 00 38` — only the register ID, slot, and data payload vary.

> See [Appendix A](#appendix-a-register-dispatch-table) for the full register dispatch table, examples by register type, register ID mappings, and the firmware dispatch implementation.

**Base Pattern:** `02 00 50 FF FF 80 00 38`

**Complete Structure:**

```
02 00 50 FF FF 80 00 38 [LENGTH] [HEADER_CHECKSUM] [REG_ID] [SLOT] [DATA...] [DATA_CHECKSUM] 03
                                 ^^^^^^^^^^^^^^^^
                                 sum(bytes 0–8) & 0xFF
                                 For this base pattern, bytes 0–7 sum to 776 ≡ 8 (mod 256),
                                 so HEADER_CHECKSUM = LENGTH + 8
```

**Example:**

```
02 00 50 FF FF 80 00 38 0F 17 C0 01 00 C1 03
                        ^^ LENGTH (0x0F = 15 bytes total)
                           ^^ HEADER_CHECKSUM (0x17 = 0x0F + 8, since bytes 0–7 sum to 8 mod 256)
                              ^^ Register ID (0xC0)
                                 ^^ Slot/Data Type (0x01)
                                    ^^ Data (Light state: 0=Off)
```

**Data Fields:**

- Byte 8 (LENGTH): Total message length in bytes (including `0x02` start and `0x03` end)
- Byte 9 (HEADER_CHECKSUM): Sum of bytes 0–8, masked to 8 bits
- Byte 10 (REG_ID): Register identifier (which setting/channel/zone)
- Byte 11 (SLOT): Data slot — defines data type and format
- Byte 12+: Data payload (varies by register and slot)

**Notes:**

- The header checksum formula `HEADER_CHECKSUM = LENGTH + 8` holds specifically for register messages because bytes 0–7 (`02 00 50 FF FF 80 00 38`) always sum to 776 ≡ 8 (mod 256). This is a consequence of the fixed base pattern, not a separate rule.

#### Timer Registers (Slot 0x04)

Timer schedule configuration. Each timer has a start time, stop time, and a days-of-week bitmask. Up to 16 timers are supported (registers `0x08`–`0x17`).

**Pattern:** `02 00 50 FF FF 80 00 38 13 1B` (LENGTH=0x13=19 bytes, HEADER_CHECKSUM=0x1B)

**Examples:**

```
02 00 50 FF FF 80 00 38 13 1B 08 04 08 00 0C 00 7F 9F 03
                              ^^ Timer 1 (reg 0x08)
                                 ^^ Slot 0x04
                                    ^^ Start hour  (08 = 08:00)
                                       ^^ Start minute
                                          ^^ Stop hour  (0C = 12:00)
                                             ^^ Stop minute
                                                ^^ Days bitmask (0x7F = every day)

02 00 50 FF FF 80 00 38 13 1B 09 04 0F 00 13 00 7F AE 03   # Timer 2: 15:00-19:00 every day
02 00 50 FF FF 80 00 38 13 1B 0A 04 00 00 00 00 00 0E 03   # Timer 3: not configured
```

**Data Fields:**

- Byte 10: Register ID (`0x08` = Timer 1, `0x09` = Timer 2, … `0x17` = Timer 16)
- Byte 11: Slot (`0x04`)
- Byte 12: Start hour (0–23, 24-hour format)
- Byte 13: Start minute (0–59)
- Byte 14: Stop hour (0–23, 24-hour format)
- Byte 15: Stop minute (0–59)
- Byte 16: Days bitmask
  - Bit 0: Monday
  - Bit 1: Tuesday
  - Bit 2: Wednesday
  - Bit 3: Thursday
  - Bit 4: Friday
  - Bit 5: Saturday
  - Bit 6: Sunday
  - `0x7F` = every day (all 7 bits set)
  - `0x00` = disabled / not configured

**Timer Register Mapping:**

| Register | Timer |
|----------|-------|
| `0x08`   | 1     |
| `0x09`   | 2     |
| `0x0A`   | 3     |
| …        | …     |
| `0x17`   | 16    |

**Notes:**

- Timers with all-zero payload (`start=00:00 stop=00:00 days=0x00`) are not configured
- Timers are not broadcast by the touchscreen, but the internet gateway requests these 16 timer registers regularly.
- Assumption of 16 timers based on how the gateway calls it — but potentially could be 8 as can only verify 8 timers on existing touchscreen.

#### Label Registers (Slot 0x02/0x03)

Assigns human-readable names to channels, lighting zones, and valves as null-terminated ASCII strings.

**Pattern:** `02 00 50 FF FF 80 00 38 1A 22` (LENGTH=0x1A=26, for longer names) or `38 16 1E` (LENGTH=0x16=22, for shorter names / valve labels)

**Example — Channel Name:**

```
02 00 50 FF FF 80 00 38 1A 22 7C 02 46 69 6C 74 65 72 20 50 75 6D 70 00 A6 03
                              ^^ Register ID (0x7C)
                                 ^^ Slot ID
                                    F  i  l  t  e  r     P  u  m  p  (null terminated)
```

**Example — Valve 1:**

```
02 00 50 FF FF 80 00 38 16 1E D0 02 56 61 6C 76 65 20 31 00 21 03
                              ^^ Register ID (0xD0 Slot 2 = Valve 1)
                                 ^^ Slot ID
                                    V  a  l  v  e     1  \0  (null-terminated ASCII string)
```

**Example — Valve 2:**

```
02 00 50 FF FF 80 00 38 16 1E D1 02 56 61 6C 76 65 20 32 00 23 03
                              ^^ Register ID (0xD1 Slot 2 = Valve 2)
                                 ^^ Slot ID
                                    V  a  l  v  e     2  \0  (null-terminated ASCII string)
```

**Data Fields:**

- Byte 10: Register ID (e.g. `0x7C`–`0x83` for channels 1–8; `0xD0`–`0xD3` for zones/valves 1–4)
- Byte 11: Slot ID
- Byte 12+: Null-terminated ASCII string

**Notes:**

- Registers `0xD0`–`0xD3` appear to be multipurpose — can represent either lighting zone colors or valve names depending on system configuration
- Maximum string length appears to be limited by message size constraints

---

### 0x39 — Register Read Request ✅

Sent by the Internet Gateway (`0x00F0`) to poll a single controller register. The Touchscreen (`0x0050`) replies with the matching [0x38 Register Data](#0x38--register-data-️) response.

**Pattern:** `02 00 F0 FF FF 80 00 39 0E B7`

**Example — Request for register 0x88:**

```
02 00 F0 FF FF 80 00 39 0E B7 88 02 8A 03
                              ^^ Register ID (0x88)
                                 ^^ Slot ID
```

**Data Fields:**

- Byte 10: Register ID to read
- Byte 11: Slot ID

**Observed Behaviour:**

- Gateway sends sequential requests (e.g. `0x88`, `0x89`, `0x8A`, `0x8B`).
- Controller responds ~120 ms after each request via [0x38](#0x38--register-data-️).
- Next request sent ~780 ms after previous response.
- Used for periodic status polling and cloud synchronisation.

**Notes:**

- Both request and response are broadcast (destination `0xFFFF`).
- The gateway appears to scan ranges of registers systematically.

---

### 0x3A — Register Write / Control ✅

Sent by the Internet Gateway (`0x00F0`) to write a single controller register. This is the gateway's write counterpart to the [0x39 Register Read Request](#0x39--register-read-request-) and is the command the gateway uses to actuate equipment that exposes its state via a register (light zones, heater, heater setpoints). The target equipment is identified by `(register, slot)` exactly as in the `0x38` data broadcasts.

**Pattern:** `02 00 F0 FF FF 80 00 3A 0F B9`

| Register   | Slot   | Purpose                | Sub-section                                                 |
|------------|--------|------------------------|-------------------------------------------------------------|
| `0xC0`–`0xC7` | `0x01` | Light Zone state    | [Light Zone Control](#light-zone-control-register-0xc00xc7-slot-0x01) |
| `0xE6`     | `0x00` | Heater 1 on/off        | [Heater Control](#heater-control-register-0xe6-slot-0x00)   |
| `0xE9`     | `0x00` | Heater 2 on/off        | [Appendix A](#appendix-a-register-dispatch-table) Heater 2 trio note |
| `0xEA`     | `0x00` | Heater 2 pool setpoint | [Appendix A](#appendix-a-register-dispatch-table) Heater 2 trio note |
| `0xEB`     | `0x00` | Heater 2 spa setpoint  | [Appendix A](#appendix-a-register-dispatch-table) Heater 2 trio note |

**Data Fields:**

- Byte 10: Register ID
- Byte 11: Slot
- Byte 12: Value to write
- Byte 13: Data checksum (sum of bytes 10–12)

**Notes:**

- Distinguished from the Touchscreen's `0x38` register-data broadcast by the source (`0x00F0` here) and the CMD byte (`0x3A` here, `0x38` for broadcasts).
- The controller applies the write and then re-broadcasts the new state via the matching `0x38` register update or a device-specific status message (e.g. [0x12 Device Status](#0x12--device-status-️) for the heater).
- This command requires the sender to impersonate the Internet Gateway (source address `0x00F0`).

---

#### Light Zone Control (Register `0xC0`–`0xC7`, Slot `0x01`) ✅

Sets a light zone's state (Off/Auto/On).

**Example — Turn ON spa light (Zone 2):**

```
02 00 F0 FF FF 80 00 3A 0F B9 C1 01 02 C4 03
                              ^^ Register ID (0xC1 = Zone 2)
                                 ^^ Slot (0x01 = State)
                                    ^^ State (0x02 = On)
                                       ^^ Checksum (0xC1 + 0x01 + 0x02 = 0xC4)
```

**Example — Turn OFF spa light (Zone 2):**

```
02 00 F0 FF FF 80 00 3A 0F B9 C1 01 00 C2 03
                              ^^ Register ID (0xC1 = Zone 2)
                                 ^^ Slot (0x01 = State)
                                    ^^ State (0x00 = Off)
                                       ^^ Checksum (0xC1 + 0x01 + 0x00 = 0xC2)
```

**Register IDs:**

- `0xC0`: Light Zone 1
- `0xC1`: Light Zone 2 (Spa)
- `0xC2`: Light Zone 3
- `0xC3`: Light Zone 4
- `0xC4`: Light Zone 5
- `0xC5`: Light Zone 6
- `0xC6`: Light Zone 7
- `0xC7`: Light Zone 8

**State Values:** `0x00` = Off, `0x01` = Auto, `0x02` = On

---

#### Heater Control (Register `0xE6`, Slot `0x00`) ✅

Turns Heater 1 on or off (register `0xE6`). Heater 2 uses the analogous register `0xE9` — see [Appendix A](#appendix-a-register-dispatch-table) for the full Heater 2 register set (`0xE9` state, `0xEA` pool setpoint, `0xEB` spa setpoint).

**Example — Turn Heater On:**

```
02 00 F0 FF FF 80 00 3A 0F B9 E6 00 01 E7 03
                              ^^ Register ID (0xE6 = Heater)
                                 ^^ Slot (0x00)
                                    ^^ State (0x01 = On)
                                       ^^ Checksum (0xE6 + 0x00 + 0x01 = 0xE7)
```

**Example — Turn Heater Off:**

```
02 00 F0 FF FF 80 00 3A 0F B9 E6 00 00 E6 03
                              ^^ Register ID (0xE6 = Heater)
                                 ^^ Slot (0x00)
                                    ^^ State (0x00 = Off)
                                       ^^ Checksum (0xE6 + 0x00 + 0x00 = 0xE6)
```

**State Values:** `0x00` = Off, `0x01` = On

**Notes:**

- Unlike light zones (slot `0x01`), the heater uses slot `0x00`.
- The controller will respond with an updated heater state via the Connect 8/10 Controller variant of [0x12 — Device Status](#0x12--device-status-️).

---

### 0x3B — Pump Speed ✅

Speed telemetry broadcast by the Viron XT Variable Speed Pump (`0x00A0`). Emitted every ~60 seconds while the pump is running.

**Pattern:** `02 00 A0 FF FF 80 00 3B 0E 69`

**Example:**

```
02 00 A0 FF FF 80 00 3B 0E 69 04 65 69 03
                              ^^^^^ Speed in RPM (big-endian uint16)
                                    ^^ Data checksum (sum of bytes 10–11)
```

**Data Fields:**

- Bytes 10–11: Pump speed in RPM, **big-endian** `uint16` (e.g. `04 65` = 0x0465 = 1125 RPM)
- Byte 12: Data checksum (sum of bytes 10–11, masked to 8 bits)

**Observed speed values:**

| Bytes 10–11 | RPM  | Notes                         |
|-------------|------|-------------------------------|
| `00 00`     | 0    | Pump stopped (transitioning)  |
| `04 65`     | 1125 | LOW Preset                    |
| `05 46`     | 1350 | MED Preset                    |
| `05 F5`     | 1525 | Manual adjustment (via DOWN)  |
| `05 DC`     | 1500 | —                             |
| `06 40`     | 1600 | HIGH Preset                   |
| `08 02`     | 2050 | Priming / Manual ON           |

**Notes:**

- Encoding is **big-endian** (most-significant byte first), unlike the little-endian convention used elsewhere in this protocol. This likely reflects the pump's own native encoding.
- Published to MQTT as `pool/{device_id}/pump/state` with JSON payload `{"speed_rpm": <value>}`.
- Decoded in code by `handle_pump_speed`; speed value stored in `pool_state.pump_speed` / `pool_state.pump_speed_valid`.
- When buttons are pressed on the pump panel, [CMD `0x1B`](#0x1b--pump-button-activity-️) bursts are emitted first; the next `0x3B` after the ~60 s interval reflects the newly committed speed.

---

### 0xFD — Controller Day/Time/Clock ✅

Current time from the Touchscreen's (`0x0050`) internal clock. Broadcast periodically for device time synchronisation.

**Pattern:** `02 00 50 FF FF 80 00 FD 0F DC`

**Example:**

```
02 00 50 FF FF 80 00 FD 0F DC 39 08 05 46 03
                              ^^ Minutes (57)
                                 ^^ Hours (8)
                                    ^^ Day of Week (5)
                                       → 08:57 on Saturday
```

**Example — Minute rollover:**

```
02 00 50 FF FF 80 00 FD 0F DC 3B 08 05 48 03  → 05:08:59
02 00 50 FF FF 80 00 FD 0F DC 00 09 05 0E 03  → 05:09:00
```

**Data Fields:**

- Byte 10: Minutes (0–59)
- Byte 11: Hours (0–23, 24-hour format)
- Byte 12: Day of Week (`0` = Monday → `6` = Sunday)

**Notes:**

- Used by connected devices (touchscreen, internet gateway) to maintain consistent time
- Appears to be sent every minute

---

## Appendix A: Register Dispatch Table

The register ID and slot together determine the message meaning. The slot distinguishes different data aspects of the same register. Used by the universal register message format ([0x38](#0x38--register-data-️)).

### Dispatch Table

| Register Range  | Slot   | Purpose                | Data Format                                      |
|-----------------|--------|------------------------|--------------------------------------------------|
| `0x08`–`0x17`  | `0x04` | Timers 1–16            | start/stop time + days bitmask (see [0x38 Timer Registers](#timer-registers-slot-0x04))   |
| `0x20` ⚠️      | `0x03` | Unknown                | Only `0xFF` observed. Repeats ~every 8 minutes    |
| `0x21`–`0x28`  | `0x03` | Favourite/Mode Enable  | 1-byte flag (`0x01`=enabled, `0x00`=disabled). Maps to CMD `0x2A` values `0x00`–`0x07` in order. Pool (`0x21`) and Spa (`0x22`) are always `0x01`. |
| `0x31`–`0x38`  | `0x03` | Favourite/Mode Labels  | Null-terminated ASCII string. Maps to CMD `0x2A` values `0x00`–`0x07` in order. `0x31`=Pool, `0x32`=Spa, `0x33`–`0x38`=user Favourites 1–6. |
| `0x64`-`0x65` ⚠️| `0x00`| Unknown                | Only `0x01` observed. Repeats ~every 8 minutes   |
| `0x6C`–`0x73`  | `0x02` | Channel Types          | 1-byte type code (see [0x0B](#0x0b--channel-status-) channel types)    |
| `0x7C`–`0x83`  | `0x02` | Channel Names          | Null-terminated ASCII string                     |
| `0x8C`–`0x93`  | `0x02` | Channel State          | 1-byte value (0=Off, 1=Auto, 2=On) — read-only; writes ignored by controller |
| `0xA0`–`0xA7`  | `0x01` | Light Zone Multicolor  | 1-byte flag (`0x00`=No, `0x01`=Yes)              |
| `0xAC`-`0xAF` ⚠️| `0x0D`| Unknown                | Only `0xFF` observed. Repeats ~every 8 minutes   |
| `0xB0`–`0xB7`  | `0x01` | Light Zone Name        | 1-byte preset name code (see [name codes table](#light-zone-name-codes)) |
| `0xB0`–`0xB3` ⚠️| `0x0D` | Unknown               | Only `0xFF` observed. Repeats ~every 8 minutes   |
| `0xB8`–`0xB9` ⚠️| `0x0B` | Unknown               | Only `0x00` observed. Repeats ~every 8 minutes   |
| `0xBC`–`0xBF` ⚠️| `0x0D` | Unknown               | Only `0x00` observed. Repeats ~every 8 minutes   |
| `0xC0`–`0xC7`  | `0x01` | Light Zone State       | 1-byte value (0=Off, 1=Auto, 2=On)               |
| `0xC0`–`0xC3` ⚠️| `0x0D` | Unknown               | Only `0xFF` observed. Repeats ~every 8 minutes   |
| `0xC8` ⚠️      | `0x00` | Unknown                | Only `0x01` observed. Repeats ~every 8 minutes   |
| `0xD0`–`0xD1`  | `0x02` | Valve Labels           | Null-terminated ASCII string                     |
| `0xD0`–`0xD7`  | `0x01` | Light Zone Color       | 1-byte color code                                |
| `0xE0`–`0xE7`  | `0x01` | Light Zone Active      | 1-byte binary (`0x00`=Inactive, `0x01`=Active)   |
| `0xF4`         | `0x01` | Channel Count          | 1-byte total number of channels in the system    |
| `0xE6`         | `0x00` | Heater State (Heater 1)   | 1-byte (`0x00`=Off, `0x01`=On)                |
| `0xE7`         | `0x00` | Pool Temperature Setpoint (Heater 1) | 1-byte °C value                    |
| `0xE8`         | `0x00` | Spa Temperature Setpoint (Heater 1)  | 1-byte °C value                    |
| `0xE9`         | `0x00` | Heater 2 State         | 1-byte (`0x00`=Off, `0x01`=On) — writable via gateway CMD `0x3A`. See note below. |
| `0xE8`–`0xE9` ⚠️| `0x03` | Unknown               | Only `0x01` observed. Repeats ~every 8 minutes   |
| `0xEA`         | `0x00` | Heater 2 Pool Setpoint | 1-byte °C value — writable via gateway CMD `0x3A`. See note below. |
| `0xEB`         | `0x00` | Heater 2 Spa Setpoint  | 1-byte °C value — writable via gateway CMD `0x3A`. See note below.   |
| `0xEC` ⚠️       | `0x00` | Unknown                | Only `0x01` observed. Repeats ~every 8 minutes |
| `0xF0` ⚠️       | `0x01` | Unknown                | Only `0xFF` observed. Repeats ~every 8 minutes |
| `0xF5`–`0xFA` ⚠️| `0x01` | Unknown                | Only `0x01` observed. Repeats ~every 8 minutes |

**Notes:**

- Register ranges can overlap (e.g., `0xD0`–`0xD7`) but are distinguished by the slot value
- The same slot value (e.g., `0x02`) can represent different data formats depending on the register
- Slot values appear to be context-dependent rather than globally defining a data type
- **`0xE9`/`0xEA`/`0xEB` (Heater 2 trio) — confirmed ✅**: Slot `0x00` holds the Heater 1 trio at `0xE6` (state), `0xE7` (Pool setpoint), `0xE8` (Spa setpoint), and `0xE9`/`0xEA`/`0xEB` are the analogous trio for the second heater: `0xE9` = state (`0x00`=Off, `0x01`=On), `0xEA` = Pool setpoint, `0xEB` = Spa setpoint (1-byte °C). All three are writable via the gateway register-write command (CMD `0x3A` / second-byte `0xB9`). `0xEA` was confirmed by UI capture — changing the setpoint in the UI sends a `0x3A` write to `0xEA`/slot `0x00` and the touchscreen rebroadcasts the new value via CMD `0x38` (observed 21°C `EA 00 15`, 22°C `EA 00 16`, 27°C `EA 00 1B`; the 27°C value matched the **H2** value in the heater's `0x0070` CMD `0x17` broadcast). `0xE9` and `0xEB` are confirmed as Heater 2 state and spa setpoint respectively. (On the test install the second heater is a heat pump; "Heater 2" is kept as the generic name since another install's second heater may be a different type.)
- **Mutually exclusive broadcast**: in captures observed so far the touchscreen broadcasts *either* the `E6/E7/E8` trio *or* the `E9/EA/EB` trio in slot `0x00`, but not both — consistent with a config-dependent enable (likely [0x26](#0x26--configuration-️) byte 10 bit 3 = heater count).
- **`0xEB` default**: when the second heater isn't plumbed to spa, `0xEB` reads `0x0A` (10°C) — an unused default at the minimum setpoint rather than a live value.

### Examples by Register Type

**Channel Type Configuration (`0x6C`–`0x73`, Slot `0x02`):**

```
02 00 50 FF FF 80 00 38 0F 17 6C 02 01 6F 03
                              ^^ Channel 1 (0x6C)
                                 ^^ Slot 0x02 (Type)
                                    ^^ Type code: 0x01 = Filter
```

**Channel Name (`0x7C`–`0x83`, Slot `0x02`):**

```
02 00 50 FF FF 80 00 38 17 1F 7C 02 46 69 6C 74 65 72 00 A6 03
                              ^^ Channel 1 (0x7C)
                                 ^^ Slot 0x02 (Name)
                                    F  i  l  t  e  r  \0
```

**Channel State (`0x8C`–`0x93`, Slot `0x02`):**

```
02 00 50 FF FF 80 00 38 0F 17 8C 02 02 90 03
                              ^^ Channel 1 (0x8C)
                                 ^^ Slot 0x02 (State)
                                    ^^ Value: 0x02 = On
```

State values: `0x00` = Off, `0x01` = Auto, `0x02` = On

> Read-only — write commands (`0x3A`) targeting these registers are silently ignored.

**Light Zone State (`0xC0`–`0xC7`, Slot `0x01`):**

```
02 00 50 FF FF 80 00 38 0F 17 C0 01 02 C3 03
                              ^^ Light Zone 1 (0xC0)
                                 ^^ Slot 0x01 (State)
                                    ^^ Value: 0x02 = On
```

**Light Zone Multicolor Capability (`0xA0`–`0xA7`, Slot `0x01`):**

```
02 00 50 FF FF 80 00 38 0F 17 A0 01 01 A2 03
                              ^^ Light Zone 1 (0xA0)
                                 ^^ Slot 0x01 (Multicolor)
                                    ^^ Value: 0x01 = Multicolor capable

02 00 50 FF FF 80 00 38 0F 17 A1 01 00 A2 03
                              ^^ Light Zone 2 (0xA1)
                                 ^^ Slot 0x01 (Multicolor)
                                    ^^ Value: 0x00 = Not multicolor capable
```

**Light Zone Name (`0xB0`–`0xB7`, Slot `0x01`):**

```
02 00 50 FF FF 80 00 38 0F 17 B0 01 00 B1 03
                              ^^ Light Zone 1 (0xB0)
                                 ^^ Slot 0x01 (Name)
                                    ^^ Name code: 0x00 = Pool
```

**Light Zone Name Codes:**

| Code   | Name       |
|--------|------------|
| `0x00` | Pool       |
| `0x01` | Spa        |
| `0x02` | Pool & Spa |
| `0x03` | Waterfall 1|
| `0x04` | Waterfall 2|
| `0x05` | Waterfall 3|

**Light Zone Color (`0xD0`–`0xD7`, Slot `0x01`):**

```
02 00 50 FF FF 80 00 38 0F 17 D0 01 05 D6 03
                              ^^ Light Zone 1 (0xD0)
                                 ^^ Slot 0x01 (Color)
                                    ^^ Color code: 0x05 = Blue
```

**Valve Label (`0xD0`–`0xD1`, Slot `0x02`):**

```
02 00 50 FF FF 80 00 38 16 1E D0 02 56 61 6C 76 65 20 31 00 21 03
                              ^^ Valve 1 (0xD0) - same register as Light Zone 1!
                                 ^^ Slot 0x02 (Label)
                                    V  a  l  v  e     1  \0
```

**Note:** Register `0xD0` serves dual purpose:
- With slot `0x01`: Light zone 1 color (numeric)
- With slot `0x02`: Valve 1 label (text)

**Light Zone Active (`0xE0`–`0xE7`, Slot `0x01`):**

```
02 00 50 FF FF 80 00 38 0F 17 E0 01 01 E2 03
                              ^^ Light Zone 1 (0xE0)
                                 ^^ Slot 0x01 (Active flag)
                                    ^^ Value: 0x01 = Active
```

### Register ID Mappings

**Channels:**

| Register | Channel | Type (`0x02`) | Name (`0x02`) | State (`0x02`) |
|----------|---------|---------------|---------------|----------------|
| `0x6C`   | 1       | ✅            | —             | —              |
| `0x6D`   | 2       | ✅            | —             | —              |
| …        | …       | ✅            | —             | —              |
| `0x73`   | 8       | ✅            | —             | —              |
| `0x7C`   | 1       | —             | ✅            | —              |
| …        | …       | —             | ✅            | —              |
| `0x83`   | 8       | —             | ✅            | —              |
| `0x8C`   | 1       | —             | —             | ✅ read-only   |
| `0x8D`   | 2       | —             | —             | ✅ read-only   |
| …        | …       | —             | —             | ✅ read-only   |
| `0x93`   | 8       | —             | —             | ✅ read-only   |

> Channel state is **read-only** via the register system. To change channel state, use the [Channel Toggle Command (0x10)](#0x10--channel-toggle-command-️).

**Lighting Zones:**

- Multicolor (`0xA0`–`0xA7`): `0xA0` = Zone 1, `0xA1` = Zone 2, etc.
- Name (`0xB0`–`0xB7`): `0xB0` = Zone 1, `0xB1` = Zone 2, etc.
- State (`0xC0`–`0xC7`): `0xC0` = Zone 1, `0xC1` = Zone 2, etc.
- Color (`0xD0`–`0xD7`): `0xD0` = Zone 1, `0xD1` = Zone 2, etc.
- Active (`0xE0`–`0xE7`): `0xE0` = Zone 1, `0xE1` = Zone 2, etc.

### Implementation

The firmware uses a dispatch table to route register messages to appropriate handlers. See `message_decoder.c` for the complete implementation:

```c
static const register_handler_t REGISTER_HANDLERS[] = {
    {0x6C, 0x73, 0x02, handle_channel_type,          "Channel Type"},
    {0x7C, 0x83, 0x02, handle_channel_name,          "Channel Name"},
    {0xA0, 0xA7, 0x01, handle_light_zone_multicolor, "Light Zone Multicolor"},
    {0xB0, 0xB7, 0x01, handle_light_zone_name,       "Light Zone Name"},
    {0xC0, 0xC7, 0x01, handle_light_zone_state,      "Light Zone State"},
    {0x08, 0x17, 0x04, handle_timer,                 "Timer"},
    {0xD0, 0xD7, 0x01, handle_light_zone_color,      "Light Zone Color"},
    {0xE0, 0xE7, 0x01, handle_light_zone_active,     "Light Zone Active"},
    {0xD0, 0xD1, 0x02, handle_valve_label,           "Valve Label"},
    {0x31, 0x38, 0x03, handle_register_label_generic,"Favourite Label"},
};
```

The dispatcher:

1. Validates header checksum (byte 9 = sum(bytes 0–8) & 0xFF)
2. Extracts register ID and slot
3. Looks up matching handler in table
4. Routes to appropriate handler function

---

## Implementation Notes

### Message Validation

All messages should be validated before processing:

1. **Start byte:** Must be `0x02`
2. **End byte:** Must be `0x03`
3. **Minimum length:** At least 13 bytes for checksum verification
4. **Checksum:** Calculate and compare with received checksum byte

### Thread Safety

When implementing a decoder:

- Protect shared state with mutexes/semaphores
- Use snapshots for publishing to avoid holding locks during I/O
- Validate all array indices before access

### UART Configuration

The Connect 10 bus uses:

- **Baud rate:** 9600
- **Data bits:** 8
- **Parity:** None
- **Stop bits:** 1
- **TX inversion:** May be required depending on interface hardware

## Example: Complete Message Decode

```
02 00 50 FF FF 80 00 14 0D F1 01 01 03
^^ Start byte
   ^^^^^  Source: 0x0050 (Touchscreen)
         ^^^^^  Destination: 0xFFFF (Broadcast)
               ^^^^^  Control: 0x8000
                     ^^^^^^^^  Command: Mode message pattern
                              ^^ Data: 0x01 = Pool mode
                                 ^^ Checksum: 0x01 (sum of byte 10)
                                    ^^ End byte
```

**Decoded:** Touchscreen broadcasts Pool mode to all devices.

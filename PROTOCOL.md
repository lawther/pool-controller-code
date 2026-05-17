# Connect 10 Pool Controller Protocol Documentation

This document describes the proprietary serial protocol used by the Connect 10 pool controller and has been clean-room developed by sniffing the messages on the RS-232 like bus that is used for communications.

## Table of Contents

- [Message Structure](#message-structure)
  - [Message Format](#message-format)
  - [Checksum Calculation](#checksum-calculation)
  - [Device Addresses](#device-addresses)
  - [Known Command Bytes](#known-command-bytes)
- [Quick Reference](#quick-reference)
- [Message Types](#message-types)
  - [1. Mode Message (Spa/Pool) ✅](#1-mode-message-spapool-)
  - [2. Temperature Settings ✅](#2-temperature-settings-)
  - [3. Temperature Reading ⚠️](#3-temperature-reading-️)
  - [4. Heater Status ⚠️](#4-heater-status-️)
  - [5. Configuration ⚠️](#5-configuration-️)
  - [6. Active Channels Bitmask ⚠️](#6-active-channels-bitmask-️)
  - [7. Channel Status ✅](#7-channel-status-)
  - [8. Register Messages (Universal Register System) ⚠️](#8-register-messages-universal-register-system-️)
  - [9. Lighting Zone Configuration ✅](#9-lighting-zone-configuration-)
  - [10. Chlorinator pH Setpoint ✅](#10-chlorinator-ph-setpoint-)
  - [11. Chlorinator pH Reading ✅](#11-chlorinator-ph-reading-)
  - [12. Chlorinator ORP Setpoint ✅](#12-chlorinator-orp-setpoint-)
  - [13. Chlorinator ORP Reading ✅](#13-chlorinator-orp-reading-)
  - [14. Internet Gateway Serial Number ⚠️](#14-internet-gateway-serial-number-️)
  - [15. Internet Gateway Network Config ⚠️](#15-internet-gateway-network-config-️)
  - [16. Internet Gateway Communications Status ⚠️](#16-internet-gateway-communications-status-️)
  - [17. Firmware Version ✅](#17-firmware-version-)
  - [18. Internet Gateway Status Broadcast ⚠️](#18-internet-gateway-status-broadcast-️)
  - [19. Register Read Request/Response](#19-register-read-requestresponse)
  - [20. Controller Day/Time/Clock ✅](#20-controller-daytimeclock-)
  - [22. Touchscreen Unknown 1 ⚠️](#22-touchscreen-unknown-1-️)
  - [23. Valve State Broadcast ⚠️](#23-valve-state-broadcast-️)
  - [24. Valve Sync to Controller ⚠️](#24-valve-sync-to-controller-️)
- [Control Commands (Gateway to Controller)](#control-commands-gateway-to-controller)
  - [25. Light Zone Control Command ✅](#25-light-zone-control-command-)
  - [26. Channel Toggle Command ✅](#26-channel-toggle-command-)
  - [27. Temperature Setpoint Command ✅](#27-temperature-setpoint-command-)
  - [28. Heater Control Command ✅](#28-heater-control-command-)
  - [29. Mode/Favourite Control Command ✅](#29-modefavourite-control-command-)
  - [30. Valve Control Command ✅](#30-valve-control-command-)
  - [31. Chlorinator Cell Mode ⚠️](#31-chlorinator-cell-mode-️)
  - [32. Chlorinator Status Broadcast ⚠️](#32-chlorinator-status-broadcast-️)
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

| Address  | Device       | Description                       |
| -------- | ------------ | --------------------------------- |
| `0x0050` | Touch Screen | Touch screen interface            |
| `0x0062` | Temp Sensor  | Temperature sensor module         |
| `0x006F` | Controller   | Main pool controller (Connect 10) |
| `0x0070` | Heater       | Active i25 Evo electric heater    |
| `0x0084` | Chlorinator  | Chemistry/chlorinator module (alternate variant; mutually exclusive with `0x0090`) |
| `0x0090` | Chlorinator  | Chemistry/chlorinator module      |
| `0x00A0` | Salt Cell    | Chlorine generator / salt cell (suspected; subordinate to `0x0084`) |
| `0x00F0` | Internet GW  | Internet gateway module           |
| `0xFFFF` | Broadcast    | Broadcast to all devices          |

### Known Command Bytes

The command byte (byte 7) identifies the message type. Some commands are universal across sources (same payload layout regardless of who sends — e.g. `0x0A`); others are source-dependent (same CMD byte, different payload per source — e.g. `0x12`, `0x16`, `0x17`).

The **In code?** column distinguishes commands that have an actual handler in `message_decoder.c` from those that are only documented in this file. Source-dependent commands list each known source/destination on its own line in the **Direction** column.

| CMD    | Name                                | Direction                                                              | Variants / Notes                                                                            | Section(s)               | In code?                |
|--------|-------------------------------------|------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|--------------------------|-------------------------|
| `0x05` | Touchscreen Activation Ack          | `0x0050` → Broadcast                                                   | 1-byte payload `0x01`; sent after mode/favourite changes                                    | (mentioned in [§29](#29-modefavourite-control-command-)) | Yes (log-only)          |
| `0x06` | Lighting Zone Config                | `0x0050` → Broadcast                                                   |                                                                                             | [§9](#9-lighting-zone-configuration-)                    | Yes                     |
| `0x0A` | Firmware Version                    | `0x0050`, `0x0062`, `0x0070`, `0x0084`, `0x00F0` → Broadcast           | Same `{major, minor}` payload across all 5 sources; dispatched on CMD byte alone            | [§17](#17-firmware-version-) | Yes (unified handler)   |
| `0x0B` | Channel Status                      | `0x0050` → Broadcast                                                   |                                                                                             | [§7](#7-channel-status-)                                 | Yes                     |
| `0x0D` | Active Channels Bitmask             | `0x0050` → `0x006F` Controller                                         | Unicast                                                                                     | [§6](#6-active-channels-bitmask-️)                       | Yes                     |
| `0x0F` | Chlorinator mode → Touchscreen      | `0x0084` → `0x0050`                                                    | 2-byte `[01, mode]`; mirrors [§31](#31-chlorinator-cell-mode-) cell mode                    | (mentioned in [§31](#31-chlorinator-cell-mode-))         | **No (doc only)**       |
| `0x10` | Channel Toggle Command              | `0x00F0` Gateway → Broadcast                                           |                                                                                             | [§26](#26-channel-toggle-command-)                       | Yes                     |
| `0x12` | Device Status                       | `0x0050`, `0x0062`, `0x0084`, `0x0090`, `0x00F0` → Broadcast           | Payload layout differs per source                                                           | [§4](#4-heater-status-️), [§18](#18-internet-gateway-status-broadcast-), [§22](#22-touchscreen-unknown-1-️), [§32](#32-chlorinator-status-broadcast-️) | Yes (per-source)        |
| `0x14` | Mode (Spa/Pool)                     | `0x0050` → Broadcast                                                   |                                                                                             | [§1](#1-mode-message-spapool-)                           | Yes                     |
| `0x16` | Water Temperature Reading           | `0x0062` (LEN `0x0E`), `0x0070` (LEN `0x0D`) → Broadcast               | Source-dependent payload (`0x0062` = inbuilt heater, `0x0070` = add-on Active i25 Evo)      | [§3](#3-temperature-reading-️)                           | Yes (per-source)        |
| `0x17` | Temperature Settings                | `0x0050` (LEN `0x10`), `0x0070` (LEN `0x0E`) → Broadcast               | Source-dependent payload layout                                                             | [§2](#2-temperature-settings-)                           | Yes (per-source)        |
| `0x18` | Chlorinator Cell Mode               | `0x0084`, `0x0050` → `0x00A0` Salt Cell                                | Inter-device unicast                                                                        | [§31](#31-chlorinator-cell-mode-)                        | **No (doc only)**       |
| `0x19` | Temperature Setpoint Command        | `0x00F0` Gateway → Broadcast                                           |                                                                                             | [§27](#27-temperature-setpoint-command-)                 | Yes                     |
| `0x1D` | Chlorinator Setpoint                | `0x0090` → Broadcast                                                   | Byte 10: `0x01`=pH, `0x02`=ORP                                                              | [§10](#10-chlorinator-ph-setpoint-), [§12](#12-chlorinator-orp-setpoint-) | Yes                     |
| `0x1F` | Chlorinator Reading                 | `0x0090` → Broadcast                                                   | Byte 10: `0x01`=pH, `0x02`=ORP                                                              | [§11](#11-chlorinator-ph-reading-), [§13](#13-chlorinator-orp-reading-) | Yes                     |
| `0x25` | Valve Sync                          | `0x0050` → `0x006F` Controller                                         | Unicast                                                                                     | [§24](#24-valve-sync-to-controller-️)                    | **No (doc only)**       |
| `0x26` | Configuration                       | `0x0050` → Broadcast                                                   |                                                                                             | [§5](#5-configuration-️)                                 | Yes                     |
| `0x27` | Valve State Broadcast               | `0x0050` → Broadcast                                                   | Two LEN variants: `0x0D` (short) and `0x13` (full)                                          | [§23](#23-valve-state-broadcast-️)                       | Yes (both variants)     |
| `0x28` | Valve Control Command               | `0x00F0` Gateway → Broadcast                                           |                                                                                             | [§30](#30-valve-control-command-)                        | **No (doc only)**       |
| `0x2A` | Mode/Favourite Control Command      | `0x00F0` Gateway → `0x0050` Touchscreen                                | Unicast                                                                                     | [§29](#29-modefavourite-control-command-)                | Yes                     |
| `0x31` | Water Temperature Reading (alt)     | `0x0062` → Broadcast                                                   | Second variant alongside `0x16`                                                             | [§3](#3-temperature-reading-️)                           | Yes                     |
| `0x37` | Gateway Info Messages               | `0x00F0` → Broadcast                                                   | LEN distinguishes serial/IP/comms variants                                                  | [§14](#14-internet-gateway-serial-number-️), [§15](#15-internet-gateway-network-config-️), [§16](#16-internet-gateway-communications-status-️) | Yes (3 handlers)        |
| `0x38` | Register Data (Response)            | `0x0050` → Broadcast                                                   | Universal register system — sub-dispatched by register + slot                               | [§8](#8-register-messages-universal-register-system-️), [Appendix A](#appendix-a-register-dispatch-table) | Yes                     |
| `0x39` | Register Read Request               | `0x00F0` Gateway → Broadcast                                           |                                                                                             | [§19](#19-register-read-requestresponse) | Yes                     |
| `0x3A` | Register Write / Control            | `0x00F0` Gateway → Broadcast                                           | Used for both Light Zone Control and Heater Control                                         | [§25](#25-light-zone-control-command-), [§28](#28-heater-control-command-) | Yes (both)              |
| `0xFD` | Controller Day/Time/Clock           | `0x0050` → Broadcast                                                   |                                                                                             | [§20](#20-controller-daytimeclock-)      | Yes                     |

---

## Quick Reference

✅ = fully decoded, ⚠️ = partially decoded. Messages with the same pattern are distinguished by byte 10 (register ID).

| #  | Name                              | Source   | Pattern (bytes 0–9)                       | Status | Notes                               |
|----|-----------------------------------|----------|-------------------------------------------|--------|-------------------------------------|
| 1  | Mode (Spa/Pool)                   | `0x0050` | `02 00 50 FF FF 80 00 14 0D F1`           | ✅     |                                     |
| 2  | Temperature Settings              | `0x0050` | `02 00 50 FF FF 80 00 17 10 F7`           | ✅     | Source-dependent — also emitted by `0x0070` with different layout. Register variant: E7/E8 Slot 0x00 |
| 2  | Temperature Settings (Heater)     | `0x0070` | `02 00 70 FF FF 80 00 17 0E 15`           | ✅     | Heater 1 / Heater 2 setpoints (°C)  |
| 3  | Temperature Reading (A)           | `0x0062` | `02 00 62 FF FF 80 00 16 0E 06`           | ⚠️     | Source-dependent CMD `0x16` — see [§3](#3-temperature-reading-️) |
| 3  | Temperature Reading (B)           | `0x0062` | `02 00 62 FF FF 80 00 31 0E 21`           | ⚠️     | Two pattern variants                |
| 3  | Temperature Reading (Heater)      | `0x0070` | `02 00 70 FF FF 80 00 16 0D 13`           | ✅     | Heater water temperature (1 data byte) |
| 4  | Heater Status                     | `0x0062` | `02 00 62 FF FF 80 00 12 0F 03`           | ⚠️     |                                     |
| 5  | Configuration                     | `0x0050` | `02 00 50 FF FF 80 00 26 0E 04`           | ⚠️     |                                     |
| 6  | Active Channels Bitmask           | `0x0050` | `02 00 50 00 6F 80 00 0D 0D 5B`           | ⚠️     | Dst=`0x006F` (Controller)           |
| 7  | Channel Status                    | `0x0050` | `02 00 50 FF FF 80 00 0B 25 00`           | ✅     |                                     |
| 8  | Register Messages                 | `0x0050` | `02 00 50 FF FF 80 00 38 ** **`           | ⚠️     | Incl. timers (Slot `0x04`) & labels (Slot `0x02`); see [Appendix A](#appendix-a-register-dispatch-table) |
| 9  | Lighting Zone Configuration       | `0x0050` | `02 00 50 FF FF 80 00 06 0E E4`           | ✅     |                                     |
| 10 | Chlorinator pH Setpoint           | `0x0090` | `02 00 90 FF FF 80 00 1D 0F 3C`           | ✅     | Byte 10: `0x01`                     |
| 11 | Chlorinator pH Reading            | `0x0090` | `02 00 90 FF FF 80 00 1F 0F 3E`           | ✅     | Byte 10: `0x01`                     |
| 12 | Chlorinator ORP Setpoint          | `0x0090` | `02 00 90 FF FF 80 00 1D 0F 3C`           | ✅     | Byte 10: `0x02`; same pattern as [§10](#10-chlorinator-ph-setpoint-)|
| 13 | Chlorinator ORP Reading           | `0x0090` | `02 00 90 FF FF 80 00 1F 0F 3E`           | ✅     | Byte 10: `0x02`; same pattern as [§11](#11-chlorinator-ph-reading-)|
| 14 | Internet Gateway Serial Number    | `0x00F0` | `02 00 F0 FF FF 80 00 37 11 B8`           | ⚠️     |                                     |
| 15 | Internet Gateway Network Config   | `0x00F0` | `02 00 F0 FF FF 80 00 37 15 BC`           | ⚠️     |                                     |
| 16 | Internet Gateway Comms Status     | `0x00F0` | `02 00 F0 FF FF 80 00 37 0F B6`           | ⚠️     |                                     |
| 17 | Firmware Version                  | any      | `02 00 ?? FF FF 80 00 0A 0E ??`           | ✅     | CMD `0x0A` broadcast by Touchscreen (`0x0050`), Inbuilt heater (`0x0062`), Heatpump (`0x0070`), Chlorinator (`0x0084`), and Internet Gateway (`0x00F0`) with identical `{major, minor}` payload — single unified handler |
| 18 | Internet Gateway Status Broadcast | `0x00F0` | `02 00 F0 FF FF 80 00 12 0F 91`           | ✅     | Same cmd byte as §22                |
| 19 | Register Read Request             | `0x00F0` | `02 00 F0 FF FF 80 00 39 0E B7`           |        | For responses see [§10](#8-register-messages-universal-register-system-️)|
| 20 | Controller Day/Time/Clock         | `0x0050` | `02 00 50 FF FF 80 00 FD 0F DC`           | ✅     |                                     |
| 22 | Touchscreen Unknown 1             | `0x0050` | `02 00 50 FF FF 80 00 12 0E F0`           | ⚠️     | Same cmd byte as §18                |
| 23 | Valve State Broadcast             | `0x0050` | `02 00 50 FF FF 80 00 27 0D 04`           | ⚠️     | Two LENGTH variants: 0x0D (short) and 0x13 (full state) |
| 24 | Valve Sync to Controller          | `0x0050` | `02 00 50 00 6F 80 00 25 0D 73`           | ⚠️     | Dst=`0x006F` (Controller)           |
| 25 | Light Zone Control Command        | `0x00F0` | `02 00 F0 FF FF 80 00 3A 0F B9`           | ✅     | Same pattern as [§28](#28-heater-control-command-) |
| 26 | Channel Toggle Command            | `0x00F0` | `02 00 F0 FF FF 80 00 10 0D 8D`           | ✅     |                                     |
| 27 | Temperature Setpoint Command      | `0x00F0` | `02 00 F0 FF FF 80 00 19 0F 98`           | ✅     |                                     |
| 28 | Heater Control Command            | `0x00F0` | `02 00 F0 FF FF 80 00 3A 0F B9`           | ✅     | Same pattern as [§25](#25-light-zone-control-command-); different reg |
| 29 | Mode/Favourite Control Command    | `0x00F0` | `02 00 F0 00 50 80 00 2A 0D F9`           | ✅     | Dst=`0x0050`; 0x00=Pool, 0x01=Spa, 0x02–0x07=Fav 1–6, 0x80=All Off, 0x81=All Auto |
| 30 | Valve Control Command             | `0x00F0` | `02 00 F0 FF FF 80 00 28 0E A6`           | ✅     |                                     |
| 31 | Chlorinator Cell Mode             | `0x0084` | `02 00 84 00 A0 80 00 18 0D CB`           | ⚠️     | Dst=`0x00A0` (Salt Cell); 1-byte mode (`0x00`=Off, `0x01`=Manual, `0x02`=Auto — tentative). Also seen from `0x0050` |
| 32 | Chlorinator Status (variant A)    | `0x0090` | `02 00 90 FF FF 80 00 12 0D 2F`           | ⚠️     | 1-byte mode payload — observed `0x01` (Auto, tentative) |
| 32 | Chlorinator Status (variant B)    | `0x0084` | `02 00 84 FF FF 80 00 12 0D 23`           | ⚠️     | 1-byte mode payload — observed `0x02` (On, tentative) |

---

## Message Types

The messages that are fully decoded have a ✅ and the partially decoded ones have a ⚠️

### 1. Mode Message (Spa/Pool) ✅

Reports the current operating mode - pool or spa.

**Pattern:** `02 00 50 FF FF 80 00 14 0D F1`

**Example - Spa Mode:**

```
02 00 50 FF FF 80 00 14 0D F1 00 00 03
                              ^^
                              Mode: 0x00 = Spa, 0x01 = Pool
```

**Example - Pool Mode:**

```
02 00 50 FF FF 80 00 14 0D F1 01 01 03
```

**Data Fields:**

- Byte 10: Mode (`0x00` = Spa, `0x01` = Pool)

---

### 2. Temperature Settings ✅

Reports the temperature setpoints. CMD `0x17` is **shared across two sources** with different payload layouts:

- **Touchscreen (`0x0050`)** — broadcasts spa/pool setpoints in both °C and °F (4 data bytes, LENGTH `0x10`).
- **Heater (`0x0070`)** — broadcasts Heater 1 and Heater 2 setpoints in °C only (2 data bytes, LENGTH `0x0E`). See the [Heater variant](#temperature-settings--heater-variant) subsection below.

**Pattern (Touchscreen):** `02 00 50 FF FF 80 00 17 10 F7`

**Example:**

```
02 00 50 FF FF 80 00 17 10 F7 25 1D 63 54 F9 03
                              ^^ Spa setpoint Celcius (37°C in this example)
                                 ^^ Pool setpoint Celcius (29°C in this example)
                                    ^^ Spa setpoint Fahrenheit (99°F in this example)
                                       ^^ Pool setpoint Fahrenheit (84°F in this example)
```

**Data Fields:**

- Byte 10: Spa setpoint temperature Celcius
- Byte 11: Pool setpoint temperature Celcius
- Byte 12: Spa setpoint temperature Fahrenheit
- Byte 13: Pool setpoint temperature Fahrenheit

**Notes:**

- Temperature scale (Celsius/Fahrenheit) is set by configuration message ([Section 5](#5-configuration-️))
- The same setpoints are also broadcast individually via the register system — see pattern variant below

**Pattern Variant: Register-based Temperature Setpoints** `02 00 50 FF FF 80 00 38 0F 17`

The controller also broadcasts pool and spa setpoints as individual register messages (one per message). These carry the Celsius value only.

**Example - Pool setpoint (29°C):**

```
02 00 50 FF FF 80 00 38 0F 17 E7 00 1D 04 03
                              ^^ Register 0xE7 (Pool setpoint)
                                 ^^ Slot 0x00
                                    ^^ Value: 0x1D = 29°C
```

**Example - Spa setpoint (37°C):**

```
02 00 50 FF FF 80 00 38 0F 17 E8 00 25 0D 03
                              ^^ Register 0xE8 (Spa setpoint)
                                 ^^ Slot 0x00
                                    ^^ Value: 0x25 = 37°C
```

**Data Fields:**

- Byte 10: Register ID (`0xE7` = Pool setpoint, `0xE8` = Spa setpoint)
- Byte 11: Slot (`0x00`)
- Byte 12: Temperature in °C

#### Temperature Settings — Heater variant

When an Active i25 Evo heater (source `0x0070`) is fitted, it broadcasts its own setpoint frame using the same CMD `0x17` but a shorter LENGTH and a different payload — both heater setpoints in a single frame, °C only, no Fahrenheit values.

**Pattern (Heater):** `02 00 70 FF FF 80 00 17 0E 15`

**Example:**

```
02 00 70 FF FF 80 00 17 0E 15 18 1B 33 03
                              ^^ Heater 1 setpoint °C (0x18 = 24°C)
                                 ^^ Heater 2 setpoint °C (0x1B = 27°C)
                                    ^^ Data checksum (0x18 + 0x1B = 0x33)
```

**Data Fields:**

- Byte 10: Heater 1 setpoint in °C
- Byte 11: Heater 2 setpoint in °C
- Byte 12: Data checksum (sum of bytes 10–11)

**Notes:**

- LENGTH is `0x0E` (14 bytes) — two bytes shorter than the touchscreen variant
- Both heater setpoints are carried in a single broadcast; the heater never sends them separately
- The actual current water temperature is reported separately via CMD `0x16` (see [Heater variant of §3](#temperature-reading--heater-variant))

---

### 3. Temperature Reading ⚠️

Current water temperature. CMD `0x16` is **shared across two sources** with different payload layouts:

- **Temp Sensor (`0x0062`)** — 2-byte payload (LENGTH `0x0E`): temperature + trailing unknown byte (Pattern A below). The sensor also emits a second variant under CMD `0x31` (Pattern B).
- **Heater (`0x0070`)** — 1-byte payload (LENGTH `0x0D`): water temperature only, no trailing unknown. See the [Heater variant](#temperature-reading--heater-variant) subsection below.

**Pattern A (Temp Sensor):** `02 00 62 FF FF 80 00 16 0E 06`

```
02 00 62 FF FF 80 00 16 0E 06 19 00 19 03
                              ^^ Current temperature (25°C)
                                 ^^ Unknown
```

**Pattern B:** `02 00 62 FF FF 80 00 31 0E 21`

```
02 00 62 FF FF 80 00 31 0E 21 1E A6 C4 03
                              ^^ Current temperature (30°C)
                                 ^^ Unknown (always 0xA6 in observed samples)
```

**Data Fields:**

- Byte 10: Current water temperature in °C
- Byte 11: Unknown — always `0x00` in pattern A, always `0xA6` in pattern B

**Notes:**

- Patterns A and B both originate from device `0x0062` (temperature sensor)
- The purpose of byte 11 is not yet understood; it may be a secondary sensor, a raw ADC value, or a fixed status byte
- Pattern B has been observed decreasing as pool water cools (30→25°C), confirming byte 10 is the current temperature

#### Temperature Reading — Heater variant

When an Active i25 Evo heater (source `0x0070`) is fitted, it broadcasts its own current water-temperature reading using the same CMD `0x16` but with a shorter LENGTH (`0x0D`) and only one data byte — there is no trailing "unknown" byte.

**Pattern (Heater):** `02 00 70 FF FF 80 00 16 0D 13`

**Example:**

```
02 00 70 FF FF 80 00 16 0D 13 12 12 03
                              ^^ Current water temperature (0x12 = 18°C)
                                 ^^ Data checksum (equals byte 10 — only one data byte)
```

**Data Fields:**

- Byte 10: Current water temperature in °C
- Byte 11: Data checksum (equals byte 10)

**Notes:**

- LENGTH is `0x0D` (13 bytes) — one byte shorter than the Temp Sensor variant
- This is the heater's own water-temperature reading; it is independent of the `0x0062` Temp Sensor reading and may differ if the two are sited differently in the plumbing

---

### 4. Heater Status ⚠️

Reports whether the heater is on or off.

**Pattern:** `02 00 62 FF FF 80 00 12 0F 03`

**Example - Heater On:**

```
02 00 62 FF FF 80 00 12 0F 03 00 01 08 09 03
                                 ^^ 0x01 = On, 0x00 = Off
                                    ^^ Unknown
```

**Example - Heater Off:**

```
02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03
                                 ^^ 0x01 = On, 0x00 = Off
                                    ^^ Unknown
```

**Data Fields:**

- Byte 10: Padding/unused
- Byte 11: Heater state (`0x00` = Off, `0x01` = On)
- Byte 12: Unknown (maybe bitmask or interlock?)

---

### 5. Configuration ⚠️

System configuration including temperature scale.

**Pattern:** `02 00 50 FF FF 80 00 26 0E 04`

**Example - Celsius:**

```
02 00 50 FF FF 80 00 26 0E 04 01 06 07 03
                              ^^ 0x01 - Celcius
                                 ^^ Unknown
```

**Example - Fahrenheit:**

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
  - Bit 3: `0` = heater Off, `1` = heater currently On (live state, not a config flag — see note below)
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

### 6. Active Channels Bitmask ⚠️

Reports which channels are currently active.

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

### 7. Channel Status ✅

Detailed status for all configured channels.

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
  - Byte 0: Channel type - see lookup table below
  - Byte 1: Channel state (00 off, 01, Auto, 02 One)
  - Byte 2: Currently active (eg if turned on by timer)

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

---

### 8. Register Messages (Universal Register System) ⚠️

The controller uses a unified register-based system for configuration and state. All register messages share the same base pattern `02 00 50 FF FF 80 00 38` — only the register ID, slot, and data payload vary.

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

### 9. Lighting Zone Configuration ✅

Indicates which lighting zones are installed and their current on/off state.

**Pattern:** `02 00 50 FF FF 80 00 06 0E E4`

**Example:**

```
02 00 50 FF FF 80 00 06 0E E4 00 00 00 03
                              ^^ Zone index (0-3 for zones 1-4)
                                 ^^ Light status (00 off, 01 on)
```

**Data Fields:**

- Byte 10: Zone index (`0x00` to `0x03` for zones 1-4)
- Byte 11: Light status (00 off, 01 on)

---

### 10. Chlorinator pH Setpoint ✅

Target pH level for the chlorinator.

**Pattern:** `02 00 90 FF FF 80 00 1D 0F 3C` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1D 0F 3C 01 4E 00 4F 03
                              ^^ PH Setpoint
                                 ^^ ^^ pH value (little endian)
                                          78 = 7.8 pH (value / 10)
```

**Data Fields:**

- Byte 10: pH setpoint register (`0x01`)
- Bytes 11-12: pH value in tenths (little endian, divide by 10 for actual pH)

---

### 11. Chlorinator pH Reading ✅

Current pH reading from the sensor.

**Pattern:** `02 00 90 FF FF 80 00 1F 0F 3E` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1F 0F 3E 01 55 00 56 03
                              ^^ pH reading
                                 ^^ ^^ pH value (little endian)
                                          85 = 8.5 pH
```

**Data Fields:**

- Byte 10: pH setpoint register (`0x01`)
- Bytes 11-12: pH value in tenths (little endian, divide by 10 for actual pH)

---

### 12. Chlorinator ORP Setpoint ✅

Target ORP (oxidation-reduction potential) level.

**Pattern:** `02 00 90 FF FF 80 00 1D 0F 3C` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1D 0F 3C 02 8A 02 8E 03
                              ^^ ORP setpoint register
                                 ^^ ^^ ORP value in mV (little endian)
                                       650 mV (0x028A)
```

**Data Fields:**

- Byte 10: ORP setpoint register (`0x02`)
- Bytes 11-12: ORP value in millivolts (little endian)

---

### 13. Chlorinator ORP Reading ✅

Current ORP reading from the sensor.

**Pattern:** `02 00 90 FF FF 80 00 1F 0F 3E`

**Example:**

```
02 00 90 FF FF 80 00 1F 0F 3E 02 0A 02 0E 03
                              ^^ ORP reading register
                                 ^^ ^^ ORP value in mV (little endian)
                                          522 mV (0x020A)
```

**Data Fields:**

- Byte 10: ORP reading register (`0x02`)
- Bytes 11-12: ORP value in millivolts (little endian)

---

### 14. Internet Gateway Serial Number ⚠️

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

- Byte 10: Unknown (Maybe a type `0x04`)
- Bytes 11-14: Serial number (32-bit little endian)

---

### 15. Internet Gateway Network Config ⚠️

IP address and signal strength of the gateway.

**Pattern:** `02 00 F0 FF FF 80 00 37 15 BC`

**Example - On startup (no connection):**

```
02 00 F0 FF FF 80 00 37 15 BC 01 01 01 03 00 00 00 00 00 06 03
```

**Example - With IP address (wifi connected):**

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

### 16. Internet Gateway Communications Status ⚠️

Status of the gateway's internet connection.

**Pattern:** `02 00 F0 FF FF 80 00 37 0F B6`

**Example - Communicating with server:**

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

### 17. Firmware Version ✅

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
| `0x0062` | Inbuilt heater (also labelled Temp Sensor in the address table) | `02 00 62 FF FF 80 00 0A 0E FA` | `02 06 08`               | 2.6           |
| `0x0070` | Active i25 Evo heater (heatpump)         | `02 00 70 FF FF 80 00 0A 0E 08`         | _(observed; log-only, no dedicated state field)_ | —    |
| `0x0084` | Chlorinator (two-module variant)         | `02 00 84 FF FF 80 00 0A 0E 1C`         | `05 07 0C`                     | 5.7           |
| `0x00F0` | Internet Gateway                         | `02 00 F0 FF FF 80 00 0A 0E 88`         | `05 01 06` / `05 00 05`        | 5.1 / 5.0     |

**Example (Internet Gateway, v5.1):**

```
02 00 F0 FF FF 80 00 0A 0E 88 05 01 06 03
                              ^^ Major version (5)
                                 ^^ Minor version (1)
                                    → Version 5.1
```

**Notes:**

- Decoded by the source-agnostic `handle_firmware_version` handler (matches on `data[7] == 0x0A` regardless of source); state is stored in per-device fields on `pool_state` (`touchscreen_version_*`, `temp_sensor_version_*`, `chlor_version_*`, `gateway_version_*`). Heatpump (`0x0070`) firmware is logged only — no dedicated state field.
- The same `{major, minor}` pair is also redundantly embedded in the Gateway Status Broadcast ([§18](#18-internet-gateway-status-broadcast-)); firmware-version state population is performed once here.
- Broadcast at device startup; appears alongside other announcement broadcasts (mode, channel status, time).

---

### 18. Internet Gateway Status Broadcast ✅

Firmware version broadcast by the Internet Gateway on startup. Uses the same command byte (`0x12`) as the Touchscreen Unknown 1 message, but carries an extra payload byte and originates from source `0x00F0`. The payload repeats the `{major, minor}` pair from the generic Firmware Version message ([§17](#17-firmware-version-)) and follows it with an embedded data-level checksum.

**Pattern:** `02 00 F0 FF FF 80 00 12 0F 91`

**Example:**

```
02 00 F0 FF FF 80 00 12 0F 91 05 01 06 0C 03
                              ^^ Major version (5)
                                 ^^ Minor version (1)
                                    ^^ Embedded checksum (major + minor)
                                       → Version 5.1
```

**Data Fields:**

- Byte 10: Major version number
- Byte 11: Minor version number
- Byte 12: Embedded checksum — sum of bytes 10 and 11 (`major + minor`)

**Observed samples:**

| Sample (bytes 10–12) | Major | Minor | Embedded checksum |
|----------------------|-------|-------|-------------------|
| `05 01 06`           | 5     | 1     | `0x06` (=5+1)     |
| `05 00 05`           | 5     | 0     | `0x05` (=5+0)     |

**Notes:**

- Uses the same command byte (`0x12`) as Touchscreen Unknown 1 ([Section 22](#22-touchscreen-unknown-1-️)), but is 1 byte longer (15 vs 14 bytes total)
- Broadcast at startup, paired with the gateway's firmware-version announcement ([§17](#17-firmware-version-))
- The embedded checksum at byte 12 is a data-level field, distinct from the standard frame checksum at byte 13 (`major + minor + embedded_checksum`, computed by the framing layer over all payload bytes)
- Carries redundant firmware-version information already announced by §17; firmware-version state population is left to §17 alone

---

### 19. Register Read Request/Response

The Internet Gateway periodically polls controller registers to sync state with the cloud service. This uses a request-response pattern.

**Request Pattern:** `02 00 F0 FF FF 80 00 39 0E B7`

**Response Pattern:** `02 00 50 FF FF 80 00 38 0F 17`

**Example - Request for register 0x88:**

```
02 00 F0 FF FF 80 00 39 0E B7 88 02 8A 03
                              ^^ Register ID (0x88)
                                 ^^ Slot ID
```

**Example - Response with register 0x88 value:**

```
02 00 50 FF FF 80 00 38 0F 17 88 02 00 8A 03
                              ^^ Register ID (0x88)
                                 ^^ Slot ID
                                    ^^ Register value (0x00)
```

**Request Data Fields (from Gateway):**

- Byte 10: Register ID to read
- Byte 11: Slot ID

**Response Data Fields (from Controller):**

- Byte 10: Register ID (echoed from request)
- Byte 11: Slot ID
- Byte 12: Register value

**Observed Behavior:**

- Gateway sends sequential requests (e.g., 0x88, 0x89, 0x8A, 0x8B)
- Controller responds ~120ms after each request
- Next request sent ~780ms after previous response
- Used for periodic status polling and cloud synchronization

**Notes:**

- The response command pattern is documented in [8. Register Messages](#8-register-messages-universal-register-system-️)
- Both request and response are broadcast (destination 0xFFFF)
- The gateway appears to scan ranges of registers systematically

---

### 20. Controller Day/Time/Clock ✅

Current time from the controller's internal clock. Broadcast periodically for synchronization.

**Pattern:** `02 00 50 FF FF 80 00 FD 0F DC`

**Example:**

```
02 00 50 FF FF 80 00 FD 0F DC 39 08 05 46 03
                              ^^ Minutes (57)
                                 ^^ Hours (8)
                                    ^^ Day of Week (5)
                                       → 08:57 on Saturday
```

**Example - Minute rollover:**

```
02 00 50 FF FF 80 00 FD 0F DC 3B 08 05 48 03  → 05:08:59
02 00 50 FF FF 80 00 FD 0F DC 00 09 05 0E 03  → 05:09:00
```

**Data Fields:**

- Byte 10: Minutes (0-59)
- Byte 11: Hours (0-23, 24-hour format)
- Byte 12: Day of Week (0-6, 0: Monday -> 6: Sunday)

**Notes:**

- This message is broadcast by the controller for device time synchronization
- Used by connected devices (touchscreen, internet gateway) to maintain consistent time
- Appears to be sent every minute

---

### 22. Touchscreen Unknown 1 ⚠️

Broadcast consistently after the firmware version message (`0A 0E E8`). Currently appears to always have data value `05 00`.

**Pattern:** `02 00 50 FF FF 80 00 12 0E F0`

**Example:**

```
02 00 50 FF FF 80 00 12 0E F0 05 00 05 03
                              ^^ Unknown (always 0x05)
                                 ^^ Unknown (always 0x00)
```

**Data Fields:**

- Byte 10: Unknown (always `0x05` in observed samples)
- Byte 11: Unknown (always `0x00` in observed samples)

**Notes:**

- This message is broadcast by the controller as part of the regular system status sequence

---

### 23. Valve State Broadcast ✅

Broadcast by the touchscreen to report the configured and active state of all valve zones. Appears in two LENGTH variants.

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
- State transitions correlate exactly with [§24 Valve Sync to Controller](#24-valve-sync-to-controller-️)

---

### 24. Valve Sync to Controller ⚠️

Sent by the touchscreen directly to the controller (destination `0x006F`) to synchronise the overall valve active status. Emitted as part of the regular broadcast cycle.

**Pattern:** `02 00 50 00 6F 80 00 25 0D 73`

**Example — No valve active:**

```
02 00 50 00 6F 80 00 25 0D 73 00 00 03
                              ^^ Active flag: 0x00 = no valve active
```

**Example — At least one valve active:**

```
02 00 50 00 6F 80 00 25 0D 73 01 01 03
                              ^^ Active flag: 0x01 = valve(s) active
```

**Data Fields:**

- Byte 10: Valve active bitmask — one bit per valve slot (`0x00` = no valve active, bit 0 = valve 1, bit 1 = valve 2)
- Byte 11: DATA_CHECKSUM (equals byte 10)

**Observed values:**

| Value | Meaning |
|-------|---------|
| `0x00` | No valves active |
| `0x01` | Valve 1 active only |
| `0x02` | Valve 2 active only |
| `0x03` | Both valves active |

**Notes:**

- Unlike most touchscreen messages which broadcast (`FF FF`), this is addressed specifically to the Controller (`0x006F`)
- Mirrors the OR of all `active` flags in [§23](#23-valve-state-broadcast-) encoded as a bitmask
- Emitted every broadcast cycle (~60 s); may lag behind real-time valve state changes by up to one cycle

---

## Control Commands (Gateway to Controller)

The following commands can be sent from the Internet Gateway (or emulated gateway) to control pool equipment.

---

### 25. Light Zone Control Command ✅

Command to set light zone state (On/Off/Auto).

**Pattern:** `02 00 F0 FF FF 80 00 3A 0F B9`

**Example - Turn ON spa light (Zone 2):**

```
02 00 F0 FF FF 80 00 3A 0F B9 C1 01 02 C4 03
                              ^^ Register ID (0xC1 = Zone 2)
                                 ^^ Slot ID (0x01 = State)
                                    ^^ State value (0x02 = On)
                                       ^^ Checksum (0xC1 + 0x01 + 0x02 = 0xC4)
```

**Example - Turn OFF spa light (Zone 2):**

```
02 00 F0 FF FF 80 00 3A 0F B9 C1 01 00 C2 03
                              ^^ Register ID (0xC1 = Zone 2)
                                 ^^ Slot ID (0x01 = State)
                                    ^^ State value (0x00 = Off)
                                       ^^ Checksum (0xC1 + 0x01 + 0x00 = 0xC2)
```

**Data Fields:**

- Bytes 0-1: `02 00` - Start
- Bytes 2: `00 F0` - Source (Internet Gateway = 0x00F0)
- Bytes 3-4: `FF FF` - Destination (Broadcast)
- Bytes 5-6: `80 00` - Control bytes
- Bytes 7-9: `3A 0F B9` - Command pattern for register control
- Byte 10: Register ID (0xC0-0xC7 for zones 1-8)
- Byte 11: Slot ID (0x01 = State)
- Byte 12: State value (0x00 = Off, 0x01 = Auto, 0x02 = On)
- Byte 13: Checksum (sum of bytes 10-12)
- Byte 14: `03` - End byte

**Register IDs:**

- `0xC0`: Light Zone 1
- `0xC1`: Light Zone 2 (Spa)
- `0xC2`: Light Zone 3
- `0xC3`: Light Zone 4
- `0xC4`: Light Zone 5
- `0xC5`: Light Zone 6
- `0xC6`: Light Zone 7
- `0xC7`: Light Zone 8

**State Values:**

- `0x00`: Off
- `0x01`: Auto
- `0x02`: On

**Notes:**

- This command requires the sender to impersonate the Internet Gateway (source address 0x00F0)
- The controller will process the command and update the light zone state accordingly
- The command pattern `3A 0F B9` distinguishes gateway control commands from status broadcasts (`38 0F 17`)

---

### 26. Channel Toggle Command ✅

Command to cycle a channel through its available states (Auto → On → Off, or On → Off depending on channel type).

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

- Bytes 0-1: `02 00` - Start + Source High
- Byte 2: `F0` - Source Low (Internet Gateway = 0x00F0)
- Bytes 3-4: `FF FF` - Destination (Broadcast)
- Bytes 5-6: `80 00` - Control bytes
- Bytes 7-9: `10 0D 8D` - Command pattern for channel toggle
- Byte 10: Channel index (0-based)
- Byte 11: Checksum (equals channel index, as that is the only data byte)
- Byte 12: `03` - End byte

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

- Sending this command always advances the state - there is no direct way to set a specific state
- The controller will respond with an updated [Channel Status message (§7)](#7-channel-status-)
- Channel index is 0-based and corresponds to the channel's position in the controller configuration

---

### 27. Temperature Setpoint Command ✅

Command to set the pool or spa temperature setpoint. The temperature byte is repeated twice within the payload.

**Pattern:** `02 00 F0 FF FF 80 00 19 0F 98`

**Example - Set Pool to 30°C:**

```
02 00 F0 FF FF 80 00 19 0F 98 01 1E 1E 3D 03
                              ^^ Target (0x01 = Pool)
                                 ^^ Temperature °C (0x1E = 30)
                                    ^^ Temperature °C (repeated)
                                       ^^ Checksum (0x01 + 0x1E + 0x1E = 0x3D)
```

**Example - Set Spa to 37°C:**

```
02 00 F0 FF FF 80 00 19 0F 98 02 25 25 4C 03
                              ^^ Target (0x02 = Spa)
                                 ^^ Temperature °C (0x25 = 37)
                                    ^^ Temperature °C (repeated)
                                       ^^ Checksum (0x02 + 0x25 + 0x25 = 0x4C)
```

**Data Fields:**

- Byte 10: Target (`0x01` = Pool, `0x02` = Spa)
- Byte 11: Temperature in °C
- Byte 12: Temperature in °C (repeated)
- Byte 13: Checksum (sum of bytes 10-12)

**Notes:**

- The temperature value is repeated at bytes 11 and 12 — this is part of the message format, not two separate sends
- The controller will respond with an updated [Temperature Settings message (§2)](#2-temperature-settings-)

---

### 28. Heater Control Command ✅

Command to turn the heater on or off. Uses the same `3A 0F B9` command pattern as the Light Zone Control Command ([Section 25](#25-light-zone-control-command-)), but with a different register ID and slot.

**Pattern:** `02 00 F0 FF FF 80 00 3A 0F B9`

**Example - Turn Heater On:**

```
02 00 F0 FF FF 80 00 3A 0F B9 E6 00 01 E7 03
                              ^^ Register ID (0xE6 = Heater)
                                 ^^ Slot (0x00)
                                    ^^ State (0x01 = On)
                                       ^^ Checksum (0xE6 + 0x00 + 0x01 = 0xE7)
```

**Example - Turn Heater Off:**

```
02 00 F0 FF FF 80 00 3A 0F B9 E6 00 00 E6 03
                              ^^ Register ID (0xE6 = Heater)
                                 ^^ Slot (0x00)
                                    ^^ State (0x00 = Off)
                                       ^^ Checksum (0xE6 + 0x00 + 0x00 = 0xE6)
```

**Data Fields:**

- Byte 10: Register ID `0xE6` (Heater)
- Byte 11: Slot `0x00`
- Byte 12: State (`0x00` = Off, `0x01` = On)
- Byte 13: Checksum (sum of bytes 10-12)

**Notes:**

- This command uses the same pattern as Light Zone Control (`3A 0F B9`) but register `0xE6` with slot `0x00` identifies it as the heater
- Unlike light zones (slot `0x01`), the heater uses slot `0x00`
- The controller will respond with an updated [Heater Status message (§4)](#4-heater-status-️)

---

### 29. Mode/Favourite Control Command ✅

Command sent by the Internet Gateway to the Touch Screen to switch modes or activate a stored Favourite preset. A single data byte encodes the target mode or favourite index.

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

- Bytes 1-2: `00 F0` - Source (Internet Gateway = `0x00F0`)
- Bytes 3-4: `00 50` - Destination (Touch Screen = `0x0050`) — **not broadcast**
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

- **Destination is Touch Screen (`0x0050`), not broadcast** — This is addressed specifically to the touch screen, which holds the stored Favourite presets and applies them
- **Command values are inverted from status values** — In status messages ([§1 Mode Message](#1-mode-message-spapool-)), Spa=`0x00` and Pool=`0x01`; in this command, Pool=`0x00` and Spa=`0x01`
- The Touch Screen acknowledges each activation with an immediate CMD `0x05` broadcast (value `0x01`) followed by the relevant mode, active-channel, and channel-status broadcasts
- Up to 6 user Favourites are supported (`0x02`–`0x07`). The labels for all 8 slots (including the Pool and Spa built-ins) are stored in registers `0x31`–`0x38` (slot `0x03`), readable via the register protocol (§8)
- Each slot's enabled/disabled state is stored in registers `0x21`–`0x28` (slot `0x03`), with `0x01` = enabled and `0x00` = disabled. Pool (`0x21`) and Spa (`0x22`) are always `0x01`. All Off (`0x80`) and All Auto (`0x81`) have no corresponding enable registers and are always available
- This command requires the sender to impersonate the Internet Gateway (source address `0x00F0`)

---

### 30. Valve Control Command ✅

Sent by the Internet Gateway to set a valve to a specific state directly. Unlike the Channel Toggle Command (§26) which cycles through states, this sets the target state explicitly.

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
- The controller responds immediately with an updated Valve State Broadcast (§23)

---

### 31. Chlorinator Cell Mode ⚠️

Inter-device unicast carrying the chlorinator's current mode to the salt cell. Observed on systems with chlorinator address `0x0084` (mutually exclusive with the `0x0090` variant — see [Device Addresses](#device-addresses)).

**Pattern (Chlorinator → Cell):** `02 00 84 00 A0 80 00 18 0D CB`

**Pattern (Touchscreen → Cell):** `02 00 50 00 A0 80 00 18 0D 97`

Both patterns have LENGTH `0x0D` (13 bytes) and a single data byte.

**Examples:**

```
02 00 84 00 A0 80 00 18 0D CB 01 01 03   Chlorinator -> Cell, mode = 0x01 (Manual)
02 00 84 00 A0 80 00 18 0D CB 02 02 03   Chlorinator -> Cell, mode = 0x02 (Automatic)
02 00 50 00 A0 80 00 18 0D 97 02 02 03   Touchscreen echoes mode = 0x02 to Cell
```

**Data Fields:**

- Byte 10: Mode value
- Byte 11: Data checksum (equals byte 10 — only one data byte)

**Observed Mode Values:**

| Value  | Meaning (tentative) |
|--------|---------------------|
| `0x00` | Off                 |
| `0x01` | Manual              |
| `0x02` | Automatic           |

**Notes:**

- ⚠️ Mode-value mapping is **tentative**. Three distinct values (`0x00`, `0x01`, `0x02`) have been observed across one capture, and they match the standard pool-channel state encoding ([§7](#7-channel-status-)), but the order seen in the capture did not unambiguously match a user-described Manual→Off→Auto sequence — see `zagnuts_analysis.md` for the timeline.
- The chlorinator reports its current mode separately to the touchscreen via CMD `0x0F` (see below) — the two messages may briefly disagree during transitions.
- Both messages are addressed specifically to the Cell (`0x00A0`), not broadcast.
- The `0x0090` chlorinator variant has not been observed using this command; the `0x18` traffic appears specific to the `0x0084` / `0x00A0` two-module chlorinator topology.
- A related CMD `0x0F` is sent from `0x0084` to the touchscreen (`0x0050`) carrying the same mode value as a 2-byte payload `[01, mode]`. Not yet a separate section pending more captures.

---

### 32. Chlorinator Status Broadcast ⚠️

Broadcast from the chlorinator carrying its current operating mode. Both chlorinator address variants (`0x0090` and `0x0084` — mutually exclusive; see [Device Addresses](#device-addresses)) emit this message with the same structure, distinguished only by the source address and the resulting header checksum.

**Pattern (variant A — `0x0090`):** `02 00 90 FF FF 80 00 12 0D 2F`

**Pattern (variant B — `0x0084`):** `02 00 84 FF FF 80 00 12 0D 23`

Both patterns have LENGTH `0x0D` (13 bytes) and a single data byte. The header checksum differs (`0x2F` vs `0x23`) purely because the source byte changes (`0x90` vs `0x84`).

**Examples:**

```
02 00 90 FF FF 80 00 12 0D 2F 01 01 03   Chlorinator 0x0090 -> Broadcast, mode = 0x01 (Auto)
02 00 84 FF FF 80 00 12 0D 23 02 02 03   Chlorinator 0x0084 -> Broadcast, mode = 0x02 (On)
```

**Data Fields:**

- Byte 10: Mode value
- Byte 11: Data checksum (equals byte 10 — only one data byte)

**Observed Mode Values:**

| Value  | Meaning (tentative) |
|--------|---------------------|
| `0x00` | Off (not yet observed) |
| `0x01` | Auto                |
| `0x02` | On                  |

**Notes:**

- ⚠️ Mode-value mapping is **tentative**. It follows the standard `0x00=Off / 0x01=Auto / 0x02=On` channel-state convention used elsewhere in the protocol ([§7](#7-channel-status-)), but a single-device transition across Off ↔ Auto ↔ On has not yet been captured. `0x00` may simply not be broadcast when the chlorinator is powered off.
- This is distinct from the configured *cell* mode broadcast in [§31](#31-chlorinator-cell-mode-), which is sent over a unicast to the salt cell (`0x00A0`) with CMD `0x18`. The `0x12` broadcast here is the chlorinator's own overall mode, addressed to the broadcast destination (`0xFFFF`). The two can hold different values concurrently — e.g., the chlorinator overall = On while the cell is in Auto.
- CMD `0x12` is also used by four other devices as a status broadcast (heater, gateway, touchscreen — see the [Device Status table](#known-command-bytes)); each is source-dependent and carries a different payload layout.
- Observed on both chlorinator hardware variants in independent captures (round8 system: `0x0090` with payload `0x01`; zagnuts system: `0x0084` with payload `0x02`).

---

## Appendix A: Register Dispatch Table

The register ID and slot together determine the message meaning. The slot distinguishes different data aspects of the same register. Used by the universal register message format ([Section 8](#8-register-messages-universal-register-system-️)).

### Dispatch Table

| Register Range  | Slot   | Purpose                | Data Format                                      |
|-----------------|--------|------------------------|--------------------------------------------------|
| `0x08`–`0x17`  | `0x04` | Timers 1–16            | start/stop time + days bitmask (see [§8 Timer Registers](#timer-registers-slot-0x04))   |
| `0x21`–`0x28`  | `0x03` | Favourite/Mode Enable  | 1-byte flag (`0x01`=enabled, `0x00`=disabled). Maps to CMD `0x2A` values `0x00`–`0x07` in order. Pool (`0x21`) and Spa (`0x22`) are always `0x01`. |
| `0x31`–`0x38`  | `0x03` | Favourite/Mode Labels  | Null-terminated ASCII string. Maps to CMD `0x2A` values `0x00`–`0x07` in order. `0x31`=Pool, `0x32`=Spa, `0x33`–`0x38`=user Favourites 1–6. |
| `0x6C`–`0x73`  | `0x02` | Channel Types          | 1-byte type code (see [Section 7](#7-channel-status-) channel types)   |
| `0x7C`–`0x83`  | `0x02` | Channel Names          | Null-terminated ASCII string                     |
| `0x8C`–`0x93`  | `0x02` | Channel State          | 1-byte value (0=Off, 1=Auto, 2=On) — read-only; writes ignored by controller |
| `0xA0`–`0xA7`  | `0x01` | Light Zone Multicolor  | 1-byte flag (`0x00`=No, `0x01`=Yes)              |
| `0xB0`–`0xB7`  | `0x01` | Light Zone Name        | 1-byte preset name code (see [name codes table](#light-zone-name-codes)) |
| `0xC0`–`0xC7`  | `0x01` | Light Zone State       | 1-byte value (0=Off, 1=Auto, 2=On)               |
| `0xD0`–`0xD1`  | `0x02` | Valve Labels           | Null-terminated ASCII string                     |
| `0xD0`–`0xD7`  | `0x01` | Light Zone Color       | 1-byte color code                                |
| `0xE0`–`0xE7`  | `0x01` | Light Zone Active      | 1-byte binary (`0x00`=Inactive, `0x01`=Active)   |
| `0xF4`         | `0x01` | Channel Count          | 1-byte total number of channels in the system    |
| `0xE6`         | `0x00` | Heater State (Heater 1)   | 1-byte (`0x00`=Off, `0x01`=On)                |
| `0xE7`         | `0x00` | Pool Temperature Setpoint (Heater 1) | 1-byte °C value                    |
| `0xE8`         | `0x00` | Spa Temperature Setpoint (Heater 1)  | 1-byte °C value                    |
| `0xE9` ⚠️       | `0x00` | Heater 2 State (tentative)     | 1-byte (`0x00`=Off, `0x01`=On). See note below. |
| `0xEA` ⚠️       | `0x00` | Heater 2 Pool Setpoint (tentative) | 1-byte °C value — writable via gateway CMD `0x3A`. See note below. |
| `0xEB` ⚠️       | `0x00` | Heater 2 Spa Setpoint (tentative)  | 1-byte °C value. See note below.   |

**Notes:**

- Register ranges can overlap (e.g., `0xD0`–`0xD7`) but are distinguished by the slot value
- The same slot value (e.g., `0x02`) can represent different data formats depending on the register
- Slot values appear to be context-dependent rather than globally defining a data type
- **`0xE9`/`0xEA`/`0xEB` (Heater 2 trio) — tentative ⚠️**: Slot `0x00` already holds the Heater 1 trio at `0xE6` (state), `0xE7` (Pool setpoint), `0xE8` (Spa setpoint). The next three registers (`0xE9`/`0xEA`/`0xEB`) appear to be the analogous trio for a second heater, based on the following evidence:
  - **Structural symmetry**: a 3-register block in the same slot, immediately adjacent to the Heater 1 trio.
  - **`0xEA` is user-writable** via the gateway register-write command (CMD `0x3A` / second-byte `0xB9`). An observed write `02 00 F0 FF FF 80 00 3A 0F B9 EA 00 1B 05 03` set the value to `0x1B` (27°C) and the touchscreen immediately rebroadcast `EA 00 1B`.
  - **Value match to H2**: that 27°C exactly matches the **H2** value carried in the heater's (`0x0070`) CMD `0x17` `[H1, H2]` broadcast (where H1=24°C also matches `0xE7`).
  - **Mutually exclusive broadcast**: in captures observed so far the touchscreen broadcasts *either* the `E6/E7/E8` trio *or* the `E9/EA/EB` trio in slot `0x00`, but not both — consistent with a config-dependent enable (likely §5 byte 10 bit 3 = heater count).
  - **Caveats**: `0xEB` has been observed fixed at `0x0A` (10°C) across multiple installs — an unusual spa setpoint, but explainable as an unused default when the second heater isn't actually plumbed to spa. Single-bit toggle confirmation (changing spa setpoint in the UI and watching which register updates) has not yet been performed.

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

> Channel state is **read-only** via the register system. To change channel state, use the [Channel Toggle Command (§26)](#26-channel-toggle-command-).

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
   ^^^^^  Source: 0x0050 (Controller)
         ^^^^^  Destination: 0xFFFF (Broadcast)
               ^^^^^  Control: 0x8000
                     ^^^^^^^^  Command: Mode message pattern
                              ^^ Data: 0x01 = Pool mode
                                 ^^ Checksum: 0x01 (sum of byte 10)
                                    ^^ End byte
```

**Decoded:** Controller broadcasts Pool mode to all devices.

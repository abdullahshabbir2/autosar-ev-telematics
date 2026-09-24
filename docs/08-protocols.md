# Wire Formats and Protocols

**Audience:** anyone implementing against these formats — a replacement ECU, a test harness, a
consumer of the telemetry stream — or debugging a bus with a logic analyser. Assumes you can read a
byte layout; assumes nothing about this firmware.

Every layout here is verified by a host test, and every non-obvious constant was derived
independently before being written into one. Where a figure came from a published specification, the
specification is named; where it came from v1's own hardware-proven code, that is said too.

---

## 1. RS485 battery bus

**Physical:** half duplex, twisted pair, 120 Ω at each end. MAX3485 with `DE` and `/RE` tied.
**Framing:** 4800 baud, 8 data bits, even parity, 1 stop (8E1). Fixed by the packs' BMS.
**Topology:** up to four packs, addressed 1–4, polled in turn.

### Request

```
Offset  Size  Field          Value
------  ----  -------------  -----------------------------------------------
  0       1   Start          0x7E
  1       1   Address        0x01 .. 0x04  (pack number)
  2       1   Command        0x03 = read serial number
                             0x04 = read pack summary
                             0x05 = read cell voltages
  3       1   Length         Payload length, 0 for a read
  4..n    n   Payload        Absent for a read
  n+1     2   CRC-16         CCITT-FALSE, big endian, over offsets 0..n
```

A read request is 6 bytes. At 4800 8E1 each character is 10 bits, so **6 × 10 / 4800 = 12.5 ms** on
the wire.

### Response

```
Offset  Size  Field          Notes
------  ----  -------------  -----------------------------------------------
  0       1   Start          0x7E
  1       1   Address        Echoes the request
  2       1   Command        Echoes the request
  3       1   Length         Payload length
  4..n    n   Payload        Per command, below
  n+1     2   CRC-16         CCITT-FALSE, big endian, over offsets 0..n
```

Longest response is the cell-voltage frame at **81 bytes = 169 ms** on the wire. That figure is what
sets `RS485IF_RESPONSE_TIMEOUT_MS`: 400 ms is 2.4× the transmission time, which tolerates one retry
without the timeout itself becoming the limiting factor.

### Payloads

**Command 0x03 — serial number.** 16 bytes ASCII, space padded, not NUL terminated.

**Command 0x04 — pack summary**, 22 bytes:

| Offset | Size | Field | Units | Range |
|---|---|---|---|---|
| 0 | 2 | Pack voltage | 10 mV | 0 – 655.35 V |
| 2 | 2 | Pack current | 10 mA, signed | ±327.68 A |
| 4 | 2 | State of charge | 0.01 % | 0 – 100 % |
| 6 | 2 | Remaining capacity | 10 mAh | — |
| 8 | 2 | Full capacity | 10 mAh | — |
| 10 | 2 | Cycle count | cycles | — |
| 12 | 2 | Temperature 1 | 0.1 K | — |
| 14 | 2 | Temperature 2 | 0.1 K | — |
| 16 | 2 | Protection status | bit field | — |
| 18 | 2 | Balance status | bit field | — |
| 20 | 2 | Reserved | — | — |

**Temperature is in tenths of a kelvin, offset 2731.** `°C × 10 = raw − 2731`. Getting that offset
wrong gives readings about 273 °C too high, which is obvious — the dangerous error is applying it
twice.

**Command 0x05 — cell voltages**, 2 + 46 bytes:

| Offset | Size | Field | Units |
|---|---|---|---|
| 0 | 1 | Cell count | — (23 on these packs) |
| 1 | 1 | Reserved | — |
| 2 | 46 | 23 × cell voltage, big endian | 1 mV |

### Byte order

**All multi-byte fields are big endian**, including the CRC. This is stated flatly because it is the
single most common way to get this protocol wrong, and because v1's CAN code had exactly the opposite
problem — a comment claiming big endian over code that did little endian.

### Verified against v1's own frames

`tools/rs485_reference.py` reproduces the `DUMMY_DATA` frames embedded in v1's source:

| Frame | v1's CRC | Computed | |
|---|---|---|---|
| `Dummy_SN_response` | `0xAC56` | `0xAC56` | ✅ |
| `Dummy_CELL_response` | `0xB047` | `0xB047` | ✅ |
| `Dummy_BATT_response` | `0x2B76` | `0xF831` | ❌ **v1's constant is wrong** |

Two exact matches cross-validate the layout above against code that demonstrably talked to real
packs — much stronger evidence than any frame invented for the purpose.

The third is a genuine defect in v1, not in the model. It survived because `readResponse` returned
`true` unconditionally on the dummy-data path: a wrong CRC sat in the source, inside the function
whose job was to detect exactly that, for the life of the product.

### Timing

Half duplex means the turnaround has to be right:

```
  ┌── DE high ──┬─────── request on the wire ───────┬── DE low ──┬─ response ─┐
  │   setup     │            12.5 ms                │  turnaround│  12–169 ms │
  │   100 µs    │                                   │   100 µs   │            │
```

`DE` may only drop once the last bit has physically left the shift register. Arduino's `write()`
returns when the bytes are *buffered*, so `Uart_DrainTx` waits for both the ring buffer to empty and
the shift register to clear. v1 dropped `DE` straight after `write()`, truncating the last character
of every request it ever sent — tolerated only because the truncated byte was the second CRC byte and
the packs checked only the first.

---

## 2. CAN — motor controller

**Physical:** ISO 11898-2, 500 kbit/s, 120 Ω at each end.
**Controller:** MCP2515 at 10 MHz over SPI, TJA1050 transceiver.
**Frames:** extended (29-bit) identifiers, 8 data bytes.

### Identifiers

| ID | Contents | Period |
|---|---|---|
| `0x0CF11E05` | Motor speed, motor current, DC-link voltage | 100 ms |
| `0x0CF11F05` | Controller and motor temperature, status, error flags | 100 ms |
| `0x0CF11G05` | Throttle, brake, drive mode | 100 ms |

### `0x0CF11E05` layout

| Bytes | Field | Units | Encoding |
|---|---|---|---|
| 0–1 | Motor speed | rpm | Unsigned, little endian |
| 2–3 | Motor current | 0.1 A | Signed, little endian |
| 4–5 | DC-link voltage | 0.1 V | Unsigned, little endian |
| 6 | Status | bit field | — |
| 7 | Error | bit field | — |

**Little endian, and this was a real ambiguity.** v1's comment said big endian; v1's code did little
endian. The code was adopted, because it was the version that demonstrably produced plausible speed
readings from real hardware. It is now behind `CANIF_MCU_BYTE_ORDER_LITTLE_ENDIAN` so a future
correction is one configuration change rather than an edit spread across a decoder.

### Signal freshness

Every decoded signal carries the monotonic timestamp of the frame it came from. A signal older than
`CANIF_MCU_SIGNAL_TIMEOUT_MS` (500 ms, five frame periods) is not used, and
`DEM_EVENT_CAN_SIGNAL_STALE` is raised.

This matters specifically for odometry: a held-over speed value integrated over an interval adds
distance the vehicle did not travel. A stale reading must be *absent*, not *old*.

### MCP2515 identifier encoding

The controller splits a 29-bit identifier across four registers in a layout that is easy to get
wrong. For `0x0CF11E05`:

```
  29-bit ID:  0 1100 1111 0001 0001 1110 0000 0101

  SIDH = bits 28..21                     = 0x67
  SIDL = bits 20..18, EXIDE, bits 17..16 = 0xC8
         └─ 110 ──────┘ 1 ─── └─ 00 ──┘
  EID8 = bits 15..8                      = 0x10
  EID0 = bits 7..0                       = 0x05
```

**`SIDL = 0xC8`, not `0xC9`.** I wrote `0xC9` into the test first and it was wrong — bit 0 of `SIDL`
is the low bit of the two extended-identifier bits, not part of `EXIDE`. The driver was right and the
test was wrong. The derivation above is reproduced in `test_can.c` so the next person can check it
against the datasheet rather than trusting the code.

`Can_EncodeIdentifier` and `Can_DecodeIdentifier` are exposed in the header specifically so this can
be tested exhaustively across both identifier ranges instead of sampled.

---

## 3. NMEA 0183 — GNSS

**Physical:** software UART, receive only, GPIO34.
**Framing:** 9600 8N1.
**Sentences consumed:** `GPRMC` (position, speed, date, time), `GPGGA` (altitude, satellites, fix
quality). Everything else is discarded without being parsed.

### Example

```
$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230326,003.1,W*6A
       │      │ │         │ │         │ │     │     │      │      │└ checksum
       │      │ │         │ │         │ │     │     │      │      └─ variation E/W
       │      │ │         │ │         │ │     │     │      └──────── magnetic variation
       │      │ │         │ │         │ │     │     └─────────────── date DDMMYY
       │      │ │         │ │         │ │     └───────────────────── course, degrees
       │      │ │         │ │         │ └─────────────────────────── speed, knots
       │      │ │         │ │         └───────────────────────────── E/W
       │      │ │         │ └─────────────────────────────────────── longitude DDDMM.MMM
       │      │ │         └───────────────────────────────────────── N/S
       │      │ └─────────────────────────────────────────────────── latitude DDMM.MMM
       │      └───────────────────────────────────────────────────── A = valid, V = warning
       └────────────────────────────────────────────────────────────  UTC HHMMSS
```

### Checksum

XOR of every byte between `$` and `*`, as two uppercase hex digits. A sentence failing it is
discarded and counted. Not optional — a corrupted position would otherwise be recorded as fact.

### Coordinates as integers

NMEA gives degrees and decimal minutes (`DDMM.MMMM`). This firmware carries coordinates as **degrees
× 10⁷ in a `sint32`**, which resolves about 1.1 cm and needs no floating point.

```
  4807.038  →  48 degrees + 07.038 minutes
            →  48 × 10⁷ + (7.038 / 60) × 10⁷
            →  480 000 000 + 1 173 000
            →  481 173 000
```

**The conversion rounds to nearest, not toward zero.** The integer division adds half the divisor
first:

```c
minutesScaled = ((minutes * 10000000uL) + (minutesDivisor / 2u)) / minutesDivisor;
```

Truncation here would bias every reading toward the equator and the prime meridian. That is
one-directional, so it does not average out over a journey — it is a systematic position offset, not
noise. This was a genuine bug in the first draft.

### Plausibility

A new fix is rejected if the implied speed from the previous fix exceeds
`GNSSIF_MAX_PLAUSIBLE_SPEED_KMH`.

Speed-based, deliberately, rather than a geographic box. v1 rejected any coordinate outside a
hard-coded latitude/longitude rectangle covering Pakistan — so the firmware silently stopped
recording position if the vehicle were ever shipped elsewhere, and the failure would have looked like
a receiver fault.

The unit conversion in this check was also wrong in the first draft: a coordinate count is 10⁻⁷
degrees, so millimetres are `count × METRES_PER_DEGREE / 10⁴`, not `/ 10³`. The factor of ten would
have rejected any movement above about 10 km/h.

---

## 4. SD record format

One file per day, `/YYYYMMDD.csv`. One line per acquisition.

### Why that filename

Lexicographic order *is* chronological order, so "the oldest file" is answerable by string comparison
with no timestamps read and no parsing. `FsAbs_PlatformFindOldestLog` relies on it.

v1 parsed filenames with `atoi` and fell back to the first directory entry whenever the conversion
failed — which on a card holding any other file picked that one and deleted it.

### Line format

```
<field>,<field>,...,<field>|<crc32>\n
```

A CSV body, a `|`, the CRC-32 of the body as eight uppercase hex digits, a newline.

```
20260324T143052Z,481173000,113100000,142,47,721,183,...,4102|A3F19C2E
```

The CRC covers the body only — not the separator, not the CRC field, not the newline. A line whose
CRC does not match is skipped during backfill and counted; it is never transmitted as if valid.

`|` as the separator because it cannot appear in any field: every field is numeric or a fixed-format
timestamp. A comma would have been ambiguous.

### Fields

199 per record: **23 vehicle-level + 44 × 4 packs**.

```c
#define COM_RECORD_FIELD_COUNT (23u + (44u * COM_PACK_COUNT))
```

Computed rather than written as a literal, because the literal was wrong. I declared 198 and the
actual count is 199 — an off-by-one in a hand-maintained constant that would have truncated the last
pack's final field in every record ever written. A computed expression cannot drift from the layout
it describes.

| Group | Count | Contents |
|---|---|---|
| Timestamp | 1 | ISO 8601 basic, UTC |
| Position | 4 | Latitude, longitude ×10⁷; altitude m; satellites |
| Odometry | 3 | Total distance mm; trip distance mm; speed rpm |
| Electrical | 4 | DC-link voltage 0.1 V; motor current 0.1 A; aux voltage mV; throttle % |
| Thermal | 2 | Controller °C ×10; motor °C ×10 |
| Status | 9 | Reset reason, degraded mask, bearer, RSSI, DTC count, task overruns, free heap, card free MiB, uptime s |
| Per pack | 44 × 4 | Voltage, current, SoC, capacity, cycles, 2 temperatures, status ×2, 23 cell voltages, cell count, spread, min, max, responding flag |

An absent pack writes empty fields, not zeros. A zero is a reading; an empty field is the absence of
one, and the consumer must be able to tell them apart.

---

## 5. MQTT

### Topics

| Topic | Direction | QoS | Payload |
|---|---|---|---|
| `odo/<device>/record` | publish | 0 | One CSV record |
| `odo/<device>/health` | publish | 0 | Health record, CSV |
| `odo/<device>/dtc` | publish | 1 | DTC list on change |
| `odo/<device>/backfill` | publish | 0 | Historical records, chunked |
| `odo/<device>/cmd` | subscribe | 1 | Command |
| `odo/<device>/cmd/resp` | publish | 1 | Command response |

`<device>` is the eFuse MAC as 12 lowercase hex digits. From eFuse, not `WiFi.macAddress()` — the
latter returns zeros until the radio has initialised, which would give an unprovisioned unit an
all-zero identity at exactly the moment it needs a unique one.

### QoS 0 for records, and why that is not a weakness

Store-and-forward gives a **stronger** guarantee than broker-level QoS 1: a record stays on the card
until it is acknowledged at application level. QoS 1 would duplicate a guarantee that already exists
and add a round trip per record.

`NETIF_DIAGNOSTIC_QOS` is declared 1 for the DTC and command topics. PubSubClient implements QoS 0
for publishing only, so the value currently documents intent for a client that supports it; the
parameter is accepted and ignored, and `NetIf_Esp32.cpp` says so at the call site rather than
pretending otherwise.

### Commands

```
<sequence>,<command>[,<arg>...]
```

| Command | Arguments | Effect |
|---|---|---|
| `READ_DTC` | — | Publish the full DTC store |
| `CLEAR_DTC` | — | Clear confirmed DTCs |
| `READ_DID` | DID | Read one data identifier |
| `BACKFILL` | start, end | Publish records in a time range |
| `SET_CFG` | key, value | Write a configuration value |
| `RESET` | — | Orderly shutdown and reset |
| `CLEAR_CRASH_LOOP` | — | Clear the crash-loop counter |

The sequence number is echoed in the response so a caller can match them.

**Backfill is bounded at parse time.** A range above `TELEMSWC_MAX_BACKFILL_RECORDS` is rejected
before any record is read. v1 would read an unbounded number into the heap on request — a denial of
service reachable by anyone who could publish to the topic.

`Com_ComputeChunkPlan` computes the chunking in one place and is tested with `totalRecords == 0`,
because the obvious expression `(total - 1) / max + 1` underflows to a loop bound of about four
billion. That `size_t` underflow was in the first draft.

### Reduced UDS

`READ_DTC` and `READ_DID` follow ISO 14229 in structure and status semantics, not in transport — the
payloads are text over MQTT, not ISO-TP over CAN. DTC status bytes use the standard bit definitions
(`testFailed`, `confirmedDTC`, `testNotCompletedThisOperationCycle`, ...), so an engineer who knows
UDS can read them without a separate table.

The deviation is deliberate: full UDS over a cellular link is a poor fit — it assumes a low-latency
request/response channel — and the value here is in the *semantics* being standard, not the framing.

---

## 6. CRC reference

Six profiles, all AUTOSAR SWS_CRCLibrary. Every one self-checked against its published `check` value
by `tools/crc_reference.py` before any test vector is emitted.

| Profile | Poly | Init | RefIn | RefOut | XorOut | Check ("123456789") | Used for |
|---|---|---|---|---|---|---|---|
| CRC8 | `0x1D` | `0xFF` | no | no | `0xFF` | `0x4B` | — |
| CRC8H2F | `0x2F` | `0xFF` | no | no | `0xFF` | `0xDF` | — |
| CRC16 | `0x1021` | `0xFFFF` | no | no | `0x0000` | `0x29B1` | **RS485 frames** |
| CRC16/ARC | `0x8005` | `0x0000` | yes | yes | `0x0000` | `0xBB3D` | — |
| CRC32 | `0x04C11DB7` | `0xFFFFFFFF` | yes | yes | `0xFFFFFFFF` | `0xCBF43926` | **SD records, NvM blocks** |
| CRC32P4 | `0xF4ACFB13` | `0xFFFFFFFF` | yes | yes | `0xFFFFFFFF` | `0x1697D06A` | — |

The two unused-in-production profiles are implemented and tested because they are part of the
specified library, and a partial implementation of a standard interface is worse than a complete one:
the next module that needs CRC8H2F would otherwise add a second, untested implementation.

The reference model computes bit-at-a-time — the slow, obvious way. The production code is
table-driven and reflected, which is fast and easy to get subtly wrong. Two independent
implementations agreeing on six published constants is evidence; one agreeing with itself is not.

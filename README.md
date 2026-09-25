# EV Telematics & Odometry ECU

Production-grade firmware for an electric vehicle telematics and odometry control unit, built on an
ESP32 in an AUTOSAR Classic layered architecture.

**333 host tests** across 16 suites · **`-Werror`** with eleven warning flags · **42.6 %** of one OTA
slot · four hardware conflicts turned into build errors

---

## What it does

Every three seconds, the unit:

- reads motor speed, DC-link voltage and current from the motor controller over **CAN** (MCP2515)
- polls up to four battery packs over a shared **RS485** bus — pack voltages, currents, temperatures,
  state of charge, and all 23 cell voltages per pack
- reads position from a **GNSS** receiver
- measures the auxiliary supply voltage
- integrates motor speed into an **odometer** reading, in exact integer millimetres
- writes one CSV record to an **SD card**
- publishes that record to an **MQTT** broker over WiFi or GPRS, whichever is available
- serves historical backfill requests from the broker

It also supervises itself: a diagnostic trouble-code store that survives power cycles, per-task
watchdog supervision, and crash-loop detection that degrades rather than reboots forever.

---

## Why it was rebuilt

The predecessor was a 719-line `main.cpp` plus twelve modules that reached directly for hardware,
shared state through a `byte flags[15]` array, and allocated an Arduino `String` on every path. It
worked, mostly. The problems were not stylistic:

| Symptom | Root cause |
|---|---|
| Odometer reading silently degraded over months | Distance stored as text, read back with `atol()`, which discards the fraction. The truncated value was written straight back, so the loss compounded. |
| CAN data never arrived | `init_can()` returned `true` unconditionally with the hardware call commented out. Nothing in the system could detect it. |
| A hung task stalled the logger indefinitely | Watchdog configured but no task ever subscribed to it — the boot log read as evidence of protection that did not exist. |
| Intermittent SD failures on a healthy card | Two devices sharing one SPI bus with no arbitration. |
| A returned unit carried no diagnostic evidence | `flags[15]` was overwritten every cycle and lost on reset, so the fault recurred on the replacement. |
| Credentials in the repository | A Firebase token and WiFi password committed as string literals — permanently, since history keeps them. |

Every one of those is now either impossible or detected. [docs/02-architecture.md](docs/02-architecture.md)
has the full account with file and function names.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Application      OdoSwc   BattSwc   TelemSwc   DiagSwc  HmiSwc │
├─────────────────────────────────────────────────────────────────┤
│  RTE / Scheduler  SchM  —  4 tasks, pinned, budgeted, supervised│
├─────────────────────────────────────────────────────────────────┤
│  Services         EcuM  Det  Dem  WdgM  NvM  Fee  Crc  Com      │
│                   ComM  Log                                      │
├─────────────────────────────────────────────────────────────────┤
│  ECU Abstraction  CanIf  Rs485If  GnssIf  IoHwAb  TimeAbs        │
│                   FsAbs  NetIf                                   │
├─────────────────────────────────────────────────────────────────┤
│  MCAL             Mcu Port Dio Adc Gpt Spi Uart Can Wdg Fls      │
│                   ── the only layer that touches hardware ──     │
└─────────────────────────────────────────────────────────────────┘
```

Each hardware-touching module splits into a **pure core** (`<M>.c`, compiled on host *and* target) and a
**platform leaf** (`<M>_Esp32.cpp`, target only). Roughly 90 % of the source is in the cores, where the
host suite tests the real implementation.

That split is enforced mechanically, not by convention: the test runner links every `src/**/*.c` into
every test binary, so a platform dependency leaking above the MCAL is a **link error**.

### Diagrams

Eight standalone SVGs in [docs/diagrams/](docs/diagrams/) — hand-written, so they render in any browser
with no plugin and diff as text:

| Diagram | What it answers |
|---|---|
| [Layer architecture](docs/diagrams/01-layer-architecture.svg) | The five layers in full, and the two deliberate exceptions to strict layering |
| [Startup flow](docs/diagrams/02-startup-flow.svg) | Which startup failures are survivable, and which two are not |
| [Acquisition data flow](docs/diagrams/03-acquisition-dataflow.svg) | A sensor reading becoming one field of a 199-column record |
| [Odometry](docs/diagrams/04-odometry-flow.svg) | Speed sample → distance, and why each gate rejects |
| [Crash-safe commit](docs/diagrams/05-crash-safe-commit.svg) | Where a power loss lands at each step of a write |
| [Bearer arbitration](docs/diagrams/06-bearer-arbitration.svg) | WiFi vs GPRS, and the decay that keeps a fallback temporary |
| [Task timing](docs/diagrams/07-task-timing.svg) | Four tasks to scale — periods, budgets, priorities, cores |
| [Store and forward](docs/diagrams/08-store-and-forward.svg) | The card as a queue, and a send cursor that survives a reset |

Four of them carry a note panel describing a defect their subject actually had, found by a test rather
than by review — a missing transition is visible in a state machine and invisible in a diff.

---

## Engineering highlights

**Integer-only odometry.** `float` has a 24-bit mantissa, so past 16.8 km it can no longer represent
every millimetre and increments are silently discarded — at 50 000 km a vehicle must travel about 4 m
before the reading changes. Distance is accumulated as `uint64` millimetres using a Q32 factor derived
from a Q24 π, trapezoidally integrated, with the sub-millimetre remainder carried between intervals.
Measured accuracy over a simulated 1.26 km journey: **0.85 mm**, with 0.046 mm of total rounding drift.
([ADR-0005](docs/adr/0005-integer-only-odometry.md))

**Crash-safe persistence.** Append-only records with a separate one-byte commit marker written last, so
the instant a record becomes valid is a single byte write. A power loss at any step leaves either the old
value or the new one — never a mixture. Each of the four garbage-collection steps is individually
fault-injected.

**Four production defects found by the tests.** The fallback from WiFi to cellular was permanent for the
life of the run — the failure count that triggered it could only be cleared by the bearer it was blocking,
so a vehicle that failed WiFi leaving its depot paid for cellular beside a healthy access point all day,
every day. The telemetry backlog stopped draining after midnight:
the transfer cursor was established on the first record ever written and nothing moved it to the next
day's file, so every later record was unreachable — and housekeeping then correctly refused to reclaim
the file the stuck cursor sat on, so the card filled and appends began failing, presenting as a storage
fault. Separately, `NvM` cleared a block's dirty flag on a failed write, so `NvM_WriteImmediate`
reported success without writing and the shutdown flush skipped it, silently discarding up to 100 m of
distance; and `Fee` did not advance its cursor past a partially-programmed slot, so every retry wrote
into poisoned media and every scan stopped there. The first needed only two days of records; the other
two are unreachable without injecting a flash failure.

**Independently derived test values.** Every non-obvious constant was computed by a separate model before
being written into a test. `tools/crc_reference.py` implements all six AUTOSAR CRC profiles
bit-at-a-time and self-checks against their published check values; `tools/rs485_reference.py` reproduces
the predecessor's own hardware-proven frames — and found that one of its three embedded CRCs is wrong,
undetected for the life of the product because the response validator returned `true` unconditionally.

**Hardware conflicts as build errors.** `Ecu_PinMap.h` detects a double-assigned GPIO by comparing the
bitwise OR of the pin bits against their arithmetic sum. It also rejects the internal flash pins and
input-only pins used as outputs. Three more static-assertion sets cover the schedule and the storage
block sizes; the latter fired on three under-sized blocks before any test ran.

**Shared-bus arbitration.** The MCP2515 and the SD card share VSPI at different clock rates and with
transaction lengths differing by five orders of magnitude. `Spi_Lock` makes the bus an owned resource,
held across a whole logical operation — a transfer attempted without it fails rather than working most
of the time.

---

## Quick start

```bash
# Credentials (git-ignored; the template is committed)
cp config/Secrets.h.template config/Secrets.h

# Host tests — no hardware needed
python tools/run_native_tests.py

# Target firmware
pio run

# Flash (first time, or after a partition change, needs a full erase)
pio run --target erase
pio run --target upload
```

`pio run --target erase` destroys the odometer reading — it lives in the `nvdata` partition. Read it
first. [docs/09-operations.md](docs/09-operations.md) covers flashing, provisioning, diagnosis and
credential rotation.

---

## Documentation

Start with [docs/README.md](docs/README.md), which says which document answers what. In reading order:

| Document | Answers |
|---|---|
| [01-requirements.md](docs/01-requirements.md) | What it must do — 129 requirements, each with its rationale |
| [02-architecture.md](docs/02-architecture.md) | How it is built, and what the predecessor got wrong |
| [diagrams/](docs/diagrams/) | The same design visually — eight SVGs, plus the dependency graph |
| [07-hardware.md](docs/07-hardware.md) | Schematic, pin map, bill of materials |
| [05-test-strategy.md](docs/05-test-strategy.md) | How the claims above are verified — and what is not covered |

Reference: [03-interfaces.md](docs/03-interfaces.md) ·
[04-safety-analysis.md](docs/04-safety-analysis.md) ·
[06-traceability.md](docs/06-traceability.md) ·
[08-protocols.md](docs/08-protocols.md) ·
[09-operations.md](docs/09-operations.md) ·
[10-coding-standard.md](docs/10-coding-standard.md)

Six decision records in [docs/adr/](docs/adr/) cover the choices a reader would otherwise have to
reverse-engineer — including the ones that deviate from AUTOSAR convention, and why.

---

## Repository layout

```
config/            Ecu_Cfg.h, Ecu_PinMap.h, Secrets.h.template
src/base/          Std_Types, Platform_Types, Compiler abstractions
src/mcal/          Hardware drivers — the only layer touching hardware
src/ecuabs/        ECU abstraction: protocol and device interfaces
src/services/      BSW: EcuM, Det, Dem, WdgM, NvM, Fee, Crc, Com, ComM, Log
src/app/           Application components
test/              16 suites plus the MCAL test doubles
tools/             Reference models, test runner, traceability and validation
docs/              Documentation, decision records, schematic, BOM
partitions/        Flash partition table
scripts/           Build-time provenance injection
```

---

## Status and scope

The firmware builds, the host tests pass, and the structural claims above are enforced by the build.
What is **not** claimed:

- **Not silicon-verified end to end.** The ~10 % of source in the platform leaves is verified only on
  hardware, and hardware bring-up is the remaining step.
  [docs/05-test-strategy.md](docs/05-test-strategy.md) §7 lists each gap and what covers it instead.
- **Not a conforming AUTOSAR implementation.** It is AUTOSAR *structured*. MemIf and BswM are omitted
  and the RTE is hand-written; [ADR-0002](docs/adr/0002-handwritten-rte.md) and
  [ADR-0003](docs/adr/0003-omit-memif.md) say why.
- **Not a functional-safety item.** The unit observes and records; it actuates nothing, so no failure of
  it can affect vehicle behaviour. [docs/04-safety-analysis.md](docs/04-safety-analysis.md) states the
  scope plainly and records six accepted risks with reasoning.

---

## Licence

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

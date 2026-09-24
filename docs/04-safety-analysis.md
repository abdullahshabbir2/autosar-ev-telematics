# Safety and Failure Analysis

**Audience:** reviewers assessing whether the failure handling is adequate, and engineers changing
code that one of these mitigations depends on. Assumes [02-architecture.md](02-architecture.md).

---

## 1. Scope, stated honestly

**This ECU is not a safety-related item, and this document is not a functional safety case.**

It observes and records. It actuates nothing: no torque request, no contactor, no thermal management,
no warning to the driver. Every output is an indicator LED and a data record. No failure of this unit
— including a total failure — can affect how the vehicle behaves.

That matters for two reasons. It is why no ASIL applies and why there is no ISO 26262 work product
here. And it is why the analysis below is worth doing anyway: the hazards are to *data integrity and
diagnosability*, not to persons, and those are the hazards the product actually has.

What follows is an FMEA in substance — failure mode, cause, effect, detection, mitigation — applied to
the things that can actually go wrong. Where a risk is accepted rather than mitigated, §6 says so and
says why.

### What is at stake

| Asset | Why it matters | Worst case if it fails |
|---|---|---|
| Odometer reading | The vehicle's legal mileage record | A wrong reading is a warranty and resale dispute with no way to reconstruct the truth |
| Battery history | Pack warranty claims and failure prediction | A claim cannot be assessed; a failing cell is not caught before it fails |
| Diagnostic record | The only evidence a returned unit carries | A returned unit is undiagnosable and the fault recurs on the replacement |
| Position history | Fleet operation and incident reconstruction | Gaps that cannot be filled after the fact |

---

## 2. Failure mode analysis

Severity is scored against the assets above, 1 (negligible) to 5 (the asset is lost and cannot be
reconstructed).

### 2.1 Odometry

| # | Failure mode | Cause | Effect | Sev | Detection | Mitigation |
|---|---|---|---|---|---|---|
| O1 | Reading degrades over time | Stored as text, read back with `atol()`, truncated value written back so the loss compounds | Reading drifts arbitrarily low; asset lost | 5 | None in v1 — this is the defect that motivated the rewrite | Integer millimetres end to end, binary storage, CRC-32 (SWREQ-ODO-0001) |
| O2 | Increments silently discarded | `float` has 24 bits of mantissa; past 16.8 km an increment below the current resolution is lost | Reading stops advancing correctly, worsening with mileage | 5 | Invisible without a reference odometer | No floating point in the odometry path at all ([ADR-0005](adr/0005-integer-only-odometry.md)) |
| O3 | Systematic under-read | Sub-millimetre remainder discarded each interval, 28 800 times a day | ~14 m/day, >5 km/year, all one direction | 4 | Only against a reference | Remainder carried into the next interval; total error bounded at 1 mm regardless of runtime (SWREQ-ODO-0003) |
| O4 | Distance from a corrupt reading | A corrupted CAN frame decodes to a plausible speed | Distance added that cannot be removed — the odometer is monotonic by design | 4 | Speed and acceleration plausibility gates | Reading rejected without contributing distance; `DEM_EVENT_ODO_IMPLAUSIBLE` raised (SWREQ-ODO-0010) |
| O5 | Loss of accumulated distance | Power cut between persists | Up to the persist threshold lost | 2 | Reset reason on next boot | 100 m threshold bounds the loss; flushed on orderly shutdown (SWREQ-ODO-0012) |
| O6 | Reading resets to zero | Both NvM copies unreadable and the default is zero | Asset lost | 5 | CRC failure on both copies | Two physical copies each with its own CRC; on double failure the last known value is used, never zero (SWREQ-ODO-0015) |
| O7 | Integration interval wrong | Task period drifts under load | Distance short by the drift | 3 | `SchM_GetTaskStats` reports actual period | `vTaskDelayUntil` measured from the previous boundary, not the end of the body; overrun counted and reported |

**O4 deserves emphasis.** The odometer cannot go down, so there is no correction path for a spurious
increment. That asymmetry is why the gates reject rather than clamp: a rejected reading costs one
interval of real distance, at most a few metres, while an accepted bad reading is permanent.

### 2.2 Storage

| # | Failure mode | Cause | Effect | Sev | Detection | Mitigation |
|---|---|---|---|---|---|---|
| S1 | Partial write indistinguishable from valid data | No integrity check; brown-out during engine cranking | Corrupt data used as if correct | 5 | None in v1 | CRC-32 on every block, verified on every read (SWREQ-NVM-0002) |
| S2 | Both copies lost in one event | Power loss mid-update with no ordered commit | Asset lost | 5 | Both CRCs fail | Append-only records with a separate one-byte commit marker written last, so the instant of validity is a single byte write (SWREQ-NVM-0003) |
| S3 | Flash worn out | Writing unconditionally every cycle | Writes begin failing; storage lost | 4 | `Fls_GetStatistics` erase count; wear event at 90 % of rated endurance | Write-on-change only, plus the 100 m threshold — roughly 40 writes/day in heavy use against 100 000 cycles (SWREQ-NVM-0004) |
| S4 | Garbage collection interrupted | Power loss mid-GC | Block store inconsistent | 4 | Recovery scan on next boot | Four-step GC designed so an interruption at any step is recoverable; each step individually fault-injected in `test_fee` |
| S5 | SD write fails intermittently | Two devices on one SPI bus with no arbitration | Records lost; looks like a failing card | 3 | Write failure count; card tests fine afterwards | `Spi_Lock` held across the whole card operation (SWREQ-COM-0020) |
| S6 | Card fills | No housekeeping | Writes fail | 2 | Free space reported in the health record | Oldest log removed below `FSABS_LOW_SPACE_LIMIT_MIB`; critical threshold raises an event |
| S7 | File handles exhausted | `File` leaked on an error path | Every later open fails; looks like a failing card | 3 | Open failures with a healthy card | Every `File` closed on every path; the FAT driver's table is 5 entries |
| S8 | Worn cell programs wrong | Flash cell at end of life | Silent corruption on a future boot | 4 | Read-back comparison after every write | `FLS_VERIFY_AFTER_WRITE`; failure converts to an immediate `E_VERIFY_FAILED` NvM can answer with the redundant copy |

### 2.3 Acquisition

| # | Failure mode | Cause | Effect | Sev | Detection | Mitigation |
|---|---|---|---|---|---|---|
| A1 | Device never started, reported healthy | `init_can()` returned `true` with the hardware call commented out | No CAN data for an unknown period; nothing can detect it | 5 | None — this is the point | No operation may return a value indistinguishable from success (SWREQ-SAF-0001); `Can_Init` verifies the controller entered configuration mode |
| A2 | Stale data used as current | Signal held from a previous frame after the source stops | Odometry integrates a speed that is no longer real | 4 | Per-signal age against `CANIF_MCU_SIGNAL_TIMEOUT_MS` | Signals carry a freshness timestamp; stale signals are not used |
| A3 | Response accepted without validation | `readResponse` returned `true` unconditionally | Arbitrary data treated as a battery reading | 5 | None in v1 — a wrong CRC sat in v1's own source undetected | CRC-16, length and address all checked (SWREQ-BAT-0002) |
| A4 | Previous pack's reply decoded as this one | `flush()` used intending to discard input | Pack N's data attributed to pack N+1 | 4 | Address field mismatch | `Uart_DiscardRx` before each request, distinct from `Uart_DrainTx` (SWREQ-COM-0001) |
| A5 | Request truncated on the wire | Driver enable dropped before the shift register emptied | Last byte lost; frames malformed but tolerated | 3 | Packs' CRC check — which checked only the first byte, so it was not detected | `Uart_DrainTx` before dropping DE (SWREQ-COM-0002) |
| A6 | Stack corrupted by frame build | Fixed-length write into a caller buffer with no capacity parameter | Corruption manifesting far from its cause | 5 | None | Capacity passed and checked; `E_NO_SPACE` returned (SWREQ-BAT-0001) |
| A7 | Missing pack looks like a fault | Absent pack averaged in as zero | Three healthy packs read as failing | 3 | Responding-pack count | Aggregates over responding packs only, with the count recorded (SWREQ-BAT-0010) |
| A8 | ADC reads plausible nonsense | Channel on ADC2, which returns stale data with no error while WiFi is active | Fictional battery voltage, faithfully averaged | 4 | None — the values look stable and reasonable | Sense pin is on ADC1; documented in both the pin map and `Adc_Esp32.cpp` |
| A9 | Position from a corrupt sentence | NMEA accepted without checksum verification | Wrong position recorded | 2 | NMEA checksum | Checksum verified; speed-based plausibility against the previous fix |

### 2.4 Execution

| # | Failure mode | Cause | Effect | Sev | Detection | Mitigation |
|---|---|---|---|---|---|---|
| E1 | Hung task stalls everything | One `loop()` doing all work | Complete loss of function until power cycle | 4 | None in v1 — watchdog configured but no task subscribed | Four independent tasks; per-task TWDT subscription (SWREQ-SAF-0004) |
| E2 | Watchdog satisfied by the wrong task | A single global watchdog any task may pet | A stalled task never trips it | 4 | None | Per-task subscription; one stalled task resets the ECU even while others are healthy |
| E3 | Task runs too often | Loop condition defect | Starves lower-priority tasks; corrupts the odometer's integration interval | 3 | Upper alive bound in WdgM | Alive counting has both bounds, not just a lower one (SWREQ-SAF-0005) |
| E4 | Heap exhausted by fragmentation | `String` per record and per field | Allocation fails while plenty is free; failure appears in an unrelated module | 4 | Largest contiguous block in the health record | No dynamic allocation after startup (SWREQ-SAF-0011) |
| E5 | Stack overflow | Deep call path plus a large automatic | Corrupts adjacent memory; fault appears nowhere near its cause | 4 | Stack high-water mark in the health record | Stacks sized from the deepest path with margin; large buffers module-static, so the cost is visible at link time |
| E6 | Reset loop | A subsystem failure treated as fatal | Unit logs nothing at all, including the fault | 5 | Restart count in NvM | No single subsystem failure is fatal (SWREQ-SYS-0060); crash-loop detection starts degraded (SWREQ-SYS-0070) |
| E7 | Timeout never expires | `time()` stepped backwards by NTP | A 3 s timeout becomes as long as the correction | 3 | None | All elapsed time from one monotonic source (SWREQ-SYS-0010) |
| E8 | Timeouts break after 49.7 days | 32-bit millisecond counter wraps | Timeouts fire immediately or never | 3 | None — appears seven weeks in | Modular arithmetic in `Gpt_HasElapsed`, tested across the boundary (SWREQ-SYS-0011) |
| E9 | State lost on reset | `ESP.restart()` from eleven places, none flushing | Up to 100 m of distance and the whole run's diagnostics lost | 3 | Reset reason | One supported shutdown path, which flushes everything first (SWREQ-SYS-0080) |

### 2.5 Communication and security

| # | Failure mode | Cause | Effect | Sev | Detection | Mitigation |
|---|---|---|---|---|---|---|
| C1 | Records lost though the card was healthy | Transmit before store | A failed send loses data that could have been kept | 4 | Record count against stored count | Store before send, always (SWREQ-TEL-0001) |
| C2 | Memory exhausted by a remote request | Unbounded backfill range from the broker | Denial of service, reachable by anyone who can publish | 4 | None in v1 | Bounded parse; range above the maximum rejected (SWREQ-TEL-0010) |
| C3 | Credentials disclosed | Committed to version control | Fleet database writable by anyone with repository read access | 5 | None | Git-ignored `Secrets.h`; v1's committed credentials are permanently compromised and **must be rotated** — see [09-operations.md](09-operations.md) (SWREQ-SEC-0001) |
| C4 | Odometer rewritten remotely | Unauthenticated on-vehicle web server | Legal mileage record altered by anyone within radio range | 5 | None | Web server removed; configuration only over the authenticated broker channel (SWREQ-SEC-0002) |
| C5 | Bearer paths diverge | Two parallel implementations of publish and retry | A fix applied to one and not the other; already the case in v1 | 2 | Code review, which did not catch it | One shared state machine; only bearer-specific operations differ (SWREQ-COM-0030) |
| C6 | Data charges while out of coverage | Retry at full rate indefinitely | Measurable cost for zero benefit; modem at full power | 2 | Attempt count | Exponential backoff 2 s → 300 s (SWREQ-COM-0045) |
| C7 | Malicious firmware accepted | Unverified update | Permanent compromise of the unit | 5 | HMAC verification before marking the slot bootable | Shared-secret HMAC-SHA256; weaker than asymmetric signing — see §6 (SWREQ-SEC-0010) |

---

## 3. Degraded operation

The design principle is that **every subsystem failure reduces what is recorded rather than stopping
recording.** What survives each failure:

| Failed | Lost | Still working |
|---|---|---|
| CAN controller | Speed, DC-link voltage and current, odometry | Battery data, position, storage, telemetry |
| All battery packs | Pack and cell data | Odometry, position, storage, telemetry |
| One battery pack | That pack's data; aggregates over the rest | Everything else, with the responding count recorded |
| GNSS | Position | Everything else |
| SD card | Store-and-forward buffer, historical backfill | Live publishing, odometry, all acquisition |
| Both bearers | Live publishing | Everything, buffered on the card for later |
| Real-time clock | Absolute timestamps until NTP succeeds | Everything; monotonic timing is unaffected |
| NvM (both copies) | Persistence; configured defaults used | Everything for this power cycle |
| **A task** | **That function entirely** | **This is the one fatal case — there is nothing to degrade to** |

Each entry raises a distinct diagnostic event, so a degraded unit is identifiable from its health
record rather than by inspection.

---

## 4. Defence in depth

The failures that cost the most are the silent ones, so the mitigations are layered — no single
check is the only thing standing between a corrupt input and a corrupt record.

**Battery data, from wire to storage:**

1. Hardware parity (8E1) — the UART flags a corrupted character.
2. CRC-16/CCITT-FALSE over the frame.
3. Length and address fields checked against what was requested.
4. Per-field range plausibility.
5. Aggregation over responding packs only, with the count recorded.
6. CRC-32 over the stored record.

**Odometer, from CAN frame to flash:**

1. CAN frame CRC (by the controller).
2. Signal freshness — a held-over value is not used.
3. Speed plausibility against the configured maximum.
4. Acceleration plausibility against the previous reading.
5. Integer arithmetic throughout, so no precision is lost silently.
6. CRC-32 on the stored block, two physical copies.
7. Monotonic check on read — a lower value than the last known is rejected.

Layer 4 catches what layer 3 cannot: a value individually plausible but impossible given the previous
one. Layer 7 catches what layers 1–6 cannot: a fault in the storage stack itself.

---

## 5. Systematic prevention

Some failure classes are better removed than detected:

| Class | How it is prevented |
|---|---|
| Discarded error status | `CHECK_RETURN` makes it a compile error; `STD_DISCARD` makes the deliberate cases auditable |
| Buffer overflow | Capacity passed with every writable buffer; no unbounded `str*` functions |
| Pin conflict | `Ecu_PinMap.h` compares the OR of the pin bits against their sum — a duplicate fails the build |
| Impossible schedule | `SchM_Cfg.h` asserts each budget is below its period and that supervision outranks what it supervises |
| Undersized storage block | `Fee_Cfg.h` asserts every block fits; it caught three during development |
| Platform leak above the MCAL | Host test runner links every `.c` into every suite — a leak is a link error |
| Unhandled new enumerator | `-Wswitch-enum -Werror` |
| Silent narrowing | `-Wconversion -Wsign-conversion -Werror` |
| Header shadowed by a library | Layer-qualified includes against two `-iquote` roots |

---

## 6. Accepted risks

Recorded explicitly, because an unmitigated risk that is not written down is indistinguishable from
one nobody thought of.

### R1 — Credentials are readable from the flash

**Risk.** Credentials are compiled into the image. Anyone with physical access can read the flash and
extract the WiFi password, APN and broker credentials.

**Why accepted.** Mitigating it needs ESP32 flash encryption and secure boot, which bring a
substantial operational cost: keys must be managed and stored securely, a unit with secure boot
enabled cannot be downgraded, and a lost key makes a unit unrecoverable. That is a fleet-scale
infrastructure commitment.

**Residual.** Physical access to one unit yields fleet WiFi and broker credentials. Bounded by the
broker's per-device authorisation — a compromised device identity can publish its own topics, not
another unit's.

**What would change this.** A deployment where units are physically accessible to untrusted parties,
or a broker that cannot enforce per-device authorisation.

### R2 — OTA uses a shared secret rather than asymmetric signing

**Risk.** Image authenticity is verified with HMAC-SHA256 against a secret every unit holds.
Extracting it from one unit compromises the update path for the whole fleet.

**Why accepted.** Asymmetric signing needs key infrastructure and a signing step in the release
process. The shared secret is a large improvement over v1, which verified nothing.

**Residual.** Physical compromise of one unit allows a forged image for all units.

**What would change this.** Any deployment beyond a controlled fleet. This is the first thing to fix
if the product scales.

### R3 — No redundant speed source

**Risk.** Motor speed comes only from the motor controller over CAN. A controller reporting a
consistently wrong value produces a consistently wrong odometer, and no check inside this ECU can
detect it.

**Why accepted.** A second source — a wheel sensor, or GNSS-derived speed — is a hardware change.
GNSS speed is available but too coarse to cross-check a 3-second interval usefully.

**Residual.** A miscalibrated or faulty controller yields a wrong odometer with no internal
indication.

**What would change this.** Cross-checking GNSS-derived distance against odometer distance over a
long window — hours, not intervals — would catch a gross error. Worth doing; not done.

### R4 — Single SD card, no redundancy

**Risk.** A card failure loses everything not yet transmitted.

**Why accepted.** A second card is a hardware change, and the failure is partly covered: records are
published as they are written, so only the backlog is at risk.

**Residual.** A unit out of coverage when its card fails loses that backlog.

### R5 — Clock depends on the RTC backup cell

**Risk.** A dead CR2032 means no valid time until a bearer comes up and NTP succeeds. Records before
that carry no absolute timestamp.

**Why accepted.** The failure is detected — `TimeAbs_PlatformRtcInit` reports the oscillator-stopped
flag rather than a plausible wrong time — and it is reported in the health record. Records remain
correctly *ordered*; they lack only an absolute reference.

**Residual.** A unit with a dead cell and no coverage produces records that can be ordered but not
dated.

### R6 — The DTC store can fill

**Risk.** `DTC_STORE` is 256 bytes. A unit with many distinct faults can fill it, after which new
DTCs are not recorded.

**Why accepted.** A unit with more than about a dozen distinct faults has a systemic problem that the
DTCs already recorded will describe. Growing the store trades flash space against a case where the
first faults are the informative ones.

**Residual.** The most recent faults on a badly-failed unit may be absent. The store is full and that
fact is itself reported, so the gap is visible rather than silent.

---

## 7. If you change one of these

Each mitigation above exists because of a specific failure. Before weakening one:

- **Find the failure mode it addresses** in §2 and check whether the cause still applies.
- **Check the requirement** it implements — the rationale in
  [01-requirements.md](01-requirements.md) records what it was protecting.
- **Check the test** that verifies it — [06-traceability.md](06-traceability.md) maps requirement to
  test. A mitigation with no test is one that can already have regressed.

The mitigations most likely to look like unnecessary cost, and most expensive to remove:

| Looks like | Actually |
|---|---|
| Verify-after-write doubles flash writes | It does not — it adds a *read*. It is what turns a worn cell into a detected error instead of a future corruption |
| `STD_DISCARD` is noise | It is the audit trail for every deliberately-ignored status. The alternative is disabling the warning, which hides the accidental ones too |
| The remainder carry is over-engineering | It is 5 km/year of odometer error |
| Two NvM copies waste flash | The odometer is the product. One copy plus a brown-out is the failure that motivated the rewrite |
| Plausibility gates reject good data | They reject at most one interval — a few metres. An accepted bad reading is permanent |

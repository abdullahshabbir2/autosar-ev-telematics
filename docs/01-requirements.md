# Software Requirements

**Audience:** engineers implementing or reviewing this firmware, and anyone assessing whether it does
what it claims. No prior knowledge of the vehicle or of AUTOSAR is assumed.

---

## How to read this document

Each requirement has an identifier, a statement, and a rationale. The identifier appears in the
source as a Doxygen `@req` tag and in the tests as a case comment, so
[06-traceability.md](06-traceability.md) is generated from the code rather than maintained alongside
it — which means a requirement with no implementation shows up as a gap instead of being quietly
dropped. `tools/check_doc_links.py` fails the build if any source file cites an identifier that is
not defined here.

**The rationale is not decoration.** A requirement whose reason is not recorded is one that gets
relaxed the first time it is inconvenient, by someone with no way to know what it was protecting.
Where a requirement exists because v1 failed in a specific way, the rationale says so and names the
function.

**must** marks anything whose violation makes the firmware unfit; **should** marks anything where a
documented deviation is acceptable. Nothing uses "may" — a requirement that permits anything
constrains nothing and belongs in the architecture document.

### Areas and number allocation

Numbers are allocated per module so that a block of identifiers maps to one place in the code. This
is why the ranges have gaps: the gaps are room to add a requirement next to its neighbours instead of
at the end.

| Prefix | Area | Range | Module |
|---|---|---|---|
| `SWREQ-SYS` | System | 0001–0002 | `Mcu` |
| | | 0010–0011 | `Gpt` |
| | | 0020–0021 | `Port`, `Dio` |
| | | 0030–0038 | `TimeAbs` |
| | | 0040–0058 | `SchM` |
| | | 0060–0085 | `EcuM` |
| `SWREQ-ODO` | Odometry | 0001–0015 | `OdoSwc` |
| `SWREQ-BAT` | Battery | 0001–0020 | `Rs485If` |
| | | 0030–0048 | `BattSwc` |
| `SWREQ-COM` | Communication | 0001–0003 | `Uart` |
| | | 0010–0015 | `Can` |
| | | 0020–0021 | `Spi` |
| | | 0030–0038 | `CanIf` |
| | | 0040–0060 | `NetIf` |
| | | 0070–0082 | `ComM` |
| `SWREQ-NVM` | Storage | 0001–0009 | `Fls`, `NvM` |
| | | 0010–0025 | `Fee` |
| | | 0030–0038 | `NvM` |
| `SWREQ-STO` | Filesystem | 0001–0020 | `FsAbs` |
| `SWREQ-TEL` | Telemetry | 0001–0020 | `Com` |
| | | 0030–0060 | `TelemSwc` |
| `SWREQ-DIAG` | Diagnostics | 0001–0003 | `Det` |
| | | 0010–0032 | `Dem` |
| | | 0040–0068 | `DiagSwc` |
| `SWREQ-SAF` | Safety | 0001–0012 | `WdgM`, `Wdg`, `Mcu`, `Spi` |
| `SWREQ-INT` | Integrity | 0010–0012 | `Crc` |
| `SWREQ-GNS` | Position | 0001–0014 | `GnssIf` |
| `SWREQ-SNS` | Analogue sensing | 0030–0038 | `Adc`, `IoHwAb` |
| `SWREQ-HMI` | Indication | 0001–0012 | `IoHwAb`, `HmiSwc` |
| `SWREQ-SEC` | Security | 0001–0010 | cross-cutting |

---

## 1. System

### SWREQ-SYS-0001 — Reset cause is recorded

The ECU **must** determine why it last reset and make that available to the startup sequence and the
health record.

> **Rationale.** It is the input to crash-loop detection and the first thing anyone diagnosing a
> returned unit wants. A brownout, a watchdog reset and a panic call for entirely different
> responses, and without the cause all three look identical from outside.

### SWREQ-SYS-0002 — Stable device identity

Each unit **must** have a unique identifier available before any network interface starts.

> **Rationale.** It is the MQTT client identifier and the topic prefix. It comes from eFuse, not from
> `WiFi.macAddress()` — the latter returns zeros until the radio has initialised, which would give an
> unprovisioned unit an all-zero identity at exactly the moment it needs a unique one, and two such
> units would collide on the broker.

### SWREQ-SYS-0010 — Single monotonic time source

All elapsed-time and timeout arithmetic **must** derive from one monotonic time base that is
unaffected by wall-clock corrections.

> **Rationale.** v1 mixed `millis()`, FreeRTOS ticks and `time(nullptr)` in timeout expressions. Two
> of those three break: `millis()` wraps after 49.7 days, and `time()` steps backwards when NTP
> corrects the clock — turning a 3-second timeout into a wait as long as the correction. Both
> failures are invisible in testing and appear weeks into deployment.

### SWREQ-SYS-0011 — Wrap-safe interval comparison

Every elapsed-time comparison **must** remain correct across a counter wrap.

> **Rationale.** A 32-bit millisecond counter wraps after 49.7 days. A unit running longer than that
> must not begin misbehaving, and must not do so in a way only a seven-week test would catch.
> `Gpt_HasElapsed` uses modular arithmetic and is verified across the boundary; direct comparison of
> two timestamps is forbidden by coding-standard rule CS-TIME-01.

### SWREQ-SYS-0020 — Single point of pin configuration

Every pin **must** be configured in exactly one place, and the allocation **must** be checked
mechanically for conflicts.

> **Rationale.** v1 configured pins from seven translation units, which is how GPIO2 came to be both
> the built-in LED and the CAN chip select without anyone noticing. A duplicated assignment is a
> static fact and is now a build error — see SWREQ-SAF-0010.

### SWREQ-SYS-0021 — Safe initial pin states

Each output **must** reach its inactive level before it is configured as an output.

> **Rationale.** A chip select at its power-on default of low, while the bus is being brought up,
> makes the attached device read the other device's traffic as its own command stream. That is the
> most plausible explanation for the intermittent "Card Initialization Failed" entries in v1's logs
> on cards that tested perfectly afterwards.

### SWREQ-SYS-0030 — Wall-clock time from a redundant source

Absolute time **must** come from a battery-backed real-time clock, with NTP as a second source, and
the two **must** be reconciled rather than one silently preferred.

> **Rationale.** Neither alone is sufficient. The RTC survives a power cycle but drifts and depends
> on a backup cell; NTP is accurate but needs a bearer, which a parked vehicle does not have.

### SWREQ-SYS-0031 — A stopped clock is reported, not guessed

If the real-time clock reports that its oscillator has stopped, its reading **must not** be used.

> **Rationale.** v1 used Adafruit RTClib, which reports a missing or unpowered device by returning a
> default-constructed `DateTime` — so an unplugged RTC read as 1 January 2000 with no error. That
> value went into log file names and record timestamps, and the only symptom was a day's data filed
> under the wrong date.

### SWREQ-SYS-0032 — Plausibility window on absolute time

A wall-clock reading outside 2024–2099 **must** be rejected.

> **Rationale.** A well-formed but impossible date passes every structural check. The window is wide
> enough never to reject a real time and narrow enough to catch the two failure modes that occur: a
> reset RTC reading 2000, and a bus error decoding to something arbitrary.

### SWREQ-SYS-0035 — Timestamps are UTC

Every timestamp the ECU produces **must** be UTC, with no timezone or daylight-saving adjustment.

> **Rationale.** A device that applies a timezone itself produces a log whose ordering breaks twice a
> year, and nothing downstream can repair it because the offset was never recorded. Local time is
> applied by whoever displays it.

### SWREQ-SYS-0038 — Calendar arithmetic is self-contained

Conversion between Unix time and broken-down time **must** be implemented as pure functions, not
taken from the C library.

> **Rationale.** `gmtime` returns a pointer into a static buffer, so two tasks calling it share one
> result. Implementing it makes it host-testable against known dates, including the 2100 non-leap
> year that a naive four-year rule gets wrong.

### SWREQ-SYS-0040 — Acquisition period

The ECU **must** acquire and record one complete data set every 3000 ms ± 50 ms, measured between
consecutive acquisitions rather than between the end of one and the start of the next.

> **Rationale.** The odometer integrates motor speed over time (SWREQ-ODO-0002), so the integration
> interval must match the assumed period or the distance comes out short by the difference. v1 mixed
> `vTaskDelay`, bare `delay()` and blocking I/O in one loop body; its nominal 3 s cycle measured
> between 3.2 s and 13 s depending on what the network was doing, and every millisecond of that
> excess was distance the odometer never counted. ±50 ms is 1.7 %, which at the vehicle's top speed
> is under 0.5 m per interval.

### SWREQ-SYS-0041 — Independent task execution

Each functional area — supervision, acquisition, storage, connectivity — **must** execute in its own
task, so a delay in one cannot defer another beyond its period.

> **Rationale.** In v1 one `loop()` body did everything, so a slow SD write or a stalled modem
> stopped acquisition entirely. Data never acquired cannot be recovered by any later retry; data
> acquired and not yet sent can.

### SWREQ-SYS-0045 — Period measured from the previous boundary

A task's next activation **must** be scheduled from its previous activation, not from the end of its
body.

> **Rationale.** Waiting a fixed interval *after* the body makes the activation rate
> `period + executionTime`, so every slow activation pushes all later ones further out and the
> sampling interval silently becomes a function of load. `vTaskDelayUntil` recovers; `vTaskDelay`
> accumulates.

### SWREQ-SYS-0050 — Execution budget

Each task **must** have a configured execution budget below its period, and an overrun **must** be
recorded as a diagnostic event.

> **Rationale.** A task reaching 100 % of its period has already missed one. A budget below the period
> makes the approach to the limit visible in the health record before a deadline is actually lost —
> the difference between a maintenance decision and a field failure.

### SWREQ-SYS-0055 — Bounded catch-up after an overrun

A task that overruns its whole period **must** resynchronise rather than run back-to-back to catch up.

> **Rationale.** Catching up starves every lower-priority task. On the connectivity task that means
> the network stack never runs, so the ECU stops publishing at exactly the moment it is busiest. One
> lost sample is cheaper than a starved system, and the overrun is counted either way.

### SWREQ-SYS-0058 — Observable task statistics

The ECU **must** report each task's actual period, worst-case execution time, overrun count and stack
high-water mark.

> **Rationale.** Scheduling behaviour cannot be verified on a host, so it is instrumented instead —
> a test that runs continuously on every unit rather than once on a bench. The stack figure is what
> lets the configured sizes be tightened against evidence instead of guessed downward.

### SWREQ-SYS-0060 — Non-fatal subsystem failure

Failure of any single subsystem — CAN controller, battery bus, GNSS receiver, SD card, network bearer,
real-time clock — **must not** prevent startup. Each failure **must** raise a diagnostic event and
startup **must** continue.

> **Rationale.** This is a data logger on a vehicle with no service connection. A failed CAN
> controller costs odometry but not battery data; a failed card costs the store-and-forward buffer but
> not live publishing. v1 did the opposite — `if (can.init_can()) ... else { delay(1000);
> ESP.restart(); while(1); }` — so a vehicle with a disconnected CAN harness rebooted forever and
> logged nothing at all, including the fault that would have explained why.

### SWREQ-SYS-0065 — Defined startup order

The initialisation order **must** be defined in one place, and no module **may** initialise another.

> **Rationale.** Four constraints have to hold at once: `Mcu` first so the reset cause is latched
> before anything can disturb it; `Det` second so a misconfiguration reported during another module's
> `Init` is recorded with its detail; the watchdog armed in its slow mode before the long blocking
> work; supervision activated last, after the tasks it supervises exist. Scattered initialisation
> cannot express those constraints, and cannot be reviewed against them.

### SWREQ-SYS-0070 — Crash-loop detection

The ECU **must** count resets within a window and, past a threshold, **must** start in a degraded mode
that skips the subsystem implicated by the previous reset cause.

> **Rationale.** Without it, a unit whose card is unmountable resets forever. v1 had the counting half
> — a restart count in NVS — but its only reaction was to upload the current log file and carry on
> resetting.

### SWREQ-SYS-0075 — Degraded state is reported

Which subsystems are unavailable, and why, **must** appear in the health record and be readable over
the diagnostic channel.

> **Rationale.** Otherwise "this unit is not reporting battery data" and "this unit has decided not to
> try" are indistinguishable from outside.

### SWREQ-SYS-0080 — Orderly shutdown

There **must** be exactly one supported way to reset the ECU, and it **must** persist the odometer, the
diagnostic record and every dirty non-volatile block before doing so.

> **Rationale.** v1 called `ESP.restart()` from eleven places, none of which flushed anything. Every
> one was an opportunity to lose up to 100 m of accumulated distance and the entire diagnostic history
> of the run that was about to end.

### SWREQ-SYS-0085 — Recoverable stable-run reset

The crash-loop counter **must** clear after a configured period of running without a reset, and
**must** be clearable over the diagnostic channel.

> **Rationale.** A unit that once crash-looped and has since been repaired must not stay degraded
> forever. The automatic clear handles the common case; the manual one lets a technician who has fixed
> the cause avoid waiting for it.

---

## 2. Odometry

### SWREQ-ODO-0001 — Integer-only distance

Distance **must** be accumulated in integer millimetres. No floating-point arithmetic is permitted
anywhere in the odometry path.

> **Rationale.** This is the number the product exists to produce, and `float` has 24 bits of
> mantissa. Past about 16 777 216 mm — 16.8 km — a `float` cannot represent every millimetre, so an
> increment below the current value's resolution is silently discarded. A vehicle with 50 000 km on it
> would need increments of about 4 m before they registered at all. See
> [ADR-0005](adr/0005-integer-only-odometry.md).

### SWREQ-ODO-0002 — Trapezoidal integration

Distance over an interval **must** be computed from the mean of the interval's start and end speeds,
not from either endpoint alone.

> **Rationale.** Rectangular integration over-reads on acceleration and under-reads on braking, and
> the two do not cancel: a vehicle that accelerates from rest and stops again returns to zero speed,
> so both errors favour the higher speed encountered. The trapezoidal rule is exact for any linear
> speed change, which over a 3-second interval closely approximates how a vehicle behaves.

### SWREQ-ODO-0003 — Remainder carry

The sub-millimetre remainder of each interval's distance **must** be carried into the next interval.

> **Rationale.** Discarding it truncates once per acquisition — 28 800 times a day. At a conservative
> mean of 0.5 mm lost per interval that is 14 m per day, over 5 km per year, all in one direction.
> Carrying the remainder bounds the total error at one millimetre regardless of runtime.

### SWREQ-ODO-0005 — Calibrated wheel circumference

The wheel circumference **must** be a per-unit calibration value, not a compile-time constant.

> **Rationale.** Tyre size, wear and pressure all change it, and a 1 % error is 1 % of every reading
> forever. A fleet with two wheel sizes cannot be served by one constant.

### SWREQ-ODO-0010 — Plausibility gating

A speed reading **must** be rejected without contributing distance if it exceeds the configured
maximum, or implies an acceleration beyond the configured maximum.

> **Rationale.** A corrupted CAN frame can decode to a plausible number. Without a gate, one bad frame
> adds distance that can never be removed — the odometer is monotonic by design, so there is no
> correction path. The acceleration gate catches what the speed gate cannot: a value individually
> plausible but impossible given the previous reading.

### SWREQ-ODO-0012 — Persistence threshold

The odometer **must** be written to non-volatile storage every 100 m of accumulated distance, and on
every orderly shutdown.

> **Rationale.** A trade between flash wear and worst-case loss. Writing every acquisition would be
> 28 800 writes a day and exhaust the flash endurance in months; writing only at shutdown loses
> everything since the last one on a power cut, which on a vehicle is the normal way it stops. 100 m
> bounds the loss at 100 m and the wear at roughly 40 writes a day in heavy use. The arithmetic is in
> [02-architecture.md](02-architecture.md).

### SWREQ-ODO-0015 — Monotonic reading

The odometer reading **must never** decrease, including across a reset, a storage fault, or a firmware
update.

> **Rationale.** It is the vehicle's legal mileage record. A reading that can go down is not a mileage
> record. This is why a failed non-volatile read falls back to the redundant copy and, if both are
> unreadable, to the last known value rather than to zero.

---

## 3. Battery acquisition

### SWREQ-BAT-0001 — Bounded frame construction

Every RS485 request **must** be built into a buffer whose capacity is passed to the builder, and the
builder **must** refuse a buffer too small for the frame.

> **Rationale.** v1's `sendRequest` wrote a fixed-length frame into a caller-supplied array with no
> capacity parameter. One call site passed an array two bytes short, and the resulting stack overflow
> corrupted whatever was adjacent — a fault that manifests nowhere near its cause.

### SWREQ-BAT-0002 — Response integrity

Every RS485 response **must** be rejected unless its CRC-16/CCITT-FALSE matches and its length and
address fields are as expected.

> **Rationale.** v1's `readResponse` returned `true` unconditionally on one path. The consequence was
> not theoretical: one of the three dummy response frames embedded in v1's own source carries an
> incorrect CRC, and nothing ever noticed, because nothing ever checked. See
> `tools/rs485_reference.py`.

### SWREQ-BAT-0005 — Bounded response wait

A response **must** be waited for no longer than a configured timeout derived from the frame's
transmission time, and the wait **must** end as soon as the last byte arrives.

> **Rationale.** At 4800 8E1 an 81-byte response is 169 ms on the wire, so 400 ms is 2.4× that — enough
> for one retry without the timeout becoming the limiting factor. v1's read loop had no early exit and
> burned a full second per frame; three frames per pack across four packs is 12 s of a 3 s budget spent
> waiting for data that had already arrived.

### SWREQ-BAT-0010 — Bus is left in the receive state

The RS485 transceiver **must** rest with its driver disabled.

> **Rationale.** A half-duplex bus left with the driver enabled blocks every other device on it. This
> is why `Port_Init` drives the enable low before anything else touches the bus.

### SWREQ-BAT-0020 — Stale input discarded before a request

Buffered received data **must** be discarded before each request is issued, and the discarded count
**must** be available.

> **Rationale.** v1 called `flush()` intending this; `flush()` waits for the *transmit* buffer instead.
> So every exchange began by parsing whatever the previous one had left in the receive FIFO, and a late
> reply from the previous pack could be decoded as the current pack's reply. A non-zero discard count
> before a request means the bus is out of step, which is worth logging.

### SWREQ-BAT-0030 — Aggregate over responding packs only

Pack aggregates — total voltage, mean temperature, state of charge — **must** be computed over the packs
that actually answered, and the responding count **must** be recorded alongside.

> **Rationale.** Averaging a missing pack in as zero makes three healthy packs look like a fault.
> Averaging it in as the mean of the others hides that a pack is missing. Recording the count alongside
> is the only form that lets a reader tell the two apart.

### SWREQ-BAT-0035 — Cell imbalance detection

The ECU **must** compute the spread between the highest and lowest cell voltage in each pack and raise
a diagnostic event when it exceeds the configured limit.

> **Rationale.** Imbalance is the earliest observable sign of a failing cell, and it is visible in data
> v1 already collected and discarded. Detecting it is the difference between replacing one cell at a
> service interval and replacing a pack at the roadside.

### SWREQ-BAT-0040 — Per-field range validation

Each decoded pack field **must** be checked against a physically possible range before use.

> **Rationale.** A frame can pass its CRC and still carry a field the BMS never meant to send — a
> firmware revision difference, or a sensor reading its own fault value. A state of charge of 300 % is
> not a state of charge.

### SWREQ-BAT-0048 — Absent data is distinguishable from zero

A pack that did not answer **must** be recorded as absent, not as zero.

> **Rationale.** Zero volts is a reading and an alarming one. The absence of a reading is a different
> fact, and a consumer building a time series must be able to tell them apart.

---

## 4. Communication

### SWREQ-COM-0001 — Discard and drain are distinct operations

The serial driver **must** provide separate operations for discarding buffered received data and for
waiting until transmitted data has left the shift register.

> **Rationale.** Arduino's `Serial.flush()` does the second. v1 called it in three places intending the
> first — see SWREQ-BAT-0020 for what that cost.

### SWREQ-COM-0002 — Transceiver turnaround

The RS485 driver-enable line **must not** be deasserted until the last bit of the request has physically
left the transmitter.

> **Rationale.** `write()` returns when the bytes are buffered, not sent. v1 dropped DE immediately
> after `write()`, truncating the last character of every request it ever transmitted. The packs
> tolerated it because the truncated byte was the second CRC byte and their firmware checked only the
> first — so the bus worked and the frames were wrong.

### SWREQ-COM-0003 — Early-exit reads

A read of a known-length response **must** return as soon as the last byte arrives, not after the full
timeout, and **must** report how many bytes arrived even on timeout.

> **Rationale.** The early exit is the 12 s of budget in SWREQ-BAT-0005. The partial count is what
> distinguishes "the pack is not answering" from "the pack answered and the frame was cut short".

### SWREQ-COM-0010 — CAN controller initialisation is verified

`Can_Init` **must** confirm the controller entered configuration mode, and **must** fail if it did not.

> **Rationale.** v1's `init_can()` returned `true` unconditionally with the hardware call commented out.
> The controller was never started for an unknown period, and nothing in the system could have detected
> it. This is the specific defect SWREQ-SAF-0001 generalises.

### SWREQ-COM-0012 — Platform-free CAN driver

The MCP2515 driver **must** contain no platform dependency, driving the device through `Spi`, `Dio` and
`Gpt` only.

> **Rationale.** It makes the identifier encoding and the register sequences host-testable, which for a
> device whose identifier layout spans four registers in a non-obvious order is where the errors are.
> The alternative — a library that drives chip select itself — also cannot be arbitrated against the SD
> card on the same bus.

### SWREQ-COM-0015 — Exhaustive identifier encoding

Identifier encode and decode **must** be exposed separately from frame transmission.

> **Rationale.** So they can be tested across both the 11-bit and 29-bit ranges exhaustively rather
> than sampled through whole frames. Writing the expected register values by hand caught nothing; the
> exhaustive test is what confirms the layout.

### SWREQ-COM-0020 — Shared bus arbitration

Two devices sharing one SPI bus **must** acquire it exclusively for the duration of a logical operation,
not per transfer.

> **Rationale.** An SD write is a command, a data block, a busy poll that can run for seconds during
> internal wear levelling, and a status read. Releasing the bus between those phases lets the other
> device assert its chip select mid-sequence, and the card abandons the write. v1 had no arbitration at
> all, which is the origin of the intermittent "Card Initialization Failed" entries in its logs.

### SWREQ-COM-0021 — Bus misuse is reported, not tolerated

A transfer attempted without holding the bus lock **must** fail and be reported.

> **Rationale.** An unlocked transfer works most of the time, which is the worst possible behaviour: it
> passes every test and corrupts data in the field under contention that only occurs on a loaded
> vehicle.

### SWREQ-COM-0030 — Decoded signals carry their age

Every signal decoded from CAN **must** carry the timestamp of the frame it came from.

> **Rationale.** Without it, a value held over from a source that has stopped transmitting is
> indistinguishable from a current one.

### SWREQ-COM-0032 — Stale signals are not used

A signal older than its configured timeout **must not** be used, and **must** raise a diagnostic event.

> **Rationale.** A held-over speed value integrated over an interval adds distance the vehicle did not
> travel. A stale reading must be *absent*, not *old*.

### SWREQ-COM-0035 — Byte order is configuration, not code

The byte order of each decoded signal **must** be a configuration switch.

> **Rationale.** v1's comment said big endian and its code did little endian. The code was adopted,
> because it demonstrably produced plausible readings from real hardware — but the ambiguity is real, and
> a future correction should be one configuration change rather than an edit spread through a decoder.

### SWREQ-COM-0038 — Frame filtering at the interface

Frames not in the configured set **must** be discarded without being decoded.

> **Rationale.** The bus carries traffic this ECU has no business interpreting. Decoding an unknown
> frame produces plausible nonsense from a correct implementation.

### SWREQ-COM-0040 — One bearer abstraction

WiFi and cellular **must** be driven through one shared state machine, with only the operations that
genuinely differ implemented per bearer.

> **Rationale.** v1 had two parallel implementations of publishing, reconnection and backoff, and they
> had already drifted: the WiFi path retried three times and the GPRS path retried indefinitely; the
> WiFi path checked the publish result and the GPRS path did not. Any fix had to be remembered twice.

### SWREQ-COM-0045 — Bounded reconnection backoff

Reconnection attempts **must** back off exponentially from 2 s to a ceiling of 300 s.

> **Rationale.** A unit parked out of coverage for a weekend must not spend it retrying at full rate: on
> GPRS that is a measurable data charge for zero benefit, and it keeps the modem at full power draw. The
> 300 s ceiling bounds the delay in noticing that coverage has returned.

### SWREQ-COM-0050 — Association and addressing are distinct

A bearer **must not** be reported up until it holds an IP address, not merely a link.

> **Rationale.** With DHCP there is a window of tens to hundreds of milliseconds between association and
> addressing, and a TCP connect attempted inside it fails in a way that looks like a broker fault. v1
> checked only the link status and retried the broker for that whole window on every connection.

### SWREQ-COM-0055 — Bounded publish

A publish **must not** block longer than a configured socket timeout.

> **Rationale.** Without one, the write blocks in lwIP until the TCP stack gives up — tens of seconds on
> a stalled GPRS link, long enough for the connectivity task to blow its budget and be reported as hung.

### SWREQ-COM-0060 — Signal strength in real units

Reported signal strength **must** be in dBm for both bearers.

> **Rationale.** `AT+CSQ` returns 0–31, not dBm. v1 published the raw value, so a strong signal of 31 read
> as a nonsensical +31 dBm and could not be compared against the WiFi figure beside it.

### SWREQ-COM-0070 — One bearer active at a time

Exactly one bearer **must** be active when any is, chosen by configured preference.

> **Rationale.** Running both wastes power and, worse, makes it ambiguous which one carried a given
> record when diagnosing a coverage problem.

### SWREQ-COM-0075 — Hysteresis on bearer switching

A switch to a preferred bearer **must** require it to remain available for a configured period.

> **Rationale.** A marginal WiFi link at the edge of a depot otherwise flaps the session continuously,
> and each flap costs a full broker handshake over the bearer that was working.

### SWREQ-COM-0082 — Bearer state is observable

The active bearer, its signal strength and the time since the last switch **must** appear in the health
record.

> **Rationale.** It is the first thing to check when a unit's records arrive late or in bursts.

---

## 5. Non-volatile storage

### SWREQ-NVM-0001 — Owned media

Non-volatile data **must** be stored on a raw flash partition this firmware owns outright, not in a
shared key-value store.

> **Rationale.** A key-value store reserves the right to relocate and rewrite entries, which makes the
> ordered-write guarantee SWREQ-NVM-0003 depends on impossible to express. See
> [ADR-0003](adr/0003-omit-memif.md).

### SWREQ-NVM-0002 — End-to-end integrity

Every stored block **must** carry a CRC-32 computed over its contents, verified on every read.

> **Rationale.** Without one, a partial write during the brown-out that accompanies engine cranking is
> indistinguishable from valid data. Neither of v1's two persistence paths had any integrity check.

### SWREQ-NVM-0003 — Crash-safe commit

A power loss at any instant during a write **must** leave either the previous value or the new value
intact and verifiable — never a mixture, and never nothing.

> **Rationale.** This is the requirement that makes the odometer trustworthy. It is met by an
> append-only record layout with a separate one-byte commit marker written last, so the instant at which
> a record becomes valid is a single byte write.

### SWREQ-NVM-0004 — Write on change only

A block **must not** be written when its contents have not changed.

> **Rationale.** Flash endurance is about 100 000 erase cycles per sector. Writing unconditionally once
> per acquisition would consume that in under four days.

### SWREQ-NVM-0006 — Write verification

Every flash write **must** be read back and compared before being reported successful.

> **Rationale.** It converts a cell that no longer programs from a silent corruption discovered on some
> future boot into an immediate error NvM can answer by using the redundant copy. The cost is one read
> per write, which at a few writes per minute is irrelevant.

### SWREQ-NVM-0008 — Wear is measurable

Erase counts **must** be tracked and a diagnostic event raised at a configured fraction of the rated
endurance.

> **Rationale.** The event exists to catch a configuration fault that turns a write-on-change into a
> write-every-cycle — an easy mistake to make and an expensive one to discover late. At 90 % of 100 000
> cycles there is still margin to service the unit.

### SWREQ-NVM-0009 — No implicit erase

A write **must not** erase implicitly.

> **Rationale.** Hiding an erase inside a write is how a 4 KiB sector gets destroyed to update two bytes.
> The layer above needs to control exactly when erases happen.

### SWREQ-NVM-0010 — Append-only record layout

Blocks **must** be stored as append-only records, with the most recent valid record for a block winning.

> **Rationale.** It is what makes the commit atomic: a new value is written beside the old one and becomes
> authoritative only when its marker is set, so no instant exists at which neither is valid.

### SWREQ-NVM-0015 — Crash-safe garbage collection

Reclaiming space **must** be recoverable from an interruption at any step.

> **Rationale.** Garbage collection is the one operation that temporarily has data in two places. A
> four-step sequence with a defined recovery for each step is what keeps a power loss during it from
> being the one case that loses everything. Each step is individually fault-injected in `test_fee`.

### SWREQ-NVM-0020 — Never-written is distinguishable from damaged

A read of a block that was never written **must** be reported differently from a read of a damaged one.

> **Rationale.** "Use the configured default" and "fall back to redundancy and raise a fault" are
> opposite responses. Conflating them means every brand-new unit raises a storage fault on its first
> boot — which was a real bug during development, caused by counting an uncommitted record as evidence
> that a block existed.

### SWREQ-NVM-0025 — Blocks are sized at compile time

Every block's length **must** be asserted at compile time to fit the data it holds.

> **Rationale.** An undersized block truncates silently. The assertion fired on three blocks during
> development, before any test ran.

### SWREQ-NVM-0030 — RAM mirrors

Each block **must** have a RAM mirror that is the working copy, with flash written only on an explicit
flush or a caller's threshold.

> **Rationale.** It is what makes SWREQ-NVM-0004 achievable without every caller having to track whether
> its data has changed.

### SWREQ-NVM-0035 — Configured defaults

Every block **must** have a configured default used when no stored value exists.

> **Rationale.** A first boot is not an error, and zero is not a safe default for a calibration value.

### SWREQ-NVM-0038 — Redundant copies for critical blocks

The odometer block **must** be stored in two physical copies, each with its own CRC.

> **Rationale.** The odometer is the product. One copy plus a brown-out is the failure that motivated the
> rewrite.

---

## 6. Filesystem

### SWREQ-STO-0001 — Records are self-verifying

Every record written to the card **must** carry a CRC-32 over its contents.

> **Rationale.** The card is the system of record for anything not yet transmitted. A corrupt line must be
> skippable, not transmitted as if valid.

### SWREQ-STO-0005 — Chronological file naming

Log files **must** be named so that lexicographic order is chronological order.

> **Rationale.** It makes "the oldest file" answerable by string comparison, with no timestamps read and no
> parsing. v1 parsed filenames with `atoi` and fell back to the first directory entry whenever the
> conversion failed — which on a card holding any other file picked that one and deleted it.

### SWREQ-STO-0010 — Bounded housekeeping

The oldest log **must** be removed when free space falls below a configured limit, and a critical limit
**must** raise a diagnostic event.

> **Rationale.** A full card silently stops accepting records. Two thresholds so that the reclaim happens
> long before the fault.

### SWREQ-STO-0015 — Every handle released

Every file handle **must** be closed on every path, including error paths.

> **Rationale.** The ESP-IDF FAT driver has a fixed table of open files — five by default. v1 leaked a
> `File` on several error paths, so a few thousand records into a leaking run every subsequent open
> failed, which looked exactly like a failing card.

### SWREQ-STO-0020 — Committed before reported

A record **must** be flushed to the card before the write is reported successful.

> **Rationale.** The FAT driver buffers a partial sector, so bytes are not on the card until it is written
> out. TelemSwc discards its in-memory copy on the strength of that return value.

---

## 7. Telemetry

### SWREQ-TEL-0001 — Bounded serialisation

Record serialisation **must** write into a fixed buffer whose capacity is checked, with no dynamic
allocation.

> **Rationale.** v1 built each record by concatenating Arduino `String` objects — one allocation per
> field, 199 fields, once every three seconds. See SWREQ-SAF-0011.

### SWREQ-TEL-0005 — Field count is derived, not declared

The record's field count **must** be computed from the layout, not written as a literal.

> **Rationale.** The literal was wrong. 198 was declared where the actual count is 199 — an off-by-one that
> would have truncated the last pack's final field in every record ever written.

### SWREQ-TEL-0010 — Bounded backfill parsing

A backfill request from the broker **must** be parsed into a bounded buffer and **must** be rejected if it
names a range larger than the configured maximum.

> **Rationale.** This is an externally-controlled input. v1 passed the broker payload straight into a fixed
> buffer and would read an unbounded number of records into the heap on request — a denial of service
> reachable by anyone who could publish to the topic.

### SWREQ-TEL-0020 — Chunk arithmetic is total

The chunk plan for a transfer **must** be correct for every input including zero records.

> **Rationale.** The obvious expression `(total - 1) / max + 1` underflows to a loop bound of about four
> billion when `total` is zero. That `size_t` underflow was in the first draft, and it is why the
> calculation is in one tested function rather than at each call site.

### SWREQ-TEL-0030 — Store before send

Every record **must** be committed to the SD card before any attempt is made to transmit it.

> **Rationale.** The card is the system of record and the network is best-effort. v1 transmitted first and
> wrote second, so a record lost to a failed write was gone even though it had been successfully sent — and
> one lost to a failed send was gone even though the card was healthy.

### SWREQ-TEL-0040 — Backlog drain

Records stored while no bearer was available **must** be transmitted once one becomes available, oldest
first, without displacing live records.

> **Rationale.** A vehicle out of coverage for a day should deliver that day's data on its next connection.
> Oldest-first matters because the consumer builds a time series; interleaving would require it to buffer
> and reorder.

### SWREQ-TEL-0050 — Application-level acknowledgement

A record **must** remain on the card until its delivery has been acknowledged.

> **Rationale.** This is a stronger guarantee than broker QoS 1, and it is why the telemetry topic uses QoS
> 0: the guarantee already exists one level up, and duplicating it would add a round trip per record.

### SWREQ-TEL-0060 — Health record

The ECU **must** periodically publish a record containing its firmware version, reset history, task
timing, stack high-water marks, heap figures, storage usage and active diagnostic trouble codes.

> **Rationale.** Almost every question asked about a deployed unit is answerable from this record. Without
> it, answering any of them requires physical recovery of the unit.

---

## 8. Diagnostics

### SWREQ-DIAG-0001 — Contract violations are reported

A detected contract violation **must** be reported to a central tracer with its module, instance, API and
error identity.

> **Rationale.** The alternative is a return code the caller cannot act on, or silence. Both mean the
> violation is discovered by its downstream consequence rather than at its origin.

### SWREQ-DIAG-0002 — Reports are deduplicated

Repeated identical reports **must** be counted, not stored repeatedly.

> **Rationale.** A fault in a 10 ms loop would otherwise produce thousands of entries and evict everything
> useful. One entry with a count carries the same information.

### SWREQ-DIAG-0003 — Development and runtime errors are distinguished

A caller's contract violation **must** be reported separately from a runtime failure.

> **Rationale.** The first should not survive integration testing; the second is expected in the field.
> Mixing them means neither population can be assessed.

### SWREQ-DIAG-0010 — Persistent trouble codes

Diagnostic trouble codes **must** survive a power cycle, with their occurrence count and the conditions at
first occurrence.

> **Rationale.** v1's entire fault state was a `byte flags[15]` array overwritten every cycle and lost on
> reset, so a returned unit carried no evidence of why it was returned.

### SWREQ-DIAG-0015 — Debounced confirmation

A fault **must** be confirmed only after it has been observed a configured number of consecutive times.

> **Rationale.** A single missed CAN frame is not a fault; a hundred consecutive missed frames is. Without
> debouncing, the store fills with transients and the real fault is buried.

### SWREQ-DIAG-0020 — Healing

A confirmed fault **must** clear only after a configured number of consecutive fault-free operation
cycles.

> **Rationale.** Symmetry with confirmation. A fault that clears on one good cycle flickers, and a
> flickering DTC is read as noise.

### SWREQ-DIAG-0025 — Freeze frames

Confirming a fault **must** capture a snapshot of the conditions at that moment.

> **Rationale.** "CAN timeout" says a fault occurred. "CAN timeout at 47 km/h, 71 V, 34 °C, 412 s after
> boot" is often enough to identify the cause without reproducing it.

### SWREQ-DIAG-0030 — Standard status semantics

DTC status **must** follow the ISO 14229 bit definitions.

> **Rationale.** So an engineer who knows UDS can read the status byte without a bespoke table. The
> transport is not UDS — see SWREQ-DIAG-0060 — but the semantics are worth keeping standard.

### SWREQ-DIAG-0032 — Bounded store with visible saturation

The trouble-code store **must** be bounded, and the fact that it is full **must** itself be reported.

> **Rationale.** A unit with more than a dozen distinct faults has a systemic problem the recorded DTCs
> already describe. What must not happen is silently dropping new ones — the gap has to be visible.

### SWREQ-DIAG-0040 — Remote readout

The trouble-code store **must** be readable, and clearable, over the telemetry channel.

> **Rationale.** Requiring physical access means faults are read only on units that have already been
> returned — the population whose faults are least informative.

### SWREQ-DIAG-0050 — Authenticated commands only

No diagnostic command that changes state **may** be accepted without authentication.

> **Rationale.** The command set includes clearing DTCs and writing configuration. v1 exposed an
> unauthenticated web server on the vehicle that could rewrite the odometer.

### SWREQ-DIAG-0060 — Reduced UDS over the telemetry channel

Diagnostic services **should** follow ISO 14229 in structure and status semantics, and need not follow it
in transport.

> **Rationale.** Full UDS assumes a low-latency request/response channel, which a cellular link is not.
> The value is in the semantics being standard, not the framing — and the deviation is documented rather
> than silently taken.

### SWREQ-DIAG-0068 — Correlated responses

Every command response **must** echo the request's sequence number.

> **Rationale.** Over a lossy link with retries, a response that cannot be matched to its request is
> ambiguous.

---

## 9. Safety and integrity

### SWREQ-SAF-0001 — No silent failure

No operation that can fail **may** return a value indistinguishable from success.

> **Rationale.** v1's `init_can()` returned `true` unconditionally with the hardware call commented out.
> The CAN controller was never started for an unknown length of time, and nothing in the system could
> have detected it.

### SWREQ-SAF-0002 — Checked return values

Every function that reports a status **must** be declared such that discarding its result is a compile
error, and a deliberate discard **must** be written explicitly.

> **Rationale.** `warn_unused_result` catches the mistake; an explicit `STD_DISCARD` makes the deliberate
> cases auditable. Both are needed: without the first a dropped status is invisible, and without the
> second the first gets suppressed wholesale.

### SWREQ-SAF-0004 — Per-task watchdog supervision

The ECU **must** supervise each cyclic task individually against a hardware watchdog, and a task that
stops reporting alive **must** cause a reset.

> **Rationale.** A single global watchdog any task may pet is satisfied by the healthiest task in the
> system — so a stalled acquisition task with a healthy connectivity task never trips it. v1's watchdog
> was worse than absent: `esp_task_wdt_init()` was called but no task was ever subscribed, so the
> initialisation appeared in the boot log as evidence of protection that did not exist.

### SWREQ-SAF-0005 — Alive counting has both bounds

Supervision **must** detect a task running too often as well as too rarely.

> **Rationale.** A task spinning at ten times its rate is as much a fault as one that has stopped: it
> starves everything below it and, on the acquisition task, corrupts the odometer's integration interval.
> An upper alive bound catches it; a "has it reported recently" check does not.

### SWREQ-SAF-0006 — Application code does not pet the watchdog

Only the supervision module **may** report alive to the hardware watchdog.

> **Rationale.** A task that pets the hardware directly defeats the deadline and program-flow checks
> layered on top, which is the usual way a watchdog ends up protecting nothing.

### SWREQ-SAF-0007 — One supported reset path

Resetting the ECU **must** go through the shutdown sequence, except where that sequence itself cannot run.

> **Rationale.** See SWREQ-SYS-0080. The raw reset remains available precisely for the case where the
> flush path depends on modules that do not exist — a failed task creation — and the distinction is worth
> making explicit rather than leaving both available interchangeably.

### SWREQ-SAF-0008 — Program-flow supervision

The order in which a task reaches its checkpoints **must** be checked against the configured graph.

> **Rationale.** A task that is alive and on time can still be taking a path it should not — skipping a
> stage, or repeating one. Alive counting alone cannot see that.

### SWREQ-SAF-0010 — Compile-time configuration checks

Configuration consistency **must** be enforced at compile time wherever it can be expressed as a static
assertion.

> **Rationale.** v1 assigned GPIO2 to both the built-in LED and the CAN chip select, and UART2 to both the
> RS485 bus and the modem. Both are static facts, both survived review, and both are now build errors.

### SWREQ-SAF-0011 — No dynamic allocation after startup

No heap allocation is permitted after startup completes.

> **Rationale.** v1 allocated an Arduino `String` per record and per field. The heap fragmented over hours
> until no single allocation succeeded while plenty of total memory remained free — which presents as an
> unrelated failure in whichever module happened to allocate next.

### SWREQ-SAF-0012 — Bounded loops and buffers

Every loop **must** have a bound that does not depend on external data, and every buffer write **must** be
bounded by the destination's capacity.

> **Rationale.** External data here means CAN frames, RS485 responses, NMEA sentences and MQTT payloads —
> four untrusted inputs, three of them from outside the vehicle.

### SWREQ-INT-0010 — Standard CRC profiles

Integrity checks **must** use the AUTOSAR CRC library profiles, complete rather than partially implemented.

> **Rationale.** A partial implementation of a standard interface is worse than a complete one: the next
> module needing an unimplemented profile adds a second, untested implementation beside the first.

### SWREQ-INT-0011 — Verified against published check values

Every CRC implementation **must** be verified against its profile's published check value.

> **Rationale.** The production code is table-driven and reflected, which is fast and easy to get subtly
> wrong. Agreement with a published constant is evidence; agreement with itself is not.

### SWREQ-INT-0012 — Independent reference model

CRC test vectors **must** be generated by an implementation independent of the production one.

> **Rationale.** `tools/crc_reference.py` computes bit-at-a-time — the slow, obvious way — and self-checks
> before emitting anything. Two independent implementations agreeing on six published constants is a much
> stronger claim than one implementation's output recorded as expected.

---

## 10. Position

### SWREQ-GNS-0001 — Checksum verification

An NMEA sentence failing its checksum **must** be discarded and counted.

> **Rationale.** A corrupted position would otherwise be recorded as fact, and position data has no
> plausibility check strong enough to catch a single corrupted field.

### SWREQ-GNS-0005 — Integer coordinates

Coordinates **must** be carried as degrees × 10⁷ in a signed 32-bit integer.

> **Rationale.** About 1.1 cm of resolution with no floating point, consistent with the rest of the
> project. The alternative — `double` — is what forced v1 to use a floating-point NMEA library.

### SWREQ-GNS-0008 — Round to nearest

The conversion from degrees-and-decimal-minutes **must** round to nearest, not toward zero.

> **Rationale.** Truncation biases every reading toward the equator and the prime meridian. That is
> one-directional, so it does not average out over a journey — it is a systematic position offset, not
> noise. This was a genuine bug in the first draft.

### SWREQ-GNS-0010 — Speed-based plausibility

A fix **must** be rejected if the implied speed from the previous fix exceeds a configured maximum.

> **Rationale.** v1 rejected any coordinate outside a hard-coded rectangle covering Pakistan, so the
> firmware would silently stop recording position if a vehicle were ever shipped elsewhere — and the
> failure would have looked like a receiver fault. A speed check is geography-independent.

### SWREQ-GNS-0014 — Platform-free parser

The NMEA parser **must** contain no platform dependency and no floating point.

> **Rationale.** It makes the parser host-testable against malformed input, which for four untrusted
> external formats is where the value is. TinyGPS++ is C++ and `Stream`-coupled, so it cannot be tested
> this way.

---

## 11. Analogue sensing

### SWREQ-SNS-0030 — Oversampled with outlier rejection

An analogue reading **must** be the average of several samples with the extremes discarded.

> **Rationale.** This part produces single-sample outliers near a switching supply. A plain average
> includes them; trimming the highest and lowest of seven rejects them without the cost of a median
> filter.

### SWREQ-SNS-0031 — Failed conversions are not averaged

A failed sample **must** fail the whole reading, not be averaged over the remaining samples.

> **Rationale.** A divider that has come loose reads plausibly low, and averaging over fewer samples
> hides it.

### SWREQ-SNS-0035 — Per-unit calibration

Scaling and offset **must** be per-unit calibration values, not compile-time constants.

> **Rationale.** The divider resistors are 1 % parts and the ADC has its own offset, so a firmware-wide
> constant is wrong for every individual board by a different amount.

### SWREQ-SNS-0038 — Physical units above the MCAL

Raw counts **must not** leave the MCAL; the layer above **must** expose millivolts.

> **Rationale.** A raw count is meaningless without the resolution, attenuation and divider ratio that
> produced it. Anything that carries it also has to carry those, or it is uninterpretable.

---

## 12. Operator indication

### SWREQ-HMI-0001 — Named indicators

Each indicator **must** be addressed by a name that says what it means, in its own namespace.

> **Rationale.** v1 addressed five LEDs through a shared integer index, and an off-by-one in one call
> site lit the wrong one. A technician reading the wrong indicator diagnoses the wrong fault, and
> nothing about the symptom suggests the indicator is the thing that is wrong.

### SWREQ-HMI-0004 — Indication is decoupled from state

Setting an indicator **must not** require the caller to know how it is driven.

> **Rationale.** Active-high against active-low, and steady against pulsed, are properties of the board
> and the pattern — not of the condition being indicated.

### SWREQ-HMI-0008 — Distinguishable states

The indicators **must** distinguish: acquiring, storing, bearer up, broker session established, and
scheduler alive.

> **Rationale.** These are the five things a technician at the vehicle needs to know, and they fail
> independently. A single "status" light cannot express which is wrong, which makes the first diagnostic
> step a laptop connection that this set avoids.

### SWREQ-HMI-0012 — Liveness is unambiguous

Exactly one indicator **must** mean "the scheduler is running", and it **must not** be used for anything
else.

> **Rationale.** It is the one signal that distinguishes "the firmware has stopped" from "a subsystem is
> down", and that distinction determines whether the next step is a power cycle or a diagnostic read.

---

## 13. Security

### SWREQ-SEC-0001 — No credentials in version control

No credential **may** be committed. Credentials **must** be supplied through a git-ignored header, with a
committed template showing its shape.

> **Rationale.** v1 committed a Firebase authentication token and a WiFi password as string literals in
> `include/config.h`. Both are in that repository's history permanently — removing them from the current
> revision does not remove them from history — and anyone with read access to the source had write access
> to the fleet's database. Any credential that was ever committed must be treated as compromised and
> rotated; see [09-operations.md](09-operations.md).

### SWREQ-SEC-0002 — No unauthenticated write path

No interface **may** modify the odometer, the configuration or the trouble-code store without
authentication.

> **Rationale.** v1 ran an unauthenticated web server on the vehicle that could rewrite the odometer
> reading. The whole configuration surface now arrives over the authenticated broker channel instead.

### SWREQ-SEC-0005 — Externally-supplied data is bounded at the boundary

Every input from outside the ECU **must** be length-checked before it is parsed.

> **Rationale.** Four such inputs: CAN, RS485, NMEA and MQTT. The MQTT payload is the one an attacker can
> choose freely, and v1 passed it straight into a fixed buffer.

### SWREQ-SEC-0010 — Verified updates

A firmware image **must** be rejected unless its integrity and origin are verified before the new slot is
marked bootable.

> **Rationale.** The update path is the most valuable thing to compromise on a connected device: it is the
> one that persists. The current shared-secret HMAC is weaker than asymmetric signing — every unit holds
> the same secret — and that limitation is recorded as an accepted risk in
> [04-safety-analysis.md](04-safety-analysis.md) rather than left unstated.

---

## Deliberate non-requirements

Recorded because the absence of each is a decision, and an undocumented absence reads as an oversight:

| Not required | Why |
|---|---|
| Real-time control of anything | The ECU observes and records. It actuates nothing, so no failure of it can affect vehicle behaviour. This is what keeps the whole unit outside any functional-safety scope. |
| Sub-second acquisition | The odometer integrates over the interval, so a longer interval costs resolution in the time series, not accuracy in the distance. 3 s was chosen for the RS485 bus's throughput. |
| Cell-level balancing | The packs' own BMS does this. The ECU reports imbalance and does not attempt to correct it. |
| Guaranteed delivery at the broker | Store-and-forward gives a stronger guarantee at the application level — see SWREQ-TEL-0050. |
| Local display or user interface | The vehicle has its own instrument cluster. This ECU is not in the driver's view. |
| Time synchronisation better than one second | Records are timestamped to the second and the acquisition period is 3 s. Finer synchronisation would be precision the data does not have. |
| Redundant speed source | Motor speed comes only from the motor controller. A cross-check against GNSS-derived distance over a long window would catch a gross error; it is recorded as accepted risk R3, not implemented. |

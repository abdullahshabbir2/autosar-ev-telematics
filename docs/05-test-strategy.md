# Test Strategy

**Audience:** engineers adding or reviewing tests, and anyone assessing how much of this firmware's
behaviour is actually verified. Assumes you have read [02-architecture.md](02-architecture.md).

---

## 1. The claim, and what backs it

150 tests across 8 suites, all passing, compiled with `-Werror` and eleven warning flags. That
number on its own is worth very little — it is possible to write 150 tests that verify nothing. What
matters is three properties of *how* they are built:

1. **They test the real implementation, not a reimplementation of it.** Every suite links the actual
   `src/**/*.c`. There is no mock of `Fee`, no simplified `Crc`, no test double standing in for
   `OdoSwc`. Doubles exist only at the platform boundary, below the code under test.
2. **They mechanically enforce the architecture.** `tools/run_native_tests.py` links *every*
   platform-independent source into *every* test binary. A module above the MCAL that reaches for
   hardware produces an undefined-reference error at link time. Layer discipline is therefore a
   build property, not a review convention.
3. **Their expected values are independently derived.** Every numeric constant in the suites was
   computed by a separate Python model, or taken from a published specification, before being written
   into a test. A test whose expected value came from running the code proves only that the code is
   deterministic.

That third point is the one most often skipped, so it gets its own section below.

---

## 2. What is tested where

```
                                         ┌─────────────────────────────┐
   Host tests (gcc, -Werror)             │  Not host-testable          │
   ─────────────────────────             │  ─────────────────          │
   Arithmetic, framing, protocol         │  Register access            │
   decode, state machines, integrity,    │  Library calls              │
   configuration consistency             │  RTOS task creation         │
                                         │                             │
   = every .c file                       │  = every _Esp32.cpp file    │
   = ~90% of the source                  │  = ~10% of the source       │
                                         └─────────────────────────────┘
```

The split is not a compromise; it is the reason the architecture is shaped the way it is. Every
module that touches hardware is divided into a pure core (`<M>.c`) and a platform leaf
(`<M>_Esp32.cpp`), with `<M>_Platform.h` as the contract between them. The core is host-compiled and
tested against the real code; the leaf is kept as thin as it can be, because whatever is in it is
verified only on hardware.

**The working rule when adding code: if it can be computed, it goes in the `.c` file.** A platform
leaf containing a calculation is a calculation that is not tested. `TimeAbs_Esp32.cpp` reads four BCD
registers and hands them to `TimeAbs.c`; the calendar arithmetic, the leap-year rules and the epoch
conversion are all on the tested side.

### The suites

| Suite | Tests | What it establishes |
|---|---|---|
| `test_crc` | 12 | All six AUTOSAR CRC profiles against their published check values |
| `test_can` | 15 | MCP2515 identifier encoding, register sequences, frame decode |
| `test_rs485` | 28 | Battery frame build and parse, CRC, timeouts, malformed input |
| `test_fee` | 18 | Crash-safe commit and garbage collection under fault injection |
| `test_odo` | 18 | Integer distance accumulation, plausibility gates, persistence |
| `test_diag` | 21 | Dem debouncing, healing, freeze frames; WdgM supervision |
| `test_sensors` | 21 | CAN signal decode and freshness, NMEA parsing |
| `test_com` | 17 | Record serialisation, chunk planning, backfill parsing |

---

## 3. Independently derived expected values

A test that asserts the code does what the code does is worthless. Every non-obvious constant in
these suites was derived from an independent source first. Three examples, each of which caught a
real error.

### CRC check values

`tools/crc_reference.py` implements all six profiles bit-at-a-time — the slow, obvious way — and
self-checks against the published `check` constants before emitting anything:

| Profile | Check value over "123456789" |
|---|---|
| CRC8 (SAE J1850) | `0x4B` |
| CRC8H2F | `0xDF` |
| CRC16 (CCITT-FALSE) | `0x29B1` |
| CRC16/ARC | `0xBB3D` |
| CRC32 (Ethernet) | `0xCBF43926` |
| CRC32P4 (CRC-32/AUTOSAR) | `0x1697D06A` |

The production code uses table-driven and reflected implementations, which are fast and easy to get
subtly wrong. Two independent implementations agreeing on six published constants is evidence; one
implementation agreeing with itself is not.

### MCP2515 identifier encoding

I wrote the expected register values for an extended CAN identifier into the test by hand, and they
were wrong — `SIDL=0xC9, EID8=0x00` where the correct encoding is `SIDL=0xC8, EID8=0x10`. The driver
was right and the test was wrong.

That is the useful outcome: the error surfaced because the value was checked against the datasheet's
bit layout independently rather than lifted from the driver's output. The derivation is now in the
test as a comment, so the next person to touch it can check it the same way instead of trusting it.

### Wheel circumference

The odometer's reference circumference for a 19-inch wheel is 1 516 133 µm
(19 × 25.4 × π = 1516.1326 mm). My first draft of the test used 1 516 195 µm, which is wrong in the
fifth significant figure — and the test passed anyway, because the implementation's tolerance
absorbed it.

Fixing the reference let the test measure what it was supposed to: over a simulated 1.26 km journey
the implementation is accurate to **0.85 mm**, with **0.046 mm** of total rounding drift. Those two
figures are the actual verification of SWREQ-ODO-0001 and SWREQ-ODO-0003, and neither would have
been visible against a wrong reference.

### v1's own frames as golden vectors

`tools/rs485_reference.py` reproduces the `DUMMY_DATA` response frames embedded in v1's source. Two
of the three CRCs match exactly, which cross-validates the frame layout against code that
demonstrably talked to real hardware — a far stronger check than any frame I could invent.

The third does not match, and that is a finding rather than a problem with the model:
`Dummy_BATT_response` carries CRC `0x2B76` where the correct value is `0xF831`. It was never caught
because v1's `readResponse` returned `true` unconditionally on the dummy-data path. A wrong constant
sat in the source, inside a function whose job was to detect exactly that kind of error, for the
life of the product.

---

## 4. Fault injection

Testing that the happy path works is the easy half. These are the cases where the interesting
behaviour lives.

### Crash-safe storage

`test_fee` interrupts a write at each step of the commit sequence and asserts that what survives is
either the old value or the new one — never a mixture, never nothing. The stubbed flash driver can
be told to fail a write at an exact address:

```c
Stub_Fls_FailWriteAtAddress(recordAddress + FEE_HEADER_SIZE);
```

**That function used to be `...CoveringAddress`, and the rename is the point.** Range matching made
the injection hit the *header* write — which spans the commit byte — so the test was exercising a
different failure than the one it claimed to. The test passed, and it was testing the wrong thing.
Exact start-address matching is what made the injection land where intended.

A second case in the same suite: a corruption mask of `& 0xFE` applied to a byte that was already
even changed nothing, so the "detects corruption" test was asserting that uncorrupted data reads
back correctly. It now uses `value & (value - 1)`, which clears the lowest *set* bit and therefore
always changes something.

Both were tests that passed while verifying nothing. They are worth recording because they are the
failure mode that a test count cannot show.

### A real bug the fault injection found

Fee counted an uncommitted record (state `0xFF`) as evidence that a block existed. So an interrupted
*first* write to a block returned `E_CRC_FAIL` — "your data is damaged" — instead of `E_NOT_FOUND` —
"this block has never been written".

The consequence: every brand-new unit whose first odometer write was interrupted would report a
storage fault and refuse to use its configured default. The fix moves `sawBlock = TRUE` into the
valid and invalid branches only, so an uncommitted record leaves the block looking as unwritten as it
actually is.

### Malformed external input

Every parser is given input designed to break it, because all four of these inputs come from outside
the ECU and three from outside the vehicle:

- **RS485** — truncated frames, wrong address, wrong length, valid frame with one bit flipped, a
  response arriving after its timeout, and a reply from the *previous* pack (which is what v1's
  misuse of `flush()` allowed to be decoded as the current one).
- **CAN** — every reserved bit combination, and identifiers at each boundary of the 11-bit and
  29-bit ranges. `Can_EncodeIdentifier`/`Can_DecodeIdentifier` are exposed specifically so this can
  be exhaustive rather than sampled.
- **NMEA** — sentences with a bad checksum, missing fields, extra fields, a null fix, and
  coordinates at the equator, the prime meridian and the date line.
- **MQTT backfill** — requests naming a range larger than the maximum, a negative range, a range
  that would overflow the chunk arithmetic, and a zero-length request.

That last one matters: `Com_ComputeChunkPlan` is tested with `totalRecords == 0` because the obvious
expression `(total - 1) / max + 1` underflows to a loop bound of about four billion. The `size_t`
underflow is not hypothetical — it was in the first draft.

---

## 5. Timing and wrap-around

Two classes of defect that only appear after weeks of runtime, so they have to be tested by
construction rather than by waiting.

**Counter wrap.** `Gpt_HasElapsed` is tested across the 32-bit millisecond wrap at 49.7 days by
driving the stub's clock directly to the boundary. The stub lets a test set the monotonic time to any
value, so the 49.7-day case takes microseconds to verify.

**Trapezoidal integration of a real profile.** `test_odo` drives a synthetic speed profile —
acceleration, cruise, braking, stationary — through the odometer and compares the accumulated
distance against a closed-form integral of the same profile.

One test in this suite needed correcting, and the correction is instructive. The "parked vehicle
accumulates no distance" case initially failed, and the implementation was right: the trapezoidal
rule correctly integrates the deceleration interval from 3000 rpm to 0, and that interval *is* real
distance. The test had to bring the vehicle to rest before taking its baseline. A test that had been
"fixed" by special-casing the last interval would have introduced exactly the error the trapezoidal
rule exists to avoid.

---

## 6. Compilation as verification

The host build is itself a test, and several of the flags earn their place:

```
-std=c11 -O1 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
-Wcast-qual -Wstrict-prototypes -Wswitch-enum -Wfloat-equal -Wdouble-promotion
-fno-common -DUNIT_TEST=1
```

| Flag | What it catches here |
|---|---|
| `-Wconversion` / `-Wsign-conversion` | Implicit narrowing in fixed-point arithmetic. The odometer's Q32 factor makes this the highest-value flag in the list. |
| `-Wswitch-enum` | A new enumerator added without handling it everywhere. Adding a Dem event or a DTC is exactly this. |
| `-Wfloat-equal` | Float comparison in code that should have no floats. It fires nowhere, which is the intended result. |
| `-Wdouble-promotion` | An accidental `double` — costly in both code size and time on this part. |
| `-Wshadow` | A local shadowing a module-static, which in a `_Cfg`-driven module is easy to do and hard to see. |
| `-fno-common` | A duplicate definition across translation units, rather than the linker silently merging them. |

**Compile-time configuration checks.** `_Static_assert` enforces what can be stated statically, and
three of these caught real errors during development:

- `Ecu_PinMap.h` detects a double-assigned GPIO by comparing the bitwise OR of the pin bits against
  their arithmetic sum — a duplicated pin makes the two differ. It also asserts that no signal lands
  on the internal flash pins, and that no input-only pin is used as an output.
- `SchM_Cfg.h` asserts every task's budget is below its period, and that supervision outranks
  everything it supervises.
- `Fee_Cfg.h` asserts every block fits its configured length. This one fired on three under-sized
  blocks, which is the assertion doing its job before any test ran.

---

## 7. What is not tested, and what covers it instead

Stated plainly, because an unstated gap reads as a claim of coverage.

| Not covered by host tests | Why | What covers it |
|---|---|---|
| Register access in `*_Esp32.cpp` | Needs the silicon | Hardware bring-up; kept minimal so there is little to get wrong |
| FreeRTOS scheduling behaviour | Needs the RTOS | `SchM_GetTaskStats` reports actual periods and overruns in the health record |
| Real SPI bus contention timing | Needs two live devices | `Spi_GetStatistics` counts contentions and lock timeouts on the target |
| SD card wear and failure | Needs years, or a worn card | Verify-after-write plus `Fls_GetStatistics` erase counts |
| Modem AT command dialogue | TinyGSM's responsibility | Library's own tests; our state machine is tested against a stub |
| Actual RF behaviour | Needs a radio chamber | Out of scope for firmware verification |
| Long-run heap fragmentation | Needs hours of runtime | `Mcu_GetHeapInfo` reports the largest contiguous block, not just total free |

The pattern: where a property cannot be verified before deployment, the firmware is instrumented to
report it from the field. That is why the health record contains task timing, stack high-water marks,
SPI contention counts and flash erase totals — each of those is a test that runs continuously on
every unit instead of once on a bench.

---

## 8. Running the tests

```bash
python tools/run_native_tests.py              # all suites
python tools/run_native_tests.py test_odo     # one suite
```

Requires a host C compiler (`gcc` or `clang`, or `$CC`) and Unity, which PlatformIO provides at
`~/.platformio/lib/Unity/src`.

**Not through PlatformIO's test runner**, and that is deliberate. PlatformIO would satisfy each
suite's references from the framework, so a platform dependency leaking above the MCAL would link
successfully and go unnoticed. Compiling directly and linking every source into every suite is what
makes the leak a build failure. The `[env:native]` environment in `platformio.ini` exists only to
fail with a pointer to this script.

### Adding a suite

1. `test/test_<name>/test_<name>.c`, with `setUp`/`tearDown` and a `main` running the cases. The
   runner discovers it; no build-system edit.
2. Call each module's `DeInit` in `setUp`. State leaking between cases makes a test pass or fail
   depending on execution order — it happened during development and cost an afternoon before
   `Can_DeInit` and `Rs485If_ResetState` were added for exactly this.
3. Derive expected values independently, and put the derivation in a comment. If you cannot state
   where a number came from, the test is asserting the implementation's own behaviour back at itself.
4. Reference the requirement the case verifies, so [06-traceability.md](06-traceability.md) can pick
   it up.

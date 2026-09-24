# ADR-0006 — Split every hardware-touching module into a pure core and a platform leaf

**Status:** accepted
**Date:** 2026-02-24

---

## Context

v1 could not be tested off the target. Every module included `Arduino.h`, so nothing compiled without
the framework, and the only way to exercise any logic was to flash a board and observe it.

The consequence was not that testing was merely inconvenient. It was that the parts most worth testing
were the parts hardest to reach:

- **Arithmetic.** The odometer's distance calculation had never been checked against a computed
  reference. It was the number the product existed for.
- **Protocol framing.** The RS485 CRC was never verified — one of the three response frames embedded in
  v1's own source carries an incorrect CRC and nothing noticed, because `readResponse` returned `true`
  unconditionally.
- **Error paths.** A brown-out during a flash write, a truncated response, a card that stops
  acknowledging. These are unreachable on a bench and are precisely the ones that matter in a vehicle.

Layering alone (see [ADR-0001](0001-autosar-layering.md)) puts the hardware boundary at the MCAL, which
is necessary but not sufficient: a module *in* the MCAL still contains logic worth testing, and a module
above it can still accidentally reach for hardware.

## Options considered

**Test on target, with a harness.** Flash a test image and report over UART. Tests the real thing on the
real silicon, which is genuinely valuable — but each cycle is tens of seconds, fault injection needs
hardware to inject faults with, and the 49.7-day counter wrap cannot be reached at all.

**Mock the MCAL headers at link time, keeping modules as they are.** Provide a `Stub_Mcal.c` supplying
the MCAL symbols and link test binaries against it. Almost the chosen design, and it is half of it —
but on its own it does not stop a module including `Arduino.h` directly, so the discipline is a
convention again.

**Split each module explicitly: pure core plus platform leaf.** Chosen.

## Decision

Every module that touches hardware is divided in two:

| File | Compiled for | Contains |
|---|---|---|
| `<M>.c` | host **and** target | Everything derived: arithmetic, state machines, framing, validation, policy |
| `<M>_Esp32.cpp` | target only | Register access and library calls, and nothing else |
| `<M>_Platform.h` | both | The contract between them |
| `test/support/Stub_*.c` | host only | The host's implementation of that contract |

The working rule when adding code: **if it can be computed, it goes in the `.c` file.** A platform leaf
containing a calculation is a calculation that is not tested.

Worked example — `TimeAbs`, the wall-clock abstraction:

- `TimeAbs_Esp32.cpp` reads four BCD registers over I²C and converts BCD to binary. That is all.
- `TimeAbs.c` holds the calendar arithmetic, the leap-year rules, the epoch conversion and the
  plausibility window — and the host suite checks them against known dates, including the 2100
  non-leap year that a naive four-year rule gets wrong.

The ratio across the project is roughly 90 % of source in the pure cores, 10 % in the leaves.

### The part that makes it stick

`tools/run_native_tests.py` links **every** `src/**/*.c` into **every** test binary. So a module above
the MCAL that reaches for a platform symbol produces an undefined-reference error at link time.

That is the crux of this decision. Layer discipline is not a review convention here; it is a build
property. A reviewer can miss an `#include <Arduino.h>` in a diff. The linker cannot.

A second, narrower check in CI greps every `.c` file for platform symbols — `esp_*`, `xTask*`,
`digitalWrite`, `millis` and the rest — so the intent is stated as well as enforced.

This is also why the tests are **not** run through PlatformIO's test runner. PlatformIO would satisfy
each suite's references from the framework, so a leak would link successfully and go unnoticed. The
enforcement depends on the framework being absent.

## Consequences

**What it costs.**

- Two files per hardware-touching module instead of one, plus a contract header. Thirteen platform
  leaves across the project.
- An interface boundary that would otherwise not exist. `TimeAbs_PlatformRtcRead` exists only because
  the split does.
- Some duplication in the doubles: `Stub_Mcal.c` is about 1200 lines, all of which is test support that
  has to be maintained.

**What it buys.**

- 333 host tests across 16 suites, running in about 5 seconds each, exercising the real implementation
  rather than a reimplementation of it. No mock of `Fee`, no simplified `Crc`, no double standing in for
  `OdoSwc` — doubles exist only at the platform boundary, below the code under test.
- Fault injection that is otherwise unreachable: a flash write that fails at an exact address, a
  response truncated mid-frame, a counter driven to the 49.7-day wrap in microseconds.
- Defects actually found. Writing the suites against this structure surfaced a silent-failure bug in
  `NvM` — a failed write left the block clean, so `NvM_WriteImmediate` reported success without writing
  and `NvM_WriteAll` skipped it at shutdown — and a media-recovery gap in `Fee`, where a partially
  programmed slot blocked every retry and every scan for the rest of the session. Both would have lost
  odometer data in the field, and neither is reachable without injecting a flash failure.

**What is not covered, stated plainly.** The 10 % in the platform leaves is verified only on hardware.
That is why the leaves are kept as thin as they can be, and why
[05-test-strategy.md](../05-test-strategy.md) §7 lists what covers each gap instead — mostly
instrumentation that reports from the field, which is a test that runs continuously on every unit rather
than once on a bench.

## When this would be the wrong decision

If a module's logic were genuinely trivial — a leaf that only forwards, with nothing derived — the split
adds a boundary for no return. `Dio` is close to this line: its pure part is a bit test against the pin
map's own mask, and the argument for splitting it is consistency with the other MCAL modules rather than
coverage.

It would also be wrong where the platform API *is* the logic. A driver whose whole behaviour is a
documented register sequence with no derived state has nothing to put in the core, and pretending
otherwise produces a `.c` file that exists to satisfy a pattern.

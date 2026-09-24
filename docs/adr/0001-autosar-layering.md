# ADR-0001 — Adopt AUTOSAR Classic layering

**Status:** accepted
**Date:** 2026-02-14

---

## Context

v1 was a 719-line `main.cpp` plus twelve modules that each reached directly for hardware. Any module
could call `digitalWrite`, open a `HardwareSerial`, or touch the SD library. Shared state was a
`byte flags[15]` array.

The consequences were not stylistic. Three of them mattered:

- **Nothing could be tested off the target.** Every module pulled in `Arduino.h`, so the only way to
  exercise any logic was to flash a board and watch it. The arithmetic that produces the odometer
  reading — the one number the product exists for — had never been checked against a computed
  reference.
- **Hardware conflicts were invisible.** GPIO2 was both the built-in LED and the CAN chip select.
  UART2 was claimed by both the RS485 bus and the modem. Both are static facts about the
  configuration, and both survived review because no single file was responsible for either.
- **Failure handling was inconsistent because there was nowhere to put it.** Some functions returned
  `bool`, some returned `void` and printed, one returned `true` unconditionally with the hardware
  call commented out. There was no shared vocabulary for "this failed" and no place a fault could be
  recorded.

The firmware had to be restructured. The question was what to restructure it *into*.

## Options considered

**A flat set of modules with a coding convention.** Cheapest. Solves none of the three problems above:
a convention that hardware access lives in certain files is exactly the kind of thing that erodes, and
nothing would detect the erosion.

**A two-layer split — "drivers" and "application".** Enough to make the application testable, which is
most of the value. But it leaves no place for the cross-cutting services this ECU needs — diagnostics,
supervision, non-volatile management — so those end up in whichever layer touched them first, which is
how v1's diagnostics ended up as a byte array in the application.

**AUTOSAR Classic layering.** MCAL → ECU abstraction → services → RTE → application. Chosen.

## Decision

Adopt AUTOSAR Classic layering, with AUTOSAR's naming, its `Std_ReturnType` conventions, and its
module decomposition where a standard module fits.

Five reasons, in the order they actually influenced the decision:

1. **It puts the testability boundary in a defensible place.** The MCAL is the only layer that touches
   hardware, so replacing it with test doubles makes everything above it host-testable. That is not
   unique to AUTOSAR, but AUTOSAR says exactly where the line goes and what crosses it, which removes
   the argument.
2. **It has a place for every cross-cutting concern this ECU has.** Det for contract violations, Dem
   for diagnostic events, WdgM for supervision, NvM/Fee for persistence, EcuM for startup. Each of
   those existed in v1 as an ad-hoc fragment; each now has a home with a defined interface.
3. **The conventions carry information.** `Std_ReturnType` with `CHECK_RETURN` makes a dropped status
   a compile error. `<M>_Cfg.h` makes configuration separable from logic. `<M>_Init`/`<M>_DeInit` gives
   every module a lifecycle, which is what makes test isolation possible at all.
4. **It is the vocabulary of the industry this ECU belongs to.** An automotive engineer reading
   `CanIf`, `Dem`, `NvM` knows what each is for before opening it. For a project that will be read by
   people who did not write it, that is worth a great deal.
5. **The structure is checkable.** Layer rules can be enforced — the host test runner links every
   platform-independent source into every suite, so a leak above the MCAL is a link error rather than
   a review comment.

## Consequences

**Accepted costs.**

- More files. 28 platform-independent sources plus 13 platform leaves, against v1's 13 files. A module
  with one function still gets a header, a configuration header and an implementation.
- Indirection on paths that do not need it. Reading the battery voltage goes through `Adc` and
  `IoHwAb` where v1 called `analogRead`. The layering is what makes the calibration and the
  oversampling testable, but on the happy path it is two extra calls.
- Terminology that has to be learned. `Fee` is not an obvious name for flash EEPROM emulation.

**What it bought.** 333 host tests across 16 suites, all compiled with `-Werror` and eleven warning
flags, exercising the real implementation rather than a reimplementation of it. Four hardware conflicts
turned into build errors. A diagnostic record that survives a power cycle. And, measurably, defects
found: the test suites written against this structure found a silent-failure bug in `NvM` and a
media-recovery gap in `Fee`, both of which would have lost odometer data in the field.

## When this would be the wrong decision

If the ECU were genuinely trivial — read one sensor, write one output, no persistence, no diagnostics —
the layering would be pure overhead. The threshold is roughly whether the thing has state that must
survive a power cycle. This one does, and that state is the product.

It would also be wrong if the team had no interest in the conventions. The value in point 4 above
depends on the reader recognising the names; against a reader who does not, the same structure with
plainer names would be better.

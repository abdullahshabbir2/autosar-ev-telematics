# ADR-0003 — Omit MemIf; NvM calls Fee directly

**Status:** accepted
**Date:** 2026-02-18

---

## Context

The AUTOSAR memory stack is four layers:

```
  NvM     blocks, CRC, defaults, RAM mirrors
  MemIf   dispatch to one of several memory abstraction modules
  Fee     flash EEPROM emulation        Ea   EEPROM abstraction
  Fls     flash driver                  Eep  external EEPROM driver
```

MemIf exists to let NvM address several devices without knowing which is which. A block is configured
with a device index; MemIf routes the call. That is genuinely useful on an ECU with internal flash for
some blocks and an external EEPROM for others, or where a supplier's NvM must work against a memory
stack chosen later.

This ECU has one storage device: an 8 KiB partition of the ESP32's internal flash, which `Fee` manages
and `Fls` drives. There is no external EEPROM, no second flash, and no prospect of either — the board
is designed and the partition table is fixed.

## Options considered

**Implement MemIf faithfully.** A module whose every function is `return Fee_X(...)`, with a device
index parameter that has exactly one legal value. Roughly 150 lines of pass-through plus a header, a
configuration header, and its share of the test suite.

**Implement MemIf as a set of macros.** `#define MemIf_Write(dev, blk, buf) Fee_WriteBlock(blk, buf)`.
Cheaper, and arguably worse: it has the appearance of the layer without the substance, so a reader
would expect a routing capability that does not exist, and the unused `dev` parameter at every call
site invites someone to pass something meaningful to it.

**Omit it and call Fee directly.** Chosen.

## Decision

`NvM` calls `Fee` directly. MemIf is not implemented.

The reasoning is that MemIf's only responsibility is dispatch, and with one target there is nothing to
dispatch. A layer that routes to a single destination does not abstract anything; it adds a name.

The dependency is explicit and narrow: `NvM.c` includes `services/Fee/Fee.h` and uses
`Fee_ReadBlock`, `Fee_WriteBlock` and `Fee_BlockIdType`. Block identity is mapped in one place — the
descriptor table at the top of `NvM.c` pairs each `NVM_BLOCK_*` with its `FEE_BLOCK_*` — so the coupling
is a single table rather than scattered through the code.

## Consequences

**What it costs.**

- A second storage device would require either adding MemIf or editing `NvM`. That edit is bounded and
  visible: the descriptor table would gain a device column and the three call sites in `NvM_Commit`,
  `NvM_LoadBlock` and `NvM_Init` would need routing. It is an afternoon, and it is the afternoon MemIf
  would have cost anyway.
- The stack is not conformant. Anyone comparing against the AUTOSAR specification will notice the gap,
  which is why it is recorded here rather than left to be discovered.

**What it buys.**

- One fewer layer between a write and the media, on the path that persists the odometer — which is both
  the most important path in the firmware and the one most often traced by hand when something is wrong.
- No pass-through module to keep in step. A layer that only forwards still has to be updated whenever
  the interface below it changes, and still has to be tested, and every one of those tests asserts that
  forwarding forwards.

## Related

The same reasoning was applied to two other standard modules, and the outcomes differed:

- **BswM** (basic software mode manager) is also omitted. Its job is arbitrating mode requests from
  several sources; this ECU's modes are decided in one place by `EcuM` and `ComM`, so there is nothing
  to arbitrate.
- **Det** is *not* omitted even though it could be, and [ADR-0004](0004-keep-det-enabled-in-production.md)
  covers why. The difference is that Det does something — deduplication, counting, recording — rather
  than forwarding.

The test that distinguishes them: if the module's implementation would be entirely `return
Other_Thing(...)`, it is a name and not a layer.

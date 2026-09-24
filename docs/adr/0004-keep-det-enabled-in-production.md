# ADR-0004 — Ship with Det enabled

**Status:** accepted
**Date:** 2026-02-20

---

## Context

Det is AUTOSAR's Default Error Tracer. Modules report contract violations to it: a NULL pointer, an
out-of-range index, an API called before `Init`. Each report carries module, instance, API and error
identifiers.

The convention is to compile it out of production. Every module has a `<M>_DEV_ERROR_DETECT` switch, and
the expectation is that development builds set it `STD_ON` and production builds set it `STD_OFF`. The
reasoning is that a development error is a defect that should have been found before release, so by
release there are none left to report, and the checks are dead weight.

That reasoning has a premise: that the checks only catch things testing would have caught. It is worth
examining rather than assuming.

## Options considered

**Compile Det out in production, as convention suggests.** Saves the code and the cycles. Means a
contract violation in the field either does nothing observable or corrupts something, with no record
either way.

**Keep the checks, drop the reporting.** Each check still guards its function — a NULL pointer still
returns `E_NOT_OK` instead of dereferencing — but nothing is recorded. Half the cost for a fraction of
the value: the ECU survives the violation and nobody ever learns it happened.

**Keep Det fully enabled.** Chosen.

## Decision

`<M>_DEV_ERROR_DETECT` is `STD_ON` in every module, in every build variant.

Four reasons.

**1. The premise does not hold for this ECU.** A development error here is not only a coding mistake. A
misconfigured unit — wrong calibration, a block whose stored length disagrees with the firmware's, a
provisioning step skipped — produces exactly the same reports, and those conditions arise after release
by definition. Compiling out the reporting would hide the class of fault most likely to be seen in
service.

**2. The cost is measured, not assumed.** The checks and the report store together account for about
3 KB of flash and 1.2 KB of RAM. Against 42.6 % flash and 17.6 % RAM usage, that is 0.15 % and 0.4 %.
On the acquisition path the checks are a handful of comparisons against a 2400 ms budget. There is no
trade being made here — the resource argument for removing it simply does not apply at these margins.

**3. Reports are bounded, so a fault storm cannot do damage.** Det deduplicates by (module, instance,
API, error) and counts repeats. A violation in the 10 ms scheduler tick produces one entry with a
count, not a hundred a second. The store is fixed-size and its saturation is itself reported, so the
worst case is bounded and visible.

**4. It is what makes a returned unit diagnosable.** The report store is published in the health record
and readable over the diagnostic channel. v1's entire fault state was a `byte flags[15]` array that was
overwritten every cycle and lost on reset, so a returned unit carried no evidence of why it was
returned — which meant the fault recurred on the replacement. A unit that can say "module 91, API 0x20,
E_PARAM_POINTER, 412 times" has answered the question before anyone opens the case.

## Consequences

**What it costs.** ~3 KB flash, ~1.2 KB RAM, a few cycles per guarded call. A reviewer expecting the
convention will notice the deviation, which is why this record exists.

**What it buys.** Contract violations are visible in the field rather than only in testing, and the
distinction between a development error and a runtime error is preserved in the field data —
`Det_ReportError` against `Det_ReportRuntimeError` — so the two populations can be assessed separately.
A unit reporting development errors has a configuration or integration problem; one reporting only
runtime errors is meeting its contracts and the world is not cooperating. That is a genuinely useful
split and it is only available if both are recorded.

**Not affected.** `Det_Panic` remains the one path that resets deliberately, and it is reached only from
conditions where continuing would corrupt persistent state. Enabling Det does not make the ECU more
likely to reset; reports are recorded and execution continues.

## When this would be the wrong decision

On a part where 3 KB of flash is 20 % of the budget rather than 0.15 %. The decision follows from the
margin, not from a principle, and it should be revisited if the image ever approaches the OTA slot limit
— at which point the honest move is to reduce `DET_REPORT_STORE_SIZE` rather than to remove the checks,
since the checks are what prevent the corruption and the store is only what records it.

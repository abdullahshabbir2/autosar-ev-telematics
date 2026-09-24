# Coding Standard

**Audience:** anyone writing or reviewing code in this repository.

Every rule below has a defect attached. A rule whose cost is visible and whose benefit is not gets
dropped the first time it is inconvenient, so each one names what it prevents — and where that is a
specific v1 failure, it says so.

Rules are numbered `CS-<area>-<nn>` so a review comment can cite one.

---

## How this relates to MISRA

MISRA C:2012 is the reference, not the requirement. The rules here are the subset that earns its place
on this project, plus several MISRA does not cover.

Deviations from MISRA that are taken deliberately:

| MISRA rule | Deviation | Why |
|---|---|---|
| 21.6 — no `<stdio.h>` | `snprintf` is used for record serialisation | The alternative is a hand-written formatter, which is more code to get wrong for no benefit. Bounded form only; see CS-BUF-02 |
| 8.7 — objects at block scope where possible | Large buffers are module-static | A 3 KiB buffer on a 6 KiB task stack overflows. Module-static makes the cost visible at link time |
| 15.5 — single point of exit | Early returns on argument validation | A guard clause at the top is clearer than nesting the whole body. Applies to validation only, not to logic |
| 11.4 — no integer/pointer conversion | Used in the MCAL for register access | Unavoidable at the hardware boundary, and confined to the platform leaves |

Static analysis runs cppcheck with a MISRA-informed rule set over the platform-independent sources only.
The platform leaves pull in the Arduino core, which cppcheck cannot parse usefully without the whole
framework include tree — and the result would be pages of findings in code this project does not own,
which is how an analysis gate stops being read.

---

## CS-ERR — Error handling

### CS-ERR-01 — Every fallible operation returns `Std_ReturnType`

No `bool`, no sentinel `-1`, no `void` with a side channel.

> **Prevents:** v1 used all three inconsistently, so a caller had to know each function's private
> convention. One of them — `init_can()` — returned `true` unconditionally with the hardware call
> commented out.

### CS-ERR-02 — Every status-returning function is `CHECK_RETURN`

Expands to `__attribute__((warn_unused_result))`. Discarding the result is a compile error.

### CS-ERR-03 — A deliberate discard uses `STD_DISCARD`

```c
STD_DISCARD(Dem_ReportErrorStatus(DEM_EVENT_CAN_TIMEOUT, DEM_EVENT_STATUS_FAILED));
```

A bare `(void)` cast does **not** silence `warn_unused_result` in GCC — that is real compiler behaviour,
not a style preference. `STD_DISCARD` binds the value to a named `const` and discards that.

> **Prevents:** the only other way to suppress the warning is to disable it wholesale, which suppresses
> the accidental cases too. `grep -c STD_DISCARD` counts every deliberate drop, and each is a decision a
> reviewer can look at.

### CS-ERR-04 — Distinguish "absent" from "damaged"

`E_NOT_FOUND` and `E_CRC_FAIL` are different answers calling for opposite responses: use the configured
default, or fall back to redundancy and raise a fault.

> **Prevents:** a real bug during development. Fee counted an uncommitted record as evidence a block
> existed, so an interrupted first write returned `E_CRC_FAIL`. Every brand-new unit would have reported
> a storage fault on its first boot.

### CS-ERR-05 — A failed operation leaves its work retryable

If a write fails, the data stays marked as owing a write.

> **Prevents:** a real bug this project's own tests found. `NvM_Commit` cleared the dirty flag on
> failure, so `NvM_WriteImmediate` reported success for a block it considered clean, `NvM_WriteAll`
> skipped it at shutdown, and one transient flash failure discarded up to 100 m of odometer distance
> with no indication and no way to recover it.

### CS-ERR-06 — Development errors and runtime errors are reported separately

`Det_ReportError` for a violated contract; `Det_ReportRuntimeError` for a correct call the world did not
cooperate with.

> **Why:** the two populations need different responses. A unit reporting development errors has a
> configuration or integration problem; one reporting only runtime errors is meeting its contracts.
> Conflating them makes neither assessable.

---

## CS-BUF — Buffers and memory

### CS-BUF-01 — A writable buffer parameter is accompanied by its capacity

```c
Std_ReturnType Rs485If_BuildRequest(uint8 *frame, uint8 frameSize, ...);
```

The function checks it and returns `E_NO_SPACE` rather than writing past the end.

> **Prevents:** v1's `sendRequest` took no capacity. One call site passed an array two bytes short, and
> the stack overflow corrupted whatever was adjacent — a fault that manifests nowhere near its cause.

### CS-BUF-02 — Bounded string functions only

`snprintf`, `strncpy`, `strncat`, `memcpy` with an explicit length. Never `sprintf`, `strcpy`, `strcat`,
`gets`.

After `strncpy`, terminate explicitly: it does not when the source fills the destination.

### CS-BUF-03 — No dynamic allocation after startup

No `malloc`, no `new`, no Arduino `String`.

> **Prevents:** v1 allocated a `String` per record and per field. The heap fragmented over hours until
> no single allocation succeeded while plenty of total memory remained free — which presents as an
> unrelated failure in whichever module happened to allocate next. This is why `Mcu_GetHeapInfo` reports
> the largest contiguous block: total free tells you nothing about fragmentation.

### CS-BUF-04 — A buffer above 256 bytes is module-static, not automatic

With its size derived in the module's `_Cfg.h`.

> **Why:** a stack overflow on this part corrupts whatever is adjacent rather than trapping, and the
> resulting fault appears nowhere near its cause. Module-static puts the cost in the link map where it
> can be seen.

### CS-BUF-05 — Every loop bound is independent of external data

External data here means CAN frames, RS485 responses, NMEA sentences and MQTT payloads — four untrusted
inputs, three from outside the vehicle.

> **Prevents:** v1 would read an unbounded number of records into the heap on a broker request — a
> denial of service reachable by anyone who could publish to the topic.

---

## CS-TIME — Time

### CS-TIME-01 — Never compare two timestamps directly

```c
if (Gpt_HasElapsed(since, timeoutMs)) { ... }     /* correct   */
if ((now - since) >= timeoutMs) { ... }           /* forbidden */
if (now > deadline) { ... }                       /* forbidden */
```

> **Prevents:** a 32-bit millisecond counter wraps after 49.7 days. Direct comparison is correct until
> then and silently wrong after — which no test short of seven weeks would catch. `Gpt_HasElapsed` uses
> modular arithmetic and is verified across the boundary.

### CS-TIME-02 — Timeouts use monotonic time; timestamps use wall-clock time

Never the reverse.

> **Prevents:** v1 used `time(nullptr)` in timeout expressions. The first NTP correction stepped the
> clock backwards and turned a three-second timeout into a wait as long as the correction.

### CS-TIME-03 — Nothing above the MCAL calls a platform time function

No `millis()`, no `esp_timer_get_time()`, no `xTaskGetTickCount()`. Enforced by the host build, where
none of those symbols exists.

---

## CS-TYPE — Types and arithmetic

### CS-TYPE-01 — AUTOSAR base types only

`uint8`, `sint16`, `uint32`, `boolean`. Not `int`, `unsigned`, `long`, `char` for numbers.

`char` is permitted for actual text, and `int` where a C library function requires it.

### CS-TYPE-02 — Every narrowing conversion is explicit

`-Wconversion -Wsign-conversion -Werror` makes this mechanical rather than a matter of discipline.

> **Why this is the highest-value flag on the project:** the odometer's fixed-point arithmetic is where
> an implicit narrowing does the most damage, and it is exactly where the compiler cannot infer intent.

### CS-TYPE-03 — Compare a boolean against `FALSE`, never against `TRUE`

```c
if (flag != FALSE) { ... }     /* correct   */
if (flag == TRUE) { ... }      /* forbidden */
```

> **Why:** `boolean` is `unsigned char` in C and `bool` in a C++ translation unit including the Arduino
> core (see `Platform_Types.h`). A value of 2 is `!= FALSE` under both and `== TRUE` under neither
> reliably. CS-TYPE-04 forbids producing such a value; this rule means a slip cannot become a wrong
> branch.

### CS-TYPE-04 — A boolean holds only `TRUE` or `FALSE`

Convert explicitly: `(x > y) ? TRUE : FALSE`, not an implicit truncation of an integer.

### CS-TYPE-05 — No floating point in the odometry path

None at all, including in the derivation of the conversion factor.
[ADR-0005](adr/0005-integer-only-odometry.md) has the arithmetic; the short version is that `float` stops
representing every millimetre past 16.8 km, so increments are silently discarded.

Floating point is permitted elsewhere, where a value is replaced by the next sample rather than
accumulated.

### CS-TYPE-06 — Every `switch` on an enumeration handles every enumerator

`-Wswitch-enum -Werror`. A `default` that swallows unknown cases does not satisfy this — list them.

> **Why:** adding a Dem event or a DTC is exactly this, and the places needing an update are otherwise
> found by testing rather than by the compiler.

---

## CS-CFG — Configuration

### CS-CFG-01 — Compile-time configuration lives in `<M>_Cfg.h`

Not in the implementation, not in a header shared between modules.

### CS-CFG-02 — Every configured value carries its derivation

```c
/**
 * @brief Bound on an RS485 response, in milliseconds.
 *
 * 400 ms. An 81-byte response at 4800 8E1 is 81 * 10 / 4800 = 169 ms on the wire, and 400 ms is
 * 2.4x that -- enough to tolerate one retry without the timeout becoming the limiting factor.
 */
#define RS485IF_RESPONSE_TIMEOUT_MS 400u
```

> **Why:** `#define RS485IF_RESPONSE_TIMEOUT_MS 400u` alone is unreviewable and uncorrectable. With the
> derivation, a reader can check it and can fix it when the baud rate changes.

### CS-CFG-03 — Configuration consistency is asserted at compile time where it can be

Three of these caught real errors during development:

- `Ecu_PinMap.h` detects a double-assigned GPIO by comparing the bitwise OR of the pin bits against
  their arithmetic sum — a duplicate makes the two differ. It also rejects the internal flash pins and
  input-only pins used as outputs.
- `SchM_Cfg.h` asserts every budget is below its period, and that supervision outranks everything it
  supervises.
- `Fee_Cfg.h` asserts every block fits its configured length. It fired on three under-sized blocks
  before any test ran.

### CS-CFG-04 — A module does not read another module's `_Cfg.h`

Where two must agree, the dependency is explicit: `SchM_Cfg.h` includes `WdgM_Cfg.h` and maps tasks to
supervised entities by name, so the two cannot silently disagree about which task is which.

---

## CS-STRUCT — Structure

### CS-STRUCT-01 — A module that touches hardware splits into a pure core and a platform leaf

`<M>.c` compiles on host and target; `<M>_Esp32.cpp` on target only. **If it can be computed, it goes in
the `.c` file** — a platform leaf containing a calculation is a calculation that is not tested.

Enforced by the host test runner linking every `.c` into every suite, so a leak is a link error. See
[ADR-0006](adr/0006-pure-core-platform-leaf.md).

### CS-STRUCT-02 — Project includes are layer-qualified

```c
#include "base/Std_Types.h"
#include "mcal/Spi/Spi.h"
#include "Ecu_PinMap.h"        /* config/ only */
```

Project headers quoted and qualified; external headers in angle brackets.

> **Prevents:** on a case-insensitive filesystem, `Spi.h` and the Arduino core's `SPI.h` are the same
> file — and the collision resolves in opposite directions for `-I` and `-iquote`, so both our code and
> the framework break in the same build. The same trap waits for `Can.h`, `Uart.h`, `Adc.h` and `Dio.h`.
> `platformio.ini` has the full account.
>
> **Also buys:** a file's include list now states which layers it depends on, so a layer violation is
> visible at a glance and greppable in CI.

### CS-STRUCT-03 — Calls go downward or sideways, never upward

The permitted edges are in [03-interfaces.md](03-interfaces.md) §2, including the two deliberate
exceptions (MCAL may call Det; ECU abstraction may call Dem, Crc and Log) and why each is worth taking.

### CS-STRUCT-04 — A module exposes `Init`, and `DeInit` where it holds a resource

`DeInit` is not padding. Without it, state leaks between test cases and a test passes or fails depending
on execution order.

> **Prevents:** exactly that, during development. `Can_State` persisted across cases until `Can_DeInit`
> was added, and the same for `Rs485If`.

---

## CS-DOC — Documentation

### CS-DOC-01 — Every public entity has a Doxygen comment

`@brief` at minimum; `@param` and `@return` where they are not self-evident.

### CS-DOC-02 — A module header explains why the module exists

Not what its functions are called — the function comments do that. What problem it solves, and what goes
wrong without it. That is the part a reader cannot reconstruct.

### CS-DOC-03 — A non-obvious decision carries its reason in a comment

Particularly: why a bound has the value it has, why an operation is ordered the way it is, and why an
apparently simpler alternative does not work.

> **Why:** these are the comments that stop a later change from reintroducing a fixed defect. "Level
> before direction, so the pin never presents its power-on default to the attached device" is what
> prevents the two lines being swapped back.

### CS-DOC-04 — A requirement is cited with `@req`

`tools/gen_traceability.py` builds the matrix from these tags, so an uncited implementation appears as a
gap. `tools/check_doc_links.py` fails the build on a requirement that is cited but not defined.

### CS-DOC-05 — Comments do not narrate the code

```c
i++;                               /* increment i          -- worthless */
counter++;                         /* count the entry checkpoint only, because counting both
                                    * entry and exit doubles the observed rate against the
                                    * configured bounds            -- worth having */
```

---

## CS-TEST — Tests

### CS-TEST-01 — Expected values are derived independently

From a specification, a separate model, or hand computation — never from running the implementation.

> **Why:** a test whose expected value came from the code proves only that the code is deterministic.
> This rule is what caught the MCP2515 identifier encoding (the test was wrong, the driver was right)
> and the wheel circumference reference (wrong in the fifth significant figure, and the test passed
> anyway).

### CS-TEST-02 — The derivation goes in a comment

If you cannot say where a number came from, the test is asserting the implementation's behaviour back at
itself.

### CS-TEST-03 — `setUp` resets every module the case touches

Call each module's `DeInit`. See CS-STRUCT-04.

### CS-TEST-04 — Error paths are tested, with faults injected

The happy path is the easy half. A flash write that fails at an exact address, a truncated response, a
counter at the 49.7-day wrap.

> **Why:** the two production defects this project's tests found — `NvM` clearing a dirty flag on
> failure, and `Fee` poisoning a slot it then could not write past — are both unreachable without
> injecting a flash failure.

### CS-TEST-05 — A test that cannot fail is worse than no test

Two cases during development passed while verifying nothing: a fault injector matching the wrong address,
and a corruption mask of `& 0xFE` applied to a byte that was already even. Both looked like coverage.

When writing a negative test, break it deliberately once and confirm it fails.

---

## Mechanical enforcement

Most of this is checked rather than reviewed:

| Rule | Enforced by |
|---|---|
| CS-ERR-02, CS-ERR-03 | `warn_unused_result` with `-Werror` |
| CS-TYPE-02 | `-Wconversion -Wsign-conversion -Werror` |
| CS-TYPE-06 | `-Wswitch-enum -Werror` |
| CS-TYPE-05 | `-Wfloat-equal -Wdouble-promotion`; fires nowhere, which is the point |
| CS-CFG-03 | `_Static_assert` |
| CS-STRUCT-01 | Host runner links every `.c` into every suite |
| CS-STRUCT-02 | Two `-iquote` roots; a flat include fails to compile |
| CS-DOC-04 | `tools/check_doc_links.py`, `tools/gen_traceability.py --check` |
| Formatting | `clang-format --dry-run --Werror` |

What is left to review: CS-CFG-02, CS-DOC-02, CS-DOC-03, CS-TEST-01 and CS-TEST-02 — all of which are
about whether a reason is recorded, and none of which a tool can judge.

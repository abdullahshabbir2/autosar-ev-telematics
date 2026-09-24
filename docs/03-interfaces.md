# Interface Reference

**Audience:** engineers writing code against these modules. Assumes you have read
[02-architecture.md](02-architecture.md) and know which layer you are working in.

This is the contract, not the full API — the headers carry per-function documentation and this page
does not duplicate it. What is here is the part that is *not* in any one header: the rules that
apply across all of them, and the places where a module's contract is easy to get wrong.

---

## 1. Rules that hold everywhere

These apply to every module in the project. Each exists because violating it produced a defect in
v1, and each is checked mechanically where that is possible.

### 1.1 Return `Std_ReturnType`, and check it

Any operation that can fail returns `Std_ReturnType`, declared `CHECK_RETURN` — which expands to
`__attribute__((warn_unused_result))`. Discarding the result is a compile error.

Where a result genuinely should be dropped, say so:

```c
STD_DISCARD(Dem_ReportErrorStatus(DEM_EVENT_CAN_TIMEOUT, DEM_EVENT_STATUS_FAILED));
```

`STD_DISCARD` binds the value to a named `const` and discards that, because a bare `(void)` cast
does **not** silence `warn_unused_result` in GCC. That is a real compiler behaviour, not a style
rule: `(void)f()` still warns.

The point of the macro is auditability. `grep -c STD_DISCARD` counts every place a status is
deliberately ignored, and each is a decision a reviewer can look at. Without it the only way to
suppress the warning is to disable it wholesale, which suppresses the accidental cases too.

**Extended return codes.** `Std_ReturnType` carries more than `E_OK`/`E_NOT_OK`:

| Code | Means | Caller should |
|---|---|---|
| `E_OK` | Succeeded | Continue |
| `E_NOT_OK` | Failed, no more detail | Handle the failure |
| `E_PENDING` | Started, not finished | Poll again |
| `E_BUSY` | Resource held by someone else | Retry later |
| `E_TIMEOUT` | Deadline passed | Back off, count it |
| `E_CRC_FAIL` | Data present but corrupt | Use the redundant copy |
| `E_NOT_FOUND` | Never existed | Use a default; **not** the same as `E_CRC_FAIL` |
| `E_NO_SPACE` | Destination too small | Fix the caller; this is a defect |
| `E_INVALID_PARAM` | Argument out of contract | Fix the caller; this is a defect |
| `E_NOT_INITIALISED` | Module used before `Init` | Fix the startup order |
| `E_UNSUPPORTED` | Not built into this variant | Feature is switched off |

The distinction between `E_CRC_FAIL` and `E_NOT_FOUND` is worth dwelling on, because getting it
wrong was an actual bug during development. "The block is damaged" and "the block was never
written" call for opposite responses — the first means fall back to redundancy and raise a fault,
the second means use the configured default and raise nothing. A first-ever boot returning
`E_CRC_FAIL` would raise a storage fault on every new unit.

### 1.2 Report through Det, not through the log

A contract violation goes to `Det_ReportError`, a runtime failure to `Det_ReportRuntimeError`. The
difference is which is a bug:

- **Development error** — the caller violated the contract. A NULL pointer, an out-of-range index, a
  call before `Init`. These should not survive integration testing.
- **Runtime error** — the code is correct and the world is not. A timeout, a CRC mismatch, a device
  that stopped answering. These are expected in the field and are what Dem exists to record.

Det reports are deduplicated by (module, instance, API, error), so a fault in a 10 ms loop produces
one entry with a count rather than thousands of lines.

### 1.3 No dynamic allocation after startup

No `malloc`, no `new`, no Arduino `String`. Buffers are module-static or caller-supplied, and every
one is sized in a `_Cfg.h` header with its derivation written down.

v1 allocated a `String` per record and per field. The heap fragmented over hours until no single
allocation succeeded while plenty of total memory remained free — which presents as an unrelated
failure in whichever module happened to allocate next. `Mcu_GetHeapInfo` reports the largest
contiguous block for exactly this reason: total free tells you nothing about fragmentation.

### 1.4 Pass capacity with every writable buffer

```c
Std_ReturnType Rs485If_BuildRequest(uint8 *buffer, uint8 bufferSize, ...);
```

The function checks `bufferSize` and returns `E_NO_SPACE` rather than writing past the end. v1's
equivalent took no capacity, and one call site passed an array two bytes short.

### 1.5 One time source

Elapsed time comes from `Gpt_GetMonotonicMs` and comparisons go through `Gpt_HasElapsed` or
`Gpt_ElapsedSince`. Nothing above the MCAL calls `millis()`, `esp_timer_get_time()` or
`xTaskGetTickCount()`.

Wall-clock time is a different thing and comes from `TimeAbs`. The rule is simple: **a timeout never
uses wall-clock time, and a timestamp never uses monotonic time.** Wall-clock time steps when NTP
corrects it; monotonic time does not survive a reboot.

Direct `<` or `>` comparison of two `Gpt_TimestampType` values is forbidden (rule CS-TIME-01):
it is correct until the counter wraps at 49.7 days and then silently wrong.

### 1.6 Pure core, platform leaf

Every module that touches hardware splits in two:

| File | Compiled for | Contains |
|---|---|---|
| `<M>.c` | host **and** target | Everything derived: arithmetic, state machines, framing, validation |
| `<M>_Esp32.cpp` | target only | Register access and library calls, and nothing else |
| `<M>_Platform.h` | both | The contract between them |
| `test/support/Stub_*.c` | host only | The host's implementation of that contract |

The split is enforced mechanically, not by review: `tools/run_native_tests.py` links every
`src/**/*.c` into each test binary, so a platform dependency that leaks above the MCAL becomes an
undefined-reference error. See [ADR-0006](adr/0006-pure-core-platform-leaf.md).

The practical rule when adding code: **if it can be computed, it goes in the `.c` file.** A platform
leaf that contains a calculation is a calculation that is not tested.

### 1.7 Include by layer

```c
#include "base/Std_Types.h"
#include "mcal/Spi/Spi.h"
#include "services/Det/Det.h"
#include "Ecu_PinMap.h"        /* config/ only */
```

Project headers are quoted and layer-qualified; external headers use angle brackets. Two reasons,
one forced and one chosen:

- **Forced.** On a case-insensitive filesystem a flat include path makes `Spi.h` and the framework's
  `SPI.h` the same file — and the collision resolves in opposite directions depending on include
  order, so both our code and the framework's break, in the same build. The same trap waits for
  `Can.h`, `Uart.h`, `Adc.h` and `Dio.h`. platformio.ini explains it in full.
- **Chosen.** A file's include list now states which layers it depends on, so a layer violation is
  visible at a glance — and greppable. An application file that includes `mcal/` has reached through
  two layers, and CI can say so.

---

## 2. Layer rules

A module may call **downward** and **sideways within its own layer**. It may never call upward.

```
app/      →  services/, ecuabs/, base/
services/ →  services/, ecuabs/, mcal/, base/
ecuabs/   →  mcal/, services/ (Det, Dem, Crc, Log only), base/
mcal/     →  base/, services/Det only
base/     →  nothing
```

Two deliberate exceptions, both because the alternative is worse:

**MCAL may call Det.** Strictly, the MCAL is below the service layer. But a driver that detects a
contract violation and cannot report it has to either ignore it or invent a return path for
something the caller cannot act on. AUTOSAR itself permits this, and Det is designed for it: it has
no dependencies of its own and functions before its own `Init` (counting reports it cannot yet
record in detail).

**ECU abstraction may call Dem, Crc and Log.** Same reasoning. `CanIf` detecting a stale signal must
raise a diagnostic event; routing that up through the RTE and back down would add a layer of
indirection whose only purpose is to satisfy the diagram.

Nothing else. In particular no service calls an application SWC, and no `ecuabs` module calls
another `ecuabs` module — the RTE composes them.

---

## 3. The initialisation contract

Every module follows the same shape:

```c
CHECK_RETURN Std_ReturnType M_Init(void);      /* or void M_Init(void) if it cannot fail */
CHECK_RETURN Std_ReturnType M_DeInit(void);    /* where the module holds a resource */
void M_GetVersionInfo(Std_VersionInfoType *versioninfo);
```

Rules:

1. **Calling an API before `Init` returns `E_NOT_INITIALISED`** and reports `E_UNINIT` to Det. It does
   not crash, and it does not appear to work.
2. **`Init` is not idempotent unless the header says so.** A second `Init` on a module holding a
   resource reports `E_NOT_OK`.
3. **`DeInit` exists where a module holds something a test must reset** — `Can_DeInit`,
   `Rs485If_DeInit`. This is not padding: without it, state leaks between test cases and a test
   passes or fails depending on execution order. That happened during development and cost an
   afternoon.
4. **`EcuM` owns the order.** No module initialises another. The order and its four constraints are
   in `EcuM.h`.

---

## 4. Contracts that are easy to get wrong

The rest of each module's interface is in its header. These are the specific points where the
documentation is worth repeating, because getting them wrong produces a defect that does not look
like an interface misuse.

### Uart — discard and drain are different

```c
uint16 Uart_DiscardRx(Uart_InstanceType instance);                       /* throw away input */
Std_ReturnType Uart_DrainTx(Uart_InstanceType instance, uint32 ms);      /* wait for output */
```

Arduino's `flush()` is the second. v1 called it meaning the first, in three places. Before issuing a
request, `Uart_DiscardRx`; before dropping a half-duplex driver enable, `Uart_DrainTx`.

`Uart_DiscardRx` returns the count discarded, and that return value is worth logging: a non-zero
count before a request means the previous exchange left the bus out of step.

### Uart_ReadExact — returns early, and reports partial reads

```c
Std_ReturnType Uart_ReadExact(instance, buffer, length, timeoutMs, &actualLength);
```

Returns as soon as the last byte arrives. `actualLength` is valid on **both** `E_OK` and `E_TIMEOUT`,
so a caller can log how much of a truncated frame arrived — which is the difference between "the
pack is not answering" and "the pack answered and the frame was cut short".

### Spi — the lock spans the operation, not the transfer

```c
Std_ReturnType Spi_Lock(Spi_DeviceType device, uint32 timeoutMs);
/* ... several transfers ... */
Std_ReturnType Spi_Unlock(Spi_DeviceType device);
```

`Spi_Lock` does **not** assert chip select; `Spi_Transfer` does that per transaction. That is what
lets a lock span several transfers with gaps between them, which the MCP2515's read-modify-write
sequences need.

Hold the lock for a whole logical operation. An SD write is a command, a data block, a busy poll
that can run for seconds, and a status read — releasing the bus between those phases lets the other
device assert its chip select mid-sequence and the card abandons the write.

`Spi_Transfer` outside a lock returns `SPI_E_NOT_LOCKED`. It does not silently work.

### NvM — the RAM mirror is the working copy

```c
Std_ReturnType NvM_ReadBlock(NvM_BlockIdType id, void *dest);
Std_ReturnType NvM_WriteBlock(NvM_BlockIdType id, const void *src);   /* to the mirror */
Std_ReturnType NvM_FlushBlock(NvM_BlockIdType id);                    /* mirror to flash */
```

`NvM_WriteBlock` updates the mirror and marks it dirty; it does not touch flash. Flash is written by
`NvM_FlushBlock`, or by `EcuM_ShutdownAndReset`, or when the caller's own threshold fires — which
for the odometer is every 100 m.

This is what keeps flash wear bounded. Writing through on every update would be 28 800 writes a day
and would exhaust the endurance in under four days.

A block whose contents have not changed is not written even on an explicit flush (SWREQ-NVM-0004).

### Dem — events are debounced, so `ReportErrorStatus` is not "raise a fault"

```c
Std_ReturnType Dem_ReportErrorStatus(Dem_EventIdType id, Dem_EventStatusType status);
```

Reporting `DEM_EVENT_STATUS_FAILED` once does not confirm a fault: it increments a counter that must
reach the configured threshold. Healing works the same way in reverse and needs consecutive
fault-free operation cycles.

Two consequences that surprise people:

- A fault reported once and then not reported again stays unconfirmed. That is correct — one missed
  CAN frame is not a fault.
- The operation cycle in which a fault last failed *resets* healing rather than advancing it, so
  clearing a confirmed fault takes `DEM_HEALING_CYCLE_COUNT + 1` cycles, not
  `DEM_HEALING_CYCLE_COUNT`. This is tested explicitly, because it is off by one from what you would
  guess.

### WdgM — the entry checkpoint is what counts as alive

```c
void WdgM_CheckpointReached(WdgM_SupervisedEntityIdType se, WdgM_CheckpointIdType cp);
```

Alive counting increments on `WDGM_CP_ENTRY` only. Counting every checkpoint would give two per
execution — entry and exit — which doubles the observed rate against the configured bounds and makes
a healthy task look like it is running twice as often as it should.

Application code never calls `Wdg_Trigger` directly. A task that pets the hardware watchdog itself
defeats the deadline and program-flow checks WdgM layers on top, which is the usual way a watchdog
ends up protecting nothing.

### Com — the chunk planner is where an underflow used to live

```c
Std_ReturnType Com_ComputeChunkPlan(uint32 totalRecords, uint32 maxPerChunk, Com_ChunkPlanType *plan);
```

Computed in one place, and tested with `totalRecords == 0`, because the obvious expression
(`(total - 1) / max + 1`) underflows to a huge chunk count when `total` is zero. That is a `size_t`
underflow producing a loop bound of about four billion.

### GnssIf — coordinates are integers, and rounding matters

Coordinates are degrees × 10⁷ as `sint32`. The conversion from NMEA's degrees-and-decimal-minutes
rounds to nearest (`+ divisor / 2`) rather than truncating. Truncation biases every reading toward
the equator and prime meridian — one-directional, so it does not average out over a journey.

---

## 5. Configuration headers

Every module has `<M>_Cfg.h` holding its compile-time configuration. Three rules:

1. **Every value carries its derivation.** `#define RS485IF_RESPONSE_TIMEOUT_MS 400u` is unreviewable.
   The same value with "81-byte response at 4800 8E1 is 169 ms; 400 ms is 2.4× that, which tolerates
   one retry" can be checked, and corrected when the baud rate changes.
2. **Consistency is asserted at compile time** wherever it can be. `Ecu_PinMap.h` detects a
   double-assigned GPIO by comparing the OR of the pin bits against their sum; `SchM_Cfg.h` asserts
   every budget is below its period and that supervision outranks what it supervises;
   `Fee_Cfg.h` asserts every block fits its configured length. All three caught real errors during
   development.
3. **No configuration is read from anywhere else.** A module does not consult another module's
   `_Cfg.h`. Where two must agree, the dependency is explicit — `SchM_Cfg.h` includes `WdgM_Cfg.h`
   and maps its tasks to supervised entities by name, so the two cannot silently disagree about
   which task is which.

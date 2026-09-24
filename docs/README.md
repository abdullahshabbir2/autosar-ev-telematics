# Documentation

**Audience:** anyone picking this project up — a reviewer assessing the design, an engineer
joining the work, or a technician diagnosing a unit in the field. Each document states its own
audience at the top; this page only says which one to open.

---

## Read in this order

If you are new to the project, these four documents in sequence give you the whole picture in
about an hour:

| # | Document | What it answers |
|---|---|---|
| 1 | [01-requirements.md](01-requirements.md) | What the ECU must do, as numbered requirements the code and tests refer back to |
| 2 | [02-architecture.md](02-architecture.md) | How it is built, why the layers are where they are, and what v1 got wrong |
| 3 | [07-hardware.md](07-hardware.md) | The board: schematic, pin allocation, bill of materials |
| 4 | [05-test-strategy.md](05-test-strategy.md) | How the claims in the other three are verified |

---

## Reference

Open these when you need them rather than reading them through:

| Document | Contents |
|---|---|
| [03-interfaces.md](03-interfaces.md) | Every module's API contract, and the rules that apply across all of them |
| [04-safety-analysis.md](04-safety-analysis.md) | Hazard analysis, failure modes, and the risks accepted with reasons |
| [06-traceability.md](06-traceability.md) | Requirement → module → test, in both directions |
| [08-protocols.md](08-protocols.md) | Wire formats: RS485 battery frames, CAN identifiers, NMEA, the CSV record, MQTT topics |
| [09-operations.md](09-operations.md) | Building, flashing, provisioning, diagnosing a returned unit, credential rotation |
| [10-coding-standard.md](10-coding-standard.md) | The rules this code follows, each with the defect it prevents |

### Decision records

Short notes on the decisions that a reader would otherwise have to reverse-engineer. Each says what
was decided, what was rejected, and what would make the decision wrong.

| ADR | Decision |
|---|---|
| [0001](adr/0001-autosar-layering.md) | Adopt AUTOSAR Classic layering rather than a flat module set |
| [0002](adr/0002-handwritten-rte.md) | Hand-write the runtime environment instead of generating it |
| [0003](adr/0003-omit-memif.md) | Omit MemIf and have NvM call Fee directly |
| [0004](adr/0004-keep-det-enabled-in-production.md) | Ship with the development error tracer enabled |
| [0005](adr/0005-integer-only-odometry.md) | Integer-only odometry with a Q32 conversion factor |
| [0006](adr/0006-pure-core-platform-leaf.md) | Split every hardware-touching module into a pure core and a platform leaf |

### Diagrams

[diagrams/](diagrams/) holds the source and rendered form of every diagram referenced above. They
are Mermaid where the content is structural (layers, state machines, sequences) and SVG where it is
spatial (the schematic, the board layout).

### Hardware

[hardware/](hardware/) holds the schematic, the bill of materials, the netlist and the pin map in
the forms a person building the board would want them.

---

## Conventions in these documents

**Requirement identifiers** are `SWREQ-<area>-<number>`, for example `SWREQ-ODO-0012`. They appear
in Doxygen `@req` tags in the source and in test case comments, and
[06-traceability.md](06-traceability.md) is generated from those tags rather than maintained by
hand — so a requirement with no implementation, or an implementation with no test, shows up as a
gap rather than being quietly forgotten.

**References to v1** mean the firmware this replaces, which is in this repository's history. Where a
document explains why something is built a particular way, it usually also says what v1 did
instead, because the defect is what justifies the cost of the structure. Those references are
specific and checkable — file and function — rather than general complaints.

**Numbers** are given with their derivation wherever a reader might otherwise have to trust them.
A timeout of 2400 ms means nothing on its own; the same figure with the baud rate and frame length
it came from can be checked, and corrected when the hardware changes.

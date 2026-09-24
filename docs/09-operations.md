# Operations

**Audience:** whoever builds, flashes, provisions or diagnoses these units. Assumes command-line
comfort; assumes no knowledge of this firmware's internals.

---

## 1. Building

### Prerequisites

| Tool | Purpose |
|---|---|
| Python 3.10+ | Build scripts and the host test runner |
| PlatformIO Core | Target toolchain — `pip install platformio` |
| A host C compiler | `gcc` or `clang`, for the host tests |
| Unity 2.6.1 | Test framework; PlatformIO provides it at `~/.platformio/lib/Unity` |

### Credentials

`config/Secrets.h` is git-ignored and must exist before the firmware will build:

```bash
cp config/Secrets.h.template config/Secrets.h
# then edit config/Secrets.h
```

**It must never be committed.** CI fails the build if it becomes tracked. §6 explains why that check
exists.

### Build and test

```bash
pio run                                  # target firmware
python tools/run_native_tests.py          # all host tests
python tools/run_native_tests.py test_odo # one suite
```

A successful target build reports its size against the OTA slot:

```
RAM:   [==        ]  17.6% (used 57708 bytes from 327680 bytes)
Flash: [====      ]  42.6% (used 837590 bytes from 1966080 bytes)
```

**The flash figure is against one OTA slot, not the device.** Both `app0` and `app1` are 1 920 KiB, and
the image must fit both or a failed update cannot roll back. CI warns above 85 % and fails above 100 %.

### Build provenance

`scripts/build_version.py` compiles `git describe`, the commit hash, a UTC timestamp and the build host
into the image. These reach the health record and the diagnostic identification response, so a unit in
the field reports which commit it is running.

**A `-dirty` suffix means the image cannot be reproduced from the repository.** Do not ship one: a field
report from a dirty image cannot be diagnosed against source, and by the time anyone notices, the working
tree that produced it is gone. Commit first.

---

## 2. Flashing

### First time, or after a partition change

The partition table must be written, not only the application:

```bash
pio run --target erase       # erase the whole device
pio run --target upload      # writes bootloader, partition table and app0
```

**`erase` destroys the odometer reading.** It is in the `nvdata` partition. Read it first (§4) and
restore it afterwards (§3), or the vehicle's mileage record is gone.

The partition table changed in v2 — `nvdata` was added and `spiffs` shrank — so **every unit upgrading
from v1 needs a full erase and flash.** An application-only upload leaves the v1 table in place, and
`Fls_Init` will report a missing partition and run without persistence.

### Subsequent updates

```bash
pio run --target upload
```

### Verifying

```bash
pio device monitor
```

A healthy boot ends with the scheduler running and the heartbeat LED at 1 Hz. §5 covers what the other
indicators mean.

---

## 3. Provisioning a new unit

A new unit comes up on configured defaults, which are deliberately not a working configuration: the
device identifier is empty and the broker host is the placeholder. One firmware image serves the whole
fleet, and per-unit values live in `nvdata`.

Three things must be set, all over the authenticated diagnostic channel:

| What | Command | Notes |
|---|---|---|
| Device identifier | `SET_CFG deviceId <id>` | Defaults to the eFuse MAC if unset |
| Broker endpoint | `SET_CFG brokerHost <host>`, `SET_CFG brokerPort <port>` | |
| Calibration | `SET_CFG tyreDiameterMilliInch <n>`, `SET_CFG gearRatioMilli <n>` | **Get these right before the vehicle moves** |

### Calibration is not optional

The defaults are a 19-inch wheel and a 6.000:1 gear ratio. If the vehicle differs, every distance is
wrong by the ratio of the difference — permanently, because the odometer is monotonic and there is no
correction path. A 5 % error is 5 % of every kilometre the vehicle ever travels.

Set the calibration, then verify against a measured distance before the unit is considered provisioned.

### Restoring a mileage reading

When replacing a unit or after an erase, the reading is carried across with:

```
SET_CFG totalDistanceMm <value>
```

This is the only write path to the odometer and it exists for exactly this purpose. It is authenticated,
it is logged, and it is the reason SWREQ-SEC-0002 exists — v1 exposed the same capability on an
unauthenticated web server.

---

## 4. Diagnosing a unit

### Start with the health record

Published on `odo/<device>/health`. It answers most questions without touching the vehicle:

| Field | Read it for |
|---|---|
| `firmwareVersion`, `gitDescribe` | Which commit is running; `-dirty` means unreproducible |
| `resetReason` | `BROWNOUT` points at power, `WATCHDOG` at a stalled task, `PANIC` at a fault |
| `restartCount`, `crashLoopDetected` | Whether the unit is resetting repeatedly |
| `degradedSubsystemMask` | Which subsystems were skipped, and why |
| `taskOverruns` | A task exceeding its budget |
| `stackHighWater` | How close a task came to overflowing |
| `heapLargestBlock` | **Fragmentation.** Total free can look healthy while no allocation succeeds |
| `dtcCount` | Confirmed faults awaiting readout |
| `cardFreeMiB` | Storage headroom |

### Then read the trouble codes

```
READ_DTC
```

Each entry carries its occurrence count and a freeze frame — the conditions at first occurrence. "CAN
timeout" says a fault happened; "CAN timeout at 47 km/h, 71 V, 34 °C, 412 s after boot" usually
identifies the cause without reproducing it.

### Common findings

| Symptom | Where to look first |
|---|---|
| Records arrive in bursts, not steadily | Bearer coverage. Check `activeBearer` and RSSI history |
| Odometer advancing too slowly | Calibration. Then `taskOverruns` on the acquisition task |
| Odometer not advancing at all | CAN. `DEM_EVENT_CAN_SIGNAL_STALE` or a degraded mask bit |
| Battery data missing for one pack | `packsResponding` against `packsPresent`; then that pack's DTC |
| Intermittent storage failures | SPI contention counts, then the card itself |
| Unit resets every few minutes | `resetReason`. `BROWNOUT` is power — see the SIM800L note in [07-hardware.md](07-hardware.md) §8 |
| Nothing at all, no console output | Power, or GPIO12 pulled high — [07-hardware.md](07-hardware.md) §3, conflict 4 |

### A unit in degraded mode

Crash-loop detection starts the ECU with a subsystem skipped after repeated resets. That is deliberate
and the unit is working as designed. Once the cause is fixed:

```
CLEAR_CRASH_LOOP
```

Or leave it — the counter clears itself after the configured stable-run period.

---

## 5. At the vehicle, without a laptop

Five indicators, readable from outside:

| LED | Colour | Normal | Off means |
|---|---|---|---|
| ACQ | Green | Flash per 3 s | No acquisition completed |
| STORAGE | Yellow | Flash per record | Card absent or writes failing |
| LINK | Blue | Steady | No IP bearer |
| CLOUD | Red | Steady | No broker session |
| HEARTBEAT | White | 1 Hz | **Scheduler stopped** |

**Heartbeat off is the only pattern meaning the firmware is not running.** Everything else means the
firmware is healthy and a subsystem is down, which is the design intent — a failed CAN controller costs
odometry but not battery data.

---

## 6. Credential rotation — required, not optional

**v1 committed a Firebase authentication token and a WiFi password as string literals in
`include/config.h`.**

Those credentials are in this repository's history permanently. Deleting the file — which v2 did — does
**not** remove them: anyone who can clone the repository can recover them with `git log -p`. Anyone with
read access to the source had write access to the fleet's database.

**Every credential that was ever committed must be treated as compromised and rotated.** Specifically:

1. **The Firebase token.** Revoke it. The Firebase integration is gone in v2, but a live token on a live
   project is a live hole.
2. **The WiFi password.** Change it on every access point, and on every other device that uses it.
3. **Any broker credential** that was ever in a committed file.

Rotation is the only remedy. Options short of it do not work:

- *Rewriting history* (`filter-branch`, `filter-repo`) changes the hashes but not any existing clone,
  fork, or cached copy. It also invalidates every reference to every commit.
- *Deleting the file* removes it from the current revision only, which is what v2 already did.
- *Making the repository private* does nothing about who already cloned it.

### Keeping it out

`config/Secrets.h` is git-ignored, and CI fails if it becomes tracked or if a credential macro is
assigned a non-placeholder literal in committed source.

That check is deliberately narrow. A broad secret scanner on a repository that documents v1's leaked
credentials as a case study fires on the documentation itself, and a gate that always fires is a gate
that gets bypassed.

### What this does not protect

Credentials are compiled into the image. Anyone with physical access can read the flash and extract
them. Keeping them out of version control stops them leaking through the repository — nothing more.
Protecting them on the device needs ESP32 flash encryption and secure boot, recorded as accepted risk R1
in [04-safety-analysis.md](04-safety-analysis.md) with the reasoning.

---

## 7. Releasing

1. **Host tests pass.** `python tools/run_native_tests.py` — all suites, no failures.
2. **Target builds clean**, and the image is comfortably inside one OTA slot.
3. **Traceability is current.** `python tools/gen_traceability.py --check`.
4. **Documentation links resolve.** `python tools/check_doc_links.py`.
5. **Working tree is clean**, so `git describe` produces no `-dirty`.
6. **Tag the release**, so the version in the health record maps to a commit.
7. **Flash one unit and watch a full cycle** — acquisition, storage, publish — before any fleet rollout.

Step 7 is the one that catches what the others cannot: the platform leaves are the ~10 % of the source
that host tests do not reach ([ADR-0006](adr/0006-pure-core-platform-leaf.md)), and a register-level
mistake is invisible until silicon runs it.

### Rolling back

The A/B partition layout means the previous image stays bootable in the other slot. A failed update
rolls back automatically if the new image does not confirm itself.

This only works while the image fits **both** slots, which is why the CI size check is a hard failure
rather than a warning.

# ADR-0005 — Integer-only odometry with a Q32 conversion factor

**Status:** accepted
**Date:** 2026-02-22

---

## Context

The odometer is the number this product exists to produce. It is the vehicle's mileage record, it is
used for warranty and resale, and it is monotonic by design — so any error in it is permanent.

v1 computed it in floating point and lost it twice over:

- **The SD copy** was written as text and read back with `atol()`, which parses an integer. A stored
  reading of `1234.56` came back as `1234`. The truncated value was then written back, so the loss
  compounded on every boot. On a vehicle that reboots a few times a day this destroys the reading
  gradually and invisibly.
- **The NVS copy** was written with `putDouble()` and read into a `float`. `put_dist_preferences()` also
  assigned the *return value* of `putDouble` — the byte count — over its own `distance` parameter, which
  was harmless only because the parameter was passed by value.

Neither path had an integrity check, so a partial write during the brown-out that accompanies engine
cranking was indistinguishable from a valid reading.

Fixing the storage was necessary but not sufficient. The arithmetic itself had to be examined.

## The problem with `float`

`float` has a 24-bit mantissa, so it represents integers exactly only up to 2²⁴ = 16 777 216. In
millimetres that is **16.8 km**.

Past that point the representable values are spaced further apart than one millimetre, and an increment
smaller than the spacing is discarded entirely — `x + δ == x`. The spacing doubles with each power of
two:

| Odometer reading | Spacing of representable values | Smallest increment that registers |
|---|---|---|
| 16.8 km | 1 mm | 1 mm |
| 33.6 km | 2 mm | 2 mm |
| 134 km | 8 mm | 8 mm |
| 1 073 km | 64 mm | 64 mm |
| 17 180 km | 1 m | 1 m |
| 68 719 km | 4 m | 4 m |

At 50 000 km — an unremarkable mileage — a vehicle must travel about 4 m before the reading changes at
all. The ECU samples every 3 s, so at low speed no interval produces enough distance to register and the
odometer simply stops advancing. The failure is gradual, silent, and worsens with age, which makes it
close to undiagnosable in the field.

`double` postpones this to 2⁵³ mm, which is far beyond any vehicle. But the ESP32 has no
double-precision hardware, so every operation is a software routine, and the deeper objection remains:
the rounding of a floating-point accumulation depends on the order of operations and on the compiler,
so two units given identical inputs could disagree about their mileage.

## Options considered

**`double` throughout.** Correct in range, slow, and non-reproducible between toolchains.

**Fixed-point with a floating-point conversion factor.** Accumulate integer millimetres but compute the
per-revolution distance in `float` once at startup. Removes the accumulation problem, keeps a
compiler-dependent constant at the heart of the calculation.

**Integer millimetres with an integer conversion factor.** Chosen.

## Decision

Distance is accumulated as `uint64` millimetres. No floating-point arithmetic appears anywhere in the
odometry path, including the derivation of the conversion factor.

### The factor

Wheel circumference is π × diameter, and π must therefore be available as an integer. It is held as a
Q24 fixed-point constant:

```
  ODO_PI_Q24 = round(π × 2²⁴) = 52 707 179
```

π × 2²⁴ = 52 707 178.9... so the rounded value is accurate to about 6 × 10⁻⁹ — roughly two parts per
billion, which over a 1516 mm circumference is 3 nanometres. The constant is verified against an
independently computed value in the test suite rather than trusted.

From it, the distance per motor revolution is derived once and held as a Q32 factor, so the per-interval
computation is one 64-bit multiply and one shift:

```
  distanceMm = (revolutions × factorQ32) >> 32
```

A shift rather than a divide, because a power-of-two divisor makes the truncation exactly predictable —
which is what the remainder carry below depends on.

### Trapezoidal integration

Distance over an interval uses the mean of the interval's start and end speeds, not either endpoint.
Rectangular integration over-reads on acceleration and under-reads on braking, and the two do **not**
cancel: a vehicle that accelerates from rest and stops again returns to zero speed, so both errors are
in the direction of the higher speed encountered. The trapezoidal rule is exact for any linear speed
change, which over 3 s is a close approximation to how a vehicle behaves.

### Remainder carry

The shift truncates, so each interval discards up to 1 mm. That happens 28 800 times a day. At a
conservative mean loss of 0.5 mm per interval it is 14 m per day and over 5 km per year, all in one
direction.

The remainder is therefore carried into the next interval. This bounds the total accumulated error at
**one millimetre**, regardless of how long the unit runs.

## Consequences

**Measured accuracy.** Over a simulated 1.26 km journey with acceleration, cruise and braking, the
implementation is accurate to **0.85 mm** against a closed-form integral of the same speed profile, with
**0.046 mm** of total rounding drift. Both figures come from `test_odo`, against a reference computed
independently of the implementation.

**Reproducibility.** Two units with the same calibration compute bit-identical distances on any
toolchain. Nothing in the path depends on rounding mode, evaluation order or FPU behaviour.

**Range.** `uint64` millimetres reaches 1.8 × 10¹³ km. Not a limit any vehicle approaches.

**Cost.** The arithmetic is less obvious to read than `distance += rpm * circumference * dt`. The
derivation of `ODO_PI_Q24` and the Q32 factor is written out in `OdoSwc_Cfg.h` with the reasoning, which
is the mitigation — the comment is longer than the code, deliberately.

**An error caught by this decision.** Writing the test reference by hand, the wheel circumference was
initially taken as 1 516 195 µm; the correct value for a 19-inch wheel is 1 516 133 µm
(19 × 25.4 × π = 1516.1326 mm). The test passed with the wrong reference because the implementation's
tolerance absorbed it. Fixing the reference is what made the 0.85 mm and 0.046 mm figures above
meaningful — with a wrong reference, neither would have been visible.

## When this would be the wrong decision

If the quantity were not monotonic and not the product. A speed reading or a temperature can be a
`float` without consequence: the next sample replaces it, so an error does not accumulate. The argument
here rests entirely on the value being cumulative and uncorrectable.

# Hardware

**Audience:** whoever builds, repairs or modifies the board, and anyone diagnosing a fault that
might be electrical. Assumes you can read a schematic; assumes nothing about this vehicle.

---

## 1. Overview

A single board carrying an ESP32-WROOM-32 module and the interfaces it needs:

| Interface | Device | Bus |
|---|---|---|
| Motor controller | MCP2515 + TJA1050 | SPI (shared) + CAN |
| Battery packs (×4) | MAX3485 | UART1, RS485 half-duplex |
| Position | NEO-6M | Software UART, receive only |
| Cellular | SIM800L | UART2 |
| Logging | microSD | SPI (shared) |
| Time | DS3231 | I²C |
| Pack voltage | Resistive divider | ADC1 |
| Indication | 5 LEDs | GPIO |

Full schematic: [hardware/schematic.svg](hardware/schematic.svg).
Bill of materials: [hardware/bom.csv](hardware/bom.csv).
Pin allocation with justification: [`config/Ecu_PinMap.h`](../config/Ecu_PinMap.h) — that file is
authoritative, and it is the one that fails the build if a pin is assigned twice.

---

## 2. Pin allocation

| GPIO | Signal | Direction | Note |
|---|---|---|---|
| 0 | BOOT | — | Strapping pin; leave unloaded |
| 1 | CONSOLE_TX | out | UART0, also the flashing interface |
| 2 | LED_CLOUD | out | Red. Broker session established |
| 3 | CONSOLE_RX | in | UART0, also the flashing interface |
| 4 | CAN_CS | out | MCP2515 /CS. **Was GPIO2 in v1** |
| 5 | SD_CS | out | microSD /CS |
| 6–11 | — | — | **Internal SPI flash. Unusable.** |
| 12 | MTDI | — | **Leave unconnected.** See §4 |
| 13 | RS485_DE | out | MAX3485 driver enable. Low = receive |
| 14 | GSM_PWRKEY | out | Active low, >1 s pulse toggles power |
| 15 | LED_HEARTBEAT | out | White. Scheduler alive |
| 16 | RS485_RX | in | UART1 ← MAX3485 RO |
| 17 | RS485_TX | out | UART1 → MAX3485 DI |
| 18 | SPI_SCK | out | Shared: MCP2515 + microSD |
| 19 | SPI_MISO | in | Shared |
| 21 | I2C_SDA | bidir | DS3231. External 4k7 pull-up |
| 22 | I2C_SCL | out | DS3231. External 4k7 pull-up |
| 23 | SPI_MOSI | out | Shared |
| 25 | LED_LINK | out | Blue. An IP bearer is up |
| 26 | GSM_RX | in | UART2 ← SIM800L TXD |
| 27 | GSM_TX | out | UART2 → SIM800L RXD, level shifted |
| 32 | LED_ACQ | out | Green. A record was acquired |
| 33 | LED_STORAGE | out | Yellow. A record was written |
| 34 | GNSS_RX | in | Software UART. **Input only** |
| 35 | CAN_INT | in | MCP2515 /INT. **Input only**, external 10k pull-up |
| 36 | VBATT_SENSE | in | ADC1_CH0. **Input only** |
| 39 | GSM_STATUS | in | **Input only** |

---

## 3. Four conflicts inherited from v1

Every one of these was in the v1 wiring and firmware. They are listed with what the symptom was,
because that is what makes each fix checkable rather than a matter of opinion.

### Conflict 1 — GPIO2 was both the built-in LED and the CAN chip select

v1 defined `LED_BUILTIN` as GPIO2 and `MCP2515_CSPIN` as GPIO2. Every LED update toggled the CAN
controller's chip select.

**Symptom.** Corrupted MCP2515 register reads, appearing as intermittent CAN initialisation
failures. Never reproducible on the bench, because the bench unit's LED was not being driven at the
same rate.

**Fix.** CAN chip select moved to GPIO4. GPIO2 is now `LED_CLOUD` and nothing else.

### Conflict 2 — UART2 was claimed by both the RS485 bus and the modem

v1 configured UART2 at 4800 8E1 for the battery bus and at 9600 8N1 for the SIM800L. Whichever
initialised second reconfigured the peripheral out from under the first.

**Symptom.** Battery data or modem communication worked, never both, and which one depended on
initialisation order.

**Fix.** RS485 on UART1 (GPIO16/17), modem on UART2 (GPIO26/27), GNSS on a receive-only software
UART. The ESP32 has three UARTs and this design needs four endpoints; §5 explains why GNSS is the
one that can be synthesised.

### Conflict 3 — GNSS was on the internal flash pins

v1 constructed the GNSS port as `HardwareSerial(1)` with no pin arguments, which on this framework
defaults UART1 to GPIO9 and GPIO10 — both wired to the internal SPI flash on every WROOM-32 module.

**Symptom.** Reconfiguring those pins as a UART either produced no GNSS data or crashed the module,
depending on flash timing. The failure looks like a corrupt firmware image, which sends the
investigation in entirely the wrong direction.

**Fix.** GNSS on GPIO34, receive only. `Ecu_PinMap.h` asserts at compile time that no signal is
assigned to GPIO6–11.

### Conflict 4 — GPIO12 must stay unconnected

GPIO12 (MTDI) is a strapping pin sampled at reset. Held high, it selects a 1.8 V flash supply
voltage. On a board with 3.3 V flash the module does not boot, and the symptom is a device that
appears dead with no output at all.

**Fix.** GPIO12 is deliberately unallocated, and `Ecu_PinMap.h` asserts it stays that way — because
"remember not to use GPIO12" is exactly the kind of constraint that survives in one person's head
and nowhere else.

---

## 4. The shared SPI bus

The MCP2515 and the microSD card share VSPI. This is the part of the board that most needs
understanding before it is modified.

```
                    ┌──────────── GPIO18 SCK  ───────┬──────────────┐
                    │  ┌───────── GPIO19 MISO ───────┼──────┐       │
                    │  │  ┌────── GPIO23 MOSI ───────┼────┐ │       │
   ESP32 ───────────┤  │  │                          │    │ │       │
                    │  │  │   GPIO4  ─── /CS ────────┘    │ │       │
                    │  │  │   GPIO5  ─── /CS ─────────────┼─┼───────┼──┐
                    │  │  │   GPIO35 ─── /INT ──┐         │ │       │  │
                    └──┼──┼─────────────────────┼─────────┼─┼───────┼──┼──
                       │  │                  ┌──┴──────────────────┐│  │
                       │  └──────────────────┤      MCP2515        ├┘  │
                       └─────────────────────┤  (10 MHz, mode 0)   │   │
                                             └──────────┬──────────┘   │
                                                   TJA1050            │
                                                   CANH/CANL          │
                                             ┌───────────────────┐    │
                                             │     microSD       ├────┘
                                             │ (20 MHz, mode 0)  │
                                             └───────────────────┘
```

**They disagree about everything that matters.** Clock rate: 10 MHz against 20 MHz. Transaction
length: tens of microseconds against up to two seconds while the card does internal wear levelling.

If the CAN task asserts its chip select while the SD driver is mid-block, the card sees a truncated
command and returns to idle. The SD driver then reports a write failure that looks exactly like a
failing card — and that signature is all over the archived v1 device logs, on cards that tested
perfectly afterwards. v1 had no arbitration of any kind: the CAN code drove chip select directly
while the Arduino `SD` library drove its own.

`Spi_Lock`/`Spi_Unlock` make the bus an explicitly owned resource, and `FsAbs_Esp32.cpp` holds the
lock across a whole card operation rather than per transfer. `Spi_Transfer` outside a lock returns
`SPI_E_NOT_LOCKED` rather than working most of the time.

### If you modify this bus

- **Both chip selects must be driven high before anything clocks the bus.** `Port_Init` does this
  first, before `Spi_Init`. A chip select left floating — or at the module's power-on default of low —
  makes one device read the other's traffic as its own command stream.
- **Keep the MCP2515 on a short trace.** 10 MHz is its datasheet maximum. On a flying lead from a
  breakout module, drop `SPI_CLOCK_CAN_HZ` to 4 MHz; the failure mode is corrupted register reads,
  which surface as a CAN init failure rather than anything subtle.
- **Do not add a third device without reviewing `SPI_LOCK_TIMEOUT_MS`.** It is 2500 ms, set by the
  card's worst-case internal programming time. A third device that also blocks for seconds needs
  that recalculated.

---

## 5. GNSS on a software UART

Three hardware UARTs, four endpoints needed. GNSS is the one that can be synthesised, and it is the
*only* one:

- It never transmits. The NEO-6M is left in its default NMEA output mode, so nothing is ever sent to
  it. A software UART that had to transmit would disable interrupts for a whole character time, and
  at 4800 baud that is 2 ms — long enough to drop RS485 bytes.
- 9600 baud is 104 µs per bit, which the interrupt-driven soft UART handles comfortably. The modem's
  traffic volumes would not be safe at the same rate.

`Uart_Open` refuses a transmit pin on the software instance rather than accepting one and
half-working.

**Consequence for the receiver.** Its baud rate must not be reconfigured. A NEO-6M switched to 38400
or put into UBX binary mode will not be read correctly, and the failure is silent — `GnssIf` simply
never sees a valid sentence. If a receiver has been reconfigured, restore it before fitting.

---

## 6. Battery voltage sense

A resistive divider from the pack positive rail to ADC1_CH0 (GPIO36).

**ADC1, not ADC2, and this is not incidental.** ADC2 shares its hardware with the WiFi radio and
returns whatever was last converted — with no error indication at all — whenever the radio is
active, which on this ECU is nearly always. A design that put the battery sense on ADC2 would read
plausible, stable, entirely fictional voltages, and the oversampling in `Adc.c` would faithfully
average them.

Conditioning:

- 12-bit resolution, 11 dB attenuation, nominal 0–3.3 V span.
- 7 samples per reading, highest and lowest discarded, remaining 5 averaged (`ADC_OVERSAMPLE_COUNT`,
  `ADC_TRIM_COUNT`). The trim is what rejects the single-sample outliers this part produces near a
  switching supply; a plain average includes them.
- Scaling and offset are per-unit calibration values in `NVM_BLOCK_CALIBRATION`, not constants. The
  divider resistors are 1 % parts and the ADC has its own offset, so a firmware-wide constant is
  wrong for every individual board by a different amount.

`IoHwAb_GetBatteryVoltage` returns millivolts. The raw count never leaves the MCAL.

---

## 7. RS485 battery bus

Half duplex, 4800 8E1, up to four packs on a shared pair.

```
  GPIO17 TX ──── DI ┐                        ┌── A ──┬── Pack 1
  GPIO16 RX ──── RO ├── MAX3485              ├── B ──┤   Pack 2
  GPIO13 DE ──┬─ DE ┤   (120 Ω termination)  │       │   Pack 3
              └─ /RE ┘                       └───────┴── Pack 4
```

`DE` and `/RE` are tied together, so the transceiver is either transmitting or receiving and never
both. Low is receive, which is the resting state — a half-duplex bus left with the driver enabled
blocks every other device on it, which is why `Port_Init` drives it low before anything else.

**The turnaround is the part that is easy to get wrong.** `DE` may only drop once the last bit has
physically left the shift register. Arduino's `write()` returns when the bytes are *buffered*, not
sent. v1 dropped `DE` immediately after `write()`, truncating the last character of every request it
ever transmitted — and the packs tolerated it, because the truncated byte was the second CRC byte
and their firmware checked only the first. So the bus worked, and the frames were wrong.

`Uart_DrainTx` waits for both the ring buffer to empty and the shift register to clear, and
`Rs485If` calls it before dropping `DE`.

**Termination.** 120 Ω at each end of the pair, not at each node. At 4800 baud over a few metres a
missing terminator usually still works, which is what makes it a hard fault to find later when cable
length changes.

---

## 8. Power

| Rail | Source | Consumers |
|---|---|---|
| 12 V | Vehicle auxiliary | Buck converter input |
| 5 V | Buck, 2 A | SIM800L, microSD, MAX3485, TJA1050 |
| 3.3 V | Module LDO | ESP32, MCP2515, DS3231, level shifters |

**The SIM800L is the constraint.** It draws up to 2 A in 577 µs bursts during a GSM transmit slot. A
supply that cannot deliver it browns out the ESP32, and the resulting reset looks like a firmware
crash — the reset reason reads as brownout, but only if someone thinks to look, and v1 did not
record it.

Requirements:

- ≥1000 µF low-ESR bulk capacitance at the modem's supply pins, plus 100 nF local.
- A 5 V rail rated at 2 A continuous, not 2 A peak.
- Modem supply kept off the 3.3 V rail entirely.

`Mcu_GetResetReason` reports `MCU_RESET_BROWNOUT` distinctly, and EcuM's crash-loop detection counts
it, so a unit with marginal power is identifiable from its health record rather than by
instrumenting the vehicle.

**The DS3231 needs its backup cell.** Without one the clock resets on every power cycle, and
`TimeAbs_PlatformRtcInit` reports the oscillator-stopped flag rather than returning a plausible
wrong time. Fit a CR2032 or an LIR2032 — the DS3231 module's charging circuit differs between the
two, so check which the fitted module expects before substituting.

---

## 9. Diagnosing at the vehicle

The five indicators are readable without a laptop:

| LED | Colour | Off means | Flashing means |
|---|---|---|---|
| ACQ | Green | No acquisition cycle completed | Normal, once per 3 s |
| STORAGE | Yellow | Card not mounted or write failing | Normal, once per record |
| LINK | Blue | No IP bearer | Bearer coming up |
| CLOUD | Red | No broker session | Session negotiating |
| HEARTBEAT | White | **Scheduler stopped** | Normal, 1 Hz |

**HEARTBEAT off is the only one that means the firmware is not running.** Every other combination
means the firmware is running and a subsystem is down, which is what the design intends — a failed
CAN controller costs odometry but not battery data.

Common combinations:

| Pattern | Most likely cause |
|---|---|
| Heartbeat only | No subsystems came up. Check power rails and the SD card first |
| Heartbeat + ACQ, no STORAGE | Card absent, unformatted, or write-protected |
| Heartbeat + ACQ + STORAGE, no LINK | No coverage, or credentials wrong |
| LINK on, CLOUD off | Bearer up, broker unreachable or rejecting the session |
| Nothing at all | Power, or GPIO12 pulled high — see Conflict 4 |

/**
 * @file    Ecu_PinMap.h
 * @brief   Single authoritative GPIO allocation for the telematics ECU.
 *
 * This file is the contract between the firmware and the hardware. The schematic in
 * [schematic.svg](docs/hardware/schematic.svg), the netlist, the BOM and every MCAL
 * configuration are generated from or checked against it, and no other file in the
 * project may name a raw GPIO number.
 *
 * @par Conflicts inherited from the v1 firmware, and how they are resolved
 * The v1 pin assignment could not physically work as written. Each conflict and its
 * resolution is recorded here because the reasoning is the useful part:
 *
 *  1. **GPIO2 was both @c LED_BUILTIN and @c MCP2515_CSPIN.** Asserting chip select
 *     also lit the LED and, worse, every LED update toggled the CAN controller's
 *     chip select mid-transaction. CAN moves to GPIO4; GPIO2 keeps the on-board LED.
 *
 *  2. **UART2 was claimed by both the RS485 battery bus (4800 8E1) and the SIM800L
 *     modem (9600 8N1).** With @c GSM_ENABLE defined, the modem's @c begin()
 *     silently reconfigured the port and the battery bus stopped answering. The
 *     ESP32 has three UARTs; the allocation below gives RS485 and the modem one
 *     each and moves the GNSS receiver to a receive-only software UART, which is
 *     sound because NMEA is a 1 Hz line protocol with a per-sentence checksum, so a
 *     dropped sentence is detected and simply skipped. This is also why
 *     @c espsoftwareserial was already among the v1 dependencies.
 *
 *  3. **The GNSS receiver used @c HardwareSerial(1) with no pin arguments**, which
 *     defaults to GPIO9/GPIO10 -- both wired to the internal SPI flash on every
 *     WROOM-32 module. Any real traffic there would corrupt flash access. GNSS RX
 *     moves to GPIO34.
 *
 *  4. **GPIO12 is left unconnected on purpose.** It is the MTDI strapping pin and
 *     selects the internal flash regulator voltage at reset; a pull-up on a 3.3 V
 *     module makes the part unbootable. v1 drove it as the WiFi status LED.
 *
 * @par Strapping pins still in use, and why it is safe
 * GPIO2 and GPIO15 drive LEDs. Both are sampled at reset and both must not be held
 * high then. An LED wired anode-to-pin, cathode-to-ground through a resistor
 * presents a path to ground at reset and reads low, which is the required state for
 * both. The LEDs are therefore active-high and must not be re-wired to a pull-up
 * arrangement. [07-hardware.md](docs/07-hardware.md) tabulates the full allocation with the four
 * inherited conflicts and how each was resolved.
 *
 * @req SWREQ-SYS-0020, SWREQ-SYS-0021, SWREQ-SAF-0010
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ECU_PINMAP_H
#define ECU_PINMAP_H

/*==================================================================================================
 *  Sentinel
 *================================================================================================*/

/** Marks a signal that is not populated on this board variant. */
#define PIN_NOT_CONNECTED 0xFFu

/*==================================================================================================
 *  Console / programming (UART0)
 *================================================================================================*/

#define PIN_CONSOLE_TX 1u /**< UART0 TX. Also the flashing interface.  */
#define PIN_CONSOLE_RX 3u /**< UART0 RX. Also the flashing interface.  */

/*==================================================================================================
 *  SPI bus (VSPI) -- shared by the CAN controller and the SD card
 *
 *  Two devices on one bus, so every transaction must be bracketed by its own chip
 *  select and the bus must be locked for the duration. Spi_Lock()/Spi_Unlock() do
 *  that; the v1 firmware did not, which is the most likely cause of the intermittent
 *  "Card Initialization Failed" entries in the archived device logs.
 *================================================================================================*/

#define PIN_SPI_SCK 18u  /**< VSPI CLK  -> MCP2515 SCK, microSD CLK  */
#define PIN_SPI_MISO 19u /**< VSPI MISO <- MCP2515 SO,  microSD DO   */
#define PIN_SPI_MOSI 23u /**< VSPI MOSI -> MCP2515 SI,  microSD DI   */

#define PIN_CAN_CS 4u   /**< MCP2515 /CS. Was GPIO2 in v1 (conflict 1).      */
#define PIN_CAN_INT 35u /**< MCP2515 /INT. Input-only pin, which is all it needs. */

#define PIN_SD_CS 5u /**< microSD /CS. */

/*==================================================================================================
 *  I2C bus -- DS3231 real-time clock
 *================================================================================================*/

#define PIN_I2C_SDA 21u /**< DS3231 SDA (external 4k7 pull-up to 3V3). */
#define PIN_I2C_SCL 22u /**< DS3231 SCL (external 4k7 pull-up to 3V3). */

/*==================================================================================================
 *  RS485 battery bus (UART1) -- 4800 baud, 8E1, half duplex
 *================================================================================================*/

#define PIN_RS485_RX 16u /**< UART1 RX <- MAX3485 RO. */
#define PIN_RS485_TX 17u /**< UART1 TX -> MAX3485 DI. */

/**
 * @brief MAX3485 DE and /RE, tied together: high = transmit, low = receive.
 *
 * Held low except while a request is being clocked out. The turnaround delay is
 * ::RS485IF_TURNAROUND_US and exists because releasing DE before the UART shift
 * register has drained truncates the last character on the wire.
 */
#define PIN_RS485_DE 13u

/*==================================================================================================
 *  GSM/GPRS modem (UART2) -- SIM800L
 *================================================================================================*/

#define PIN_GSM_RX 26u     /**< UART2 RX <- SIM800L TXD.                        */
#define PIN_GSM_TX 27u     /**< UART2 TX -> SIM800L RXD (level shifted to 2.8V). */
#define PIN_GSM_PWRKEY 14u /**< SIM800L PWRKEY, active low, >1 s pulse to toggle. */
#define PIN_GSM_STATUS 39u /**< SIM800L STATUS, high once the modem is running.  */

/*==================================================================================================
 *  GNSS receiver -- NEO-6M, receive-only software UART at 9600 baud
 *================================================================================================*/

/**
 * @brief GNSS RX. Input-only pin, which suits a receive-only NMEA link exactly.
 *
 * Nothing is ever transmitted to the receiver, so no TX pin is allocated: the
 * module runs in its default NMEA configuration.
 */
#define PIN_GNSS_RX 34u
#define PIN_GNSS_TX PIN_NOT_CONNECTED

/*==================================================================================================
 *  Analogue inputs
 *================================================================================================*/

/**
 * @brief Auxiliary battery voltage, via a 100k/10k divider into ADC1_CH0.
 *
 * ADC1 is mandatory rather than incidental: ADC2 is unusable whenever the WiFi
 * radio is active, which on this ECU is nearly always, and reads would return
 * garbage with no error indication.
 */
#define PIN_VBATT_SENSE 36u

/*==================================================================================================
 *  Status indication -- active high, LED anode to pin, cathode to GND via resistor
 *================================================================================================*/

#define PIN_LED_ACQ 32u       /**< Green  -- a data record was acquired.    */
#define PIN_LED_STORAGE 33u   /**< Yellow -- a record was written to SD.    */
#define PIN_LED_LINK 25u      /**< Blue   -- an IP bearer is up.            */
#define PIN_LED_CLOUD 2u      /**< Red    -- broker session established.    */
#define PIN_LED_HEARTBEAT 15u /**< White -- scheduler alive.              */

/*==================================================================================================
 *  Deliberately unused
 *================================================================================================*/

/**
 * @brief GPIO12 (MTDI) must be left floating.
 *
 * Sampled at reset to select the internal flash regulator voltage. Pulled high, a
 * 3.3 V module is configured for 1.8 V flash and will not boot. Recorded as a
 * symbol so that a future pin allocation cannot quietly reuse it.
 */
#define PIN_RESERVED_MTDI 12u

/** GPIO0 (BOOT). Pulled low at reset to enter the serial bootloader. */
#define PIN_RESERVED_BOOT 0u

/*==================================================================================================
 *  Compile-time conflict detection
 *
 *  A duplicated pin number is the failure this file exists to prevent, so the
 *  assignment is checked mechanically rather than by review. Each usable GPIO
 *  contributes one bit; a sum that disagrees with the OR of the same bits proves
 *  some pin was allocated twice, and the build stops.
 *================================================================================================*/

#define ECU_PIN_BIT(p) (1uLL << (p))

/* clang-format off */
#define ECU_PINMAP_BITS_OR                                                        \
    (ECU_PIN_BIT(PIN_CONSOLE_TX) | ECU_PIN_BIT(PIN_CONSOLE_RX)                    \
     | ECU_PIN_BIT(PIN_SPI_SCK) | ECU_PIN_BIT(PIN_SPI_MISO) | ECU_PIN_BIT(PIN_SPI_MOSI) \
     | ECU_PIN_BIT(PIN_CAN_CS) | ECU_PIN_BIT(PIN_CAN_INT) | ECU_PIN_BIT(PIN_SD_CS) \
     | ECU_PIN_BIT(PIN_I2C_SDA) | ECU_PIN_BIT(PIN_I2C_SCL)                        \
     | ECU_PIN_BIT(PIN_RS485_RX) | ECU_PIN_BIT(PIN_RS485_TX) | ECU_PIN_BIT(PIN_RS485_DE) \
     | ECU_PIN_BIT(PIN_GSM_RX) | ECU_PIN_BIT(PIN_GSM_TX)                          \
     | ECU_PIN_BIT(PIN_GSM_PWRKEY) | ECU_PIN_BIT(PIN_GSM_STATUS)                  \
     | ECU_PIN_BIT(PIN_GNSS_RX) | ECU_PIN_BIT(PIN_VBATT_SENSE)                    \
     | ECU_PIN_BIT(PIN_LED_ACQ) | ECU_PIN_BIT(PIN_LED_STORAGE)                    \
     | ECU_PIN_BIT(PIN_LED_LINK) | ECU_PIN_BIT(PIN_LED_CLOUD)                     \
     | ECU_PIN_BIT(PIN_LED_HEARTBEAT))

#define ECU_PINMAP_BITS_SUM                                                       \
    (ECU_PIN_BIT(PIN_CONSOLE_TX) + ECU_PIN_BIT(PIN_CONSOLE_RX)                    \
     + ECU_PIN_BIT(PIN_SPI_SCK) + ECU_PIN_BIT(PIN_SPI_MISO) + ECU_PIN_BIT(PIN_SPI_MOSI) \
     + ECU_PIN_BIT(PIN_CAN_CS) + ECU_PIN_BIT(PIN_CAN_INT) + ECU_PIN_BIT(PIN_SD_CS) \
     + ECU_PIN_BIT(PIN_I2C_SDA) + ECU_PIN_BIT(PIN_I2C_SCL)                        \
     + ECU_PIN_BIT(PIN_RS485_RX) + ECU_PIN_BIT(PIN_RS485_TX) + ECU_PIN_BIT(PIN_RS485_DE) \
     + ECU_PIN_BIT(PIN_GSM_RX) + ECU_PIN_BIT(PIN_GSM_TX)                          \
     + ECU_PIN_BIT(PIN_GSM_PWRKEY) + ECU_PIN_BIT(PIN_GSM_STATUS)                  \
     + ECU_PIN_BIT(PIN_GNSS_RX) + ECU_PIN_BIT(PIN_VBATT_SENSE)                    \
     + ECU_PIN_BIT(PIN_LED_ACQ) + ECU_PIN_BIT(PIN_LED_STORAGE)                    \
     + ECU_PIN_BIT(PIN_LED_LINK) + ECU_PIN_BIT(PIN_LED_CLOUD)                     \
     + ECU_PIN_BIT(PIN_LED_HEARTBEAT))
/* clang-format on */

/** Pins wired to the internal SPI flash on every WROOM-32 module. */
#define ECU_PINMAP_FLASH_PINS                                                                 \
    (ECU_PIN_BIT(6u) | ECU_PIN_BIT(7u) | ECU_PIN_BIT(8u) | ECU_PIN_BIT(9u) | ECU_PIN_BIT(10u) \
     | ECU_PIN_BIT(11u))

/** Pins that can only ever be inputs on the ESP32. */
#define ECU_PINMAP_INPUT_ONLY_PINS (ECU_PIN_BIT(34u) | ECU_PIN_BIT(35u) | ECU_PIN_BIT(36u) | ECU_PIN_BIT(39u))

/** Every pin this design drives as an output. */
#define ECU_PINMAP_OUTPUT_PINS                                                             \
    (ECU_PIN_BIT(PIN_CONSOLE_TX) | ECU_PIN_BIT(PIN_SPI_SCK) | ECU_PIN_BIT(PIN_SPI_MOSI)    \
     | ECU_PIN_BIT(PIN_CAN_CS) | ECU_PIN_BIT(PIN_SD_CS) | ECU_PIN_BIT(PIN_RS485_TX)        \
     | ECU_PIN_BIT(PIN_RS485_DE) | ECU_PIN_BIT(PIN_GSM_TX) | ECU_PIN_BIT(PIN_GSM_PWRKEY)   \
     | ECU_PIN_BIT(PIN_LED_ACQ) | ECU_PIN_BIT(PIN_LED_STORAGE) | ECU_PIN_BIT(PIN_LED_LINK) \
     | ECU_PIN_BIT(PIN_LED_CLOUD) | ECU_PIN_BIT(PIN_LED_HEARTBEAT))

#if defined(__cplusplus)
static_assert(ECU_PINMAP_BITS_OR == ECU_PINMAP_BITS_SUM, "Ecu_PinMap.h: a GPIO is assigned to two signals");
static_assert((ECU_PINMAP_BITS_OR & ECU_PINMAP_FLASH_PINS) == 0uLL,
              "Ecu_PinMap.h: a signal is assigned to a GPIO wired to the internal flash");
static_assert((ECU_PINMAP_BITS_OR & ECU_PIN_BIT(PIN_RESERVED_MTDI)) == 0uLL,
              "Ecu_PinMap.h: GPIO12 (MTDI) must stay unconnected -- see conflict 4");
static_assert((ECU_PINMAP_OUTPUT_PINS & ECU_PINMAP_INPUT_ONLY_PINS) == 0uLL,
              "Ecu_PinMap.h: an input-only GPIO is assigned to an output signal");
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(ECU_PINMAP_BITS_OR == ECU_PINMAP_BITS_SUM, "Ecu_PinMap.h: a GPIO is assigned to two signals");
_Static_assert((ECU_PINMAP_BITS_OR & ECU_PINMAP_FLASH_PINS) == 0uLL,
               "Ecu_PinMap.h: a signal is assigned to a GPIO wired to the internal flash");
_Static_assert((ECU_PINMAP_BITS_OR & ECU_PIN_BIT(PIN_RESERVED_MTDI)) == 0uLL,
               "Ecu_PinMap.h: GPIO12 (MTDI) must stay unconnected -- see conflict 4");
_Static_assert((ECU_PINMAP_OUTPUT_PINS & ECU_PINMAP_INPUT_ONLY_PINS) == 0uLL,
               "Ecu_PinMap.h: an input-only GPIO is assigned to an output signal");
#endif

#endif /* ECU_PINMAP_H */

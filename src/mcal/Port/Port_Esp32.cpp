/**
 * @file    Port_Esp32.cpp
 * @brief   ESP32 platform leaf of the Port driver: pin direction and initial level.
 *
 * Runs once, near the top of EcuM_Init, and is the only place in the project that calls @c pinMode(). That
 * restriction is the point of the module: a second configuration of a pin already in service is nearly
 * invisible in review and trivially prevented by keeping the capability in one file. v1 configured pins from
 * seven different translation units, which is how GPIO2 came to be both the built-in LED and the CAN chip
 * select without anyone noticing.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>

#include "services/Det/Det.h"
#include "Ecu_PinMap.h"
#include "mcal/Port/Port.h"

/*==================================================================================================
 *  Configuration
 *================================================================================================*/

/** One pin's configuration. */
typedef struct
{
    uint8 pin;          /**< GPIO number, or ::PIN_NOT_CONNECTED to skip.     */
    uint8 mode;         /**< Arduino mode: INPUT, OUTPUT, INPUT_PULLUP, ...   */
    sint8 initialLevel; /**< 0 or 1 for an output; -1 for an input.           */
} Port_PinConfigType;

/**
 * @brief The pin table, in the order it is applied.
 *
 * Order matters in one respect. Both SPI chip selects are driven inactive-high *before* anything starts
 * clocking the bus. A chip select left floating -- or worse, sitting at the module's power-on default of low
 * -- makes the attached device read the other device's traffic as its own command stream. That is the most
 * plausible explanation for the intermittent "Card Initialization Failed" lines in the archived v1 device
 * logs: v1 created its `SD` and `MCP_CAN` objects in file-scope constructors, so whichever ran first clocked
 * the bus while the other's `/CS` was still an unconfigured input.
 *
 * Every entry's justification is the pin map's; this table only says which direction each one takes.
 */
static const Port_PinConfigType Port_PinTable[] = {
    /* Chip selects first, inactive high. */
    {PIN_CAN_CS, OUTPUT, 1},
    {PIN_SD_CS, OUTPUT, 1},

    /* RS485 driver enable: low is receive, which is the resting state. A half-duplex bus left with the
     * driver enabled blocks every other device on it, so this must not spend even the startup window high. */
    {PIN_RS485_DE, OUTPUT, 0},

    /* Modem power key is active low and toggles on a pulse, so it rests high. Driving it low here would
     * begin a power toggle before the modem driver is ready to time the pulse. */
    {PIN_GSM_PWRKEY, OUTPUT, 1},

    /* Indicators, all off. HmiSwc owns them from here on. */
    {PIN_LED_ACQ, OUTPUT, 0},
    {PIN_LED_STORAGE, OUTPUT, 0},
    {PIN_LED_LINK, OUTPUT, 0},
    {PIN_LED_CLOUD, OUTPUT, 0},
    {PIN_LED_HEARTBEAT, OUTPUT, 0},

    /* Inputs. The MCP2515 interrupt is open-drain and needs a pull-up, but GPIO35 has no internal one on
     * this part -- an external 10k to 3V3 is fitted, so the pin is a plain input. See docs/07-hardware.md. */
    {PIN_CAN_INT, INPUT, -1},
    {PIN_GSM_STATUS, INPUT, -1},

    /* The battery sense divider and the GNSS receive line. Both are input-only GPIOs. ADC pins are left
     * without a pull so the divider is not loaded; Adc_Init sets the attenuation. */
    {PIN_VBATT_SENSE, INPUT, -1},
    {PIN_GNSS_RX, INPUT, -1},
};

/*==================================================================================================
 *  Platform leaf
 *================================================================================================*/

extern "C" Std_ReturnType Port_Init(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)STD_ARRAY_SIZE(Port_PinTable); i++)
    {
        const Port_PinConfigType *const config = &Port_PinTable[i];

        if (config->pin == PIN_NOT_CONNECTED)
        {
            continue;
        }

        if (config->initialLevel >= 0)
        {
            /* The level is written before the direction, so that the moment the pin becomes an output it
             * already holds the intended value. Configuring first and writing second leaves a window --
             * short, but real -- in which the pin drives its power-on default into the attached device. For a
             * chip select that window is long enough to be sampled. */
            digitalWrite(config->pin, (config->initialLevel != 0) ? HIGH : LOW);
            pinMode(config->pin, config->mode);
            /* And again afterwards: on this part the pin matrix latch is only committed once the pad is
             * configured as an output, so the pre-write alone is not sufficient on every silicon revision. */
            digitalWrite(config->pin, (config->initialLevel != 0) ? HIGH : LOW);
        }
        else
        {
            pinMode(config->pin, config->mode);
        }
    }

    return E_OK;
}

extern "C" Std_ReturnType Port_SetPinDirection(uint8 pin, Port_PinDirectionType direction)
{
    if ((pin == PIN_NOT_CONNECTED) || (pin >= 40u))
    {
        (void)Det_ReportError(MODULE_ID_PORT, INSTANCE_ID_SINGLE, PORT_API_ID_SET_PIN_DIRECTION,
                              PORT_E_PARAM_PIN);
        return E_NOT_OK;
    }

    /* GPIO34..36 and 39 have no output driver. A request to make one an output cannot be honoured, and
     * silently ignoring it would leave the caller writing to a pin that never changes -- a fault that
     * presents as dead hardware. Reported so it fails at the call site instead. */
    if ((direction == PORT_PIN_OUT) && ((ECU_PIN_BIT(pin) & ECU_PINMAP_INPUT_ONLY_PINS) != 0uLL))
    {
        (void)Det_ReportError(MODULE_ID_PORT, INSTANCE_ID_SINGLE, PORT_API_ID_SET_PIN_DIRECTION,
                              PORT_E_DIRECTION_UNCHANGEABLE);
        return E_NOT_OK;
    }

    pinMode(pin, (direction == PORT_PIN_OUT) ? OUTPUT : INPUT);
    return E_OK;
}

extern "C" Std_ReturnType Port_SetPinPull(uint8 pin, Port_PullType pull)
{
    if ((pin == PIN_NOT_CONNECTED) || (pin >= 40u))
    {
        (void)Det_ReportError(MODULE_ID_PORT, INSTANCE_ID_SINGLE, PORT_API_ID_SET_PIN_DIRECTION,
                              PORT_E_PARAM_PIN);
        return E_NOT_OK;
    }

    /* The input-only pins have no internal pull resistors either, which is why the CAN interrupt carries an
     * external one. Requesting a pull on them would appear to succeed and change nothing. */
    if ((pull != PORT_PULL_NONE) && ((ECU_PIN_BIT(pin) & ECU_PINMAP_INPUT_ONLY_PINS) != 0uLL))
    {
        (void)Det_ReportError(MODULE_ID_PORT, INSTANCE_ID_SINGLE, PORT_API_ID_SET_PIN_DIRECTION,
                              PORT_E_PARAM_PIN);
        return E_NOT_OK;
    }

    switch (pull)
    {
    case PORT_PULL_UP:
        pinMode(pin, INPUT_PULLUP);
        break;

    case PORT_PULL_DOWN:
        pinMode(pin, INPUT_PULLDOWN);
        break;

    case PORT_PULL_NONE:
    default:
        pinMode(pin, INPUT);
        break;
    }

    return E_OK;
}

extern "C" void Port_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = PORT_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_PORT;
        versioninfo->sw_major_version = PORT_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = PORT_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = PORT_SW_PATCH_VERSION;
    }
}

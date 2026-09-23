/**
 * @file    Port.h
 * @brief   AUTOSAR Port driver (SWS_PortDriver) -- pin direction and mode setup.
 *
 * Runs once, early in ::EcuM_Init, and puts every pin named in Ecu_PinMap.h into its
 * configured direction, drive mode and initial level. Nothing else in the project is
 * allowed to call @c pinMode(): a second, later configuration of a pin already in
 * use is a class of fault that is very hard to see in review and trivial to prevent
 * by keeping the capability in one module.
 *
 * @par Initial levels matter
 * Both SPI chip selects are driven high before the SPI peripheral is started. A chip
 * select that floats low while the bus is initialised makes the attached device
 * interpret the initialisation traffic as a command, which is one plausible
 * explanation for the intermittent SD mount failures in the v1 logs.
 *
 * @req SWREQ-SYS-0020, SWREQ-SYS-0021
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef PORT_H
#define PORT_H

#include "Autosar_ModuleIds.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_VENDOR_ID 0xFFFEu
#define PORT_AR_RELEASE_MAJOR_VERSION 4u
#define PORT_AR_RELEASE_MINOR_VERSION 4u
#define PORT_SW_MAJOR_VERSION 2u
#define PORT_SW_MINOR_VERSION 0u
#define PORT_SW_PATCH_VERSION 0u

#define PORT_API_ID_INIT 0x00u
#define PORT_API_ID_SET_PIN_DIRECTION 0x01u

#define PORT_E_PARAM_PIN 0x0Au
#define PORT_E_DIRECTION_UNCHANGEABLE 0x0Bu

/** Pin direction. */
typedef enum
{
    PORT_PIN_IN = 0,  /**< Input.  */
    PORT_PIN_OUT = 1  /**< Output. */
} Port_PinDirectionType;

/** Internal pull resistor selection. */
typedef enum
{
    PORT_PULL_NONE = 0, /**< High impedance.        */
    PORT_PULL_UP = 1,   /**< Internal pull-up.      */
    PORT_PULL_DOWN = 2  /**< Internal pull-down.    */
} Port_PullType;

/**
 * @brief Configure every pin in the ECU pin map.
 *
 * @return E_OK if every pin was accepted by the hardware; E_NOT_OK if any was
 *         rejected (the remaining pins are still configured, so a single bad entry
 *         does not leave the board half-initialised).
 */
CHECK_RETURN Std_ReturnType Port_Init(void);

/**
 * @brief Change one pin's direction after ::Port_Init.
 *
 * Only pins declared as direction-changeable accept this; the rest report
 * ::PORT_E_DIRECTION_UNCHANGEABLE. Used by nothing in the current design and kept
 * because AUTOSAR requires it and because a future bidirectional bus would need it.
 */
CHECK_RETURN Std_ReturnType Port_SetPinDirection(uint8 pin, Port_PinDirectionType direction);

/**
 * @brief Configure one pin's pull resistor.
 */
CHECK_RETURN Std_ReturnType Port_SetPinPull(uint8 pin, Port_PullType pull);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Port_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* PORT_H */

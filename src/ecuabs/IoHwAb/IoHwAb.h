/**
 * @file    IoHwAb.h
 * @brief   I/O hardware abstraction -- calibrated analogue values and logical indicators.
 *
 * Turns raw driver-level I/O into quantities the application can use: ADC counts become calibrated
 * millivolts, and named indicators replace pin numbers. Sits above the MCAL so that the calibration
 * and the indicator semantics are configuration rather than code.
 *
 * @par Why the voltage scaling lives here
 * v1 computed the auxiliary battery voltage inside the RS485 module, of all places, as
 * @c ((value / 5) * 115) / 4095 with every constant inline. That put a vehicle calibration in a bus
 * driver, made it unadjustable without a rebuild, and meant no two units could be trimmed
 * differently even though their dividers have real tolerances. Here the divider ratio and a per-unit
 * offset come from NvM, so a technician can trim a unit in the field and the correction survives a
 * firmware update.
 *
 * @req SWREQ-SNS-0030 .. SWREQ-SNS-0038, SWREQ-HMI-0001 .. SWREQ-HMI-0004
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef IOHWAB_H
#define IOHWAB_H

#include "base/Autosar_ModuleIds.h"
#include "ecuabs/IoHwAb/IoHwAb_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOHWAB_VENDOR_ID 0xFFFEu
#define IOHWAB_SW_MAJOR_VERSION 2u
#define IOHWAB_SW_MINOR_VERSION 0u
#define IOHWAB_SW_PATCH_VERSION 0u

#define IOHWAB_API_ID_INIT 0x00u
#define IOHWAB_API_ID_READ_VOLTAGE 0x20u
#define IOHWAB_API_ID_SET_INDICATOR 0x21u
#define IOHWAB_API_ID_CONVERT 0x22u

#define IOHWAB_E_UNINIT E_UNINIT
#define IOHWAB_E_PARAM_POINTER E_PARAM_POINTER
#define IOHWAB_E_PARAM_INDICATOR 0x20u
#define IOHWAB_E_READ_FAILED 0x21u
#define IOHWAB_E_BAD_CALIBRATION 0x22u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/**
 * @brief Logical status indicators.
 *
 * Named by what they mean, not by where they are wired. The pin assignment is in Ecu_PinMap.h and
 * the channel mapping in Dio_Cfg.h, so moving an LED is a one-line change that cannot miss a call
 * site.
 */
typedef enum
{
    IOHWAB_INDICATOR_ACQUISITION = 0, /**< A data record was acquired.        */
    IOHWAB_INDICATOR_STORAGE = 1,     /**< A record was written to the card.  */
    IOHWAB_INDICATOR_LINK = 2,        /**< An IP bearer is up.                */
    IOHWAB_INDICATOR_CLOUD = 3,       /**< A broker session is established.   */
    IOHWAB_INDICATOR_HEARTBEAT = 4    /**< The scheduler is running.          */
} IoHwAb_IndicatorType;

/** Number of configured indicators. */
#define IOHWAB_INDICATOR_COUNT 5u

/** Analogue reading with its validity. */
typedef struct
{
    uint16 milliVolts; /**< Calibrated value. Meaningless unless @c valid is TRUE. */
    uint16 rawCounts;  /**< The ADC counts it came from, for diagnostics.          */
    boolean valid;     /**< FALSE if the conversion failed.                        */
} IoHwAb_VoltageType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Load the calibration from NvM and put every indicator into its inactive state.
 * @return E_OK on success; E_NOT_OK if the calibration is unusable.
 */
CHECK_RETURN Std_ReturnType IoHwAb_Init(void);

/**
 * @brief Read the auxiliary battery voltage.
 *
 * @param[out] voltage Destination. On a conversion failure @c valid is cleared and
 *                     @c milliVolts is left at its previous value rather than zeroed, so a caller
 *                     that ignores the flag reads a stale value instead of an apparent flat battery.
 * @return E_OK if the reading succeeded; E_NOT_OK otherwise.
 */
CHECK_RETURN Std_ReturnType IoHwAb_ReadAuxVoltage(IoHwAb_VoltageType *voltage);

/**
 * @brief Convert ADC counts to millivolts under the current calibration.
 *
 * Pure function, exposed so the conversion can be tested across the full count range independently
 * of the hardware.
 *
 * @param rawCounts    ADC counts, 0 .. ::ADC_MAX_COUNT.
 * @param dividerMilli External divider ratio x 1000.
 * @param offsetMv     Per-unit offset trim, millivolts, applied after scaling.
 * @return Millivolts, saturated at 0 and 0xFFFF rather than wrapping.
 */
uint16 IoHwAb_CountsToMilliVolts(uint16 rawCounts, uint16 dividerMilli, sint16 offsetMv);

/**
 * @brief Drive @p indicator on or off.
 *
 * Handles the active level, so callers never need to know whether the LED is wired active high or
 * active low.
 */
CHECK_RETURN Std_ReturnType IoHwAb_SetIndicator(IoHwAb_IndicatorType indicator, boolean on);

/** Invert @p indicator's current state. Used for blink patterns. */
CHECK_RETURN Std_ReturnType IoHwAb_ToggleIndicator(IoHwAb_IndicatorType indicator);

/** TRUE if @p indicator is currently lit. */
boolean IoHwAb_GetIndicator(IoHwAb_IndicatorType indicator);

/** Turn every indicator off. Used on the shutdown path. */
void IoHwAb_AllIndicatorsOff(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void IoHwAb_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* IOHWAB_H */

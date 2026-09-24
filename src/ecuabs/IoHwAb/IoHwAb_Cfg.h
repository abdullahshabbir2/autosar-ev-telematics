/**
 * @file    IoHwAb_Cfg.h
 * @brief   I/O abstraction configuration: indicator channels and voltage limits.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef IOHWAB_CFG_H
#define IOHWAB_CFG_H

#include "Adc_Cfg.h"
#include "Dio_Cfg.h"
#include "Std_Types.h"

#define IOHWAB_DEV_ERROR_DETECT STD_ON

/** ADC channel the auxiliary battery divider feeds. */
#define IOHWAB_CHANNEL_VBATT ADC_CHANNEL_VBATT

/*==================================================================================================
 *  Voltage conversion
 *
 *  The ADC reference and the external divider together set the scaling:
 *
 *      pin millivolts     = counts * ADC_REFERENCE_MV / ADC_MAX_COUNT
 *      battery millivolts = pin millivolts * dividerRatio
 *
 *  where dividerRatio is held as an integer scaled by 1000 so that no floating point is involved.
 *  The ratio comes from NvM, which is what makes a per-unit trim possible; the reference is a
 *  property of the silicon and stays here.
 *================================================================================================*/

/**
 * @brief ADC full-scale reference, in millivolts.
 *
 * 3300 nominal. The ESP32's SAR ADC is not linear across its whole range and its true reference
 * varies by part, which is why a per-unit offset trim exists in the calibration block rather than a
 * single number being assumed correct for every board.
 */
#define IOHWAB_ADC_REFERENCE_MV 3300uL

/** Largest value the conversion will report, for saturation rather than wraparound. */
#define IOHWAB_MAX_MILLIVOLTS 65535uL

/*==================================================================================================
 *  Plausibility
 *================================================================================================*/

/**
 * @brief Auxiliary supply below which a low-voltage event is reported, in millivolts.
 *
 * 11 000. A 12 V lead-acid auxiliary battery at 11.0 V is genuinely discharged; below about 10.5 V
 * the ESP32's regulator starts to drop out and the SD card becomes unreliable, so reporting at 11.0 V
 * gives warning before data collection is affected.
 */
#define IOHWAB_VBATT_LOW_MV 11000u

/**
 * @brief Auxiliary supply above which the reading is treated as a sensor fault, in millivolts.
 *
 * 16 000. A 12 V system on charge reaches about 14.4 V; anything above 16 V means the divider or the
 * ADC channel has failed rather than that the battery is at 16 V.
 */
#define IOHWAB_VBATT_IMPLAUSIBLE_MV 16000u

/*==================================================================================================
 *  Indicator channels
 *================================================================================================*/

#define IOHWAB_DIO_ACQUISITION DIO_CHANNEL_LED_ACQ
#define IOHWAB_DIO_STORAGE DIO_CHANNEL_LED_STORAGE
#define IOHWAB_DIO_LINK DIO_CHANNEL_LED_LINK
#define IOHWAB_DIO_CLOUD DIO_CHANNEL_LED_CLOUD
#define IOHWAB_DIO_HEARTBEAT DIO_CHANNEL_LED_HEARTBEAT

/** Level that lights an indicator. See the strapping-pin note in Ecu_PinMap.h. */
#define IOHWAB_INDICATOR_ON_LEVEL DIO_LED_ACTIVE_LEVEL
#define IOHWAB_INDICATOR_OFF_LEVEL DIO_LED_INACTIVE_LEVEL

#endif /* IOHWAB_CFG_H */

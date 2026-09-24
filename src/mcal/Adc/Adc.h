/**
 * @file    Adc.h
 * @brief   AUTOSAR ADC driver (SWS_ADCDriver) -- ESP32 ADC1 adaptation.
 *
 * Reads the auxiliary battery voltage. Small module, two decisions worth recording:
 *
 * @par Oversampling with a settling gap
 * The ESP32 SAR ADC is noisy -- roughly +/-30 LSB of spread on a quiet input. v1
 * averaged five back-to-back @c analogRead() calls, which barely helps: consecutive
 * conversions share the same sample-and-hold disturbance, so their errors are
 * correlated and averaging them does not reduce the noise much.
 * ::Adc_ReadChannel takes ::ADC_OVERSAMPLE_COUNT samples with a short gap between
 * them so the noise decorrelates, then discards the extreme pair and averages the
 * rest, which rejects the occasional single-sample outlier that an unfiltered mean
 * would pass straight through into the telemetry record.
 *
 * @par Raw counts, not volts
 * This module returns raw counts; the calibrated conversion to millivolts lives in
 * IoHwAb. Keeping the scaling out of the MCAL means the divider ratio and the
 * per-unit calibration trim are configuration data that NvM can hold and a technician
 * can adjust, rather than a constant compiled into a driver. v1 had
 * @c (value/5 * 115) / 4095 inline in the RS485 module -- both the wrong layer and
 * impossible to trim in the field.
 *
 * @req SWREQ-SNS-0030, SWREQ-SNS-0031
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ADC_H
#define ADC_H

#include "mcal/Adc/Adc_Cfg.h"
#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_VENDOR_ID 0xFFFEu
#define ADC_AR_RELEASE_MAJOR_VERSION 4u
#define ADC_AR_RELEASE_MINOR_VERSION 4u
#define ADC_SW_MAJOR_VERSION 2u
#define ADC_SW_MINOR_VERSION 0u
#define ADC_SW_PATCH_VERSION 0u

#define ADC_API_ID_INIT 0x00u
#define ADC_API_ID_READ_CHANNEL 0x20u
#define ADC_API_ID_READ_CHANNEL_RAW 0x21u

#define ADC_E_PARAM_CHANNEL 0x0Au
#define ADC_E_PARAM_POINTER E_PARAM_POINTER
#define ADC_E_UNINIT E_UNINIT
#define ADC_E_CONVERSION_FAILED 0x20u

/** Logical ADC channel; values are defined in Adc_Cfg.h. */
typedef uint8 Adc_ChannelType;

/** A raw conversion result, 0 .. ::ADC_MAX_COUNT. */
typedef uint16 Adc_ValueType;

/**
 * @brief Configure the ADC unit and every configured channel.
 * @return E_OK on success.
 */
CHECK_RETURN Std_ReturnType Adc_Init(void);

/**
 * @brief Perform an oversampled, outlier-trimmed conversion on @p channel.
 *
 * @param[in]  channel One of the ADC_CHANNEL_* symbols.
 * @param[out] value   Averaged raw count.
 * @return E_OK on success; E_NOT_OK on an invalid channel, a NULL @p value, or a
 *         hardware conversion failure.
 *
 * @note On failure @p value is left untouched rather than zeroed. A caller that
 *       ignores the status then reads its previous sample, instead of a zero that
 *       would be indistinguishable from a flat battery.
 */
CHECK_RETURN Std_ReturnType Adc_ReadChannel(Adc_ChannelType channel, Adc_ValueType *value);

/**
 * @brief Single unaveraged conversion, for diagnostics and calibration.
 * @copydetails Adc_ReadChannel
 */
CHECK_RETURN Std_ReturnType Adc_ReadChannelRaw(Adc_ChannelType channel, Adc_ValueType *value);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Adc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* ADC_H */

/**
 * @file    Adc_Cfg.h
 * @brief   ADC pre-compile configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ADC_CFG_H
#define ADC_CFG_H

#include "Ecu_PinMap.h"
#include "Std_Types.h"

#define ADC_DEV_ERROR_DETECT STD_ON

/** Auxiliary battery voltage sense: ADC1_CH0 on GPIO36. */
#define ADC_CHANNEL_VBATT ((Adc_ChannelType)PIN_VBATT_SENSE)

#define ADC_CONFIGURED_CHANNEL_COUNT 1u

/** 12-bit SAR resolution: counts run 0 .. 4095. */
#define ADC_RESOLUTION_BITS 12u
#define ADC_MAX_COUNT 4095u

/**
 * @brief Samples taken per ::Adc_ReadChannel call.
 *
 * Seven, so that trimming the highest and lowest leaves an odd five to average and
 * the trimmed mean is exact in integer arithmetic with no rounding bias. At
 * ::ADC_SAMPLE_GAP_US spacing the whole call costs about 350 us, negligible against
 * the 3 s acquisition period.
 */
#define ADC_OVERSAMPLE_COUNT 7u

/** Samples discarded from each end of the sorted set before averaging. */
#define ADC_TRIM_COUNT 1u

/**
 * @brief Gap between oversamples, in microseconds.
 *
 * 50 us is comfortably longer than the SAR sample-and-hold recovery time, which is
 * what makes successive samples independent enough for averaging to actually reduce
 * noise rather than merely repeat it.
 */
#define ADC_SAMPLE_GAP_US 50u

/**
 * @brief Attenuation setting, selecting a nominal 0 .. 3.3 V input span.
 *
 * Corresponds to ESP32 @c ADC_ATTEN_DB_11. The genuinely linear span is closer to
 * 0.15 .. 2.45 V before the transfer function bends, which is why the external
 * divider is sized to land the full battery range inside that window rather than at
 * the nominal 3.3 V full scale. The divider calculation is tabulated in
 * @ref docs/hardware/pinmap.md.
 */
#define ADC_ATTENUATION_SETTING 3u

#endif /* ADC_CFG_H */

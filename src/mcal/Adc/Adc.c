/**
 * @file    Adc.c
 * @brief   Platform-independent part of the ADC driver: oversampling and trimming.
 *
 * ::Adc_Init and ::Adc_ReadChannelRaw are the platform leaf and live in Adc_Esp32.cpp
 * (target) or test/support/Stub_Adc.c (host). The filter below is the part worth
 * testing, so it is written once, here, in portable C.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "mcal/Adc/Adc.h"

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"

/** Samples remaining after the extremes are discarded. */
#define ADC_KEPT_SAMPLE_COUNT (ADC_OVERSAMPLE_COUNT - (2u * ADC_TRIM_COUNT))

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(ADC_OVERSAMPLE_COUNT > (2u * ADC_TRIM_COUNT),
               "Adc_Cfg.h: trimming would discard every sample");
_Static_assert((ADC_KEPT_SAMPLE_COUNT % 2u) == 1u,
               "Adc_Cfg.h: keep an odd number of samples so the mean has no rounding bias");
#endif

/**
 * @brief Insertion-sort @p count samples ascending, in place.
 *
 * Insertion sort rather than anything cleverer because @p count is 7: the comparison
 * count is trivial, the code has no recursion and no scratch buffer, and its worst case
 * is as predictable as its best, which is what a bounded-WCET driver wants.
 */
STATIC void Adc_SortAscending(Adc_ValueType *samples, uint8 count)
{
    uint8 i;

    for (i = 1u; i < count; i++)
    {
        const Adc_ValueType key = samples[i];
        uint8 j = i;

        while ((j > 0u) && (samples[j - 1u] > key))
        {
            samples[j] = samples[j - 1u];
            j--;
        }
        samples[j] = key;
    }
}

Std_ReturnType Adc_ReadChannel(Adc_ChannelType channel, Adc_ValueType *value)
{
    Adc_ValueType samples[ADC_OVERSAMPLE_COUNT];
    uint32 accumulator = 0u;
    uint8 i;

    DET_CHECK_RETURN(value != NULL_PTR, MODULE_ID_ADC, INSTANCE_ID_SINGLE,
                     ADC_API_ID_READ_CHANNEL, ADC_E_PARAM_POINTER, E_NOT_OK);

    for (i = 0u; i < ADC_OVERSAMPLE_COUNT; i++)
    {
        if (Adc_ReadChannelRaw(channel, &samples[i]) != E_OK)
        {
            /* Abandon the whole reading rather than average the samples collected so
             * far. A partial average would be indistinguishable from a good one, and a
             * caller that sees E_NOT_OK keeps its previous value -- which is the more
             * useful behaviour for a slowly-varying quantity like battery voltage. */
            return E_NOT_OK;
        }

        /* The gap is what makes averaging effective: back-to-back conversions share the
         * same sample-and-hold disturbance, so their errors are correlated and the mean
         * of them is barely quieter than one sample. */
        if ((i + 1u) < ADC_OVERSAMPLE_COUNT)
        {
            Gpt_DelayUs(ADC_SAMPLE_GAP_US);
        }
    }

    Adc_SortAscending(samples, ADC_OVERSAMPLE_COUNT);

    for (i = ADC_TRIM_COUNT; i < (ADC_OVERSAMPLE_COUNT - ADC_TRIM_COUNT); i++)
    {
        accumulator += (uint32)samples[i];
    }

    *value = (Adc_ValueType)(accumulator / ADC_KEPT_SAMPLE_COUNT);
    return E_OK;
}

void Adc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = ADC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_ADC;
        versioninfo->sw_major_version = ADC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = ADC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = ADC_SW_PATCH_VERSION;
    }
}

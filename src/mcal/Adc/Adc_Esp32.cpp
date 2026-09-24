/**
 * @file    Adc_Esp32.cpp
 * @brief   ESP32 platform leaf of the ADC driver: one raw conversion.
 *
 * The oversampling, outlier trimming and averaging that turn this into a usable reading are in Adc.c, where
 * the host suite tests them against synthetic sample sets including the noise patterns this part produces.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>

#include "mcal/Adc/Adc.h"
#include "mcal/Adc/Adc_Cfg.h"
#include "services/Det/Det.h"

static boolean Adc_Initialised = FALSE;

extern "C" Std_ReturnType Adc_Init(void)
{
    /* 12-bit resolution and 11 dB attenuation, giving a nominal 0 .. 3.3 V span across 0 .. 4095 counts. The
     * divider on the sense pin is sized in Ecu_PinMap.h to keep the pack voltage inside that span.
     *
     * The sense pin is on ADC1, and that is not incidental. ADC2 shares its hardware with the WiFi radio and
     * returns whatever was last converted -- with no error indication at all -- whenever the radio is active,
     * which on this ECU is nearly always. A design that had put the battery sense on ADC2 would read
     * plausible, stable, entirely fictional voltages, and the oversampling above would happily average them. */
    analogReadResolution((int)ADC_RESOLUTION_BITS);
    analogSetPinAttenuation((uint8_t)ADC_CHANNEL_VBATT, ADC_11db);

    Adc_Initialised = TRUE;
    return E_OK;
}

extern "C" Std_ReturnType Adc_ReadChannelRaw(Adc_ChannelType channel, Adc_ValueType *value)
{
    int raw;

#if (ADC_DEV_ERROR_DETECT == STD_ON)
    if (value == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_ADC, INSTANCE_ID_SINGLE, ADC_API_ID_READ_CHANNEL_RAW,
                              ADC_E_PARAM_POINTER);
        return E_NOT_OK;
    }
    if (Adc_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_ADC, INSTANCE_ID_SINGLE, ADC_API_ID_READ_CHANNEL_RAW, ADC_E_UNINIT);
        return E_NOT_OK;
    }
    if (channel != ADC_CHANNEL_VBATT)
    {
        (void)Det_ReportError(MODULE_ID_ADC, INSTANCE_ID_SINGLE, ADC_API_ID_READ_CHANNEL_RAW,
                              ADC_E_PARAM_CHANNEL);
        return E_NOT_OK;
    }
#else
    if (value == NULL_PTR)
    {
        return E_NOT_OK;
    }
#endif

    raw = analogRead((uint8_t)channel);
    if (raw < 0)
    {
        /* A runtime error rather than a development error: the argument was valid, the hardware did not
         * answer. Adc.c treats a failed sample as a failed read of the whole channel rather than averaging
         * over the remaining ones, because a divider that has come loose reads plausibly low and averaging
         * would hide it. */
        (void)Det_ReportRuntimeError(MODULE_ID_ADC, INSTANCE_ID_SINGLE, ADC_API_ID_READ_CHANNEL_RAW,
                                     ADC_E_CONVERSION_FAILED);
        return E_NOT_OK;
    }

    /* Saturate rather than truncate. A reading above full scale can only come from a driver change, and a
     * wrapped value would present as a near-zero pack voltage -- an alarm condition -- instead of a
     * pegged-high one. */
    *value = (Adc_ValueType)((raw > (int)ADC_MAX_COUNT) ? (int)ADC_MAX_COUNT : raw);
    return E_OK;
}

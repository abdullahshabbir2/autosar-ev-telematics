/**
 * @file    IoHwAb.c
 * @brief   I/O hardware abstraction implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "IoHwAb.h"

#include "Adc.h"
#include "Dem.h"
#include "Det.h"
#include "Dio.h"
#include "NvM.h"

/*==================================================================================================
 *  Indicator channel table
 *================================================================================================*/

STATIC const Dio_ChannelType IoHwAb_IndicatorChannel[IOHWAB_INDICATOR_COUNT] = {
    IOHWAB_DIO_ACQUISITION, IOHWAB_DIO_STORAGE, IOHWAB_DIO_LINK, IOHWAB_DIO_CLOUD,
    IOHWAB_DIO_HEARTBEAT,
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(STD_ARRAY_SIZE(IoHwAb_IndicatorChannel) == IOHWAB_INDICATOR_COUNT,
               "indicator channel table does not match IOHWAB_INDICATOR_COUNT");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean IoHwAb_Initialised = FALSE;
STATIC uint16 IoHwAb_DividerMilli;
STATIC sint16 IoHwAb_OffsetMv;
STATIC IoHwAb_VoltageType IoHwAb_LastVoltage;
STATIC uint16 IoHwAb_ConsecutiveReadFailures;

/*==================================================================================================
 *  Conversion
 *================================================================================================*/

uint16 IoHwAb_CountsToMilliVolts(uint16 rawCounts, uint16 dividerMilli, sint16 offsetMv)
{
    uint64 pinMilliVolts;
    sint64 batteryMilliVolts;

    if (dividerMilli == 0u)
    {
        return 0u;
    }

    /* Pin voltage first, in 64 bits so the intermediate product cannot overflow: counts reach 4095
     * and the reference is 3300, so the product is about 1.4e7 -- well inside 32 bits, but the
     * subsequent multiply by the divider ratio would not be. */
    pinMilliVolts = ((uint64)rawCounts * (uint64)IOHWAB_ADC_REFERENCE_MV) / (uint64)ADC_MAX_COUNT;

    /* Then the external divider, whose ratio is scaled by 1000. */
    batteryMilliVolts = (sint64)((pinMilliVolts * (uint64)dividerMilli) / 1000uLL);

    /* The per-unit trim is applied last, on the battery-side value, because that is where the
     * measurement is compared against a specification. Applying it to the pin voltage would make the
     * trim's effect depend on the divider ratio. */
    batteryMilliVolts += (sint64)offsetMv;

    /* Saturate rather than wrap. A negative result is physically impossible and a wrapped one would
     * read as a very high voltage, which is the opposite of the truth. */
    if (batteryMilliVolts < 0)
    {
        return 0u;
    }
    if (batteryMilliVolts > (sint64)IOHWAB_MAX_MILLIVOLTS)
    {
        return (uint16)IOHWAB_MAX_MILLIVOLTS;
    }

    return (uint16)batteryMilliVolts;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType IoHwAb_Init(void)
{
    NvM_CalibrationType calibration;

    IoHwAb_LastVoltage.milliVolts = 0u;
    IoHwAb_LastVoltage.rawCounts = 0u;
    IoHwAb_LastVoltage.valid = FALSE;
    IoHwAb_ConsecutiveReadFailures = 0u;

    if (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
    {
        return E_NOT_OK;
    }

    IoHwAb_DividerMilli = calibration.vbattDividerMilli;
    IoHwAb_OffsetMv = calibration.vbattOffsetMv;

    if (IoHwAb_DividerMilli == 0u)
    {
        /* A zero ratio would report every voltage as zero, which reads as a flat battery on every
         * unit. Better to refuse to start the module and raise the fault. */
        (void)Det_ReportError(MODULE_ID_IOHWAB, INSTANCE_ID_SINGLE, IOHWAB_API_ID_INIT,
                              IOHWAB_E_BAD_CALIBRATION);
        return E_NOT_OK;
    }

    IoHwAb_AllIndicatorsOff();
    IoHwAb_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType IoHwAb_ReadAuxVoltage(IoHwAb_VoltageType *voltage)
{
    Adc_ValueType counts = 0u;

    DET_CHECK_RETURN(IoHwAb_Initialised != FALSE, MODULE_ID_IOHWAB, INSTANCE_ID_SINGLE,
                     IOHWAB_API_ID_READ_VOLTAGE, IOHWAB_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(voltage != NULL_PTR, MODULE_ID_IOHWAB, INSTANCE_ID_SINGLE,
                     IOHWAB_API_ID_READ_VOLTAGE, IOHWAB_E_PARAM_POINTER, E_NOT_OK);

    if (Adc_ReadChannel(IOHWAB_CHANNEL_VBATT, &counts) != E_OK)
    {
        /* The previous reading is returned with its flag cleared, rather than a zero. A caller that
         * ignores the flag then sees a stale value instead of an apparent flat battery, which is the
         * less harmful of the two wrong answers. */
        IoHwAb_LastVoltage.valid = FALSE;
        *voltage = IoHwAb_LastVoltage;

        if (IoHwAb_ConsecutiveReadFailures < 0xFFFFu)
        {
            IoHwAb_ConsecutiveReadFailures++;
        }
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_VBATT_SENSE_FAULT, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
        return E_NOT_OK;
    }

    IoHwAb_ConsecutiveReadFailures = 0u;
    IoHwAb_LastVoltage.rawCounts = counts;
    IoHwAb_LastVoltage.milliVolts =
        IoHwAb_CountsToMilliVolts(counts, IoHwAb_DividerMilli, IoHwAb_OffsetMv);
    IoHwAb_LastVoltage.valid = TRUE;

    /* An implausibly high reading means the divider or the channel has failed, not that the battery
     * is at that voltage. Distinguished from a low battery, because the two need different responses:
     * one is a vehicle problem and the other is an ECU problem. */
    if (IoHwAb_LastVoltage.milliVolts > (uint16)IOHWAB_VBATT_IMPLAUSIBLE_MV)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_VBATT_SENSE_FAULT, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
    }
    else
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_VBATT_SENSE_FAULT, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_PASSED));

        if (IoHwAb_LastVoltage.milliVolts < (uint16)IOHWAB_VBATT_LOW_MV)
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_VBATT_LOW, INSTANCE_ID_SINGLE,
                                           DEM_EVENT_STATUS_FAILED));
        }
        else
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_VBATT_LOW, INSTANCE_ID_SINGLE,
                                           DEM_EVENT_STATUS_PASSED));
        }
    }

    *voltage = IoHwAb_LastVoltage;
    return E_OK;
}

Std_ReturnType IoHwAb_SetIndicator(IoHwAb_IndicatorType indicator, boolean on)
{
    DET_CHECK_RETURN(indicator < (IoHwAb_IndicatorType)IOHWAB_INDICATOR_COUNT, MODULE_ID_IOHWAB,
                     (uint8)indicator, IOHWAB_API_ID_SET_INDICATOR, IOHWAB_E_PARAM_INDICATOR,
                     E_NOT_OK);

    Dio_WriteChannel(IoHwAb_IndicatorChannel[indicator],
                     (on != FALSE) ? (Dio_LevelType)IOHWAB_INDICATOR_ON_LEVEL
                                   : (Dio_LevelType)IOHWAB_INDICATOR_OFF_LEVEL);
    return E_OK;
}

Std_ReturnType IoHwAb_ToggleIndicator(IoHwAb_IndicatorType indicator)
{
    DET_CHECK_RETURN(indicator < (IoHwAb_IndicatorType)IOHWAB_INDICATOR_COUNT, MODULE_ID_IOHWAB,
                     (uint8)indicator, IOHWAB_API_ID_SET_INDICATOR, IOHWAB_E_PARAM_INDICATOR,
                     E_NOT_OK);

    (void)Dio_FlipChannel(IoHwAb_IndicatorChannel[indicator]);
    return E_OK;
}

boolean IoHwAb_GetIndicator(IoHwAb_IndicatorType indicator)
{
    if (indicator >= (IoHwAb_IndicatorType)IOHWAB_INDICATOR_COUNT)
    {
        return FALSE;
    }
    return (Dio_ReadChannel(IoHwAb_IndicatorChannel[indicator]) ==
            (Dio_LevelType)IOHWAB_INDICATOR_ON_LEVEL)
               ? TRUE
               : FALSE;
}

void IoHwAb_AllIndicatorsOff(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)IOHWAB_INDICATOR_COUNT; i++)
    {
        Dio_WriteChannel(IoHwAb_IndicatorChannel[i], (Dio_LevelType)IOHWAB_INDICATOR_OFF_LEVEL);
    }
}

void IoHwAb_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = IOHWAB_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_IOHWAB;
        versioninfo->sw_major_version = IOHWAB_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = IOHWAB_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = IOHWAB_SW_PATCH_VERSION;
    }
}

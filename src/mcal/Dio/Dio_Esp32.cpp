/**
 * @file    Dio_Esp32.cpp
 * @brief   ESP32 platform leaf of the DIO driver.
 *
 * Channel ids are GPIO numbers, so validity is a bit test against the pin map's own mask. That is not just
 * convenient: it makes it impossible for a channel to be accepted here and absent from the schematic, because
 * both facts come from the same expression in Ecu_PinMap.h.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>

#include "services/Det/Det.h"
#include "mcal/Dio/Dio.h"
#include "mcal/Dio/Dio_Cfg.h"
#include "Ecu_PinMap.h"

/** TRUE if @p channelId names a pin this build actually configures. */
static boolean Dio_ChannelIsConfigured(Dio_ChannelType channelId)
{
    if (channelId > (Dio_ChannelType)DIO_MAX_CHANNEL_ID)
    {
        return FALSE;
    }

    return ((ECU_PIN_BIT(channelId) & ECU_PINMAP_BITS_OR) != 0uLL) ? TRUE : FALSE;
}

extern "C" Dio_LevelType Dio_ReadChannel(Dio_ChannelType channelId)
{
#if (DIO_DEV_ERROR_DETECT == STD_ON)
    if (Dio_ChannelIsConfigured(channelId) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_DIO, INSTANCE_ID_SINGLE, DIO_API_ID_READ_CHANNEL,
                              DIO_E_PARAM_INVALID_CHANNEL_ID);
        return (Dio_LevelType)STD_LOW;
    }
#endif

    return (digitalRead((uint8_t)channelId) == HIGH) ? (Dio_LevelType)STD_HIGH : (Dio_LevelType)STD_LOW;
}

extern "C" void Dio_WriteChannel(Dio_ChannelType channelId, Dio_LevelType level)
{
#if (DIO_DEV_ERROR_DETECT == STD_ON)
    if (Dio_ChannelIsConfigured(channelId) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_DIO, INSTANCE_ID_SINGLE, DIO_API_ID_WRITE_CHANNEL,
                              DIO_E_PARAM_INVALID_CHANNEL_ID);
        return;
    }

    /* Writing an input-only pin is electrically a no-op, so without this check the caller would see success
     * and no effect. The distinction matters because the two input-only channels this build has -- the CAN
     * interrupt and the modem status line -- are exactly the sort of pin someone later mistakes for an
     * output. */
    if ((ECU_PIN_BIT(channelId) & ECU_PINMAP_INPUT_ONLY_PINS) != 0uLL)
    {
        (void)Det_ReportError(MODULE_ID_DIO, INSTANCE_ID_SINGLE, DIO_API_ID_WRITE_CHANNEL,
                              DIO_E_PARAM_INVALID_CHANNEL_ID);
        return;
    }
#endif

    digitalWrite((uint8_t)channelId, (level == (Dio_LevelType)STD_HIGH) ? HIGH : LOW);
}

extern "C" Dio_LevelType Dio_FlipChannel(Dio_ChannelType channelId)
{
    Dio_LevelType next;

#if (DIO_DEV_ERROR_DETECT == STD_ON)
    if ((Dio_ChannelIsConfigured(channelId) == FALSE) ||
        ((ECU_PIN_BIT(channelId) & ECU_PINMAP_INPUT_ONLY_PINS) != 0uLL))
    {
        (void)Det_ReportError(MODULE_ID_DIO, INSTANCE_ID_SINGLE, DIO_API_ID_FLIP_CHANNEL,
                              DIO_E_PARAM_INVALID_CHANNEL_ID);
        return (Dio_LevelType)STD_LOW;
    }
#endif

    /* Read-modify-write under a critical section. Two tasks toggling one channel is not a pattern this design
     * uses -- HmiSwc owns every indicator -- but the guard costs a few cycles on a path that runs at most ten
     * times a second, and a lost update here presents as an indicator stuck on, which is precisely the kind of
     * symptom nobody manages to reproduce. */
    portDISABLE_INTERRUPTS();
    next = (digitalRead((uint8_t)channelId) == HIGH) ? (Dio_LevelType)STD_LOW : (Dio_LevelType)STD_HIGH;
    digitalWrite((uint8_t)channelId, (next == (Dio_LevelType)STD_HIGH) ? HIGH : LOW);
    portENABLE_INTERRUPTS();

    return next;
}

extern "C" void Dio_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = DIO_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_DIO;
        versioninfo->sw_major_version = DIO_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = DIO_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = DIO_SW_PATCH_VERSION;
    }
}

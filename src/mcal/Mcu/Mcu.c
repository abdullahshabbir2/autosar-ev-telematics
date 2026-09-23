/**
 * @file    Mcu.c
 * @brief   Platform-independent part of the MCU driver.
 *
 * The platform leaf lives in Mcu_Esp32.cpp for the target and in
 * test/support/Stub_Mcu.c for the host.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Mcu.h"

const char *Mcu_GetResetReasonName(Mcu_ResetReasonType reason)
{
    const char *name;

    /* A table indexed by the enum would be shorter, but a switch makes the compiler
     * warn when a new reason is added and this function is not updated -- which is
     * exactly when an unnamed reason would start appearing in field logs as a bare
     * number. -Wswitch-enum is enabled for this reason. */
    switch (reason)
    {
    case MCU_RESET_POWER_ON:
        name = "POWER_ON";
        break;
    case MCU_RESET_SOFTWARE:
        name = "SOFTWARE";
        break;
    case MCU_RESET_WATCHDOG:
        name = "WATCHDOG";
        break;
    case MCU_RESET_PANIC:
        name = "PANIC";
        break;
    case MCU_RESET_BROWNOUT:
        name = "BROWNOUT";
        break;
    case MCU_RESET_DEEPSLEEP:
        name = "DEEPSLEEP";
        break;
    case MCU_RESET_EXTERNAL:
        name = "EXTERNAL";
        break;
    case MCU_RESET_UNKNOWN:
    default:
        name = "UNKNOWN";
        break;
    }

    return name;
}

Std_ReturnType Mcu_GetDeviceIdString(char *buffer, uint8 bufferSize)
{
    static const char hexDigits[] = "0123456789ABCDEF";
    uint8 raw[MCU_DEVICE_ID_LENGTH];
    uint8 i;

    /* 12 hex digits plus a terminator. Checked before anything is written so that a
     * caller with a short buffer gets an untouched one rather than a truncated ID that
     * would register as a second device in the cloud. */
    if ((buffer == NULL_PTR) || (bufferSize < ((MCU_DEVICE_ID_LENGTH * 2u) + 1u)))
    {
        return E_NOT_OK;
    }

    if (Mcu_GetDeviceId(raw, (uint8)sizeof(raw)) != E_OK)
    {
        return E_NOT_OK;
    }

    for (i = 0u; i < MCU_DEVICE_ID_LENGTH; i++)
    {
        buffer[i * 2u] = hexDigits[(raw[i] >> 4u) & 0x0Fu];
        buffer[(i * 2u) + 1u] = hexDigits[raw[i] & 0x0Fu];
    }
    buffer[MCU_DEVICE_ID_LENGTH * 2u] = '\0';

    return E_OK;
}

void Mcu_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = MCU_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_MCU;
        versioninfo->sw_major_version = MCU_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = MCU_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = MCU_SW_PATCH_VERSION;
    }
}

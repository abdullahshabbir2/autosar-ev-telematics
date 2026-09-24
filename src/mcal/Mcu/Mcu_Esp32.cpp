/**
 * @file    Mcu_Esp32.cpp
 * @brief   ESP32 platform leaf of the MCU driver.
 *
 * Reset cause, reset request, device identity and heap figures. Everything derived -- the reset-reason
 * name table, the device-id string formatting -- is in Mcu.c and is unit tested.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "mcal/Mcu/Mcu.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/**
 * @brief Reset cause, latched during Mcu_Init.
 *
 * Latched rather than queried on demand. @c esp_reset_reason() is stable on this part, but the value is
 * consulted from several places during startup and caching it removes any question of whether something
 * in between could have disturbed it.
 */
static Mcu_ResetReasonType Mcu_LatchedResetReason = MCU_RESET_UNKNOWN;
static boolean Mcu_Initialised = FALSE;

/*==================================================================================================
 *  Platform leaf
 *================================================================================================*/

extern "C" Std_ReturnType Mcu_Init(void)
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        Mcu_LatchedResetReason = MCU_RESET_POWER_ON;
        break;
    case ESP_RST_SW:
        Mcu_LatchedResetReason = MCU_RESET_SOFTWARE;
        break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        /* All three watchdog sources collapse to one reason. The distinction between them matters when
         * debugging the silicon; what the crash-loop detector needs to know is only "the previous run was
         * terminated because something stopped responding". */
        Mcu_LatchedResetReason = MCU_RESET_WATCHDOG;
        break;
    case ESP_RST_PANIC:
        Mcu_LatchedResetReason = MCU_RESET_PANIC;
        break;
    case ESP_RST_BROWNOUT:
        Mcu_LatchedResetReason = MCU_RESET_BROWNOUT;
        break;
    case ESP_RST_DEEPSLEEP:
        Mcu_LatchedResetReason = MCU_RESET_DEEPSLEEP;
        break;
    case ESP_RST_EXT:
        Mcu_LatchedResetReason = MCU_RESET_EXTERNAL;
        break;
    case ESP_RST_UNKNOWN:
    case ESP_RST_SDIO:
    default:
        Mcu_LatchedResetReason = MCU_RESET_UNKNOWN;
        break;
    }

    Mcu_Initialised = TRUE;
    return E_OK;
}

extern "C" Mcu_ResetReasonType Mcu_GetResetReason(void)
{
    return (Mcu_Initialised != FALSE) ? Mcu_LatchedResetReason : MCU_RESET_UNKNOWN;
}

extern "C" void Mcu_PerformReset(void)
{
    /* esp_restart() does not return, but the compiler cannot know that through the C boundary, so the
     * infinite loop keeps NORETURN honest and stops the optimiser emitting a return path. */
    esp_restart();
    for (;;)
    {
    }
}

extern "C" Std_ReturnType Mcu_GetDeviceId(uint8 *buffer, uint8 bufferSize)
{
    if ((buffer == NULL_PTR) || (bufferSize < MCU_DEVICE_ID_LENGTH))
    {
        return E_NOT_OK;
    }

    /* The factory MAC from eFuse, not WiFi.macAddress(). The eFuse value is available before the radio is
     * started and never changes; WiFi.macAddress() returns zeros until the driver has initialised, which
     * would give an unprovisioned unit an all-zero identity at exactly the moment it needs a unique one. */
    if (esp_efuse_mac_get_default(buffer) != ESP_OK)
    {
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" Std_ReturnType Mcu_GetHeapInfo(Mcu_HeapInfoType *info)
{
    if (info == NULL_PTR)
    {
        return E_NOT_OK;
    }

    info->heapFreeBytes = (uint32)esp_get_free_heap_size();
    info->heapMinFreeBytes = (uint32)esp_get_minimum_free_heap_size();

    /* The largest contiguous block, not just the total free. Fragmentation is the failure mode that
     * matters here: the heap can report plenty free while no single allocation of any size succeeds, and
     * that is precisely what v1's per-record String churn produced. */
    info->heapLargestBlockBytes = (uint32)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    info->internalFreeBytes = (uint32)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    return E_OK;
}

extern "C" uint32 Mcu_GetCpuFrequencyHz(void)
{
    return (uint32)getCpuFrequencyMhz() * 1000000uL;
}

/**
 * @file    Wdg_Esp32.cpp
 * @brief   ESP32 platform leaf of the watchdog driver, over the ESP-IDF task watchdog timer.
 *
 * @par Why the task watchdog and not the RTC watchdog
 * The ESP32 has three: the interrupt watchdog (protects against a critical section held too long), the RTC
 * watchdog (a last-resort hardware timer), and the task watchdog timer (TWDT), which tracks a set of
 * subscribed tasks and resets the chip if any one of them fails to check in.
 *
 * The TWDT is the right choice here because this ECU's failure mode is not a hung CPU -- it is *one task*
 * stopping while the others keep running. A single global watchdog that any task may pet is satisfied by the
 * healthiest task in the system, which means a stalled acquisition task with a healthy connectivity task
 * never triggers it. The TWDT's per-task subscription makes that impossible, and it is the reason
 * ::Wdg_SubscribeCurrentTask exists rather than a plain ::Wdg_Init.
 *
 * v1 called @c esp_task_wdt_init() but never subscribed any task to it, so the timer was configured and
 * supervised nothing. That is worse than having no watchdog, because the configuration call in the boot log
 * reads as evidence of protection that was not there.
 *
 * @par Reset, not interrupt
 * ::WDG_RESET_ON_EXPIRY is configured on, so expiry resets the chip. The alternative -- a panic handler that
 * logs first -- sounds better and is not: the state that caused a supervision failure is exactly the state in
 * which the logging path cannot be trusted. What the next boot needs is recorded *before* the fault, by WdgM
 * into the ::NVM_BLOCK_RESTART_INFO block, not after it by a handler running on a damaged system.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "services/Det/Det.h"
#include "mcal/Wdg/Wdg.h"

static Wdg_ModeType Wdg_CurrentMode = WDG_MODE_OFF;
static boolean Wdg_Initialised = FALSE;

/** Timeout in milliseconds for @p mode. */
static uint32 Wdg_TimeoutForMode(Wdg_ModeType mode)
{
    uint32 timeoutMs;

    switch (mode)
    {
    case WDG_MODE_FAST:
        timeoutMs = WDG_TIMEOUT_FAST_MS;
        break;
    case WDG_MODE_SLOW:
        timeoutMs = WDG_TIMEOUT_SLOW_MS;
        break;
    case WDG_MODE_OFF:
    default:
        timeoutMs = 0uL;
        break;
    }

    return timeoutMs;
}

/**
 * @brief (Re)configure the TWDT for @p timeoutMs.
 *
 * @c esp_task_wdt_init is idempotent on this IDF version: called again with a different timeout it
 * reconfigures the existing timer and keeps the subscribed task list, which is what a mode change needs.
 * Re-subscribing tasks after a mode change would be both unnecessary and unsafe, since this function has no
 * way to enumerate them.
 */
static Std_ReturnType Wdg_ConfigureHardware(uint32 timeoutMs)
{
    /* The IDF takes whole seconds. Rounding *up* is the only safe direction: rounding 12 000 ms down to 11 s
     * would tighten a bound that was chosen against a measured worst case. */
    const uint32 timeoutSeconds = (timeoutMs + 999uL) / 1000uL;
    const esp_err_t result = esp_task_wdt_init(timeoutSeconds, (WDG_RESET_ON_EXPIRY == STD_ON));

    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" Std_ReturnType Wdg_Init(void)
{
    /* Slow mode, because startup legitimately blocks for tens of seconds: mounting the card, attaching to
     * GPRS, the first broker handshake. Arming the fast timeout here would reset a unit that is merely
     * starting up slowly on a cold GPRS network -- and then do it again on the next attempt, forever. */
    const Std_ReturnType status = Wdg_ConfigureHardware(WDG_TIMEOUT_SLOW_MS);

    if (status != E_OK)
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_INIT, WDG_E_DRIVER_STATE);
        return E_NOT_OK;
    }

    Wdg_CurrentMode = WDG_MODE_SLOW;
    Wdg_Initialised = TRUE;

    return E_OK;
}

extern "C" Std_ReturnType Wdg_SetMode(Wdg_ModeType mode)
{
    if (Wdg_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SET_MODE, WDG_E_DRIVER_STATE);
        return E_NOT_OK;
    }

    if (mode == WDG_MODE_OFF)
    {
#if (WDG_ALLOW_DISABLE == STD_OFF)
        /* Refused at compile-time configuration level, so no code path -- including a future one -- can
         * quietly remove the protection. */
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SET_MODE,
                              WDG_E_DISABLE_REJECTED);
        return E_NOT_OK;
#else
        /* Permitted only so an OTA flash write -- which holds the flash controller for tens of seconds and
         * cannot be interrupted -- is not cut in half by a reset. The caller is responsible for restoring a
         * mode afterwards; WdgM asserts that it did. */
        if (esp_task_wdt_delete(NULL) == ESP_ERR_INVALID_STATE)
        {
            (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SET_MODE,
                                  WDG_E_DRIVER_STATE);
            return E_NOT_OK;
        }

        Wdg_CurrentMode = WDG_MODE_OFF;
        return E_OK;
#endif
    }

    if ((mode != WDG_MODE_SLOW) && (mode != WDG_MODE_FAST))
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SET_MODE, WDG_E_PARAM_MODE);
        return E_NOT_OK;
    }

    if (Wdg_ConfigureHardware(Wdg_TimeoutForMode(mode)) != E_OK)
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SET_MODE, WDG_E_DRIVER_STATE);
        return E_NOT_OK;
    }

    Wdg_CurrentMode = mode;
    return E_OK;
}

extern "C" Std_ReturnType Wdg_SubscribeCurrentTask(void)
{
    if (Wdg_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SUBSCRIBE, WDG_E_DRIVER_STATE);
        return E_NOT_OK;
    }

    /* NULL means "the task calling this", which is why the API is phrased as CurrentTask: the subscription is
     * a property of the caller and cannot be made on another task's behalf. Each supervised task therefore
     * calls this as its first action, before its loop begins. */
    {
        const esp_err_t result = esp_task_wdt_add(NULL);

        if ((result != ESP_OK) && (result != ESP_ERR_INVALID_ARG))
        {
            /* ESP_ERR_INVALID_ARG here means already subscribed, which is harmless and idempotent. Anything
             * else means the subscription table is full -- a configuration defect, since the table is sized
             * for ::SCHM_TASK_COUNT. */
            (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_SUBSCRIBE,
                                  WDG_E_SUBSCRIBE_FAILED);
            return E_NOT_OK;
        }
    }

    return E_OK;
}

extern "C" Std_ReturnType Wdg_UnsubscribeCurrentTask(void)
{
    if (Wdg_Initialised == FALSE)
    {
        return E_NOT_OK;
    }

    if (esp_task_wdt_delete(NULL) != ESP_OK)
    {
        (void)Det_ReportError(MODULE_ID_WDG, INSTANCE_ID_SINGLE, WDG_API_ID_UNSUBSCRIBE,
                              WDG_E_DRIVER_STATE);
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" void Wdg_Trigger(void)
{
    if ((Wdg_Initialised == FALSE) || (Wdg_CurrentMode == WDG_MODE_OFF))
    {
        return;
    }

    /* The return value is deliberately ignored. It reports ESP_ERR_NOT_FOUND for a task that never
     * subscribed, and the correct response to that is *not* to reset the ECU -- the task is running well
     * enough to have called this. WdgM's own alive counting is what catches a task that stopped, and it does
     * so with the deadline and program-flow information the hardware timer does not have. */
    (void)esp_task_wdt_reset();
}

extern "C" uint32 Wdg_GetTimeoutMs(void)
{
    return Wdg_TimeoutForMode(Wdg_CurrentMode);
}

extern "C" Wdg_ModeType Wdg_GetMode(void)
{
    return Wdg_CurrentMode;
}

extern "C" void Wdg_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = WDG_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_WDG;
        versioninfo->sw_major_version = WDG_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = WDG_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = WDG_SW_PATCH_VERSION;
    }
}

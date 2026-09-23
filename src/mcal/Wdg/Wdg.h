/**
 * @file    Wdg.h
 * @brief   AUTOSAR Watchdog driver (SWS_WatchdogDriver) -- ESP32 task WDT.
 *
 * A thin binding over the ESP32 task watchdog. The *policy* -- which tasks must check
 * in, how often, and what happens when one does not -- belongs to WdgM, not here.
 *
 * @par What was wrong in v1
 * The v1 firmware called @c esp_task_wdt_init(60, true), then never subscribed a
 * single task and never called @c esp_task_wdt_reset() (its one call site was
 * commented out). The watchdog was therefore inert: a hung acquisition task would
 * have stalled the logger indefinitely with no recovery. The worse problem is that it
 * *read* as protected. Splitting the driver from the supervisor makes subscription
 * explicit here and supervision real in WdgM, so the two cannot silently drift apart
 * again.
 *
 * @req SWREQ-SAF-0001, SWREQ-SAF-0002
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef WDG_H
#define WDG_H

#include "Autosar_ModuleIds.h"
#include "Std_Types.h"
#include "Wdg_Cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDG_VENDOR_ID 0xFFFEu
#define WDG_AR_RELEASE_MAJOR_VERSION 4u
#define WDG_AR_RELEASE_MINOR_VERSION 4u
#define WDG_SW_MAJOR_VERSION 2u
#define WDG_SW_MINOR_VERSION 0u
#define WDG_SW_PATCH_VERSION 0u

#define WDG_API_ID_INIT 0x00u
#define WDG_API_ID_SET_MODE 0x01u
#define WDG_API_ID_SUBSCRIBE 0x20u
#define WDG_API_ID_UNSUBSCRIBE 0x21u
#define WDG_API_ID_TRIGGER 0x22u

#define WDG_E_DRIVER_STATE 0x10u
#define WDG_E_PARAM_MODE 0x11u
#define WDG_E_DISABLE_REJECTED 0x12u
#define WDG_E_SUBSCRIBE_FAILED 0x13u

/** Watchdog operating mode. */
typedef enum
{
    WDG_MODE_OFF = 0,  /**< Disabled. Permitted only during an OTA flash write. */
    WDG_MODE_SLOW = 1, /**< Long timeout, for startup and OTA.                  */
    WDG_MODE_FAST = 2  /**< Normal operating timeout.                           */
} Wdg_ModeType;

/**
 * @brief Start the hardware watchdog in ::WDG_MODE_SLOW.
 *
 * Slow rather than fast, because startup legitimately blocks for seconds: mounting
 * the SD card, attaching to GPRS, waiting for a first GNSS fix. WdgM switches to fast
 * once the cyclic tasks are running and those long one-off waits are behind it.
 */
CHECK_RETURN Std_ReturnType Wdg_Init(void);

/**
 * @brief Change the operating mode, and hence the timeout.
 *
 * ::WDG_MODE_OFF is rejected with ::WDG_E_DISABLE_REJECTED unless
 * ::WDG_ALLOW_DISABLE is configured on, so that no code path can quietly remove the
 * protection.
 */
CHECK_RETURN Std_ReturnType Wdg_SetMode(Wdg_ModeType mode);

/**
 * @brief Subscribe the calling task to the watchdog.
 *
 * A subscribed task must be reported alive within the configured timeout or the
 * watchdog resets the ECU. Subscription is per task and the underlying API registers
 * whichever task is currently running, so each task must call this itself, as its
 * first action.
 */
CHECK_RETURN Std_ReturnType Wdg_SubscribeCurrentTask(void);

/** Remove the calling task from supervision, for an orderly task shutdown. */
CHECK_RETURN Std_ReturnType Wdg_UnsubscribeCurrentTask(void);

/**
 * @brief Report the calling task as alive.
 *
 * Called only by WdgM, never directly by application code. A task that pets the
 * hardware watchdog itself defeats the deadline and program-flow checks WdgM layers
 * on top -- which is the usual way a watchdog ends up protecting nothing at all.
 */
void Wdg_Trigger(void);

/** Timeout currently in force, in milliseconds. */
uint32 Wdg_GetTimeoutMs(void);

/** Mode currently in force. */
Wdg_ModeType Wdg_GetMode(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Wdg_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* WDG_H */

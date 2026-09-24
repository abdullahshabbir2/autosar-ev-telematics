/**
 * @file    Gpt.h
 * @brief   AUTOSAR GPT driver (SWS_GPTDriver) -- time base for the whole stack.
 *
 * Every timeout, every scheduling deadline and every timestamp in this project is
 * derived from this one module. Nothing above the MCAL calls @c millis(),
 * @c esp_timer_get_time() or @c xTaskGetTickCount() directly.
 *
 * @par Why a single time source matters here
 * The original firmware mixed @c millis(), FreeRTOS ticks and @c time(nullptr) in
 * the same timeout expressions. Two of those three wrap or jump: @c millis() rolls
 * over after 49.7 days, and @c time() steps backwards the moment NTP corrects the
 * clock, which turns an elapsed-time comparison into a very long wait. Routing all
 * elapsed-time arithmetic through ::Gpt_GetMonotonicMs and ::Gpt_HasElapsed makes
 * both hazards impossible to reintroduce at a call site.
 *
 * @par Monotonic versus wall clock
 *  - ::Gpt_GetMonotonicMs / ::Gpt_GetMonotonicUs -- never steps, never runs
 *    backwards, zero at boot. Use for *all* durations and timeouts.
 *  - Wall-clock time (calendar date) belongs to TimeAbs, not here. A timestamp that
 *    must survive a reboot comes from TimeAbs; a timeout never does.
 *
 * @req SWREQ-SYS-0010, SWREQ-SYS-0011
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef GPT_H
#define GPT_H

#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *  Published information
 *================================================================================================*/

#define GPT_VENDOR_ID 0xFFFEu
#define GPT_AR_RELEASE_MAJOR_VERSION 4u
#define GPT_AR_RELEASE_MINOR_VERSION 4u
#define GPT_SW_MAJOR_VERSION 2u
#define GPT_SW_MINOR_VERSION 0u
#define GPT_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  API service IDs
 *================================================================================================*/

#define GPT_API_ID_INIT 0x01u
#define GPT_API_ID_GET_MONOTONIC_MS 0x20u
#define GPT_API_ID_GET_MONOTONIC_US 0x21u
#define GPT_API_ID_HAS_ELAPSED 0x22u
#define GPT_API_ID_DELAY_MS 0x23u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/**
 * @brief A captured instant on the monotonic time base, in milliseconds.
 *
 * 32 bits wide and therefore wrapping after 49.7 days of continuous uptime. That
 * is deliberate -- it keeps timestamps cheap to store and transmit -- and it is
 * safe *provided* every comparison goes through ::Gpt_HasElapsed or
 * ::Gpt_ElapsedSince, both of which use modular arithmetic and stay correct across
 * the wrap. Direct @c < or @c > comparison of two Gpt_TimestampType values is
 * forbidden by coding-standard rule CS-TIME-01.
 */
typedef uint32 Gpt_TimestampType;

/** Longest interval ::Gpt_HasElapsed can express unambiguously (~24.8 days). */
#define GPT_MAX_INTERVAL_MS 0x7FFFFFFFuL

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Start the monotonic time base.
 * @return E_OK always.
 */
Std_ReturnType Gpt_Init(void);

/**
 * @brief Milliseconds since ::Gpt_Init.
 *
 * Monotonic: unaffected by NTP corrections, RTC adjustment or DST. Wraps after
 * 49.7 days; see ::Gpt_TimestampType.
 */
Gpt_TimestampType Gpt_GetMonotonicMs(void);

/**
 * @brief Microseconds since ::Gpt_Init, 64-bit and therefore non-wrapping.
 *
 * Used for the execution-time measurements that SchM records per runnable; too
 * expensive in storage to use as a general timestamp.
 */
uint64 Gpt_GetMonotonicUs(void);

/**
 * @brief Milliseconds elapsed since @p since, correct across a counter wrap.
 *
 * @param since An instant previously returned by ::Gpt_GetMonotonicMs.
 * @return Elapsed milliseconds. If @p since is in the future (possible only if the
 *         caller fabricated it), the modular result is returned rather than a
 *         nonsensical huge number being special-cased.
 */
uint32 Gpt_ElapsedSince(Gpt_TimestampType since);

/**
 * @brief Whether at least @p intervalMs has passed since @p since.
 *
 * The one correct way to express a timeout in this codebase.
 *
 * @param since      Instant the interval is measured from.
 * @param intervalMs Duration, at most ::GPT_MAX_INTERVAL_MS.
 * @return TRUE once the interval has fully elapsed.
 */
boolean Gpt_HasElapsed(Gpt_TimestampType since, uint32 intervalMs);

/**
 * @brief Block the calling task for @p ms, yielding the CPU.
 *
 * Yields to the scheduler rather than spinning, so lower-priority tasks still run.
 * Not callable from an ISR. Prefer a state machine driven by ::Gpt_HasElapsed;
 * this exists for hardware settling times that genuinely have to be waited out
 * (an RS485 transceiver direction turnaround, a modem reset pulse).
 */
void Gpt_DelayMs(uint32 ms);

/**
 * @brief Busy-wait for @p us without yielding.
 *
 * For sub-millisecond hardware timing only -- bus turnaround and chip-select
 * setup. Callers must keep @p us below 1000; longer spins starve the scheduler and
 * are rejected with a Det report.
 */
void Gpt_DelayUs(uint32 us);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Gpt_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* GPT_H */

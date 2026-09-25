/**
 * @file    HmiSwc.h
 * @brief   Status indication component -- what the five LEDs mean.
 *
 * The only feedback this ECU gives anyone standing next to the vehicle, so what each LED does has to
 * be unambiguous and has to stay correct.
 *
 * @par The v1 defect
 * v1 drove the LEDs from a `byte flags[15]` array whose enum was:
 *
 *     aq_blink_f=0, sd_blink_f=1, wf_blink_f=2, cloud_blink_f=3, can_blink_f=4,
 *     aq_f=5,       sd_f=6,       wf_f=7,       cloud_f=8, ...
 *
 * The LED task read the steady-state level with `flags[_flag + 4]` for `_flag` in 0..3, which indexes
 * 4..7 -- `can_blink_f`, `aq_f`, `sd_f`, `wf_f`. The intended indices were 5..8. So **every one of the
 * four LEDs displayed the wrong signal**: the acquisition LED showed a blink flag, the storage LED
 * showed acquisition state, the link LED showed storage state, the cloud LED showed link state, and
 * `cloud_f` was never displayed at all. The `+ 4` was almost certainly written when the enum had one
 * fewer blink flag, and nothing in the code's shape made the coupling visible.
 *
 * The fix is not a corrected offset. It is removing the shared index space: each indicator here has
 * its own named state, set through a named function, and no arithmetic relates one indicator to
 * another.
 *
 * @par Indication scheme
 * | Indicator | Off | Slow blink (1 Hz) | Fast blink (4 Hz) | Solid |
 * |---|---|---|---|---|
 * | Acquisition | not acquiring | -- | -- | a record was acquired this cycle |
 * | Storage | no card | card present, idle | write failing | writing |
 * | Link | no bearer | associating | -- | bearer up |
 * | Cloud | no session | connecting | publish failing | session up, publishing |
 * | Heartbeat | scheduler stopped | normal | supervision failure | -- |
 *
 * A blink is used wherever the interesting distinction is *progress* rather than *state*: a solid
 * storage LED on a unit whose card has silently stopped accepting writes looks identical to a healthy
 * one, whereas a fast blink does not.
 *
 * @req SWREQ-HMI-0001 .. SWREQ-HMI-0012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef HMISWC_H
#define HMISWC_H

#include "base/Autosar_ModuleIds.h"
#include "app/HmiSwc/HmiSwc_Cfg.h"
#include "ecuabs/IoHwAb/IoHwAb.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HMISWC_VENDOR_ID 0xFFFEu
#define HMISWC_SW_MAJOR_VERSION 2u
#define HMISWC_SW_MINOR_VERSION 0u
#define HMISWC_SW_PATCH_VERSION 0u

#define HMISWC_API_ID_INIT 0x00u
#define HMISWC_API_ID_SET_PATTERN 0x20u
#define HMISWC_API_ID_MAIN_FUNCTION 0x0Eu
#define HMISWC_API_ID_PULSE 0x21u

#define HMISWC_E_UNINIT E_UNINIT
#define HMISWC_E_PARAM_INDICATOR 0x20u
#define HMISWC_E_PARAM_PATTERN 0x21u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** What an indicator is doing. */
typedef enum
{
    HMI_PATTERN_OFF = 0,        /**< Dark.                                        */
    HMI_PATTERN_SOLID = 1,      /**< Continuously lit.                            */
    HMI_PATTERN_BLINK_SLOW = 2, /**< 1 Hz, equal on and off.                      */
    HMI_PATTERN_BLINK_FAST = 3, /**< 4 Hz, equal on and off. Signals a fault.     */
    HMI_PATTERN_PULSE = 4       /**< One short flash, then back to the previous pattern. */
} HmiSwc_PatternType;

/** Per-indicator state, for diagnostics. */
typedef struct
{
    HmiSwc_PatternType pattern; /**< Pattern currently requested.            */
    boolean lit;                /**< Whether the indicator is lit right now. */
    uint32 transitionCount;     /**< Level changes since Init.               */
} HmiSwc_IndicatorStateType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Put every indicator into ::HMI_PATTERN_OFF and reset the blink phase.
 * @return E_OK on success.
 */
CHECK_RETURN Std_ReturnType HmiSwc_Init(void);

/**
 * @brief Set @p indicator's pattern.
 *
 * Idempotent: setting the pattern an indicator already has does not disturb its blink phase, so a
 * caller may call this every cycle without producing an irregular blink.
 */
CHECK_RETURN Std_ReturnType HmiSwc_SetPattern(IoHwAb_IndicatorType indicator, HmiSwc_PatternType pattern);

/**
 * @brief Flash @p indicator once, then return to its previous pattern.
 *
 * Used to mark a discrete event -- a record acquired, a record published -- where a steady level
 * would say nothing about whether anything is still happening.
 */
CHECK_RETURN Std_ReturnType HmiSwc_Pulse(IoHwAb_IndicatorType indicator);

/**
 * @brief Advance the blink phases and drive the outputs.
 *
 * Driven cyclically by SchM at ::HMI_TICK_MS. All timing derives from the tick count rather than from
 * wall-clock arithmetic, so the pattern periods are exact multiples of the tick and two indicators on
 * the same pattern stay in phase.
 */
void HmiSwc_MainFunction(void);

/**
 * @brief Read @p indicator's state.
 * @param[in]  indicator Which indicator to read.
 * @param[out] state     Destination.
 */
CHECK_RETURN Std_ReturnType HmiSwc_GetState(IoHwAb_IndicatorType indicator, HmiSwc_IndicatorStateType *state);

/**
 * @brief Drive every indicator solid for ::HMI_SELFTEST_MS, then return them all to off.
 *
 * Run once during startup. Its purpose is to let whoever installed the unit see that all five LEDs
 * work: an indicator that is dark because it is broken is otherwise indistinguishable from one that
 * is dark because the condition it reports is absent.
 */
void HmiSwc_SelfTest(void);

/**
 * @brief Return this component's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void HmiSwc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* HMISWC_H */

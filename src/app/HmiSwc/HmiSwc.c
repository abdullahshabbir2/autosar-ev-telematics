/**
 * @file    HmiSwc.c
 * @brief   Status indication implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "HmiSwc.h"

#include "Det.h"
#include "Gpt.h"

/*==================================================================================================
 *  Local data
 *
 *  One independent record per indicator. Deliberately not a shared index space: v1's LED task read
 *  its steady-state level with `flags[_flag + 4]` into an enum whose layout had shifted, so every one
 *  of the four LEDs displayed a different signal from the one intended. The fix is structural --
 *  there is no arithmetic here that relates one indicator to another.
 *================================================================================================*/

typedef struct
{
    HmiSwc_PatternType pattern;         /**< Pattern currently requested.                */
    HmiSwc_PatternType patternBeforePulse; /**< Pattern to return to after a pulse.      */
    uint16 phaseTicks;                  /**< Ticks elapsed in the current half-period.   */
    uint16 pulseTicksRemaining;         /**< Ticks a pulse still has to run.             */
    boolean lit;                        /**< Whether the output is currently driven on.  */
    uint32 transitionCount;             /**< Level changes since Init.                   */
} HmiSwc_EntryType;

STATIC HmiSwc_EntryType HmiSwc_Indicators[IOHWAB_INDICATOR_COUNT];
STATIC boolean HmiSwc_Initialised = FALSE;

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

STATIC boolean HmiSwc_IndicatorValid(IoHwAb_IndicatorType indicator)
{
    return (indicator < (IoHwAb_IndicatorType)IOHWAB_INDICATOR_COUNT) ? TRUE : FALSE;
}

/** Drive @p index's output to @p lit, counting only genuine transitions. */
STATIC void HmiSwc_Drive(uint8 index, boolean lit)
{
    if (HmiSwc_Indicators[index].lit == lit)
    {
        /* No change: skipped rather than rewritten. Rewriting would be harmless electrically, but it
         * would make the transition count -- which is how a blink is verified in test and in the
         * field -- count ticks instead of transitions. */
        return;
    }

    HmiSwc_Indicators[index].lit = lit;
    HmiSwc_Indicators[index].transitionCount++;
    STD_DISCARD(IoHwAb_SetIndicator((IoHwAb_IndicatorType)index, lit));
}

/** Adopt @p pattern on @p index, resetting its phase and driving the output at once. */
STATIC void HmiSwc_ApplyPattern(uint8 index, HmiSwc_PatternType pattern)
{
    HmiSwc_Indicators[index].pattern = pattern;
    HmiSwc_Indicators[index].phaseTicks = 0u;
    HmiSwc_Indicators[index].pulseTicksRemaining = 0u;

    /* Taking effect immediately rather than at the next tick means a fault indication appears at
     * once, which matters when the tick is 100 ms and someone is watching the unit. */
    switch (pattern)
    {
    case HMI_PATTERN_SOLID:
    case HMI_PATTERN_BLINK_SLOW:
    case HMI_PATTERN_BLINK_FAST:
        HmiSwc_Drive(index, TRUE);
        break;
    case HMI_PATTERN_OFF:
        HmiSwc_Drive(index, FALSE);
        break;
    case HMI_PATTERN_PULSE:
    default:
        break;
    }
}

/** Half-period, in ticks, for @p pattern. 0 for a pattern that does not blink. */
STATIC uint16 HmiSwc_HalfPeriod(HmiSwc_PatternType pattern)
{
    uint16 ticks;

    switch (pattern)
    {
    case HMI_PATTERN_BLINK_SLOW:
        ticks = (uint16)HMI_SLOW_HALF_PERIOD_TICKS;
        break;
    case HMI_PATTERN_BLINK_FAST:
        ticks = (uint16)HMI_FAST_HALF_PERIOD_TICKS;
        break;
    case HMI_PATTERN_OFF:
    case HMI_PATTERN_SOLID:
    case HMI_PATTERN_PULSE:
    default:
        ticks = 0u;
        break;
    }

    return ticks;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType HmiSwc_Init(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)IOHWAB_INDICATOR_COUNT; i++)
    {
        HmiSwc_Indicators[i].pattern = HMI_PATTERN_OFF;
        HmiSwc_Indicators[i].patternBeforePulse = HMI_PATTERN_OFF;
        HmiSwc_Indicators[i].phaseTicks = 0u;
        HmiSwc_Indicators[i].pulseTicksRemaining = 0u;
        HmiSwc_Indicators[i].lit = FALSE;
        HmiSwc_Indicators[i].transitionCount = 0u;
    }

    IoHwAb_AllIndicatorsOff();
    HmiSwc_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType HmiSwc_SetPattern(IoHwAb_IndicatorType indicator, HmiSwc_PatternType pattern)
{
    uint8 index;

    DET_CHECK_RETURN(HmiSwc_Initialised != FALSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_SET_PATTERN, HMISWC_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(HmiSwc_IndicatorValid(indicator) != FALSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_SET_PATTERN, HMISWC_E_PARAM_INDICATOR, E_NOT_OK);
    DET_CHECK_RETURN(pattern <= HMI_PATTERN_PULSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_SET_PATTERN, HMISWC_E_PARAM_PATTERN, E_NOT_OK);

    index = (uint8)indicator;

    /* Idempotent. Callers set the pattern from the current system state every cycle, so restarting the
     * phase on an unchanged request would reset the blink before it completed and produce a visibly
     * irregular flicker. */
    if (HmiSwc_Indicators[index].pattern == pattern)
    {
        return E_OK;
    }

    HmiSwc_ApplyPattern(index, pattern);
    return E_OK;
}

Std_ReturnType HmiSwc_Pulse(IoHwAb_IndicatorType indicator)
{
    uint8 index;

    DET_CHECK_RETURN(HmiSwc_Initialised != FALSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_PULSE, HMISWC_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(HmiSwc_IndicatorValid(indicator) != FALSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_PULSE, HMISWC_E_PARAM_INDICATOR, E_NOT_OK);

    index = (uint8)indicator;

    /* Only the first pulse records the pattern to return to. A second pulse arriving while one is
     * still running would otherwise save HMI_PATTERN_PULSE as the state to restore and leave the
     * indicator stuck. */
    if (HmiSwc_Indicators[index].pulseTicksRemaining == 0u)
    {
        HmiSwc_Indicators[index].patternBeforePulse = HmiSwc_Indicators[index].pattern;
    }

    HmiSwc_Indicators[index].pattern = HMI_PATTERN_PULSE;
    HmiSwc_Indicators[index].pulseTicksRemaining = (uint16)HMI_PULSE_TICKS;
    HmiSwc_Drive(index, TRUE);

    return E_OK;
}

void HmiSwc_MainFunction(void)
{
    uint8 index;

    if (HmiSwc_Initialised == FALSE)
    {
        return;
    }

    for (index = 0u; index < (uint8)IOHWAB_INDICATOR_COUNT; index++)
    {
        HmiSwc_EntryType *entry = &HmiSwc_Indicators[index];

        if (entry->pattern == HMI_PATTERN_PULSE)
        {
            if (entry->pulseTicksRemaining > 0u)
            {
                entry->pulseTicksRemaining--;
            }
            if (entry->pulseTicksRemaining == 0u)
            {
                /* Applied directly rather than through SetPattern, whose idempotency check would
                 * otherwise have to be defeated with a sentinel value. */
                HmiSwc_ApplyPattern(index, entry->patternBeforePulse);
            }
            continue;
        }

        {
            const uint16 halfPeriod = HmiSwc_HalfPeriod(entry->pattern);

            if (halfPeriod == 0u)
            {
                /* OFF and SOLID need no timing; the level was set when the pattern was requested.
                 * Re-driving it here guards against an output that was disturbed elsewhere. */
                HmiSwc_Drive(index,
                             (entry->pattern == HMI_PATTERN_SOLID) ? (boolean)TRUE
                                                                  : (boolean)FALSE);
                continue;
            }

            entry->phaseTicks++;
            if (entry->phaseTicks >= halfPeriod)
            {
                entry->phaseTicks = 0u;
                HmiSwc_Drive(index, (entry->lit != FALSE) ? (boolean)FALSE : (boolean)TRUE);
            }
        }
    }
}

Std_ReturnType HmiSwc_GetState(IoHwAb_IndicatorType indicator, HmiSwc_IndicatorStateType *state)
{
    DET_CHECK_RETURN(HmiSwc_IndicatorValid(indicator) != FALSE, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_SET_PATTERN, HMISWC_E_PARAM_INDICATOR, E_NOT_OK);
    DET_CHECK_RETURN(state != NULL_PTR, MODULE_ID_HMISWC, (uint8)indicator,
                     HMISWC_API_ID_SET_PATTERN, E_PARAM_POINTER, E_NOT_OK);

    state->pattern = HmiSwc_Indicators[indicator].pattern;
    state->lit = HmiSwc_Indicators[indicator].lit;
    state->transitionCount = HmiSwc_Indicators[indicator].transitionCount;
    return E_OK;
}

void HmiSwc_SelfTest(void)
{
    uint8 index;

    /* Every indicator solid, so an installer can see that all five work. An LED that is dark because
     * it is broken is otherwise indistinguishable from one that is dark because the condition it
     * reports is absent -- and on a unit mounted under a seat, nobody finds out for months. */
    for (index = 0u; index < (uint8)IOHWAB_INDICATOR_COUNT; index++)
    {
        STD_DISCARD(IoHwAb_SetIndicator((IoHwAb_IndicatorType)index, TRUE));
    }

    Gpt_DelayMs(HMI_SELFTEST_MS);

    IoHwAb_AllIndicatorsOff();

    for (index = 0u; index < (uint8)IOHWAB_INDICATOR_COUNT; index++)
    {
        HmiSwc_Indicators[index].lit = FALSE;
        HmiSwc_Indicators[index].pattern = HMI_PATTERN_OFF;
        HmiSwc_Indicators[index].phaseTicks = 0u;
    }
}

void HmiSwc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = HMISWC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_HMISWC;
        versioninfo->sw_major_version = HMISWC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = HMISWC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = HMISWC_SW_PATCH_VERSION;
    }
}

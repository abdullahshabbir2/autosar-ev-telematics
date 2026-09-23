/**
 * @file    Det.c
 * @brief   Default Error Tracer implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Det.h"

#include "Gpt.h"
#include "Mcu.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/**
 * @brief Deduplicated report store.
 *
 * Not a plain ring buffer. A ring would be flooded by whichever fault repeats
 * fastest -- a CRC mismatch on a disconnected bus fires every 3 s and would evict
 * every other report within two minutes, hiding exactly the rare one-off that
 * matters. Instead each distinct (module, instance, api, error) triple occupies one
 * slot and repeats only bump its counter and timestamp. Once all slots are taken, a
 * new triple evicts the least recently seen one, and ::Det_StatisticsType::
 * ringOverflowCount records that information was dropped.
 */
STATIC Det_EntryType Det_History[DET_HISTORY_SIZE];

STATIC uint16 Det_EntryCount;
STATIC Det_StatisticsType Det_Stats;
STATIC Det_NotificationFctType Det_Hook;
STATIC boolean Det_Initialised = FALSE;

/**
 * @brief Reports that arrived before ::Det_Init.
 *
 * A module Init() that violates a contract would otherwise vanish, and that is
 * precisely the window in which a misconfiguration shows up. Counted here and
 * folded into the statistics by ::Det_GetStatistics.
 */
STATIC uint32 Det_PreInitReports;

/*==================================================================================================
 *  Local functions
 *================================================================================================*/

/**
 * @brief Find the slot holding @p entry's triple, or ::DET_HISTORY_SIZE if absent.
 */
STATIC uint16 Det_FindSlot(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId,
                           uint8 severity)
{
    uint16 i;

    for (i = 0u; i < Det_EntryCount; i++)
    {
        if ((Det_History[i].moduleId == moduleId) && (Det_History[i].instanceId == instanceId) &&
            (Det_History[i].apiId == apiId) && (Det_History[i].errorId == errorId) &&
            (Det_History[i].severity == severity))
        {
            return i;
        }
    }
    return DET_HISTORY_SIZE;
}

/**
 * @brief Index of the slot whose most recent sighting is oldest.
 *
 * Uses ::Gpt_ElapsedSince rather than comparing timestamps directly, so the choice
 * stays correct when the monotonic counter wraps mid-life.
 */
STATIC uint16 Det_FindLeastRecentSlot(void)
{
    uint16 oldestIdx = 0u;
    uint32 oldestAge = 0u;
    uint16 i;

    for (i = 0u; i < Det_EntryCount; i++)
    {
        const uint32 age = Gpt_ElapsedSince(Det_History[i].timestamp);
        if (age >= oldestAge)
        {
            oldestAge = age;
            oldestIdx = i;
        }
    }
    return oldestIdx;
}

/**
 * @brief Record one report and run the notification hook.
 */
STATIC void Det_Record(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId,
                       Det_SeverityType severity)
{
    const uint8 sev = (uint8)severity;
    uint16 slot;

    if (Det_Initialised == FALSE)
    {
        /* Saturate rather than wrap: "many" is the useful signal here. */
        if (Det_PreInitReports < 0xFFFFFFFFuL)
        {
            Det_PreInitReports++;
        }
        return;
    }

    switch (severity)
    {
    case DET_SEVERITY_DEV:
        Det_Stats.devErrorCount++;
        break;
    case DET_SEVERITY_RUNTIME:
        Det_Stats.runtimeErrorCount++;
        break;
    case DET_SEVERITY_TRANSIENT:
        Det_Stats.transientFaultCount++;
        break;
    default:
        /* Unreachable: severity is produced internally, never by a caller. */
        break;
    }

    slot = Det_FindSlot(moduleId, instanceId, apiId, errorId, sev);

    if (slot < DET_HISTORY_SIZE)
    {
        /* Known triple: bump the counter, refresh the sighting time. */
        if (Det_History[slot].occurrences < DET_OCCURRENCE_MAX)
        {
            Det_History[slot].occurrences++;
        }
        Det_History[slot].timestamp = Gpt_GetMonotonicMs();
    }
    else
    {
        if (Det_EntryCount < DET_HISTORY_SIZE)
        {
            slot = Det_EntryCount;
            Det_EntryCount++;
        }
        else
        {
            slot = Det_FindLeastRecentSlot();
            if (Det_Stats.ringOverflowCount < 0xFFFFu)
            {
                Det_Stats.ringOverflowCount++;
            }
        }

        Det_History[slot].timestamp = Gpt_GetMonotonicMs();
        Det_History[slot].moduleId = moduleId;
        Det_History[slot].instanceId = instanceId;
        Det_History[slot].apiId = apiId;
        Det_History[slot].errorId = errorId;
        Det_History[slot].severity = sev;
        Det_History[slot].occurrences = 1u;
    }

    Det_Stats.distinctEntryCount = Det_EntryCount;

    /* Snapshot the hook before calling: the hook itself may not modify it, but a
     * concurrent Det_SetNotificationHook must not be able to leave a half-written
     * pointer here. A single aligned pointer load is atomic on this core. */
    {
        const Det_NotificationFctType hook = Det_Hook;
        if (hook != NULL_PTR)
        {
            hook(&Det_History[slot]);
        }
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

void Det_Init(void)
{
    uint16 i;

    for (i = 0u; i < DET_HISTORY_SIZE; i++)
    {
        Det_History[i].timestamp = 0u;
        Det_History[i].moduleId = 0u;
        Det_History[i].instanceId = 0u;
        Det_History[i].apiId = 0u;
        Det_History[i].errorId = 0u;
        Det_History[i].severity = 0u;
        Det_History[i].occurrences = 0u;
    }

    Det_EntryCount = 0u;
    Det_Stats.devErrorCount = 0u;
    Det_Stats.runtimeErrorCount = 0u;
    Det_Stats.transientFaultCount = 0u;
    Det_Stats.distinctEntryCount = 0u;
    Det_Stats.ringOverflowCount = 0u;
    Det_Hook = NULL_PTR;
    Det_Initialised = TRUE;
}

void Det_SetNotificationHook(Det_NotificationFctType hook)
{
    Det_Hook = hook;
}

Std_ReturnType Det_ReportError(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId)
{
    Det_Record(moduleId, instanceId, apiId, errorId, DET_SEVERITY_DEV);

#if (DET_HALT_ON_ERROR == STD_ON)
    /* Bench and unit-test builds only. Stopping here is what makes the first
     * violation the one you debug, instead of the tenth. */
    Det_Panic(moduleId, apiId, errorId);
#endif

    return E_OK;
}

Std_ReturnType Det_ReportRuntimeError(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId)
{
    /* Never halts, even on a bench build: runtime errors are expected events and
     * halting on a CRC mismatch from a noisy bus would make the bench unusable. */
    Det_Record(moduleId, instanceId, apiId, errorId, DET_SEVERITY_RUNTIME);
    return E_OK;
}

Std_ReturnType Det_ReportTransientFault(uint16 moduleId, uint8 instanceId, uint8 apiId,
                                        uint8 faultId)
{
    Det_Record(moduleId, instanceId, apiId, faultId, DET_SEVERITY_TRANSIENT);
    return E_OK;
}

void Det_GetStatistics(Det_StatisticsType *stats)
{
    if (stats == NULL_PTR)
    {
        return;
    }

    *stats = Det_Stats;

    /* Pre-init reports have no slot and no severity, but they are development
     * errors by definition, so they are surfaced in that count. */
    stats->devErrorCount += Det_PreInitReports;
}

uint16 Det_GetHistory(Det_EntryType *buffer, uint16 maxCount)
{
    uint16 written = 0u;
    uint16 i;

    if ((buffer == NULL_PTR) || (maxCount == 0u))
    {
        return 0u;
    }

    /* Most recent first: callers publish only the first few entries when the
     * telemetry budget is tight, and those should be the interesting ones. */
    for (i = 0u; (i < Det_EntryCount) && (written < maxCount); i++)
    {
        const uint16 src = (uint16)((Det_EntryCount - 1u) - i);
        buffer[written] = Det_History[src];
        written++;
    }
    return written;
}

void Det_ClearHistory(void)
{
    const Det_NotificationFctType hook = Det_Hook;

    Det_Init();
    Det_Hook = hook; /* Clearing history must not silently unhook logging. */
    Det_PreInitReports = 0u;
}

NORETURN void Det_Panic(uint16 moduleId, uint8 apiId, uint8 errorId)
{
    /* Recorded as a development error so it appears in the history that the next
     * boot publishes, then the core is reset. Anything that must survive should
     * already have been written by the caller. */
    Det_Record(moduleId, INSTANCE_ID_SINGLE, apiId, errorId, DET_SEVERITY_DEV);

    Mcu_PerformReset();
}

void Det_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = DET_VENDOR_ID;
        versioninfo->moduleID = (uint16)DET_MODULE_ID;
        versioninfo->sw_major_version = DET_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = DET_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = DET_SW_PATCH_VERSION;
    }
}

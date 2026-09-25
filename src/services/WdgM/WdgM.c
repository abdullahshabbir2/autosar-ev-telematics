/**
 * @file    WdgM.c
 * @brief   Watchdog manager implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/WdgM/WdgM.h"

#include <string.h>

#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Wdg/Wdg.h"

/*==================================================================================================
 *  Entity configuration table
 *================================================================================================*/

typedef struct
{
    uint16 aliveMin;   /**< Fewest check-ins allowed per supervision cycle.  */
    uint16 aliveMax;   /**< Most check-ins allowed per supervision cycle.    */
    uint32 deadlineMs; /**< Longest permitted gap between check-ins.          */
} WdgM_EntityConfigType;

STATIC const WdgM_EntityConfigType WdgM_Config[WDGM_SE_COUNT] = {
    {WDGM_ALIVE_MIN_SCHEDULER, WDGM_ALIVE_MAX_SCHEDULER, WDGM_DEADLINE_SCHEDULER_MS},
    {WDGM_ALIVE_MIN_ACQUISITION, WDGM_ALIVE_MAX_ACQUISITION, WDGM_DEADLINE_ACQUISITION_MS},
    {WDGM_ALIVE_MIN_STORAGE, WDGM_ALIVE_MAX_STORAGE, WDGM_DEADLINE_STORAGE_MS},
    {WDGM_ALIVE_MIN_TELEMETRY, WDGM_ALIVE_MAX_TELEMETRY, WDGM_DEADLINE_TELEMETRY_MS},
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(STD_ARRAY_SIZE(WdgM_Config) == WDGM_SE_COUNT,
               "WdgM entity table does not match WDGM_SE_COUNT");
/* A deadline shorter than the supervision cycle could never be checked in time. */
_Static_assert(WDGM_DEADLINE_SCHEDULER_MS >= WDGM_SUPERVISION_CYCLE_MS / 8u,
               "the scheduler deadline is too short to be meaningful");
/* Expiry must arrive before the hardware timeout, or the hardware resets first and the
 * diagnostic event never gets recorded. */
_Static_assert((WDGM_FAILED_TOLERANCE_CYCLES * WDGM_SUPERVISION_CYCLE_MS) < WDG_TIMEOUT_FAST_MS,
               "supervision must expire before the hardware watchdog bites, or no event is "
               "recorded and the reset arrives unexplained");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean WdgM_Initialised = FALSE;
STATIC boolean WdgM_SupervisionActive = FALSE;
STATIC WdgM_EntityStatusType WdgM_Entities[WDGM_SE_COUNT];
STATIC WdgM_GlobalStatusType WdgM_Global = WDGM_GLOBAL_STATUS_DEACTIVATED;
STATIC WdgM_StatisticsType WdgM_Stats;

/** Last checkpoint reached per entity, for program-flow supervision. */
STATIC WdgM_CheckpointIdType WdgM_LastCheckpoint[WDGM_SE_COUNT];

/** When each entity was deactivated, so an accidental permanent suspension is detectable. */
STATIC Gpt_TimestampType WdgM_DeactivatedAt[WDGM_SE_COUNT];

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

STATIC boolean WdgM_EntityValid(WdgM_SupervisedEntityIdType entityId)
{
    return (entityId < (WdgM_SupervisedEntityIdType)WDGM_SE_COUNT) ? TRUE : FALSE;
}

/** Record a violation against @p index and advance its status. */
STATIC void WdgM_RecordViolation(uint16 index, WdgM_ViolationType violation)
{
    WdgM_Entities[index].lastViolation = violation;

    switch (violation)
    {
    case WDGM_VIOLATION_ALIVE_LOW:
    case WDGM_VIOLATION_ALIVE_HIGH:
        WdgM_Entities[index].aliveViolations++;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_WDGM_ALIVE, (uint8)index, DEM_EVENT_STATUS_FAILED));
        break;
    case WDGM_VIOLATION_DEADLINE:
        WdgM_Entities[index].deadlineViolations++;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_WDGM_DEADLINE, (uint8)index, DEM_EVENT_STATUS_FAILED));
        break;
    case WDGM_VIOLATION_FLOW:
        WdgM_Entities[index].flowViolations++;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_WDGM_DEADLINE, (uint8)index, DEM_EVENT_STATUS_FAILED));
        break;
    case WDGM_VIOLATION_NONE:
    default:
        return;
    }

    if (WdgM_Entities[index].failedCycles < 0xFFFFu)
    {
        WdgM_Entities[index].failedCycles++;
    }

    if (WdgM_Entities[index].failedCycles >= (uint16)WDGM_FAILED_TOLERANCE_CYCLES)
    {
        WdgM_Entities[index].localStatus = WDGM_LOCAL_STATUS_EXPIRED;
    }
    else
    {
        WdgM_Entities[index].localStatus = WDGM_LOCAL_STATUS_FAILED;
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType WdgM_Init(void)
{
    uint16 i;

    (void)memset(WdgM_Entities, 0, sizeof(WdgM_Entities));
    (void)memset(&WdgM_Stats, 0, sizeof(WdgM_Stats));
    (void)memset(WdgM_DeactivatedAt, 0, sizeof(WdgM_DeactivatedAt));

    for (i = 0u; i < (uint16)WDGM_SE_COUNT; i++)
    {
        /* Entities start deactivated and are activated by ::WdgM_ActivateSupervision. Starting
         * them active would have every one of them report an alive violation for the first cycle,
         * before their tasks have even been created. */
        WdgM_Entities[i].localStatus = WDGM_LOCAL_STATUS_DEACTIVATED;
        WdgM_Entities[i].lastCheckpointMs = Gpt_GetMonotonicMs();
        WdgM_LastCheckpoint[i] = WDGM_CP_EXIT;
    }

    WdgM_Global = WDGM_GLOBAL_STATUS_DEACTIVATED;
    WdgM_SupervisionActive = FALSE;
    WdgM_Initialised = TRUE;

    if (Wdg_Init() != E_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_WDGM, INSTANCE_ID_SINGLE, WDGM_API_ID_INIT,
                                     WDGM_E_WDG_DISABLED);
        return E_NOT_OK;
    }

    return E_OK;
}

Std_ReturnType WdgM_ActivateSupervision(void)
{
    uint16 i;

    DET_CHECK_RETURN(WdgM_Initialised != FALSE, MODULE_ID_WDGM, INSTANCE_ID_SINGLE, WDGM_API_ID_SET_MODE,
                     WDGM_E_UNINIT, E_NOT_OK);

    for (i = 0u; i < (uint16)WDGM_SE_COUNT; i++)
    {
        WdgM_Entities[i].localStatus = WDGM_LOCAL_STATUS_OK;
        WdgM_Entities[i].aliveCounter = 0u;
        WdgM_Entities[i].failedCycles = 0u;
        WdgM_Entities[i].lastCheckpointMs = Gpt_GetMonotonicMs();
    }

    WdgM_Global = WDGM_GLOBAL_STATUS_OK;
    WdgM_SupervisionActive = TRUE;

    /* Now that the cyclic tasks are genuinely running, the long startup timeout is no longer
     * needed and would only delay the reaction to a real stall. */
    return Wdg_SetMode(WDG_MODE_FAST);
}

Std_ReturnType WdgM_CheckpointReached(WdgM_SupervisedEntityIdType entityId,
                                      WdgM_CheckpointIdType checkpointId)
{
    uint16 index;
    uint32 interval;

    DET_CHECK_RETURN(WdgM_Initialised != FALSE, MODULE_ID_WDGM, (uint8)entityId,
                     WDGM_API_ID_CHECKPOINT_REACHED, WDGM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(WdgM_EntityValid(entityId) != FALSE, MODULE_ID_WDGM, (uint8)entityId,
                     WDGM_API_ID_CHECKPOINT_REACHED, WDGM_E_PARAM_SEID, E_NOT_OK);
    DET_CHECK_RETURN(checkpointId < (WdgM_CheckpointIdType)WDGM_CP_COUNT_PER_ENTITY, MODULE_ID_WDGM,
                     (uint8)entityId, WDGM_API_ID_CHECKPOINT_REACHED, WDGM_E_PARAM_CPID, E_NOT_OK);

    index = (uint16)entityId;

    if (WdgM_Entities[index].localStatus == WDGM_LOCAL_STATUS_DEACTIVATED)
    {
        /* A deactivated entity's checkpoints are counted but not judged, so a task that resumes
         * on its own does not immediately trip a deadline covering the whole suspension. */
        WdgM_Entities[index].totalCheckpoints++;
        WdgM_Entities[index].lastCheckpointMs = Gpt_GetMonotonicMs();
        return E_OK;
    }

    WdgM_Entities[index].totalCheckpoints++;

    /* Alive supervision counts the entry checkpoint only, so the counter means "times this
     * runnable started" rather than "checkpoints reported". Counting every checkpoint would make
     * the configured bounds depend on how many checkpoints a runnable happens to declare -- adding
     * one would double the count and trip an alive-high violation with no behavioural change at
     * all. AUTOSAR defines alive supervision per checkpoint for this reason; entry is the one that
     * carries the rate. */
    if (checkpointId == WDGM_CP_ENTRY)
    {
        WdgM_Entities[index].aliveCounter++;
    }

    /* Deadline supervision: the interval since this entity's previous check-in. Evaluated here
     * rather than in MainFunction because a single long stall must be caught the moment the
     * entity returns -- averaged over a supervision cycle it would be invisible. */
    interval = Gpt_ElapsedSince(WdgM_Entities[index].lastCheckpointMs);
    if (interval > WdgM_Entities[index].worstIntervalMs)
    {
        WdgM_Entities[index].worstIntervalMs = interval;
    }
    if (interval > WdgM_Config[index].deadlineMs)
    {
        WdgM_RecordViolation(index, WDGM_VIOLATION_DEADLINE);
    }
    WdgM_Entities[index].lastCheckpointMs = Gpt_GetMonotonicMs();

    /* Program-flow supervision: entry must follow exit and exit must follow entry. A runnable that
     * returns early through an unintended branch reaches entry twice in a row, which neither the
     * alive count nor the deadline notices. */
    {
        const WdgM_CheckpointIdType expected =
            (WdgM_LastCheckpoint[index] == WDGM_CP_ENTRY) ? WDGM_CP_EXIT : WDGM_CP_ENTRY;

        if (checkpointId != expected)
        {
            WdgM_RecordViolation(index, WDGM_VIOLATION_FLOW);
        }
        WdgM_LastCheckpoint[index] = checkpointId;
    }

    return E_OK;
}

void WdgM_MainFunction(void)
{
    uint16 index;
    uint16 failedCount = 0u;
    boolean anyExpired = FALSE;
    WdgM_SupervisedEntityIdType firstFailed = 0u;
    boolean haveFirstFailed = FALSE;

    if ((WdgM_Initialised == FALSE) || (WdgM_SupervisionActive == FALSE))
    {
        /* Not supervising yet, but the hardware watchdog is already armed in its slow mode, so it
         * still has to be petted or startup itself would trip it. */
        if (WdgM_Initialised != FALSE)
        {
            Wdg_Trigger();
            WdgM_Stats.triggerCount++;
        }
        return;
    }

    WdgM_Stats.supervisionCycles++;

    for (index = 0u; index < (uint16)WDGM_SE_COUNT; index++)
    {
        if (WdgM_Entities[index].localStatus == WDGM_LOCAL_STATUS_DEACTIVATED)
        {
#if (WDGM_ALLOW_PERMANENT_DEACTIVATION == STD_OFF)
            if (Gpt_HasElapsed(WdgM_DeactivatedAt[index], WDGM_MAX_DEACTIVATION_MS) != FALSE)
            {
                /* A deactivation left in place indefinitely silently removes the protection. It is
                 * reported, not corrected -- forcing reactivation would reset a unit whose task is
                 * legitimately idle. */
                STD_DISCARD(
                    Dem_SetEventStatus(DEM_EVENT_WDGM_ALIVE, (uint8)index, DEM_EVENT_STATUS_PREFAILED));
                WdgM_DeactivatedAt[index] = Gpt_GetMonotonicMs();
            }
#endif
            continue;
        }

        /* Alive supervision over the cycle just ended. */
        if (WdgM_Entities[index].aliveCounter < WdgM_Config[index].aliveMin)
        {
            WdgM_RecordViolation(index, WDGM_VIOLATION_ALIVE_LOW);
        }
        else if (WdgM_Entities[index].aliveCounter > WdgM_Config[index].aliveMax)
        {
            /* Too many check-ins matters as much as too few: a runnable looping far faster than
             * its period starves every lower-priority task, and the symptom -- other entities
             * missing their deadlines -- points away from the actual cause. */
            WdgM_RecordViolation(index, WDGM_VIOLATION_ALIVE_HIGH);
        }
        else
        {
            /* Behaved this cycle. The failure count is decayed rather than zeroed, so an entity
             * that violates every other cycle still escalates instead of oscillating below the
             * tolerance forever. */
            if (WdgM_Entities[index].failedCycles > 0u)
            {
                WdgM_Entities[index].failedCycles--;
            }
            if (WdgM_Entities[index].failedCycles == 0u)
            {
                WdgM_Entities[index].localStatus = WDGM_LOCAL_STATUS_OK;
                WdgM_Entities[index].lastViolation = WDGM_VIOLATION_NONE;
                STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_WDGM_ALIVE, (uint8)index, DEM_EVENT_STATUS_PASSED));
                STD_DISCARD(
                    Dem_SetEventStatus(DEM_EVENT_WDGM_DEADLINE, (uint8)index, DEM_EVENT_STATUS_PASSED));
            }
        }

        WdgM_Entities[index].aliveCounter = 0u;

        if (WdgM_Entities[index].localStatus == WDGM_LOCAL_STATUS_EXPIRED)
        {
            anyExpired = TRUE;
        }
        if (WdgM_Entities[index].localStatus != WDGM_LOCAL_STATUS_OK)
        {
            failedCount++;
            if (haveFirstFailed == FALSE)
            {
                firstFailed = (WdgM_SupervisedEntityIdType)index;
                haveFirstFailed = TRUE;
            }
        }
    }

    WdgM_Stats.failedEntityCount = failedCount;
    WdgM_Stats.firstFailedEntity = firstFailed;

    if (anyExpired != FALSE)
    {
        WdgM_Global = WDGM_GLOBAL_STATUS_EXPIRED;
    }
    else if (failedCount > 0u)
    {
        WdgM_Global = WDGM_GLOBAL_STATUS_FAILED;
    }
    else
    {
        WdgM_Global = WDGM_GLOBAL_STATUS_OK;
    }

#if (WDGM_EXPIRED_STOPS_TRIGGER == STD_ON)
    if (anyExpired != FALSE)
    {
        /* Stop petting. The hardware resets the ECU roughly WDG_TIMEOUT_FAST_MS later, and that
         * delay is the point: it gives the telemetry task one last opportunity to publish the
         * expired entity and its violation, so the reset reaches the fleet as a diagnosis rather
         * than as an unexplained gap in the data. */
        WdgM_Global = WDGM_GLOBAL_STATUS_STOPPED;
        WdgM_Stats.withheldCount++;
        return;
    }
#endif

    /* A FAILED entity still gets the watchdog petted. One missed deadline must not reset a
     * vehicle's data logger; only exhausting the tolerance does. */
    Wdg_Trigger();
    WdgM_Stats.triggerCount++;
}

Std_ReturnType WdgM_GetLocalStatus(WdgM_SupervisedEntityIdType entityId, WdgM_EntityStatusType *status)
{
    DET_CHECK_RETURN(WdgM_EntityValid(entityId) != FALSE, MODULE_ID_WDGM, (uint8)entityId,
                     WDGM_API_ID_GET_LOCAL_STATUS, WDGM_E_PARAM_SEID, E_NOT_OK);
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_WDGM, (uint8)entityId, WDGM_API_ID_GET_LOCAL_STATUS,
                     WDGM_E_PARAM_POINTER, E_NOT_OK);

    *status = WdgM_Entities[entityId];
    return E_OK;
}

WdgM_GlobalStatusType WdgM_GetGlobalStatus(void)
{
    return WdgM_Global;
}

Std_ReturnType WdgM_DeactivateEntity(WdgM_SupervisedEntityIdType entityId)
{
    DET_CHECK_RETURN(WdgM_EntityValid(entityId) != FALSE, MODULE_ID_WDGM, (uint8)entityId,
                     WDGM_API_ID_SET_MODE, WDGM_E_PARAM_SEID, E_NOT_OK);

    WdgM_Entities[entityId].localStatus = WDGM_LOCAL_STATUS_DEACTIVATED;
    WdgM_Entities[entityId].aliveCounter = 0u;
    WdgM_DeactivatedAt[entityId] = Gpt_GetMonotonicMs();
    return E_OK;
}

Std_ReturnType WdgM_ActivateEntity(WdgM_SupervisedEntityIdType entityId)
{
    DET_CHECK_RETURN(WdgM_EntityValid(entityId) != FALSE, MODULE_ID_WDGM, (uint8)entityId,
                     WDGM_API_ID_SET_MODE, WDGM_E_PARAM_SEID, E_NOT_OK);

    /* Counters are reset so the entity begins with a clean cycle. Resuming with the stale
     * timestamp would immediately register a deadline violation spanning the whole suspension. */
    WdgM_Entities[entityId].localStatus = WDGM_LOCAL_STATUS_OK;
    WdgM_Entities[entityId].aliveCounter = 0u;
    WdgM_Entities[entityId].failedCycles = 0u;
    WdgM_Entities[entityId].lastViolation = WDGM_VIOLATION_NONE;
    WdgM_Entities[entityId].lastCheckpointMs = Gpt_GetMonotonicMs();
    WdgM_LastCheckpoint[entityId] = WDGM_CP_EXIT;
    return E_OK;
}

Std_ReturnType WdgM_GetStatistics(WdgM_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_WDGM, INSTANCE_ID_SINGLE, WDGM_API_ID_MAIN_FUNCTION,
                     WDGM_E_PARAM_POINTER, E_NOT_OK);

    WdgM_Stats.globalStatus = WdgM_Global;
    *stats = WdgM_Stats;
    return E_OK;
}

void WdgM_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = WDGM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_WDGM;
        versioninfo->sw_major_version = WDGM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = WDGM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = WDGM_SW_PATCH_VERSION;
    }
}

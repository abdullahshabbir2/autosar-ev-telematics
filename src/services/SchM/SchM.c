/**
 * @file    SchM.c
 * @brief   Scheduler implementation: the dispatch tables and the task bodies.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/SchM/SchM.h"

#include <string.h>

#include "app/BattSwc/BattSwc.h"
#include "mcal/Can/Can.h"
#include "ecuabs/CanIf/CanIf.h"
#include "services/ComM/ComM.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "app/DiagSwc/DiagSwc.h"
#include "ecuabs/FsAbs/FsAbs.h"
#include "ecuabs/GnssIf/GnssIf.h"
#include "mcal/Gpt/Gpt.h"
#include "app/HmiSwc/HmiSwc.h"
#include "ecuabs/NetIf/NetIf.h"
#include "services/NvM/NvM.h"
#include "app/OdoSwc/OdoSwc.h"
#include "services/SchM/SchM_Platform.h"
#include "app/TelemSwc/TelemSwc.h"
#include "services/WdgM/WdgM.h"

/*==================================================================================================
 *  Runnable adapters
 *
 *  The dispatch table needs a uniform `void (*)(void)`. Modules whose cyclic entry point returns a status
 *  or a count get a one-line adapter, which also documents what is deliberately being ignored at the
 *  dispatch point.
 *================================================================================================*/

STATIC void SchM_RunCanRead(void)
{
    /* The frame count is informational; CanIf polls the queue regardless. */
    (void)Can_MainFunction_Read();
}

STATIC void SchM_RunCanIf(void)
{
    (void)CanIf_MainFunction();
}

STATIC void SchM_RunGnss(void)
{
    (void)GnssIf_MainFunction();
}

/**
 * @brief Feed the odometer from the most recent CAN sample.
 *
 * Runs in the acquisition task rather than the scheduler tick, so that one distance sample corresponds to
 * one acquisition cycle and the record's speed field matches the interval the odometer integrated.
 */
STATIC void SchM_RunOdometrySample(void)
{
    CanIf_McuDataType mcu;

    if ((CanIf_GetMcuData(&mcu) != E_OK) || (CanIf_IsMcuDataFresh() == FALSE))
    {
        /* No fresh motor data. Nothing is fed, so the odometer's own gap detection handles the interval --
         * which is what stops a silent controller from accumulating distance at its last known speed. */
        return;
    }

    STD_DISCARD(OdoSwc_ProcessSpeedSample(mcu.motorRpm, mcu.driveStateTimestamp, NULL_PTR));
}

STATIC void SchM_RunBattery(void)
{
    /* A partial round is a normal outcome and is already reported per pack by BattSwc. */
    STD_DISCARD(BattSwc_MainFunction());
}

STATIC void SchM_RunTelemetryStore(void)
{
    STD_DISCARD(TelemSwc_AcquireAndStore());
}

/*==================================================================================================
 *  Dispatch tables
 *
 *  One entry per runnable: what to call, and how many task activations between calls. A divider of 1 means
 *  every activation. Dividers rather than absolute periods, so a runnable's rate is expressed relative to
 *  its task and cannot silently disagree with it.
 *================================================================================================*/

typedef struct
{
    void (*runnable)(void); /**< Entry point.                                  */
    uint16 divider;         /**< Task activations between calls; 1 = every one. */
    const char *name;       /**< For overrun reporting and the health record.   */
} SchM_RunnableType;

/*---- Scheduler tick, every 10 ms ------------------------------------------*/
STATIC const SchM_RunnableType SchM_SchedulerRunnables[] = {
    /* Supervision first, so that if anything below overruns, the supervision for this cycle has already
     * been recorded rather than being lost along with the rest of the activation. */
    {&WdgM_MainFunction, 100u, "WdgM"},   /* 1 s: the supervision cycle.        */
    {&SchM_RunCanRead, 1u, "CanRead"},    /* 10 ms: drain the controller.       */
    {&SchM_RunCanIf, 1u, "CanIf"},        /* 10 ms: decode what was drained.    */
    {&SchM_RunGnss, 10u, "Gnss"},         /* 100 ms: NMEA arrives at 1 Hz.      */
    {&HmiSwc_MainFunction, 10u, "Hmi"},   /* 100 ms: the blink tick.            */
};

/*---- Acquisition, every 3 s ------------------------------------------------*/
STATIC const SchM_RunnableType SchM_AcquisitionRunnables[] = {
    {&SchM_RunBattery, 1u, "Battery"},
    {&SchM_RunOdometrySample, 1u, "Odometry"},
};

/*---- Storage, every 3 s ----------------------------------------------------*/
STATIC const SchM_RunnableType SchM_StorageRunnables[] = {
    /* Record assembly first: it is what the cycle exists to produce. Everything after it is housekeeping
     * that can slip a cycle without consequence. */
    {&SchM_RunTelemetryStore, 1u, "TelemStore"},
    {&OdoSwc_MainFunction, 1u, "OdoPersist"},
    {&NvM_MainFunction, 1u, "NvM"},
    {&DiagSwc_MainFunction, 1u, "Diag"},
    {&FsAbs_MainFunction, 20u, "FsHousekeep"}, /* 60 s: free space changes slowly. */
};

/*---- Connectivity, every 1 s ----------------------------------------------*/
STATIC const SchM_RunnableType SchM_ConnectivityRunnables[] = {
    /* Arbitration before the link state machine, so a decision takes effect in the same cycle it is made
     * rather than a second later. */
    {&ComM_MainFunction, 1u, "ComM"},
    {&NetIf_MainFunction, 1u, "NetIf"},
    {&TelemSwc_MainFunction, 1u, "Telem"},
};

/*==================================================================================================
 *  Task configuration
 *================================================================================================*/

typedef struct
{
    const SchM_RunnableType *runnables;
    uint8 runnableCount;
    uint32 periodMs;
    uint32 budgetUs;
    uint8 priority;
    uint8 core;
    uint32 stackBytes;
    WdgM_SupervisedEntityIdType entity;
    const char *name;
} SchM_TaskConfigType;

STATIC const SchM_TaskConfigType SchM_TaskConfig[SCHM_TASK_COUNT] = {
    {SchM_SchedulerRunnables, (uint8)STD_ARRAY_SIZE(SchM_SchedulerRunnables),
     SCHM_PERIOD_SCHEDULER_MS, SCHM_BUDGET_SCHEDULER_US, SCHM_PRIORITY_SCHEDULER,
     SCHM_CORE_SCHEDULER, SCHM_STACK_SCHEDULER, SCHM_SE_SCHEDULER, "SchM"},
    {SchM_AcquisitionRunnables, (uint8)STD_ARRAY_SIZE(SchM_AcquisitionRunnables),
     SCHM_PERIOD_ACQUISITION_MS, SCHM_BUDGET_ACQUISITION_US, SCHM_PRIORITY_ACQUISITION,
     SCHM_CORE_ACQUISITION, SCHM_STACK_ACQUISITION, SCHM_SE_ACQUISITION, "Acq"},
    {SchM_StorageRunnables, (uint8)STD_ARRAY_SIZE(SchM_StorageRunnables), SCHM_PERIOD_STORAGE_MS,
     SCHM_BUDGET_STORAGE_US, SCHM_PRIORITY_STORAGE, SCHM_CORE_STORAGE, SCHM_STACK_STORAGE,
     SCHM_SE_STORAGE, "Store"},
    {SchM_ConnectivityRunnables, (uint8)STD_ARRAY_SIZE(SchM_ConnectivityRunnables),
     SCHM_PERIOD_CONNECTIVITY_MS, SCHM_BUDGET_CONNECTIVITY_US, SCHM_PRIORITY_CONNECTIVITY,
     SCHM_CORE_CONNECTIVITY, SCHM_STACK_CONNECTIVITY, SCHM_SE_CONNECTIVITY, "Conn"},
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(STD_ARRAY_SIZE(SchM_TaskConfig) == SCHM_TASK_COUNT,
               "the task table does not match SCHM_TASK_COUNT");
_Static_assert(STD_ARRAY_SIZE(SchM_SchedulerRunnables) <= SCHM_MAX_RUNNABLES_PER_TASK,
               "too many runnables in the scheduler task");
_Static_assert(STD_ARRAY_SIZE(SchM_StorageRunnables) <= SCHM_MAX_RUNNABLES_PER_TASK,
               "too many runnables in the storage task");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean SchM_Initialised = FALSE;
STATIC SchM_TaskStatsType SchM_Stats[SCHM_TASK_COUNT];

/** Activation counter per task, for the runnable dividers. */
STATIC uint32 SchM_ActivationCount[SCHM_TASK_COUNT];

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType SchM_Init(void)
{
    (void)memset(SchM_Stats, 0, sizeof(SchM_Stats));
    (void)memset(SchM_ActivationCount, 0, sizeof(SchM_ActivationCount));
    SchM_Initialised = TRUE;
    return E_OK;
}

void SchM_RunTask(SchM_TaskType task)
{
    const SchM_TaskConfigType *config;
    SchM_TaskStatsType *stats;
    uint64 bodyStartUs;
    uint8 index;

    if ((SchM_Initialised == FALSE) || (task >= (SchM_TaskType)SCHM_TASK_COUNT))
    {
        return;
    }

    config = &SchM_TaskConfig[task];
    stats = &SchM_Stats[task];

    /* Entry checkpoint before any work. WdgM's program-flow supervision pairs it with the exit, so a body
     * that returns early through an unintended path is detected. */
    STD_DISCARD(WdgM_CheckpointReached(config->entity, WDGM_CP_ENTRY));

    bodyStartUs = Gpt_GetMonotonicUs();
    SchM_ActivationCount[task]++;
    stats->activations++;

    for (index = 0u; index < config->runnableCount; index++)
    {
        const SchM_RunnableType *runnable = &config->runnables[index];
        uint64 runnableStartUs;
        uint32 runnableUs;

        if ((SchM_ActivationCount[task] % (uint32)runnable->divider) != 0u)
        {
            continue;
        }

        runnableStartUs = Gpt_GetMonotonicUs();
        runnable->runnable();
        runnableUs = (uint32)(Gpt_GetMonotonicUs() - runnableStartUs);

        /* Per-runnable timing is what makes an overrun attributable. v1's acquisition task blocked for up
         * to thirteen seconds inside a three-second period and nothing recorded which part of it was
         * responsible, so the only way to find out was to comment things out and reflash. */
        if (runnableUs > stats->worstRunnableUs)
        {
            stats->worstRunnableUs = runnableUs;
            stats->worstRunnableIndex = index;
        }
    }

    stats->lastCaseUs = (uint32)(Gpt_GetMonotonicUs() - bodyStartUs);
    if (stats->lastCaseUs > stats->worstCaseUs)
    {
        stats->worstCaseUs = stats->lastCaseUs;
    }

    if (stats->lastCaseUs > config->budgetUs)
    {
        stats->overruns++;
        (void)Det_ReportRuntimeError(MODULE_ID_SCHM, (uint8)task, SCHM_API_ID_START,
                                     SCHM_E_OVERRUN);
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_TASK_OVERRUN, (uint8)task,
                                       DEM_EVENT_STATUS_FAILED));
    }
    else
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_TASK_OVERRUN, (uint8)task,
                                       DEM_EVENT_STATUS_PASSED));
    }

    stats->stackHighWaterMark = SchM_PlatformGetStackHighWaterMark();

    STD_DISCARD(WdgM_CheckpointReached(config->entity, WDGM_CP_EXIT));
}

Std_ReturnType SchM_StartTasks(void)
{
    uint8 task;

    DET_CHECK_RETURN(SchM_Initialised != FALSE, MODULE_ID_SCHM, INSTANCE_ID_SINGLE,
                     SCHM_API_ID_START, SCHM_E_UNINIT, E_NOT_OK);

    for (task = 0u; task < (uint8)SCHM_TASK_COUNT; task++)
    {
        const SchM_TaskConfigType *config = &SchM_TaskConfig[task];

        if (SchM_PlatformCreateTask((SchM_TaskType)task, config->name, config->stackBytes,
                                    config->priority, config->core, config->periodMs) != E_OK)
        {
            (void)Det_ReportError(MODULE_ID_SCHM, task, SCHM_API_ID_START,
                                  SCHM_E_TASK_CREATE_FAILED);
            /* Fatal to the caller. An ECU missing one of its four tasks would silently stop doing part of
             * its job -- acquiring but never storing, for instance -- and nothing downstream would
             * distinguish that from a vehicle that was not moving. */
            return E_NOT_OK;
        }
    }

    return E_OK;
}

Std_ReturnType SchM_GetTaskStats(SchM_TaskType task, SchM_TaskStatsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_SCHM, (uint8)task, SCHM_API_ID_GET_STATS,
                     SCHM_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(task < (SchM_TaskType)SCHM_TASK_COUNT, MODULE_ID_SCHM, (uint8)task,
                     SCHM_API_ID_GET_STATS, SCHM_E_PARAM_TASK, E_NOT_OK);

    *stats = SchM_Stats[task];
    return E_OK;
}

void SchM_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = SCHM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_SCHM;
        versioninfo->sw_major_version = SCHM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = SCHM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = SCHM_SW_PATCH_VERSION;
    }
}

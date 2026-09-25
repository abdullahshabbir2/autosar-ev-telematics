/**
 * @file    SchM.h
 * @brief   BSW Scheduler -- fixed cyclic task and runnable dispatch.
 *
 * Creates the four tasks, dispatches each module's cyclic runnable at its configured period, and measures
 * how long each one takes. Presents the fixed-cyclic execution model AUTOSAR assumes, implemented on
 * FreeRTOS rather than on an AUTOSAR OS.
 *
 * @par Why a dispatch table rather than calls in a task body
 * Each task's body is a loop over a table of {runnable, period, supervised entity}. Three consequences,
 * all of which the v1 design lacked:
 *
 *  - **The schedule is data, so it can be inspected.** What runs, how often, and in which task is a table
 *    in SchM_Cfg.h rather than a sequence of calls buried in a 200-line task function.
 *  - **Execution time is measured per runnable.** Every dispatch is timed and the worst case is retained,
 *    so "which runnable is making the acquisition task overrun" is answerable from the health record
 *    rather than by attaching a debugger to a vehicle.
 *  - **Overruns are attributable.** A runnable that exceeds its budget is reported with its own identity.
 *    v1's acquisition task blocked for up to thirteen seconds inside a three-second period, and nothing
 *    recorded which part of it was responsible.
 *
 * @par Deliberate absence of run-time task creation
 * Every task is created once, during startup, and none is ever deleted. A system that creates tasks at
 * run time has a heap whose worst case depends on its history, which makes a stack-overflow or
 * out-of-memory fault dependent on what the vehicle happened to do beforehand -- the hardest kind of
 * fault to reproduce.
 *
 * @req SWREQ-SYS-0040 .. SWREQ-SYS-0058
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef SCHM_H
#define SCHM_H

#include "base/Autosar_ModuleIds.h"
#include "services/SchM/SchM_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHM_VENDOR_ID 0xFFFEu
#define SCHM_SW_MAJOR_VERSION 2u
#define SCHM_SW_MINOR_VERSION 0u
#define SCHM_SW_PATCH_VERSION 0u

#define SCHM_API_ID_INIT 0x00u
#define SCHM_API_ID_START 0x01u
#define SCHM_API_ID_GET_STATS 0x20u

#define SCHM_E_UNINIT E_UNINIT
#define SCHM_E_PARAM_POINTER E_PARAM_POINTER
#define SCHM_E_PARAM_TASK 0x20u
#define SCHM_E_TASK_CREATE_FAILED 0x21u
#define SCHM_E_OVERRUN 0x22u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Logical task. Values index the configuration table. */
typedef enum
{
    SCHM_TASK_SCHEDULER = 0,   /**< 10 ms tick: supervision, CAN, GNSS.       */
    SCHM_TASK_ACQUISITION = 1, /**< Acquisition period: sensors and odometry. */
    SCHM_TASK_STORAGE = 2,     /**< Acquisition period: record and storage.   */
    SCHM_TASK_CONNECTIVITY = 3 /**< 1 s: bearer and telemetry.                */
} SchM_TaskType;

/** Per-task execution figures, published in the health record. */
typedef struct
{
    uint32 activations;        /**< Times the task's loop body has run.            */
    uint32 overruns;           /**< Activations that exceeded the task's budget.    */
    uint32 worstCaseUs;        /**< Longest observed body execution, microseconds.  */
    uint32 lastCaseUs;         /**< Most recent body execution, microseconds.       */
    uint32 worstRunnableUs;    /**< Longest observed single runnable, microseconds.  */
    uint8 worstRunnableIndex;  /**< Which runnable that was.                        */
    uint32 stackHighWaterMark; /**< Smallest observed free stack, bytes.            */
} SchM_TaskStatsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Prepare the scheduler. Does not create any task.
 *
 * Separated from ::SchM_StartTasks so that EcuM can initialise every module before anything begins
 * running cyclically -- a runnable dispatched against an uninitialised module would report a contract
 * violation on its first activation.
 */
CHECK_RETURN Std_ReturnType SchM_Init(void);

/**
 * @brief Create and start all four tasks.
 *
 * The last thing ::EcuM_Init does. Returns once the tasks exist; they then run independently and this
 * function's caller becomes the idle path.
 *
 * @return E_OK if every task was created; E_NOT_OK if any could not be, which EcuM treats as fatal --
 *         an ECU missing one of its tasks would silently stop doing part of its job.
 */
CHECK_RETURN Std_ReturnType SchM_StartTasks(void);

/**
 * @brief Read a task's execution figures.
 * @param[in]  task  Which task.
 * @param[out] stats Destination.
 */
CHECK_RETURN Std_ReturnType SchM_GetTaskStats(SchM_TaskType task, SchM_TaskStatsType *stats);

/**
 * @brief Run one activation of @p task's body.
 *
 * Exposed so a host test can drive the schedule without creating threads, and so the platform's task
 * wrapper has one thing to call. Times every runnable, checks the task's budget, and reports the
 * supervised entity's checkpoints.
 */
void SchM_RunTask(SchM_TaskType task);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void SchM_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* SCHM_H */

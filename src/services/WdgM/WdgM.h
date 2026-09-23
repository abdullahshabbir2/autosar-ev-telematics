/**
 * @file    WdgM.h
 * @brief   AUTOSAR Watchdog Manager (SWS_WatchdogManager).
 *
 * Supervises the execution of every cyclic runnable and pets the hardware watchdog only while all
 * of them are behaving. The hardware watchdog alone can detect just one failure -- the whole
 * system stopping. WdgM detects the failures that actually happen: one runnable stalling while
 * the rest continue, a runnable running far more often than it should, or the program taking a
 * path it was never supposed to take.
 *
 * @par What v1 had, and why it protected nothing
 * v1 called @c esp_task_wdt_init(60, true) and then never subscribed a single task and never
 * called @c esp_task_wdt_reset() -- its one call site was commented out. The watchdog was inert.
 * A hung acquisition task would have stalled the logger indefinitely with no recovery, while the
 * code read as though it were protected. That is worse than having no watchdog at all, because
 * it stops anyone looking for the problem.
 *
 * @par Three kinds of supervision
 *  - **Alive supervision.** Within each supervision cycle, a runnable's check-in count must land
 *    between a configured minimum and maximum. Too few means it is stalling; too many means it is
 *    running away, which matters just as much because a runaway task starves the others.
 *  - **Deadline supervision.** The interval between two check-ins of the same runnable must not
 *    exceed its deadline. This catches a single long stall that an averaged count would hide --
 *    an SD write blocking for eight seconds inside a three-second cycle is invisible to a
 *    count over a minute, and it is exactly the failure this ECU suffers.
 *  - **Program-flow supervision.** Checkpoints within a runnable must be reached in the
 *    configured order. This catches a corrupted jump or a path skipped by a mis-taken branch,
 *    which neither of the other two notices at all.
 *
 * @par Reaction
 * A supervised entity that fails moves from OK to FAILED to EXPIRED. On FAILED, WdgM raises a
 * diagnostic event and keeps petting the hardware -- a single missed deadline should not reset a
 * vehicle's data logger. On EXPIRED it stops petting, and the hardware watchdog resets the ECU
 * about ::WDG_TIMEOUT_FAST_MS later. The delay is deliberate: it gives the telemetry task one
 * last chance to publish the reason, so the reset arrives at the fleet as a diagnosis rather than
 * as an unexplained gap in the data.
 *
 * @req SWREQ-SAF-0001 .. SWREQ-SAF-0012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef WDGM_H
#define WDGM_H

#include "Autosar_ModuleIds.h"
#include "Std_Types.h"
#include "WdgM_Cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDGM_VENDOR_ID 0xFFFEu
#define WDGM_AR_RELEASE_MAJOR_VERSION 4u
#define WDGM_AR_RELEASE_MINOR_VERSION 4u
#define WDGM_SW_MAJOR_VERSION 2u
#define WDGM_SW_MINOR_VERSION 0u
#define WDGM_SW_PATCH_VERSION 0u

#define WDGM_API_ID_INIT 0x00u
#define WDGM_API_ID_CHECKPOINT_REACHED 0x03u
#define WDGM_API_ID_GET_LOCAL_STATUS 0x04u
#define WDGM_API_ID_GET_GLOBAL_STATUS 0x05u
#define WDGM_API_ID_MAIN_FUNCTION 0x08u
#define WDGM_API_ID_SET_MODE 0x02u

#define WDGM_E_UNINIT E_UNINIT
#define WDGM_E_PARAM_POINTER E_PARAM_POINTER
#define WDGM_E_PARAM_SEID 0x20u
#define WDGM_E_PARAM_CPID 0x21u
#define WDGM_E_ALIVE_VIOLATION 0x22u
#define WDGM_E_DEADLINE_VIOLATION 0x23u
#define WDGM_E_FLOW_VIOLATION 0x24u
#define WDGM_E_WDG_DISABLED 0x25u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Supervised entity identifier; values are in WdgM_Cfg.h. */
typedef uint16 WdgM_SupervisedEntityIdType;

/** Checkpoint identifier within a supervised entity. */
typedef uint16 WdgM_CheckpointIdType;

/** Per-entity supervision status (SWS_WdgM_00204). */
typedef enum
{
    WDGM_LOCAL_STATUS_OK = 0,           /**< Behaving.                                 */
    WDGM_LOCAL_STATUS_FAILED = 1,       /**< Violated, within the tolerance budget.    */
    WDGM_LOCAL_STATUS_EXPIRED = 2,      /**< Tolerance exhausted; a reset will follow.  */
    WDGM_LOCAL_STATUS_DEACTIVATED = 3   /**< Not supervised in the current mode.        */
} WdgM_LocalStatusType;

/** Overall supervision status. */
typedef enum
{
    WDGM_GLOBAL_STATUS_OK = 0,          /**< Every active entity is OK.                 */
    WDGM_GLOBAL_STATUS_FAILED = 1,      /**< At least one entity has failed.            */
    WDGM_GLOBAL_STATUS_EXPIRED = 2,     /**< At least one entity has expired.           */
    WDGM_GLOBAL_STATUS_STOPPED = 3,     /**< Petting has stopped; a reset is imminent.  */
    WDGM_GLOBAL_STATUS_DEACTIVATED = 4  /**< Supervision is not running.                */
} WdgM_GlobalStatusType;

/** Which rule an entity broke. */
typedef enum
{
    WDGM_VIOLATION_NONE = 0,     /**< No violation.                                    */
    WDGM_VIOLATION_ALIVE_LOW = 1,/**< Checked in too few times in a cycle.             */
    WDGM_VIOLATION_ALIVE_HIGH = 2,/**< Checked in too many times in a cycle.           */
    WDGM_VIOLATION_DEADLINE = 3, /**< Interval between check-ins exceeded the deadline.*/
    WDGM_VIOLATION_FLOW = 4      /**< Checkpoints reached out of order.                */
} WdgM_ViolationType;

/** Everything WdgM holds about one supervised entity. */
typedef struct
{
    WdgM_LocalStatusType localStatus;  /**< Current status.                            */
    WdgM_ViolationType lastViolation;  /**< Most recent rule broken.                   */
    uint16 aliveCounter;               /**< Check-ins in the current cycle.            */
    uint16 failedCycles;               /**< Consecutive cycles with a violation.        */
    uint32 totalCheckpoints;           /**< Lifetime check-ins.                         */
    uint32 aliveViolations;            /**< Lifetime alive violations.                  */
    uint32 deadlineViolations;         /**< Lifetime deadline violations.               */
    uint32 flowViolations;             /**< Lifetime program-flow violations.           */
    uint32 worstIntervalMs;            /**< Longest observed gap between check-ins.     */
    uint32 lastCheckpointMs;           /**< When it last checked in.                    */
} WdgM_EntityStatusType;

/** Aggregate supervision figures, published in the telemetry health record. */
typedef struct
{
    WdgM_GlobalStatusType globalStatus; /**< Overall status.                            */
    uint32 supervisionCycles;           /**< Cycles evaluated since Init.               */
    uint32 triggerCount;                /**< Times the hardware watchdog was petted.    */
    uint32 withheldCount;               /**< Cycles where petting was deliberately withheld. */
    uint16 failedEntityCount;           /**< Entities currently not OK.                 */
    WdgM_SupervisedEntityIdType firstFailedEntity; /**< The first entity that failed.   */
} WdgM_StatisticsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Initialise supervision and start the hardware watchdog in its slow mode.
 *
 * Slow, because startup legitimately blocks for seconds mounting the SD card and attaching to
 * GPRS. ::WdgM_ActivateSupervision switches to the fast timeout once the cyclic tasks are
 * genuinely running -- switching earlier would reset a unit that is merely starting up slowly.
 *
 * @return E_OK on success; E_NOT_OK if the watchdog driver refused to start, which is reported
 *         and left to EcuM to act on.
 */
CHECK_RETURN Std_ReturnType WdgM_Init(void);

/**
 * @brief Begin supervising, and move the hardware watchdog to its fast timeout.
 *
 * Called by EcuM once the cyclic tasks are running.
 */
CHECK_RETURN Std_ReturnType WdgM_ActivateSupervision(void);

/**
 * @brief Report that @p checkpointId in @p entityId has been reached.
 *
 * The one call every supervised runnable makes. Updates the entity's alive counter, checks the
 * interval since its previous check-in against its deadline, and verifies the checkpoint order.
 *
 * @return E_OK if the checkpoint was accepted; E_NOT_OK for an unknown entity or checkpoint.
 *         A *violation* still returns E_OK: the call was well formed, and the violation is
 *         reported through the entity's status, not through this return value.
 */
CHECK_RETURN Std_ReturnType WdgM_CheckpointReached(WdgM_SupervisedEntityIdType entityId,
                                                  WdgM_CheckpointIdType checkpointId);

/**
 * @brief Evaluate every entity and pet or withhold the hardware watchdog.
 *
 * Driven by the highest-priority cyclic task at ::WDGM_SUPERVISION_CYCLE_MS. It is the only
 * caller of ::Wdg_Trigger in the whole project: a task that pets the hardware itself defeats
 * every check above, which is the usual way a watchdog ends up protecting nothing.
 */
void WdgM_MainFunction(void);

/**
 * @brief Read @p entityId's status.
 * @param[out] status Destination.
 */
CHECK_RETURN Std_ReturnType WdgM_GetLocalStatus(WdgM_SupervisedEntityIdType entityId,
                                                WdgM_EntityStatusType *status);

/** Overall supervision status. */
WdgM_GlobalStatusType WdgM_GetGlobalStatus(void);

/**
 * @brief Suspend supervision of @p entityId.
 *
 * For a task that is legitimately not running -- the telemetry task while no bearer is available,
 * for instance. A deactivated entity cannot cause a reset, so this must be used deliberately;
 * WdgM records how long each entity has been deactivated so an accidental permanent suspension is
 * visible in the diagnostics rather than silently disabling the protection.
 */
CHECK_RETURN Std_ReturnType WdgM_DeactivateEntity(WdgM_SupervisedEntityIdType entityId);

/** Resume supervision of @p entityId, resetting its counters so it starts with a clean cycle. */
CHECK_RETURN Std_ReturnType WdgM_ActivateEntity(WdgM_SupervisedEntityIdType entityId);

/**
 * @brief Read the aggregate supervision figures.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType WdgM_GetStatistics(WdgM_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void WdgM_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* WDGM_H */

/**
 * @file    EcuM.h
 * @brief   ECU State Manager (SWS_ECUStateManager) -- startup, shutdown and degraded operation.
 *
 * Owns the order in which the stack comes up, decides what to do when part of it does not, and provides
 * the only supported way to shut the ECU down without losing data.
 *
 * @par The startup order is not arbitrary
 * Four constraints, each of which matters:
 *
 *  1. **Mcu first.** It latches the reset cause, which is the input to crash-loop detection. Anything that
 *     runs before it risks overwriting the register.
 *  2. **Det second.** Any module's `Init` may report a contract violation, and a report arriving before
 *     `Det_Init` can only be counted, not recorded with its detail -- which is exactly the window a
 *     misconfiguration shows up in.
 *  3. **WdgM before the long blocking work, in its slow mode.** Mounting the card, attaching to GPRS and
 *     the first broker handshake legitimately take tens of seconds. Arming the fast timeout here would
 *     reset a unit that is merely starting up slowly.
 *  4. **WdgM_ActivateSupervision last.** Supervised entities start deactivated; activating them before
 *     their tasks exist would have every one report an alive violation on the first cycle.
 *
 * @par Nothing except a missing task is fatal
 * This ECU is a data logger on a vehicle with no service connection, so almost every subsystem failure
 * should reduce what it records rather than stop it recording. A failed CAN controller costs odometry but
 * not battery data; a failed card costs the store-and-forward buffer but not live publishing. Each failure
 * raises a diagnostic event and startup continues.
 *
 * v1 did the opposite: `if (can.init_can()) ... else { delay(1000); ESP.restart(); while(1); }`. A vehicle
 * with a disconnected CAN harness rebooted forever, logging nothing at all -- including the fault that
 * would have explained why.
 *
 * @par Crash-loop detection
 * A unit whose card is unmountable, or whose CAN controller never answers, would otherwise reset
 * repeatedly forever. EcuM counts resets within a window and, past the threshold, starts in a degraded
 * mode that skips the subsystem implicated by the previous run's reset cause. v1 had the counting half --
 * a restart count in NVS -- but its only reaction was to upload the current log file and carry on
 * resetting.
 *
 * @req SWREQ-SYS-0060 .. SWREQ-SYS-0085
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ECUM_H
#define ECUM_H

#include "base/Autosar_ModuleIds.h"
#include "services/EcuM/EcuM_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECUM_VENDOR_ID 0xFFFEu
#define ECUM_AR_RELEASE_MAJOR_VERSION 4u
#define ECUM_AR_RELEASE_MINOR_VERSION 4u
#define ECUM_SW_MAJOR_VERSION 2u
#define ECUM_SW_MINOR_VERSION 0u
#define ECUM_SW_PATCH_VERSION 0u

#define ECUM_API_ID_INIT 0x00u
#define ECUM_API_ID_STARTUP_TWO 0x01u
#define ECUM_API_ID_SHUTDOWN 0x02u
#define ECUM_API_ID_GET_STATE 0x03u

#define ECUM_E_UNINIT E_UNINIT
#define ECUM_E_PARAM_POINTER E_PARAM_POINTER
#define ECUM_E_STARTUP_FAILED 0x20u
#define ECUM_E_CRASH_LOOP 0x21u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** ECU state (SWS_EcuM_00532, reduced to the states this ECU actually has). */
typedef enum
{
    ECUM_STATE_STARTUP = 0,      /**< Bringing the stack up.                        */
    ECUM_STATE_RUN = 1,          /**< Normal operation; all tasks running.          */
    ECUM_STATE_RUN_DEGRADED = 2, /**< Running with one or more subsystems skipped. */
    ECUM_STATE_SHUTDOWN = 3      /**< Flushing state before a reset.                */
} EcuM_StateType;

/** Which subsystems came up, and which did not. */
typedef struct
{
    boolean canAvailable;     /**< The CAN controller initialised.                */
    boolean packsAvailable;   /**< At least one battery pack answered discovery.   */
    boolean gnssAvailable;    /**< The GNSS receiver's link opened.               */
    boolean storageAvailable; /**< The card mounted.                              */
    boolean clockValid;       /**< A plausible wall-clock time was established.    */
    boolean nvmValid;         /**< Every NvM block loaded without falling back.    */
} EcuM_SubsystemStatusType;

/** Startup and reset history, published in the health record. */
typedef struct
{
    EcuM_StateType state;                /**< Current state.                          */
    EcuM_SubsystemStatusType subsystems; /**< What is available.                  */
    uint8 lastResetReason;               /**< ::Mcu_ResetReasonType of this start.     */
    uint16 restartCount;                 /**< Resets inside the current window.        */
    uint32 totalRestarts;                /**< Lifetime resets.                        */
    boolean crashLoopDetected;           /**< TRUE if the threshold was exceeded.      */
    uint32 startupDurationMs;            /**< How long startup took.                   */
    uint8 degradedSubsystemMask;         /**< Bit set per subsystem skipped this run.  */
} EcuM_StatusType;

/** Bits in ::EcuM_StatusType::degradedSubsystemMask. */
#define ECUM_DEGRADED_CAN 0x01u
#define ECUM_DEGRADED_PACKS 0x02u
#define ECUM_DEGRADED_GNSS 0x04u
#define ECUM_DEGRADED_STORAGE 0x08u
#define ECUM_DEGRADED_NETWORK 0x10u

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Bring the whole stack up and start the tasks.
 *
 * The single entry point `main()` calls. Returns once the tasks are running.
 *
 * @return E_OK if the ECU reached ::ECUM_STATE_RUN or ::ECUM_STATE_RUN_DEGRADED; E_NOT_OK only if the
 *         tasks could not be created, which is the one condition that leaves the ECU unable to do any part
 *         of its job.
 */
CHECK_RETURN Std_ReturnType EcuM_Init(void);

/**
 * @brief Flush everything durable and reset the ECU. Never returns.
 *
 * The only supported way to reset. In order: stop supervision so the watchdog cannot bite mid-flush,
 * persist the odometer, persist the diagnostic record, flush every dirty NvM block, publish a final health
 * record if a session exists, then reset.
 *
 * ::Mcu_PerformReset is the raw alternative and flushes nothing.
 */
NORETURN void EcuM_ShutdownAndReset(void);

/** The current state. */
EcuM_StateType EcuM_GetState(void);

/**
 * @brief Read the startup and reset history.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType EcuM_GetStatus(EcuM_StatusType *status);

/**
 * @brief Whether @p subsystemMask is being skipped this run because of a crash loop.
 *
 * Consulted by the startup sequence itself, and readable over diagnostics so an operator can see *why* a
 * unit is not reporting battery data.
 */
boolean EcuM_IsSubsystemDegraded(uint8 subsystemMask);

/**
 * @brief Clear the crash-loop counter.
 *
 * Called once the ECU has run for ::ECUM_STABLE_RUN_MS without resetting, and reachable from the
 * diagnostic channel so a technician who has fixed the cause can clear the degraded mode without waiting.
 */
void EcuM_ClearCrashLoopCounter(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void EcuM_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* ECUM_H */

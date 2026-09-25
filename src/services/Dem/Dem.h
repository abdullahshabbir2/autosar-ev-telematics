/**
 * @file    Dem.h
 * @brief   AUTOSAR Diagnostic Event Manager (SWS_DEM).
 *
 * Turns "this subsystem reported a failure just now" into "this vehicle has a confirmed fault",
 * with the debouncing, the trouble code, the freeze-frame snapshot and the persistence that make
 * the second statement useful to whoever repairs it.
 *
 * @par Why this layer exists rather than logging
 * The v1 firmware had a @c byte @c flags[15] array. Each flag was a single bit meaning roughly
 * "working" or "not working", it was overwritten on every cycle, it did not survive a reset, and
 * nothing recorded *when* or *how often* anything failed. A unit returned from the field
 * therefore carried no evidence at all: the only diagnostic information was whatever happened to
 * be in the last serial log, and the units have no serial connection in service.
 *
 * Dem keeps, per fault: a debounce counter so a single transient does not raise an alarm, a
 * confirmed/pending distinction, an occurrence count, first and most recent timestamps, a
 * snapshot of the conditions when it confirmed, and -- for the faults that matter after a power
 * cycle -- all of that in NvM.
 *
 * @par Event, status and code
 *  - An **event** is something a module can report, identified by a ::Dem_EventIdType.
 *  - Its **status** is an ISO 14229 byte: test failed, test failed this cycle, pending,
 *    confirmed, and so on.
 *  - Its **DTC** is the code a diagnostic tool sees. Several instances of one event share a DTC
 *    and are distinguished by the low byte, so "pack 3 is silent" needs no separate entry.
 *
 * @par Debouncing
 * Counter-based, as AUTOSAR describes it: a reported failure increments toward the event's
 * threshold and a reported pass decrements toward zero. An event confirms only on reaching the
 * threshold, which is three consecutive failures by default. On this vehicle, at a 3 s
 * acquisition period, that is nine seconds of a fault persisting -- long enough to exclude an
 * ignition transient, short enough that a real failure is reported within one telemetry interval.
 *
 * @req SWREQ-DIAG-0010 .. SWREQ-DIAG-0032
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DEM_H
#define DEM_H

#include "base/Autosar_ModuleIds.h"
#include "services/Dem/Dem_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEM_VENDOR_ID 0xFFFEu
#define DEM_AR_RELEASE_MAJOR_VERSION 4u
#define DEM_AR_RELEASE_MINOR_VERSION 4u
#define DEM_SW_MAJOR_VERSION 2u
#define DEM_SW_MINOR_VERSION 0u
#define DEM_SW_PATCH_VERSION 0u

#define DEM_API_ID_INIT 0x01u
#define DEM_API_ID_SET_EVENT_STATUS 0x0Bu
#define DEM_API_ID_GET_EVENT_STATUS 0x0Fu
#define DEM_API_ID_CLEAR_DTC 0x13u
#define DEM_API_ID_GET_DTC_INFO 0x14u
#define DEM_API_ID_MAIN_FUNCTION 0x55u

#define DEM_E_UNINIT E_UNINIT
#define DEM_E_PARAM_POINTER E_PARAM_POINTER
#define DEM_E_PARAM_EVENT_ID 0x20u
#define DEM_E_PARAM_STATUS 0x21u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Configured event identifier; values are in Dem_Cfg.h. */
typedef uint16 Dem_EventIdType;

/** ISO 14229 three-byte diagnostic trouble code, held in the low 24 bits. */
typedef uint32 Dem_DtcType;

/** What a module is reporting about an event (SWS_Dem_00169). */
typedef enum
{
    DEM_EVENT_STATUS_PASSED = 0,    /**< The monitor ran and the condition is absent. */
    DEM_EVENT_STATUS_FAILED = 1,    /**< The monitor ran and the condition is present.*/
    DEM_EVENT_STATUS_PREPASSED = 2, /**< Tentatively absent; debounce moves one step. */
    DEM_EVENT_STATUS_PREFAILED = 3  /**< Tentatively present; debounce moves one step. */
} Dem_EventStatusType;

/*---------------------- UDS status byte bit definitions ---------------------*/

#define DEM_UDS_TEST_FAILED 0x01u                    /**< Currently failing.              */
#define DEM_UDS_TEST_FAILED_THIS_CYCLE 0x02u         /**< Failed since this cycle began.  */
#define DEM_UDS_PENDING_DTC 0x04u                    /**< Failing but not yet confirmed.  */
#define DEM_UDS_CONFIRMED_DTC 0x08u                  /**< Debounce threshold reached.     */
#define DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR 0x10u /**< Not retested since a clear.     */
#define DEM_UDS_TEST_FAILED_SINCE_CLEAR 0x20u        /**< Has failed since the last clear.*/
#define DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE 0x40u  /**< Not retested this cycle.        */
#define DEM_UDS_WARNING_INDICATOR_REQUESTED 0x80u    /**< Driver should be warned.         */

/** Conditions captured when an event confirms, so an intermittent fault is diagnosable. */
typedef struct
{
    uint32 uptimeMs;        /**< Monotonic time at confirmation.                  */
    uint32 unixTime;        /**< Wall-clock time, 0 if the clock was not valid.    */
    uint32 odometerMetres;  /**< Distance at confirmation.                        */
    uint16 speedCmPerSec;   /**< Vehicle speed at confirmation.                   */
    uint16 vbattMilliVolts; /**< Auxiliary supply at confirmation.                */
    uint8 resetReason;      /**< ::Mcu_ResetReasonType of the current start.       */
    uint8 backhaulState;    /**< Which bearer was active.                          */
    uint16 reserved;        /**< Explicit padding, written as zero.                */
} Dem_SnapshotType;

/** Everything Dem holds about one event. */
typedef struct
{
    Dem_DtcType dtc;            /**< The code a tool sees.                         */
    uint8 udsStatus;            /**< ISO 14229 status byte.                        */
    uint8 instanceId;           /**< Which instance reported it most recently.      */
    uint8 debounceCounter;      /**< Progress toward the confirmation threshold.    */
    uint8 healingCounter;       /**< Clean operation cycles since it last failed.   */
    uint16 occurrenceCount;     /**< Times it has been reported failed.             */
    uint32 firstFailedUptimeMs; /**< When it first failed this power cycle.         */
    uint32 lastFailedUptimeMs;  /**< When it most recently failed.                 */
    Dem_SnapshotType snapshot;  /**< Conditions at confirmation.                    */
    boolean snapshotStored;     /**< TRUE once a snapshot has been captured.        */
} Dem_EventRecordType;

/** Aggregate counters, published in the telemetry health record. */
typedef struct
{
    uint16 confirmedCount; /**< Events currently confirmed.                     */
    uint16 pendingCount;   /**< Events failing but not yet confirmed.           */
    uint32 totalReports;   /**< Calls to ::Dem_SetEventStatus.                   */
    uint32 totalConfirmed; /**< Confirmations since the last clear.              */
    uint32 clearCount;     /**< Times the record has been cleared.               */
    boolean warningActive; /**< TRUE if any confirmed event requests a warning.  */
} Dem_StatisticsType;

/**
 * @brief Callback through which Dem obtains snapshot data.
 *
 * Dem does not read the odometer, the ADC or the clock itself. Doing so would make it depend on
 * most of the stack and therefore untestable in isolation, and would invert the layering -- a
 * service calling into an application component. EcuM installs this hook instead.
 */
typedef void (*Dem_SnapshotProviderType)(Dem_SnapshotType *snapshot);

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Initialise Dem and restore persistent event status from NvM.
 *
 * A fault confirmed before the last power cycle comes back confirmed, which is the whole point:
 * a unit returned from the field must still be able to say what went wrong.
 *
 * @return E_OK on success. Failure to read NvM is not fatal -- Dem starts with a clean record and
 *         reports the NvM problem as its own event.
 */
CHECK_RETURN Std_ReturnType Dem_Init(void);

/** Install the snapshot provider. Pass NULL_PTR to capture snapshots with zeroed data. */
void Dem_SetSnapshotProvider(Dem_SnapshotProviderType provider);

/**
 * @brief Report a monitor result for @p eventId.
 *
 * The single entry point every module uses. Applies the event's debounce rule, updates its UDS
 * status, and on a transition to confirmed captures a snapshot and marks the block for NvM.
 *
 * @param eventId    Configured event.
 * @param instanceId Which instance is reporting, or ::INSTANCE_ID_SINGLE.
 * @param status     What the monitor found.
 * @return E_OK if the report was accepted; E_NOT_OK for an unknown event or before Init.
 */
CHECK_RETURN Std_ReturnType Dem_SetEventStatus(Dem_EventIdType eventId, uint8 instanceId,
                                               Dem_EventStatusType status);

/**
 * @brief Read @p eventId's UDS status byte.
 * @param[in]  eventId   Configured event.
 * @param[out] udsStatus Destination.
 * @return E_OK on success; E_NOT_OK for an unknown event or a NULL pointer.
 */
CHECK_RETURN Std_ReturnType Dem_GetEventStatus(Dem_EventIdType eventId, uint8 *udsStatus);

/**
 * @brief Read everything Dem holds about @p eventId.
 * @param[in]  eventId Configured event.
 * @param[out] record  Destination.
 */
CHECK_RETURN Std_ReturnType Dem_GetEventRecord(Dem_EventIdType eventId, Dem_EventRecordType *record);

/** TRUE if @p eventId is currently confirmed. */
boolean Dem_IsEventConfirmed(Dem_EventIdType eventId);

/**
 * @brief The trouble code for @p eventId, with @p instanceId in its low byte.
 * @return The code, or 0 for an unknown event.
 */
Dem_DtcType Dem_GetDtcForEvent(Dem_EventIdType eventId, uint8 instanceId);

/**
 * @brief List the currently confirmed trouble codes, most recent first.
 *
 * Serves the diagnostic channel's ReadDTCInformation service and the telemetry health record.
 *
 * @param[out] buffer   Destination array.
 * @param[in]  maxCount Capacity of @p buffer.
 * @return Number of codes written.
 */
uint16 Dem_GetConfirmedDtcs(Dem_DtcType *buffer, uint16 maxCount);

/**
 * @brief Read the snapshot captured when @p eventId confirmed.
 * @return E_OK if a snapshot exists; E_NOT_FOUND if the event has never confirmed.
 */
CHECK_RETURN Std_ReturnType Dem_GetSnapshot(Dem_EventIdType eventId, Dem_SnapshotType *snapshot);

/**
 * @brief Clear the diagnostic record.
 *
 * Serves ClearDiagnosticInformation. Clears both RAM and the persistent copy, so a technician
 * can confirm a repair produced a genuinely clean run rather than an absence of new faults.
 *
 * @param dtc The code to clear, or 0xFFFFFF for all.
 * @return E_OK on success; E_NOT_FOUND if @p dtc matches no configured event.
 */
CHECK_RETURN Std_ReturnType Dem_ClearDtc(Dem_DtcType dtc);

/**
 * @brief Begin a new operation cycle.
 *
 * Called by EcuM once per start. Clears the per-cycle status bits and advances the healing
 * counter of every event that did not fail in the previous cycle, so a repaired fault
 * self-clears after ::DEM_HEALING_CYCLE_COUNT clean journeys without needing a tool.
 */
void Dem_StartOperationCycle(void);

/**
 * @brief Persist the event status block if it has changed.
 *
 * Driven cyclically by SchM. Deferred rather than written on every confirmation so that a fault
 * reporting repeatedly does not consume flash endurance.
 */
void Dem_MainFunction(void);

/**
 * @brief Read the aggregate counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Dem_GetStatistics(Dem_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Dem_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* DEM_H */

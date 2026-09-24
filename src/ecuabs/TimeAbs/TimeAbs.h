/**
 * @file    TimeAbs.h
 * @brief   Wall-clock time abstraction over the DS3231 RTC and NTP.
 *
 * Supplies calendar time for record timestamps and log file names. Strictly separate from ::Gpt,
 * which supplies monotonic time for durations and timeouts. That separation is the point of this
 * module: v1 used @c time(nullptr) for both, so an NTP correction that stepped the clock backwards
 * turned an elapsed-time comparison into a wait of up to the correction's size.
 *
 * @par Two sources, one answer
 * The DS3231 keeps time across a power cycle on its own coin cell; NTP is authoritative but only
 * reachable when a bearer is up. The policy is:
 *
 *  - At startup, read the RTC. If it is plausible, adopt it.
 *  - When a bearer comes up, fetch NTP. If it differs from the RTC by more than
 *    ::TIMEABS_RTC_DRIFT_TOLERANCE_S, write NTP back to the RTC and record the correction.
 *  - If the RTC is implausible and NTP is unreachable, report time as invalid rather than guessing.
 *
 * @par Invalid is a real state
 * ::TimeAbs_GetUnixTime reports validity separately from the value. A record written with an invalid
 * clock carries a blank timestamp field and its monotonic uptime, so it is still orderable relative
 * to its neighbours and can be corrected later from the uptime once a valid time is established. v1
 * wrote whatever the clock held, so records from before the first NTP sync were timestamped in the
 * year 2000 and silently sorted to the beginning of the dataset.
 *
 * @req SWREQ-SYS-0030 .. SWREQ-SYS-0038
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef TIMEABS_H
#define TIMEABS_H

#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"
#include "ecuabs/TimeAbs/TimeAbs_Cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIMEABS_VENDOR_ID 0xFFFEu
#define TIMEABS_SW_MAJOR_VERSION 2u
#define TIMEABS_SW_MINOR_VERSION 0u
#define TIMEABS_SW_PATCH_VERSION 0u

#define TIMEABS_API_ID_INIT 0x00u
#define TIMEABS_API_ID_GET_UNIX_TIME 0x20u
#define TIMEABS_API_ID_SET_TIME 0x21u
#define TIMEABS_API_ID_FORMAT 0x22u
#define TIMEABS_API_ID_SYNC 0x23u

#define TIMEABS_E_UNINIT E_UNINIT
#define TIMEABS_E_PARAM_POINTER E_PARAM_POINTER
#define TIMEABS_E_RTC_ABSENT 0x20u
#define TIMEABS_E_TIME_IMPLAUSIBLE 0x21u
#define TIMEABS_E_SYNC_FAILED 0x22u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Where the current time came from. */
typedef enum
{
    TIMEABS_SOURCE_NONE = 0, /**< No usable time.                          */
    TIMEABS_SOURCE_RTC = 1,  /**< From the DS3231, not yet NTP-corrected.  */
    TIMEABS_SOURCE_NTP = 2   /**< Synchronised with NTP this power cycle.  */
} TimeAbs_SourceType;

/** Broken-down calendar time, UTC. */
typedef struct
{
    uint16 year;   /**< Full year, e.g. 2026.  */
    uint8 month;   /**< 1 .. 12.               */
    uint8 day;     /**< 1 .. 31.               */
    uint8 hour;    /**< 0 .. 23.               */
    uint8 minute;  /**< 0 .. 59.               */
    uint8 second;  /**< 0 .. 59.               */
    uint8 weekday; /**< 0 = Sunday.            */
} TimeAbs_DateTimeType;

/** Clock status, published in the telemetry health record. */
typedef struct
{
    TimeAbs_SourceType source;  /**< Current authority for the time.              */
    boolean valid;              /**< TRUE if the time may be used in a record.     */
    uint32 lastSyncUptimeMs;    /**< Monotonic time of the last NTP sync.          */
    sint32 lastCorrectionSec;   /**< Size of the last NTP correction, signed.      */
    uint32 syncCount;           /**< Successful NTP syncs since boot.              */
    uint32 syncFailureCount;    /**< NTP attempts that failed.                     */
    boolean rtcPresent;         /**< TRUE if the DS3231 answered at startup.       */
} TimeAbs_StatusType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Read the RTC and adopt its time if plausible.
 * @return E_OK if a usable time was established; E_NOT_OK if the clock is invalid, in which case the
 *         ECU still runs and records carry a blank timestamp.
 */
CHECK_RETURN Std_ReturnType TimeAbs_Init(void);

/**
 * @brief Read the current time.
 * @param[out] unixTime Seconds since the Unix epoch, UTC.
 * @param[out] valid    Whether @p unixTime may be used. May be NULL_PTR if the caller has already
 *                      checked ::TimeAbs_IsValid.
 * @return E_OK if the call was well formed, whatever the validity.
 */
CHECK_RETURN Std_ReturnType TimeAbs_GetUnixTime(uint32 *unixTime, boolean *valid);

/** TRUE if the current time is plausible and may be written into a record. */
boolean TimeAbs_IsValid(void);

/**
 * @brief Set the clock from an authoritative source, and write it through to the RTC.
 *
 * @param unixTime New time, UTC.
 * @return E_OK on success; E_NOT_OK if @p unixTime is outside the plausible window, which is checked
 *         so that a malformed NTP or diagnostic value cannot destroy a good RTC time.
 */
CHECK_RETURN Std_ReturnType TimeAbs_SetUnixTime(uint32 unixTime);

/**
 * @brief Attempt an NTP synchronisation. Requires a bearer to be up.
 * @return E_OK if the clock was synchronised; E_TIMEOUT if the server did not answer.
 */
CHECK_RETURN Std_ReturnType TimeAbs_Synchronise(void);

/**
 * @brief Convert a Unix timestamp to broken-down UTC.
 *
 * Pure function. Implemented locally rather than with @c gmtime, whose result lives in a static
 * buffer that two tasks calling it concurrently would share.
 */
CHECK_RETURN Std_ReturnType TimeAbs_ToDateTime(uint32 unixTime, TimeAbs_DateTimeType *dateTime);

/** Convert broken-down UTC to a Unix timestamp. Pure function; the inverse of ::TimeAbs_ToDateTime. */
CHECK_RETURN Std_ReturnType TimeAbs_FromDateTime(const TimeAbs_DateTimeType *dateTime,
                                                 uint32 *unixTime);

/**
 * @brief Format the current date as "YYYYMMDD", for a log file name.
 *
 * @param[out] buffer Destination, at least 9 bytes.
 * @param[in]  size   Capacity of @p buffer.
 * @return E_OK on success; E_NOT_OK if the clock is invalid or the buffer is too small. A file name is
 *         never invented from an invalid clock -- doing so would scatter records across a directory
 *         named after a date that never happened.
 */
CHECK_RETURN Std_ReturnType TimeAbs_FormatDateStamp(char *buffer, uint16 size);

/**
 * @brief Whether a timestamp falls inside the window this firmware considers possible.
 *
 * Used to reject an RTC that has lost its cell (reporting 2000-01-01) and an NTP response that is
 * obviously wrong. Pure function.
 */
boolean TimeAbs_IsPlausible(uint32 unixTime);

/**
 * @brief Read the clock status.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType TimeAbs_GetStatus(TimeAbs_StatusType *status);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void TimeAbs_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* TIMEABS_H */

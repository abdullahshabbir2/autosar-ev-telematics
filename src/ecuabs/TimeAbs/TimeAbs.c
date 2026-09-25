/**
 * @file    TimeAbs.c
 * @brief   Platform-independent part of the wall-clock abstraction.
 *
 * The calendar conversions and the plausibility rule live here and are unit tested. The platform leaf
 * -- reading and writing the DS3231 over I2C, and fetching NTP -- is in TimeAbs_Esp32.cpp for the
 * target and in test/support/Stub_Platform.c for the host.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "ecuabs/TimeAbs/TimeAbs.h"

#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "ecuabs/TimeAbs/TimeAbs_Platform.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean TimeAbs_Initialised = FALSE;
STATIC TimeAbs_StatusType TimeAbs_Status;

/**
 * @brief Offset from monotonic time to wall-clock time, in seconds.
 *
 * Wall-clock time is derived as (monotonic seconds + offset) rather than read from the RTC on every
 * call. Two reasons: an I2C transaction per record is unnecessary traffic on a bus shared with nothing
 * else that needs it, and deriving from the monotonic base means the wall clock advances smoothly
 * between syncs instead of stepping by whatever the RTC's own quantisation happens to be.
 */
STATIC uint32 TimeAbs_EpochOffsetSec;

/*==================================================================================================
 *  Calendar arithmetic
 *
 *  Implemented locally rather than with gmtime()/mktime(). gmtime returns a pointer into a static
 *  buffer, so two tasks calling it concurrently share one result; mktime interprets its input in the
 *  local timezone, which on an embedded target is whatever the C library defaulted to.
 *================================================================================================*/

/** Days in each month of a non-leap year. */
STATIC const uint8 TimeAbs_MonthDays[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};

STATIC boolean TimeAbs_IsLeapYear(uint16 year)
{
    return (((year % 4u) == 0u) && (((year % 100u) != 0u) || ((year % 400u) == 0u))) ? TRUE : FALSE;
}

STATIC uint8 TimeAbs_DaysInMonth(uint16 year, uint8 month)
{
    if ((month < 1u) || (month > 12u))
    {
        return 0u;
    }
    if ((month == 2u) && (TimeAbs_IsLeapYear(year) != FALSE))
    {
        return 29u;
    }
    return TimeAbs_MonthDays[month - 1u];
}

boolean TimeAbs_IsPlausible(uint32 unixTime)
{
    return ((unixTime >= (uint32)TIMEABS_MIN_PLAUSIBLE_UNIX)
            && (unixTime <= (uint32)TIMEABS_MAX_PLAUSIBLE_UNIX))
               ? TRUE
               : FALSE;
}

Std_ReturnType TimeAbs_ToDateTime(uint32 unixTime, TimeAbs_DateTimeType *dateTime)
{
    uint32 days;
    uint32 secondsOfDay;
    uint16 year = 1970u;
    uint8 month = 1u;

    DET_CHECK_RETURN(dateTime != NULL_PTR, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_FORMAT,
                     TIMEABS_E_PARAM_POINTER, E_NOT_OK);

    days = unixTime / 86400uL;
    secondsOfDay = unixTime % 86400uL;

    /* 1 January 1970 was a Thursday, which is weekday 4 counting Sunday as 0. */
    dateTime->weekday = (uint8)((days + 4uL) % 7uL);

    for (;;)
    {
        const uint32 yearDays = (TimeAbs_IsLeapYear(year) != FALSE) ? 366uL : 365uL;

        if (days < yearDays)
        {
            break;
        }
        days -= yearDays;
        year++;
    }

    while (month <= 12u)
    {
        const uint32 monthDays = (uint32)TimeAbs_DaysInMonth(year, month);

        if (days < monthDays)
        {
            break;
        }
        days -= monthDays;
        month++;
    }

    dateTime->year = year;
    dateTime->month = month;
    dateTime->day = (uint8)(days + 1uL);
    dateTime->hour = (uint8)(secondsOfDay / 3600uL);
    dateTime->minute = (uint8)((secondsOfDay % 3600uL) / 60uL);
    dateTime->second = (uint8)(secondsOfDay % 60uL);

    return E_OK;
}

Std_ReturnType TimeAbs_FromDateTime(const TimeAbs_DateTimeType *dateTime, uint32 *unixTime)
{
    uint32 days = 0u;
    uint16 y;
    uint8 m;

    DET_CHECK_RETURN((dateTime != NULL_PTR) && (unixTime != NULL_PTR), MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE,
                     TIMEABS_API_ID_FORMAT, TIMEABS_E_PARAM_POINTER, E_NOT_OK);

    if ((dateTime->year < 1970u) || (dateTime->month < 1u) || (dateTime->month > 12u) || (dateTime->day < 1u)
        || (dateTime->day > TimeAbs_DaysInMonth(dateTime->year, dateTime->month)) || (dateTime->hour > 23u)
        || (dateTime->minute > 59u) || (dateTime->second > 59u))
    {
        return E_NOT_OK;
    }

    for (y = 1970u; y < dateTime->year; y++)
    {
        days += (TimeAbs_IsLeapYear(y) != FALSE) ? 366u : 365u;
    }
    for (m = 1u; m < dateTime->month; m++)
    {
        days += (uint32)TimeAbs_DaysInMonth(dateTime->year, m);
    }
    days += (uint32)(dateTime->day - 1u);

    *unixTime = (days * 86400u) + ((uint32)dateTime->hour * 3600u) + ((uint32)dateTime->minute * 60u)
                + (uint32)dateTime->second;

    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType TimeAbs_Init(void)
{
    uint32 rtcTime = 0u;

    TimeAbs_Status.source = TIMEABS_SOURCE_NONE;
    TimeAbs_Status.valid = FALSE;
    TimeAbs_Status.lastSyncUptimeMs = 0u;
    TimeAbs_Status.lastCorrectionSec = 0;
    TimeAbs_Status.syncCount = 0u;
    TimeAbs_Status.syncFailureCount = 0u;
    TimeAbs_Status.rtcPresent = FALSE;
    TimeAbs_EpochOffsetSec = 0u;
    TimeAbs_Initialised = TRUE;

    if (TimeAbs_PlatformRtcInit() != E_OK)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_RTC_INVALID, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
        return E_NOT_OK;
    }
    TimeAbs_Status.rtcPresent = TRUE;

    if (TimeAbs_PlatformRtcRead(&rtcTime) != E_OK)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_RTC_INVALID, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
        return E_NOT_OK;
    }

    if (TimeAbs_IsPlausible(rtcTime) == FALSE)
    {
        /* A DS3231 whose backup cell has died reports 2000-01-01. Reporting the clock as invalid is
         * the useful answer: records then carry a blank timestamp and their monotonic uptime, and can
         * be corrected retrospectively once a real time is established. v1 wrote the year-2000 value
         * into records, which then sorted to the beginning of the dataset. */
        (void)Det_ReportRuntimeError(MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_INIT,
                                     TIMEABS_E_TIME_IMPLAUSIBLE);
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_RTC_INVALID, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
        return E_NOT_OK;
    }

    TimeAbs_EpochOffsetSec = rtcTime - (Gpt_GetMonotonicMs() / 1000u);
    TimeAbs_Status.source = TIMEABS_SOURCE_RTC;
    TimeAbs_Status.valid = TRUE;
    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_RTC_INVALID, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_PASSED));

    return E_OK;
}

Std_ReturnType TimeAbs_GetUnixTime(uint32 *unixTime, boolean *valid)
{
    DET_CHECK_RETURN(unixTime != NULL_PTR, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE,
                     TIMEABS_API_ID_GET_UNIX_TIME, TIMEABS_E_PARAM_POINTER, E_NOT_OK);

    if (TimeAbs_Status.valid != FALSE)
    {
        *unixTime = TimeAbs_EpochOffsetSec + (Gpt_GetMonotonicMs() / 1000u);
    }
    else
    {
        /* Zero rather than a guess. Com emits a zero timestamp as a blank field, so an invalid clock
         * produces a record that is honest about not knowing the time. */
        *unixTime = 0u;
    }

    if (valid != NULL_PTR)
    {
        *valid = TimeAbs_Status.valid;
    }

    return E_OK;
}

boolean TimeAbs_IsValid(void)
{
    return TimeAbs_Status.valid;
}

Std_ReturnType TimeAbs_SetUnixTime(uint32 unixTime)
{
    uint32 previous = 0u;

    DET_CHECK_RETURN(TimeAbs_Initialised != FALSE, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE,
                     TIMEABS_API_ID_SET_TIME, TIMEABS_E_UNINIT, E_NOT_OK);

    /* Checked before anything is changed, so a malformed NTP response or a mistaken diagnostic write
     * cannot destroy a good RTC time. */
    DET_CHECK_RETURN(TimeAbs_IsPlausible(unixTime) != FALSE, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE,
                     TIMEABS_API_ID_SET_TIME, TIMEABS_E_TIME_IMPLAUSIBLE, E_NOT_OK);

    if (TimeAbs_Status.valid != FALSE)
    {
        previous = TimeAbs_EpochOffsetSec + (Gpt_GetMonotonicMs() / 1000u);
        TimeAbs_Status.lastCorrectionSec = (sint32)((sint64)unixTime - (sint64)previous);
    }

    TimeAbs_EpochOffsetSec = unixTime - (Gpt_GetMonotonicMs() / 1000u);
    TimeAbs_Status.valid = TRUE;
    TimeAbs_Status.source = TIMEABS_SOURCE_NTP;
    TimeAbs_Status.lastSyncUptimeMs = Gpt_GetMonotonicMs();
    TimeAbs_Status.syncCount++;

    /* Only written back to the RTC when the disagreement is worth it. A DS3231 drifts under a minute a
     * year, so writing on every small difference would wear its registers for nothing. */
    if ((TimeAbs_Status.rtcPresent != FALSE)
        && ((TimeAbs_Status.lastCorrectionSec > (sint32)TIMEABS_RTC_DRIFT_TOLERANCE_S)
            || (TimeAbs_Status.lastCorrectionSec < -(sint32)TIMEABS_RTC_DRIFT_TOLERANCE_S)
            || (previous == 0u)))
    {
        if (TimeAbs_PlatformRtcWrite(unixTime) != E_OK)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_SET_TIME,
                                         TIMEABS_E_RTC_ABSENT);
        }
    }

    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_RTC_INVALID, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_PASSED));
    return E_OK;
}

Std_ReturnType TimeAbs_Synchronise(void)
{
    uint32 ntpTime = 0u;

    DET_CHECK_RETURN(TimeAbs_Initialised != FALSE, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_SYNC,
                     TIMEABS_E_UNINIT, E_NOT_OK);

    if (TimeAbs_PlatformNtpFetch(&ntpTime, TIMEABS_SYNC_TIMEOUT_MS) != E_OK)
    {
        TimeAbs_Status.syncFailureCount++;
        return E_TIMEOUT;
    }

    return TimeAbs_SetUnixTime(ntpTime);
}

Std_ReturnType TimeAbs_FormatDateStamp(char *buffer, uint16 size)
{
    TimeAbs_DateTimeType dateTime;
    uint32 now = 0u;
    boolean valid = FALSE;

    DET_CHECK_RETURN(buffer != NULL_PTR, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_FORMAT,
                     TIMEABS_E_PARAM_POINTER, E_NOT_OK);

    if (size < 9u)
    {
        return E_NOT_OK;
    }

    if (TimeAbs_GetUnixTime(&now, &valid) != E_OK)
    {
        return E_NOT_OK;
    }
    if (valid == FALSE)
    {
        /* A file name is never invented from an invalid clock. Doing so would scatter records into a
         * file named after a date that never happened, and they would be almost impossible to
         * reconcile afterwards. */
        return E_NOT_OK;
    }

    if (TimeAbs_ToDateTime(now, &dateTime) != E_OK)
    {
        return E_NOT_OK;
    }

    buffer[0] = (char)('0' + (char)((dateTime.year / 1000u) % 10u));
    buffer[1] = (char)('0' + (char)((dateTime.year / 100u) % 10u));
    buffer[2] = (char)('0' + (char)((dateTime.year / 10u) % 10u));
    buffer[3] = (char)('0' + (char)(dateTime.year % 10u));
    buffer[4] = (char)('0' + (char)(dateTime.month / 10u));
    buffer[5] = (char)('0' + (char)(dateTime.month % 10u));
    buffer[6] = (char)('0' + (char)(dateTime.day / 10u));
    buffer[7] = (char)('0' + (char)(dateTime.day % 10u));
    buffer[8] = '\0';

    return E_OK;
}

Std_ReturnType TimeAbs_GetStatus(TimeAbs_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_TIMEABS, INSTANCE_ID_SINGLE, TIMEABS_API_ID_GET_UNIX_TIME,
                     TIMEABS_E_PARAM_POINTER, E_NOT_OK);

    *status = TimeAbs_Status;
    return E_OK;
}

void TimeAbs_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = TIMEABS_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_TIMEABS;
        versioninfo->sw_major_version = TIMEABS_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = TIMEABS_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = TIMEABS_SW_PATCH_VERSION;
    }
}

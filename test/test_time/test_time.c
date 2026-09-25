/**
 * @file    test_time.c
 * @brief   Unit tests for the wall-clock abstraction: calendar arithmetic, plausibility, sync policy.
 *
 * @par Why the calendar arithmetic is worth testing at all
 * It is implemented rather than taken from the C library, because `gmtime` returns a pointer into a
 * static buffer that two tasks calling it would share. Implementing it makes it host-testable -- and
 * the leap-year rule is the classic place to get this wrong: 2024 is a leap year, 2100 is not, and a
 * naive every-four-years rule agrees with the truth for the next seventy-four years. A test against
 * dates the implementation can reach is therefore not enough; it has to be tested against 2100.
 *
 * Every expected value here comes from the date, not from the implementation. The epoch constants were
 * computed independently; each is written out with its derivation so it can be checked rather than
 * trusted.
 *
 * @req SWREQ-SYS-0030 .. SWREQ-SYS-0038
 * @verifies TS-TIME-001 .. TS-TIME-022
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "ecuabs/TimeAbs/TimeAbs.h"
#include "ecuabs/TimeAbs/TimeAbs_Cfg.h"
#include "services/Det/Det.h"
#include "Stub_Mcal.h"
#include "Stub_Platform.h"
#include "unity.h"

/*==================================================================================================
 *  Independently computed reference instants
 *
 *  Each is derived below rather than quoted, because a wrong epoch constant makes every case using it
 *  agree with the implementation for the wrong reason.
 *================================================================================================*/

/**
 * @brief 2024-01-01 00:00:00 UTC.
 *
 * Days from 1970-01-01: 54 years, of which the leap years are 1972, 1976, ... 2020 -- that is
 * (2020 - 1972) / 4 + 1 = 13 leap days. So 54 * 365 + 13 = 19 723 days.
 * 19 723 * 86 400 = 1 704 067 200.
 */
#define TT_2024_01_01 1704067200uL

/**
 * @brief 2024-02-29 00:00:00 UTC -- the leap day itself.
 *
 * 31 days of January plus 28 of February from the start of 2024: 59 days.
 * 1 704 067 200 + 59 * 86 400 = 1 709 164 800.
 */
#define TT_2024_02_29 1709164800uL

/**
 * @brief 2024-03-01 00:00:00 UTC, the day after the leap day.
 *
 * 1 709 164 800 + 86 400 = 1 709 251 200. An implementation that skipped 29 February would land here
 * when asked for the leap day, which is why both are tested.
 */
#define TT_2024_03_01 1709251200uL

/**
 * @brief 2025-03-01 00:00:00 UTC -- the same date in a non-leap year.
 *
 * 2025-01-01 is 1 704 067 200 + 366 * 86 400 = 1 735 689 600 (2024 had 366 days).
 * Plus 31 + 28 = 59 days: 1 735 689 600 + 5 097 600 = 1 740 787 200.
 */
#define TT_2025_03_01 1740787200uL

/**
 * @brief 2100-03-01 00:00:00 UTC.
 *
 * 2100 is NOT a leap year: divisible by 100 and not by 400. So 2100-03-01 is 59 days after
 * 2100-01-01, not 60.
 *
 * Days from 1970 to 2100: 130 years. Leap years in 1972..2096 inclusive, step 4, is
 * (2096 - 1972) / 4 + 1 = 32. None are century exceptions (2000 is divisible by 400, so it IS a leap
 * year and stays counted). 130 * 365 + 32 = 47 482 days = 4 102 444 800.
 * Plus 59 days: 4 102 444 800 + 5 097 600 = 4 107 542 400.
 */
#define TT_2100_03_01 4107542400uL

/**
 * @brief 2026-06-15 12:34:56 UTC, the general-purpose instant most cases below use.
 *
 * Derived in TS-TIME-004, which computes it independently and asserts both conversion directions
 * against it. Worth noting that the first draft of this constant was 1 781 613 296 -- exactly one day
 * later -- and TS-TIME-018 caught it by asserting the formatted date stamp. The lesson is the one
 * CS-TEST-01 states: a constant written from memory rather than derived is a constant that is wrong,
 * and it is only caught if something asserts against the value it claims to represent.
 */
#define TT_2026_06_15_123456 1781526896uL

/*==================================================================================================
 *  Fixture
 *================================================================================================*/

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Stub_Platform_ResetAll();
    Det_Init();
}

void tearDown(void)
{
}

/** Bring TimeAbs up with a present, plausible RTC. */
static void TtBringUp(uint32 rtcTime)
{
    Stub_Time_SetRtcPresent(TRUE);
    Stub_Time_SetRtcTime(rtcTime);
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_Init());
}

/** Assert a broken-down time field by field, so a failure names which field is wrong. */
static void TtAssertDateTime(const TimeAbs_DateTimeType *dt, uint16 year, uint8 month, uint8 day, uint8 hour,
                             uint8 minute, uint8 second)
{
    TEST_ASSERT_EQUAL_UINT16(year, dt->year);
    TEST_ASSERT_EQUAL_UINT8(month, dt->month);
    TEST_ASSERT_EQUAL_UINT8(day, dt->day);
    TEST_ASSERT_EQUAL_UINT8(hour, dt->hour);
    TEST_ASSERT_EQUAL_UINT8(minute, dt->minute);
    TEST_ASSERT_EQUAL_UINT8(second, dt->second);
}

/*==================================================================================================
 *  TS-TIME-001 .. 008  Calendar conversion
 *================================================================================================*/

/** TS-TIME-001: the epoch itself converts to 1970-01-01 00:00:00. */
static void test_Time_EpochConvertsExactly(void)
{
    TimeAbs_DateTimeType dt;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(0uL, &dt));
    TtAssertDateTime(&dt, 1970u, 1u, 1u, 0u, 0u, 0u);
}

/** TS-TIME-002: a known instant converts in both directions and round-trips. */
static void test_Time_KnownInstantRoundTrips(void)
{
    TimeAbs_DateTimeType dt;
    uint32 back = 0uL;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2024_01_01, &dt));
    TtAssertDateTime(&dt, 2024u, 1u, 1u, 0u, 0u, 0u);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_FromDateTime(&dt, &back));
    TEST_ASSERT_EQUAL_UINT32(TT_2024_01_01, back);
}

/**
 * TS-TIME-003: the leap day exists and is distinct from the day after.
 *
 * Both are asserted because an implementation that skipped 29 February would report 1 March when
 * asked for the leap day -- a plausible answer that a test of only one of the two would accept.
 */
static void test_Time_LeapDayIsHandled(void)
{
    TimeAbs_DateTimeType dt;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2024_02_29, &dt));
    TtAssertDateTime(&dt, 2024u, 2u, 29u, 0u, 0u, 0u);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2024_03_01, &dt));
    TtAssertDateTime(&dt, 2024u, 3u, 1u, 0u, 0u, 0u);
}

/**
 * TS-TIME-004: hours, minutes and seconds within a day.
 *
 * 2026-06-15 12:34:56 UTC. 2026-01-01 is 1 704 067 200 + (366 + 365) * 86 400 = 1 767 225 600.
 * To 15 June: 31 + 28 + 31 + 30 + 31 + 14 = 165 days = 14 256 000. So midnight is 1 781 481 600.
 * Plus 12 * 3600 + 34 * 60 + 56 = 45 296. Total 1 781 526 896.
 */
static void test_Time_TimeOfDayConverts(void)
{
    TimeAbs_DateTimeType dt;
    const uint32 instant = TT_2026_06_15_123456;
    uint32 back = 0uL;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(instant, &dt));
    TtAssertDateTime(&dt, 2026u, 6u, 15u, 12u, 34u, 56u);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_FromDateTime(&dt, &back));
    TEST_ASSERT_EQUAL_UINT32(instant, back);
}

/**
 * TS-TIME-005: the same calendar date differs by a day between a leap and a non-leap year.
 *
 * 1 March is the 61st day of a leap year and the 60th otherwise. This is the case a
 * days-since-year-start table gets wrong when it is not indexed by leap-ness.
 */
static void test_Time_MarchFirstDiffersAcrossLeapYears(void)
{
    TimeAbs_DateTimeType dt;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2024_03_01, &dt));
    TtAssertDateTime(&dt, 2024u, 3u, 1u, 0u, 0u, 0u);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2025_03_01, &dt));
    TtAssertDateTime(&dt, 2025u, 3u, 1u, 0u, 0u, 0u);
}

/**
 * TS-TIME-006: 2100 is not a leap year.
 *
 * The case a naive every-four-years rule gets wrong, and the reason it matters that this is tested:
 * such a rule agrees with the truth for every date this firmware will plausibly see, so no amount of
 * testing against realistic dates would find it. Divisible by 100 and not by 400 means no leap day.
 */
static void test_Time_Year2100IsNotLeap(void)
{
    TimeAbs_DateTimeType dt;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2100_03_01, &dt));
    TtAssertDateTime(&dt, 2100u, 3u, 1u, 0u, 0u, 0u);

    /* And 2100-02-29 does not exist, so the day before 1 March is 28 February. */
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2100_03_01 - 86400uL, &dt));
    TtAssertDateTime(&dt, 2100u, 2u, 28u, 0u, 0u, 0u);
}

/** TS-TIME-007: 2000 IS a leap year -- divisible by 400, so the century rule does not apply. */
static void test_Time_Year2000IsLeap(void)
{
    TimeAbs_DateTimeType dt;
    /* 2000-02-29 00:00:00 UTC. 1970 to 2000 is 30 years with leap days in 1972..1996 step 4 =
     * 7 days. 30 * 365 + 7 = 10 957 days = 946 684 800 (2000-01-01). Plus 59 days = 951 782 400. */
    const uint32 instant = 951782400uL;

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(instant, &dt));
    TtAssertDateTime(&dt, 2000u, 2u, 29u, 0u, 0u, 0u);
}

/**
 * TS-TIME-008: every month boundary round-trips through both conversions.
 *
 * Walks a whole leap year and a whole non-leap year day by day. A per-month length table with one
 * wrong entry shifts every later date in that year, which a handful of spot checks can easily miss.
 */
static void test_Time_EveryDayOfTwoYearsRoundTrips(void)
{
    uint32 instant;
    const uint32 start = TT_2024_01_01;
    const uint32 end = TT_2024_01_01 + ((366uL + 365uL) * 86400uL);

    for (instant = start; instant < end; instant += 86400uL)
    {
        TimeAbs_DateTimeType dt;
        uint32 back = 0uL;

        TEST_ASSERT_EQUAL(E_OK, TimeAbs_ToDateTime(instant, &dt));

        /* Every field must be in range, whatever the date. An out-of-range month or day would make
         * the round trip below succeed against an equally wrong inverse. */
        TEST_ASSERT_TRUE((dt.month >= 1u) && (dt.month <= 12u));
        TEST_ASSERT_TRUE((dt.day >= 1u) && (dt.day <= 31u));
        TEST_ASSERT_TRUE(dt.hour <= 23u);
        TEST_ASSERT_TRUE(dt.minute <= 59u);
        TEST_ASSERT_TRUE(dt.second <= 59u);

        TEST_ASSERT_EQUAL(E_OK, TimeAbs_FromDateTime(&dt, &back));
        TEST_ASSERT_EQUAL_UINT32(instant, back);
    }
}

/*==================================================================================================
 *  TS-TIME-009 .. 012  Plausibility
 *================================================================================================*/

/**
 * TS-TIME-009: SWREQ-SYS-0032 -- the window's boundaries are inclusive.
 *
 * Both sides, because an exclusive bound at the lower edge would reject the first instant of 2024 --
 * a real time, and one a unit commissioned then would report.
 */
static void test_Time_PlausibilityBoundariesAreInclusive(void)
{
    TEST_ASSERT_TRUE(TimeAbs_IsPlausible(TIMEABS_MIN_PLAUSIBLE_UNIX));
    TEST_ASSERT_TRUE(TimeAbs_IsPlausible(TIMEABS_MAX_PLAUSIBLE_UNIX));

    TEST_ASSERT_FALSE(TimeAbs_IsPlausible(TIMEABS_MIN_PLAUSIBLE_UNIX - 1uL));
    TEST_ASSERT_FALSE(TimeAbs_IsPlausible(TIMEABS_MAX_PLAUSIBLE_UNIX + 1uL));
}

/**
 * TS-TIME-010: SWREQ-SYS-0031 -- a reset RTC reading 2000-01-01 is rejected.
 *
 * The specific value v1 recorded as fact. RTClib reports a missing or unpowered device by returning a
 * default-constructed DateTime, so an unplugged RTC read as 1 January 2000 with no error, and that
 * value went into log file names.
 */
static void test_Time_ResetRtcValueIsRejected(void)
{
    /* 2000-01-01 00:00:00 UTC, derived in TS-TIME-007: 946 684 800. */
    TEST_ASSERT_FALSE(TimeAbs_IsPlausible(946684800uL));
}

/** TS-TIME-011: zero and the all-ones value are both rejected. */
static void test_Time_DegenerateValuesAreRejected(void)
{
    TEST_ASSERT_FALSE(TimeAbs_IsPlausible(0uL));
    TEST_ASSERT_FALSE(TimeAbs_IsPlausible(0xFFFFFFFFuL));
}

/** TS-TIME-012: an implausible RTC at startup leaves the clock invalid rather than trusted. */
static void test_Time_ImplausibleRtcLeavesClockInvalid(void)
{
    Stub_Time_SetRtcPresent(TRUE);
    Stub_Time_SetRtcTime(946684800uL); /* the 2000 reset value */
    Stub_Time_SetNtpResponse(0uL, FALSE);

    STD_DISCARD(TimeAbs_Init());

    TEST_ASSERT_FALSE(TimeAbs_IsValid());
}

/*==================================================================================================
 *  TS-TIME-013 .. 017  Sources and synchronisation
 *================================================================================================*/

/** TS-TIME-013: a present, plausible RTC makes the clock valid at startup. */
static void test_Time_PlausibleRtcMakesClockValid(void)
{
    uint32 now = 0uL;
    boolean valid = FALSE;

    TtBringUp(TT_2026_06_15_123456);

    TEST_ASSERT_TRUE(TimeAbs_IsValid());
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_GetUnixTime(&now, &valid));
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_UINT32(TT_2026_06_15_123456, now);
}

/** TS-TIME-014: an absent RTC is reported as absent, and does not make the clock valid. */
static void test_Time_AbsentRtcIsReported(void)
{
    TimeAbs_StatusType status;

    Stub_Time_SetRtcPresent(FALSE);
    Stub_Time_SetNtpResponse(0uL, FALSE);

    STD_DISCARD(TimeAbs_Init());

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_GetStatus(&status));
    TEST_ASSERT_FALSE(status.rtcPresent);
    TEST_ASSERT_FALSE(TimeAbs_IsValid());
}

/** TS-TIME-015: a successful NTP sync validates the clock even with no RTC. */
static void test_Time_NtpValidatesClockWithoutRtc(void)
{
    TimeAbs_StatusType status;

    Stub_Time_SetRtcPresent(FALSE);
    Stub_Time_SetNtpResponse(TT_2026_06_15_123456, TRUE);

    STD_DISCARD(TimeAbs_Init());
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_Synchronise());

    TEST_ASSERT_TRUE(TimeAbs_IsValid());
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.syncCount);
}

/**
 * TS-TIME-016: SWREQ-SYS-0030 -- a successful sync writes the corrected time back to the RTC.
 *
 * Which is what makes the RTC useful on the next boot: without the write-back it would drift
 * indefinitely and the unit would depend on having coverage at startup.
 */
static void test_Time_SyncWritesBackToRtc(void)
{
    const uint32 rtcStart = TT_2026_06_15_123456;
    const uint32 ntpTime = rtcStart + 600uL; /* the RTC is ten minutes slow */

    TtBringUp(rtcStart);
    Stub_Time_SetNtpResponse(ntpTime, TRUE);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_Synchronise());

    TEST_ASSERT_TRUE(Stub_Time_GetRtcWriteCount() > 0u);
    TEST_ASSERT_EQUAL_UINT32(ntpTime, Stub_Time_GetLastRtcWrite());
}

/**
 * TS-TIME-017: an NTP failure is counted and does not invalidate an already-valid clock.
 *
 * A vehicle out of coverage must keep timestamping records. Invalidating the clock because a sync
 * failed would stop it recording absolute time for the whole time it was parked.
 */
static void test_Time_SyncFailureDoesNotInvalidate(void)
{
    TimeAbs_StatusType status;

    TtBringUp(TT_2026_06_15_123456);
    TEST_ASSERT_TRUE(TimeAbs_IsValid());

    Stub_Time_SetNtpResponse(0uL, FALSE);
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_Synchronise());

    TEST_ASSERT_TRUE(TimeAbs_IsValid());
    TEST_ASSERT_EQUAL(E_OK, TimeAbs_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.syncFailureCount);
}

/*==================================================================================================
 *  TS-TIME-018 .. 020  Date stamp for log file names
 *================================================================================================*/

/**
 * TS-TIME-018: the stamp is "YYYYMMDD", zero padded.
 *
 * The padding is what makes lexicographic order chronological, which is the entire reason FsAbs can
 * answer "which log is oldest" by string comparison with no timestamps read.
 */
static void test_Time_DateStampFormat(void)
{
    char buffer[16];

    TtBringUp(TT_2026_06_15_123456);

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_FormatDateStamp(buffer, (uint16)sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING("20260615", buffer);
}

/** TS-TIME-019: a single-digit month and day are zero padded, not left bare. */
static void test_Time_DateStampPadsSingleDigits(void)
{
    char buffer[16];

    /* 2026-01-05 00:00:00 UTC. 2026-01-01 is 1 767 225 600 (TS-TIME-004); plus 4 days. */
    TtBringUp(1767225600uL + (4uL * 86400uL));

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_FormatDateStamp(buffer, (uint16)sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING("20260105", buffer);
}

/**
 * TS-TIME-020: no stamp is produced from an invalid clock, and none from an undersized buffer.
 *
 * Inventing a file name from an invalid clock would scatter a day's records across a directory named
 * after a date that never happened -- and because the name is how the oldest log is identified, it
 * would also make housekeeping delete the wrong file.
 */
static void test_Time_DateStampRefusesInvalidClockAndSmallBuffer(void)
{
    char buffer[16];
    char tiny[4];

    Stub_Time_SetRtcPresent(FALSE);
    Stub_Time_SetNtpResponse(0uL, FALSE);
    STD_DISCARD(TimeAbs_Init());

    TEST_ASSERT_FALSE(TimeAbs_IsValid());
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_FormatDateStamp(buffer, (uint16)sizeof(buffer)));

    /* Now with a valid clock but nowhere to put the answer. */
    TtBringUp(TT_2026_06_15_123456);
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_FormatDateStamp(tiny, (uint16)sizeof(tiny)));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_FormatDateStamp(NULL_PTR, (uint16)sizeof(buffer)));
}

/*==================================================================================================
 *  TS-TIME-021 .. 022  Contract
 *================================================================================================*/

/** TS-TIME-021: setting an implausible time is refused; a plausible one is accepted. */
static void test_Time_SetRejectsImplausible(void)
{
    TtBringUp(TT_2026_06_15_123456);

    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_SetUnixTime(946684800uL));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_SetUnixTime(0uL));

    TEST_ASSERT_EQUAL(E_OK, TimeAbs_SetUnixTime(TT_2024_01_01));
}

/** TS-TIME-022: NULL destinations are rejected by every accessor. */
static void test_Time_RejectsNullPointers(void)
{
    TimeAbs_DateTimeType dt;
    uint32 value = 0uL;
    boolean valid = FALSE;

    TtBringUp(TT_2026_06_15_123456);

    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_ToDateTime(TT_2024_01_01, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_FromDateTime(NULL_PTR, &value));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_FromDateTime(&dt, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_GetUnixTime(NULL_PTR, &valid));
    TEST_ASSERT_NOT_EQUAL(E_OK, TimeAbs_GetStatus(NULL_PTR));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Time_EpochConvertsExactly);
    RUN_TEST(test_Time_KnownInstantRoundTrips);
    RUN_TEST(test_Time_LeapDayIsHandled);
    RUN_TEST(test_Time_TimeOfDayConverts);
    RUN_TEST(test_Time_MarchFirstDiffersAcrossLeapYears);
    RUN_TEST(test_Time_Year2100IsNotLeap);
    RUN_TEST(test_Time_Year2000IsLeap);
    RUN_TEST(test_Time_EveryDayOfTwoYearsRoundTrips);

    RUN_TEST(test_Time_PlausibilityBoundariesAreInclusive);
    RUN_TEST(test_Time_ResetRtcValueIsRejected);
    RUN_TEST(test_Time_DegenerateValuesAreRejected);
    RUN_TEST(test_Time_ImplausibleRtcLeavesClockInvalid);

    RUN_TEST(test_Time_PlausibleRtcMakesClockValid);
    RUN_TEST(test_Time_AbsentRtcIsReported);
    RUN_TEST(test_Time_NtpValidatesClockWithoutRtc);
    RUN_TEST(test_Time_SyncWritesBackToRtc);
    RUN_TEST(test_Time_SyncFailureDoesNotInvalidate);

    RUN_TEST(test_Time_DateStampFormat);
    RUN_TEST(test_Time_DateStampPadsSingleDigits);
    RUN_TEST(test_Time_DateStampRefusesInvalidClockAndSmallBuffer);

    RUN_TEST(test_Time_SetRejectsImplausible);
    RUN_TEST(test_Time_RejectsNullPointers);

    return UNITY_END();
}

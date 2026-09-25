/**
 * @file    test_com.c
 * @brief   Unit tests for the telemetry serialisation and transfer-framing service.
 *
 * Three cases here pin v1 defects that were reachable from the network or from an empty file, which
 * makes them the ones most worth having: the unsigned underflow in the chunk arithmetic, the
 * unbounded heap queue fed from an MQTT payload, and the loss of the distinction between "not
 * measured" and "measured zero".
 *
 * @req SWREQ-TEL-0001 .. SWREQ-TEL-0020
 * @verifies TS-COM-001 .. TS-COM-016
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "services/Com/Com.h"
#include "services/Det/Det.h"
#include "Stub_Mcal.h"
#include "unity.h"

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Com_Init());
}

void tearDown(void)
{
}

/** Count the separators in a NUL-terminated string. */
static uint16 countFields(const char *text)
{
    uint16 count = 1u;
    uint16 i;

    for (i = 0u; text[i] != '\0'; i++)
    {
        if (text[i] == ',')
        {
            count++;
        }
    }
    return count;
}

/** Return the @p index-th comma-separated field of @p text, into @p out. */
static void extractField(const char *text, uint16 index, char *out, uint16 outSize)
{
    uint16 field = 0u;
    uint16 start = 0u;
    uint16 i;

    for (i = 0u;; i++)
    {
        if ((text[i] == ',') || (text[i] == '\0'))
        {
            if (field == index)
            {
                const uint16 len = (uint16)(i - start);
                const uint16 copy = (len < (outSize - 1u)) ? len : (uint16)(outSize - 1u);
                (void)memcpy(out, &text[start], copy);
                out[copy] = '\0';
                return;
            }
            if (text[i] == '\0')
            {
                out[0] = '\0';
                return;
            }
            field++;
            start = (uint16)(i + 1u);
        }
    }
}

/*==================================================================================================
 *  TS-COM-001 .. 005 : chunk arithmetic
 *================================================================================================*/

/**
 * @test TS-COM-001 An empty file yields a zero-chunk plan.
 *
 * The v1 defect, pinned. v1 computed a chunk count of 0 for an empty file and then looped to
 * `count - 1`, which on an unsigned type is SIZE_MAX -- about four billion iterations, each
 * publishing a chunk read from past the end of an empty file. Since the watchdog was inert, nothing
 * would have recovered the transfer task.
 */
static void test_ChunkPlan_EmptyFileYieldsNoChunks(void)
{
    Com_ChunkPlanType plan;
    uint32 offset = 0u;
    uint32 length = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_ComputeChunkPlan(0u, COM_TRANSFER_CHUNK_SIZE, &plan));

    TEST_ASSERT_EQUAL_UINT32(0u, plan.chunkCount);
    TEST_ASSERT_EQUAL_UINT32(0u, plan.fullChunkCount);
    TEST_ASSERT_EQUAL_UINT32(0u, plan.lastChunkSize);

    /* And asking for chunk 0 of a zero-chunk plan is refused rather than returning something. */
    TEST_ASSERT_EQUAL(E_NOT_FOUND, Com_GetChunkExtent(&plan, 0u, COM_TRANSFER_CHUNK_SIZE, &offset, &length));
}

/** @test TS-COM-002 A file smaller than one chunk yields exactly one short chunk. */
static void test_ChunkPlan_SmallFileYieldsOneChunk(void)
{
    Com_ChunkPlanType plan;
    uint32 offset = 0u;
    uint32 length = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_ComputeChunkPlan(100u, COM_TRANSFER_CHUNK_SIZE, &plan));

    TEST_ASSERT_EQUAL_UINT32(1u, plan.chunkCount);
    TEST_ASSERT_EQUAL_UINT32(0u, plan.fullChunkCount);
    TEST_ASSERT_EQUAL_UINT32(100u, plan.lastChunkSize);

    TEST_ASSERT_EQUAL(E_OK, Com_GetChunkExtent(&plan, 0u, COM_TRANSFER_CHUNK_SIZE, &offset, &length));
    TEST_ASSERT_EQUAL_UINT32(0u, offset);
    TEST_ASSERT_EQUAL_UINT32(100u, length);

    TEST_ASSERT_EQUAL(E_NOT_FOUND, Com_GetChunkExtent(&plan, 1u, COM_TRANSFER_CHUNK_SIZE, &offset, &length));
}

/**
 * @test TS-COM-003 A file that divides exactly has no short tail.
 *
 * The boundary an off-by-one lives at: a naive count of size/chunk + 1 would add a phantom
 * zero-length chunk, and a naive size/chunk would drop the last real one.
 */
static void test_ChunkPlan_ExactMultipleHasNoTail(void)
{
    Com_ChunkPlanType plan;
    uint32 offset = 0u;
    uint32 length = 0u;

    TEST_ASSERT_EQUAL(E_OK,
                      Com_ComputeChunkPlan(COM_TRANSFER_CHUNK_SIZE * 3u, COM_TRANSFER_CHUNK_SIZE, &plan));

    TEST_ASSERT_EQUAL_UINT32(3u, plan.chunkCount);
    TEST_ASSERT_EQUAL_UINT32(3u, plan.fullChunkCount);
    TEST_ASSERT_EQUAL_UINT32(0u, plan.lastChunkSize);

    /* Every chunk, including the last, is a full one. */
    TEST_ASSERT_EQUAL(E_OK, Com_GetChunkExtent(&plan, 2u, COM_TRANSFER_CHUNK_SIZE, &offset, &length));
    TEST_ASSERT_EQUAL_UINT32(COM_TRANSFER_CHUNK_SIZE * 2u, offset);
    TEST_ASSERT_EQUAL_UINT32(COM_TRANSFER_CHUNK_SIZE, length);

    TEST_ASSERT_EQUAL(E_NOT_FOUND, Com_GetChunkExtent(&plan, 3u, COM_TRANSFER_CHUNK_SIZE, &offset, &length));
}

/** @test TS-COM-004 Chunk extents tile the file exactly, with no gap and no overlap. */
static void test_ChunkPlan_ExtentsTileTheFile(void)
{
    const uint32 sizes[] = {1u, 2047u, 2048u, 2049u, 4095u, 4096u, 4097u, 100000u, 1u << 20u};
    uint8 s;

    for (s = 0u; s < (uint8)STD_ARRAY_SIZE(sizes); s++)
    {
        Com_ChunkPlanType plan;
        uint32 index;
        uint32 covered = 0u;
        uint32 expectedOffset = 0u;

        TEST_ASSERT_EQUAL(E_OK, Com_ComputeChunkPlan(sizes[s], COM_TRANSFER_CHUNK_SIZE, &plan));

        for (index = 0u; index < plan.chunkCount; index++)
        {
            uint32 offset = 0u;
            uint32 length = 0u;

            TEST_ASSERT_EQUAL(E_OK,
                              Com_GetChunkExtent(&plan, index, COM_TRANSFER_CHUNK_SIZE, &offset, &length));

            TEST_ASSERT_EQUAL_UINT32_MESSAGE(expectedOffset, offset, "chunk offsets are not contiguous");
            TEST_ASSERT_GREATER_THAN_UINT32(0u, length);
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(COM_TRANSFER_CHUNK_SIZE, length);

            covered += length;
            expectedOffset += length;
        }

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizes[s], covered, "the chunks do not cover the file exactly");
    }
}

/** @test TS-COM-005 A zero chunk size and NULL pointers are rejected. */
static void test_ChunkPlan_ParameterChecking(void)
{
    Com_ChunkPlanType plan;
    uint32 offset = 0u;
    uint32 length = 0u;

    TEST_ASSERT_EQUAL(E_NOT_OK, Com_ComputeChunkPlan(1000u, 0u, &plan));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_ComputeChunkPlan(1000u, 2048u, NULL_PTR));

    TEST_ASSERT_EQUAL(E_OK, Com_ComputeChunkPlan(1000u, 2048u, &plan));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_GetChunkExtent(NULL_PTR, 0u, 2048u, &offset, &length));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_GetChunkExtent(&plan, 0u, 0u, &offset, &length));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_GetChunkExtent(&plan, 0u, 2048u, NULL_PTR, &length));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_GetChunkExtent(&plan, 0u, 2048u, &offset, NULL_PTR));
}

/*==================================================================================================
 *  TS-COM-006 .. 009 : backfill requests
 *================================================================================================*/

/** @test TS-COM-006 A well-formed request yields its dates. */
static void test_Backfill_ParsesValidDates(void)
{
    const char *payload = "20240315,20240316,20240317";
    Com_BackfillDateType dates[COM_MAX_BACKFILL_DATES];
    uint8 count = 0u;
    uint8 dropped = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_ParseBackfillRequest((const uint8 *)payload, (uint16)strlen(payload), dates,
                                                     (uint8)STD_ARRAY_SIZE(dates), &count, &dropped));

    TEST_ASSERT_EQUAL_UINT8(3u, count);
    TEST_ASSERT_EQUAL_UINT8(0u, dropped);
    TEST_ASSERT_EQUAL_STRING("20240315", dates[0].date);
    TEST_ASSERT_EQUAL_STRING("20240316", dates[1].date);
    TEST_ASSERT_EQUAL_STRING("20240317", dates[2].date);
}

/**
 * @test TS-COM-007 A request with more dates than the array holds is bounded, not allocated.
 *
 * The v1 defect, pinned. v1 called `new DataRequest` for every comma-separated date in an inbound
 * MQTT payload, with no limit and no allocation check, so a single crafted message could exhaust the
 * heap. The excess is counted and discarded here.
 */
static void test_Backfill_BoundsAnOversizedRequest(void)
{
    char payload[COM_MAX_REQUEST_PAYLOAD + 64u];
    Com_BackfillDateType dates[COM_MAX_BACKFILL_DATES];
    uint8 count = 0u;
    uint8 dropped = 0u;
    uint16 offset = 0u;
    uint8 day;

    /* Far more dates than the array can hold, and longer than the scan limit. */
    for (day = 1u; day <= 40u; day++)
    {
        const int n = (int)offset;
        if ((uint16)(offset + 9u) >= (uint16)sizeof(payload))
        {
            break;
        }
        payload[offset] = '2';
        payload[offset + 1u] = '0';
        payload[offset + 2u] = '2';
        payload[offset + 3u] = '4';
        payload[offset + 4u] = '0';
        payload[offset + 5u] = '1';
        payload[offset + 6u] = (char)('0' + (char)((day % 28u) / 10u));
        payload[offset + 7u] = (char)('0' + (char)(((day % 28u) % 10u) + 1u));
        payload[offset + 8u] = ',';
        offset = (uint16)(offset + 9u);
        COMPILER_UNUSED(n);
    }
    payload[offset] = '\0';

    TEST_ASSERT_EQUAL(E_OK, Com_ParseBackfillRequest((const uint8 *)payload, offset, dates,
                                                     (uint8)STD_ARRAY_SIZE(dates), &count, &dropped));

    /* Exactly the array's capacity is filled, and the rest is reported rather than silently lost. */
    TEST_ASSERT_EQUAL_UINT8((uint8)COM_MAX_BACKFILL_DATES, count);
    TEST_ASSERT_GREATER_THAN_UINT8(0u, dropped);

    {
        Com_StatisticsType stats;
        TEST_ASSERT_EQUAL(E_OK, Com_GetStatistics(&stats));
        TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.datesDropped);
    }
}

/** @test TS-COM-008 Malformed and impossible dates are rejected. */
static void test_Backfill_RejectsInvalidDates(void)
{
    Com_BackfillDateType dates[COM_MAX_BACKFILL_DATES];
    uint8 count = 0u;

    /* 30 February, 31 April, month 13, day 0, a short field and a non-numeric field. v1 accepted any
     * day from 1 to 31 in any month, so a request for 20240230 produced a file name that could never
     * exist and a transfer reporting "Not Found" -- indistinguishable from a genuinely missing log. */
    {
        const char *payload = "20240230,20240431,20241301,20240100,2024031,2024march";

        TEST_ASSERT_EQUAL(E_NOT_FOUND,
                          Com_ParseBackfillRequest((const uint8 *)payload, (uint16)strlen(payload), dates,
                                                   (uint8)STD_ARRAY_SIZE(dates), &count, NULL_PTR));
        TEST_ASSERT_EQUAL_UINT8(0u, count);
    }

    /* 29 February is valid in a leap year and not otherwise. */
    TEST_ASSERT_TRUE(Com_IsValidDate("20240229"));
    TEST_ASSERT_FALSE(Com_IsValidDate("20230229"));
    TEST_ASSERT_TRUE(Com_IsValidDate("20241231"));
    TEST_ASSERT_FALSE(Com_IsValidDate("20241232"));
    TEST_ASSERT_FALSE(Com_IsValidDate(NULL_PTR));

    /* A year before this firmware existed cannot name a log file it produced. */
    TEST_ASSERT_FALSE(Com_IsValidDate("19991231"));
}

/** @test TS-COM-009 Mixed valid and invalid dates keep only the valid ones. */
static void test_Backfill_MixedRequest(void)
{
    const char *payload = ",,20240315,rubbish,20240230,20240316,,";
    Com_BackfillDateType dates[COM_MAX_BACKFILL_DATES];
    uint8 count = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_ParseBackfillRequest((const uint8 *)payload, (uint16)strlen(payload), dates,
                                                     (uint8)STD_ARRAY_SIZE(dates), &count, NULL_PTR));

    TEST_ASSERT_EQUAL_UINT8(2u, count);
    TEST_ASSERT_EQUAL_STRING("20240315", dates[0].date);
    TEST_ASSERT_EQUAL_STRING("20240316", dates[1].date);

    TEST_ASSERT_EQUAL(E_NOT_OK, Com_ParseBackfillRequest(NULL_PTR, 10u, dates, 4u, &count, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      Com_ParseBackfillRequest((const uint8 *)payload, 10u, NULL_PTR, 4u, &count, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      Com_ParseBackfillRequest((const uint8 *)payload, 10u, dates, 0u, &count, NULL_PTR));
}

/** @test TS-COM-009b File names are formed only from valid dates. */
static void test_Backfill_FileNameFormatting(void)
{
    char name[COM_FILENAME_SIZE];

    TEST_ASSERT_EQUAL(E_OK, Com_FormatLogFileName(name, (uint16)sizeof(name), "20240315"));
    TEST_ASSERT_EQUAL_STRING("/20240315.csv", name);

    TEST_ASSERT_EQUAL(E_NOT_OK, Com_FormatLogFileName(name, (uint16)sizeof(name), "20240230"));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_FormatLogFileName(name, 4u, "20240315"));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_FormatLogFileName(NULL_PTR, 16u, "20240315"));
}

/*==================================================================================================
 *  TS-COM-010 .. 016 : record serialisation
 *================================================================================================*/

/** Populate @p record and its pack array with plausible values. */
static void makeRecord(Com_TelemetryRecordType *record, Rs485If_PackStateType *packs,
                       GnssIf_PositionType *position, boolean packsValid, boolean driveValid)
{
    uint8 p;
    uint8 c;

    (void)memset(record, 0, sizeof(*record));
    (void)memset(packs, 0, sizeof(*packs) * COM_PACK_COUNT);
    (void)memset(position, 0, sizeof(*position));

    record->sequenceNumber = 42u;
    record->unixTime = 1710500000uL;
    record->uptimeMs = 123456uL;
    record->deviceId = "246F28AABBCC";
    record->auxVoltageMilliVolts = 12345u;
    record->auxVoltageValid = TRUE;
    record->motorRpm = 3000u;
    record->dcVoltageDeciVolt = 720u;
    record->dcCurrentDeciAmp = 155u;
    record->speedMmPerSec = 12639u;
    record->mcuFaultCode = 0u;
    record->driveDataValid = driveValid;
    record->totalDistanceMm = 123456789uLL;
    record->tripDistanceMm = 54321uLL;
    record->confirmedDtcCount = 2u;
    record->heapFreeBytes = 180000uL;
    record->bearerState = 1u;

    for (p = 0u; p < (uint8)COM_PACK_COUNT; p++)
    {
        packs[p].present = TRUE;
        packs[p].serialNumber = 0x12345678uL + p;
        packs[p].packDataValid = packsValid;
        packs[p].cellDataValid = packsValid;
        packs[p].pack.voltage = (uint16)(7000u + p);
        packs[p].pack.current = -4300 - (sint32)p;
        packs[p].pack.temperature = (sint16)(-150 + (sint16)p);
        packs[p].pack.stateOfCharge = (uint8)(50u + p);
        packs[p].pack.stateOfHealth = 98u;
        packs[p].pack.statusFlags = 0xDEADBEEFuL;
        for (c = 0u; c < (uint8)RS485IF_CELLS_PER_PACK; c++)
        {
            packs[p].cells.cellVoltage[c] = (uint16)(3300u + c);
        }
        packs[p].cells.current = -5000;
        for (c = 0u; c < (uint8)RS485IF_TEMPS_PER_PACK; c++)
        {
            packs[p].cells.temperature[c] = (sint16)(2500 + c);
        }
        packs[p].cells.statusFlags1 = 0x87654321uL;
        packs[p].cells.statusFlags2 = 0x0F0F0F0FuL;
    }
    record->packs = packs;

    position->valid = TRUE;
    position->latitudeE7 = 314791667L;
    position->longitudeE7 = 743750000L;
    position->altitudeMm = 210500L;
    position->speedMmPerSec = 12500uL;
    position->headingDeciDeg = 844u;
    position->satellitesUsed = 11u;
    position->fixQuality = 1u;
    position->hdopCentiUnits = 80u;
    record->position = position;
}

/** @test TS-COM-010 The header and a record have the same number of fields. */
static void test_Record_HeaderAndRecordFieldCountsMatch(void)
{
    char header[COM_HEADER_BUFFER_SIZE];
    char record[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 headerLen = 0u;
    uint16 recordLen = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_FormatCsvHeader(header, (uint16)sizeof(header), &headerLen));

    makeRecord(&rec, packs, &position, TRUE, TRUE);
    TEST_ASSERT_EQUAL(E_OK, Com_SerialiseCsvRecord(&rec, record, (uint16)sizeof(record), &recordLen));

    /* A mismatch here is the single most consequential CSV defect: every downstream column shifts,
     * and the data looks plausible while meaning something else entirely. */
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(countFields(header), countFields(record),
                                     "header and record disagree on the number of fields");

    /* And the count matches what the configuration claims. */
    TEST_ASSERT_EQUAL_UINT16(COM_RECORD_FIELD_COUNT, countFields(header));
}

/** @test TS-COM-011 A fully populated record serialises its values correctly. */
static void test_Record_SerialisesValues(void)
{
    char buffer[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    char field[32];

    makeRecord(&rec, packs, &position, TRUE, TRUE);
    TEST_ASSERT_EQUAL(E_OK, Com_SerialiseCsvRecord(&rec, buffer, (uint16)sizeof(buffer), &written));

    extractField(buffer, 0u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("42", field);
    extractField(buffer, 1u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("1710500000", field);
    extractField(buffer, 3u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("246F28AABBCC", field);
    extractField(buffer, 5u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("3000", field);

    /* The odometer, as a 64-bit value well past what a 32-bit field could hold in millimetres. */
    extractField(buffer, 10u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("123456789", field);

    /* Latitude and longitude as signed integers. */
    extractField(buffer, 12u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("314791667", field);

    TEST_ASSERT_GREATER_THAN_UINT16(0u, written);
    TEST_ASSERT_EQUAL_UINT16(written, (uint16)strlen(buffer));
}

/**
 * @test TS-COM-012 "Not measured" and "measured zero" are different in the output.
 *
 * v1 emitted zeros and empty fields interchangeably, so a pack that had stopped answering was
 * indistinguishable in the log from one genuinely reading 0 V -- and anything computing a fleet
 * average over that column is wrong in a way nobody notices.
 */
static void test_Record_InvalidFieldsAreBlankNotZero(void)
{
    char buffer[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    char field[32];

    /* Packs silent, drive signals stale, no position. */
    makeRecord(&rec, packs, &position, FALSE, FALSE);
    position.valid = FALSE;
    rec.auxVoltageValid = FALSE;

    TEST_ASSERT_EQUAL(E_OK, Com_SerialiseCsvRecord(&rec, buffer, (uint16)sizeof(buffer), &written));

    /* Auxiliary voltage, RPM and latitude must all be empty, not "0". */
    extractField(buffer, 4u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", field, "an unmeasured voltage was emitted as a value");
    extractField(buffer, 5u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", field, "a stale RPM was emitted as a value");
    extractField(buffer, 12u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", field, "an absent position was emitted as a value");

    /* The first pack's voltage field, which is field 23. */
    extractField(buffer, 23u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", field, "a silent pack was emitted as reading zero");

    /* But the odometer is always present: if no distance has accumulated the value is genuinely
     * zero, and blanking it would lose the one field that must never have a gap. */
    extractField(buffer, 10u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("123456789", field);

    /* The field count is unchanged, so the columns still line up with the header. */
    {
        char header[COM_HEADER_BUFFER_SIZE];
        uint16 headerLen = 0u;
        TEST_ASSERT_EQUAL(E_OK, Com_FormatCsvHeader(header, (uint16)sizeof(header), &headerLen));
        TEST_ASSERT_EQUAL_UINT16(countFields(header), countFields(buffer));
    }
}

/** @test TS-COM-013 A zero wall-clock time is blanked rather than emitted as 1970. */
static void test_Record_UnknownTimeIsBlank(void)
{
    char buffer[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    char field[32];

    makeRecord(&rec, packs, &position, TRUE, TRUE);
    rec.unixTime = 0u;

    TEST_ASSERT_EQUAL(E_OK, Com_SerialiseCsvRecord(&rec, buffer, (uint16)sizeof(buffer), &written));

    extractField(buffer, 1u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", field, "an unknown timestamp would plot as 1970");

    /* Uptime is always available and must still be there, so a record with no wall clock is still
     * orderable. */
    extractField(buffer, 2u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("123456", field);
}

/**
 * @test TS-COM-014 A record that does not fit reports the truncation and stays terminated.
 *
 * v1 grew an Arduino String without limit, so there was no such thing as "does not fit" -- it either
 * allocated or the heap failed somewhere else entirely.
 */
static void test_Record_TruncationIsReported(void)
{
    char small[64];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    Com_StatisticsType stats;

    makeRecord(&rec, packs, &position, TRUE, TRUE);

    TEST_ASSERT_EQUAL(E_NO_SPACE, Com_SerialiseCsvRecord(&rec, small, (uint16)sizeof(small), &written));

    /* Still a valid C string, and the reported length is its real length -- so the caller can log
     * the truncation with an accurate size rather than guessing. */
    TEST_ASSERT_EQUAL_UINT16(written, (uint16)strlen(small));
    TEST_ASSERT_LESS_THAN_UINT16((uint16)sizeof(small), written);

    TEST_ASSERT_EQUAL(E_OK, Com_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.truncatedRecords);
}

/** @test TS-COM-015 The configured buffer size is large enough for a worst-case record. */
static void test_Record_ConfiguredBufferIsSufficient(void)
{
    char buffer[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    uint8 p;
    uint8 c;

    /* Every field at its widest: maximum unsigned values, most negative signed values, the longest
     * plausible device id. If the configured size cannot hold this, it is not a safe configuration. */
    makeRecord(&rec, packs, &position, TRUE, TRUE);
    rec.sequenceNumber = 0xFFFFFFFFuL;
    rec.unixTime = 0xFFFFFFFFuL;
    rec.uptimeMs = 0xFFFFFFFFuL;
    rec.deviceId = "FFFFFFFFFFFFFFFFFFFFFFF";
    rec.auxVoltageMilliVolts = 0xFFFFu;
    rec.motorRpm = 0xFFFFu;
    rec.dcVoltageDeciVolt = 0xFFFFu;
    rec.dcCurrentDeciAmp = 0xFFFFu;
    rec.speedMmPerSec = 0xFFFFFFFFuL;
    rec.totalDistanceMm = 0xFFFFFFFFFFFFFFFFuLL;
    rec.tripDistanceMm = 0xFFFFFFFFFFFFFFFFuLL;
    rec.heapFreeBytes = 0xFFFFFFFFuL;
    position.latitudeE7 = -900000000L;
    position.longitudeE7 = -1800000000L;
    position.altitudeMm = -2147483647L - 1L;
    position.speedMmPerSec = 0xFFFFFFFFuL;

    for (p = 0u; p < (uint8)COM_PACK_COUNT; p++)
    {
        packs[p].pack.voltage = 0xFFFFu;
        packs[p].pack.voltageHighest = 0xFFFFu;
        packs[p].pack.voltageLowest = 0xFFFFu;
        packs[p].pack.current = -2147483647L - 1L;
        packs[p].pack.temperature = -32768;
        packs[p].pack.temperatureHigh = -32768;
        packs[p].pack.temperatureLow = -32768;
        packs[p].pack.chargeEnergyWh = 0xFFFFFFFFuL;
        packs[p].pack.dischargeEnergyWh = 0xFFFFFFFFuL;
        packs[p].pack.chargeTimeSec = 0xFFFFFFFFuL;
        packs[p].pack.dischargeTimeSec = 0xFFFFFFFFuL;
        packs[p].pack.statusFlags = 0xFFFFFFFFuL;
        for (c = 0u; c < (uint8)RS485IF_CELLS_PER_PACK; c++)
        {
            packs[p].cells.cellVoltage[c] = 0xFFFFu;
        }
        packs[p].cells.current = -2147483647L - 1L;
        for (c = 0u; c < (uint8)RS485IF_TEMPS_PER_PACK; c++)
        {
            packs[p].cells.temperature[c] = -32768;
        }
        packs[p].cells.statusFlags1 = 0xFFFFFFFFuL;
        packs[p].cells.statusFlags2 = 0xFFFFFFFFuL;
    }

    TEST_ASSERT_EQUAL_MESSAGE(E_OK, Com_SerialiseCsvRecord(&rec, buffer, (uint16)sizeof(buffer), &written),
                              "COM_RECORD_BUFFER_SIZE is too small for a worst-case record");

    /* And there is genuine headroom, not a value tuned to exactly fit. */
    TEST_ASSERT_LESS_THAN_UINT16((uint16)(COM_RECORD_BUFFER_SIZE - 64u), written);
}

/** @test TS-COM-016 Negative and extreme signed values round-trip through the formatter. */
static void test_Record_SignedExtremes(void)
{
    char buffer[COM_RECORD_BUFFER_SIZE];
    Com_TelemetryRecordType rec;
    Rs485If_PackStateType packs[COM_PACK_COUNT];
    GnssIf_PositionType position;
    uint16 written = 0u;
    char field[32];

    makeRecord(&rec, packs, &position, TRUE, TRUE);

    /* The most negative sint32. Negating it directly is undefined, so the formatter must not. */
    packs[0].pack.current = -2147483647L - 1L;

    TEST_ASSERT_EQUAL(E_OK, Com_SerialiseCsvRecord(&rec, buffer, (uint16)sizeof(buffer), &written));

    /* Pack 1's current is field 26: 23 vehicle fields, then V, V_HI, V_LO, then I. */
    extractField(buffer, 26u, field, (uint16)sizeof(field));
    TEST_ASSERT_EQUAL_STRING("-2147483648", field);

    TEST_ASSERT_EQUAL(E_NOT_OK, Com_SerialiseCsvRecord(NULL_PTR, buffer, 100u, &written));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_SerialiseCsvRecord(&rec, NULL_PTR, 100u, &written));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_SerialiseCsvRecord(&rec, buffer, 100u, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_FormatCsvHeader(NULL_PTR, 100u, &written));
    TEST_ASSERT_EQUAL(E_NO_SPACE, Com_FormatCsvHeader(buffer, 8u, &written));
    TEST_ASSERT_EQUAL(E_NOT_OK, Com_GetStatistics(NULL_PTR));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ChunkPlan_EmptyFileYieldsNoChunks);
    RUN_TEST(test_ChunkPlan_SmallFileYieldsOneChunk);
    RUN_TEST(test_ChunkPlan_ExactMultipleHasNoTail);
    RUN_TEST(test_ChunkPlan_ExtentsTileTheFile);
    RUN_TEST(test_ChunkPlan_ParameterChecking);
    RUN_TEST(test_Backfill_ParsesValidDates);
    RUN_TEST(test_Backfill_BoundsAnOversizedRequest);
    RUN_TEST(test_Backfill_RejectsInvalidDates);
    RUN_TEST(test_Backfill_MixedRequest);
    RUN_TEST(test_Backfill_FileNameFormatting);
    RUN_TEST(test_Record_HeaderAndRecordFieldCountsMatch);
    RUN_TEST(test_Record_SerialisesValues);
    RUN_TEST(test_Record_InvalidFieldsAreBlankNotZero);
    RUN_TEST(test_Record_UnknownTimeIsBlank);
    RUN_TEST(test_Record_TruncationIsReported);
    RUN_TEST(test_Record_ConfiguredBufferIsSufficient);
    RUN_TEST(test_Record_SignedExtremes);
    return UNITY_END();
}

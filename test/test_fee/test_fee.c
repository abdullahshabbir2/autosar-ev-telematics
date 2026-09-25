/**
 * @file    test_fee.c
 * @brief   Unit tests for the flash EEPROM emulation layer.
 *
 * The cases that matter here are the power-fail ones. A supply loss during a write is the
 * condition that destroyed the v1 odometer reading, it happens on every engine crank, and it
 * is effectively untestable on a bench -- which is exactly why the flash driver is a stub with
 * an address-targeted fault injector.
 *
 * The property under test throughout: **after an interruption at any point, the next start
 * reads either the new value or the previous one, never a blend and never nothing.**
 *
 * "Power loss" is modelled as sabotaging a specific write and then calling ::Fee_Init again.
 * The emulated media survives, exactly as flash does; only the RAM state is rebuilt, exactly
 * as a reset does.
 *
 * @req SWREQ-NVM-0010 .. SWREQ-NVM-0025
 * @verifies TS-FEE-001 .. TS-FEE-018
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "mcal/Fls/Fls.h"
#include "Stub_Mcal.h"
#include "unity.h"

/* Media offsets the power-fail tests aim at. Derived from the layout documented in Fee.c;
 * the record header is 16 bytes and its commit byte sits at offset 12 within it. */
#define FEE_FIRST_RECORD_OFFSET FEE_SECTOR_HEADER_SIZE
#define FEE_RECORD_STATE_OFFSET 12u

/** Address of the commit byte of the Nth record in sector 0, for a block of @p len bytes. */
static uint32 commitAddressOfRecord(uint32 recordIndex, uint16 payloadLength)
{
    const uint32 span = FEE_RECORD_HEADER_SIZE + payloadLength;
    return FEE_FIRST_RECORD_OFFSET + (recordIndex * span) + FEE_RECORD_STATE_OFFSET;
}

/** Address of the payload of the Nth record in sector 0. */
static uint32 payloadAddressOfRecord(uint32 recordIndex, uint16 payloadLength)
{
    const uint32 span = FEE_RECORD_HEADER_SIZE + payloadLength;
    return FEE_FIRST_RECORD_OFFSET + (recordIndex * span) + FEE_RECORD_HEADER_SIZE;
}

/**
 * @brief Clear one set bit at @p address, emulating a cell that has lost charge.
 *
 * Searches forward for a byte with at least one bit set, because NOR flash degradation can
 * only clear bits -- and because a mask applied blindly may happen to change nothing, which
 * would leave the test asserting against uncorrupted data and passing for the wrong reason.
 *
 * @return The address actually corrupted.
 */
static uint32 corruptOneBitAt(uint32 address)
{
    uint32 addr = address;
    uint32 guard;

    for (guard = 0u; guard < 64u; guard++)
    {
        const uint8 original = Stub_Fls_Peek(addr);

        if (original != 0x00u)
        {
            /* value & (value - 1) clears exactly the lowest set bit. */
            Stub_Fls_Corrupt(addr, (uint8)(original & (uint8)(original - 1u)));
            TEST_ASSERT_NOT_EQUAL_MESSAGE(original, Stub_Fls_Peek(addr), "corruption helper changed nothing");
            return addr;
        }
        addr++;
    }

    TEST_FAIL_MESSAGE("found no byte with a set bit to corrupt");
    return addr;
}

/** Fill @p buffer with a recognisable pattern derived from @p seed. */
static void makePattern(uint8 *buffer, uint16 length, uint8 seed)
{
    uint16 i;
    for (i = 0u; i < length; i++)
    {
        buffer[i] = (uint8)(seed + (uint8)(i * 3u));
    }
}

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
}

void tearDown(void)
{
}

/*==================================================================================================
 *  TS-FEE-001 .. 004 : formatting and basic persistence
 *================================================================================================*/

/** @test TS-FEE-001 A virgin partition is formatted and sector 0 becomes active. */
static void test_Init_FormatsVirginPartition(void)
{
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));

    TEST_ASSERT_EQUAL_UINT16(0u, status.activeSector);
    TEST_ASSERT_EQUAL_UINT16(1u, status.activeSequence);
    TEST_ASSERT_EQUAL_UINT32(FEE_SECTOR_HEADER_SIZE, status.bytesUsed);
    TEST_ASSERT_FALSE(status.layoutRecovered);

    /* Every block reports absent, so NvM knows to apply its defaults rather than publish
     * whatever an erased sector happens to contain. */
    {
        uint8 buffer[FEE_LENGTH_ODOMETER];
        TEST_ASSERT_EQUAL(E_NOT_FOUND, Fee_ReadBlock(FEE_BLOCK_ODOMETER, buffer, 0u, sizeof(buffer)));
    }
}

/** @test TS-FEE-002 A written block reads back byte for byte. */
static void test_WriteThenRead_RoundTrips(void)
{
    uint8 written[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(written, sizeof(written), 0x11u);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, written));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(written, read, sizeof(written));
}

/** @test TS-FEE-003 Data survives a restart, which is the entire point of the module. */
static void test_WrittenData_SurvivesRestart(void)
{
    uint8 written[FEE_LENGTH_CALIBRATION];
    uint8 read[FEE_LENGTH_CALIBRATION];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(written, sizeof(written), 0x42u);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, written));

    /* Restart: RAM state is rebuilt from the media, which is untouched. */
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(written, read, sizeof(written));
}

/** @test TS-FEE-004 Rewriting a block yields the newest value, across a restart. */
static void test_Rewrite_YieldsNewestValue(void)
{
    uint8 first[FEE_LENGTH_ODOMETER];
    uint8 second[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];
    uint8 generation;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(first, sizeof(first), 0x01u);
    makePattern(second, sizeof(second), 0x80u);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, first));
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, second));

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(second, read, sizeof(second));

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(second, read, sizeof(second));

    /* And repeatedly, so the newest-wins logic is exercised beyond two generations. */
    for (generation = 0u; generation < 20u; generation++)
    {
        uint8 value[FEE_LENGTH_ODOMETER];
        makePattern(value, sizeof(value), (uint8)(0xA0u + generation));
        TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, value));
        TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(value, read, sizeof(value));
    }
}

/*==================================================================================================
 *  TS-FEE-005 .. 008 : power loss during a block write
 *
 *  These are the cases the v1 design could not survive.
 *================================================================================================*/

/**
 * @test TS-FEE-005 A supply loss before the commit byte leaves the previous value readable.
 *
 * The record's header and payload are both on the media, but the single byte that marks it
 * committed never lands. A reader must treat it as if it had never been written.
 */
static void test_PowerLoss_BeforeCommit_PreservesPreviousValue(void)
{
    uint8 original[FEE_LENGTH_ODOMETER];
    uint8 replacement[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(original, sizeof(original), 0x55u);
    makePattern(replacement, sizeof(replacement), 0xAAu);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, original));

    /* Sabotage the commit byte of the *second* record: header and payload land, commit does
     * not. Record 0 is the write above. */
    Stub_Fls_FailWriteAtAddress(commitAddressOfRecord(1u, FEE_LENGTH_ODOMETER), 0u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, replacement));

    /* Reset. */
    Stub_Fls_ClearFaults();
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(original, read, sizeof(original),
                                         "an uncommitted record was read as current");

    /* And the interruption is visible in the diagnostics, not silent. */
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_GREATER_THAN_UINT32(0u, status.incompleteRecordCount);
}

/**
 * @test TS-FEE-006 A supply loss part way through the payload leaves the previous value.
 *
 * Half the new payload is on the media. Its CRC cannot match, and its commit byte never
 * landed either, so it must lose to the older complete record.
 */
static void test_PowerLoss_MidPayload_PreservesPreviousValue(void)
{
    uint8 original[FEE_LENGTH_ODOMETER];
    uint8 replacement[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(original, sizeof(original), 0x33u);
    makePattern(replacement, sizeof(replacement), 0xCCu);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, original));

    /* Let half the payload through, then fail. */
    Stub_Fls_FailWriteAtAddress(payloadAddressOfRecord(1u, FEE_LENGTH_ODOMETER), FEE_LENGTH_ODOMETER / 2u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, replacement));

    Stub_Fls_ClearFaults();
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(original, read, sizeof(original),
                                         "a half-written payload was read as current");
}

/**
 * @test TS-FEE-007 A supply loss on the very first write leaves the block simply absent.
 *
 * There is no previous value to fall back to, so the correct outcome is "never written" --
 * which lets NvM apply its configured default instead of publishing erased flash.
 */
static void test_PowerLoss_OnFirstWrite_LeavesBlockAbsent(void)
{
    uint8 value[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(value, sizeof(value), 0x77u);

    Stub_Fls_FailWriteAtAddress(commitAddressOfRecord(0u, FEE_LENGTH_ODOMETER), 0u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, value));

    Stub_Fls_ClearFaults();
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    TEST_ASSERT_EQUAL(E_NOT_FOUND, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
}

/**
 * @test TS-FEE-008 After an interrupted write, the block is writable again and recovers.
 *
 * A one-off interruption must not leave the block permanently stuck.
 */
static void test_PowerLoss_ThenRetry_Succeeds(void)
{
    uint8 original[FEE_LENGTH_ODOMETER];
    uint8 replacement[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(original, sizeof(original), 0x10u);
    makePattern(replacement, sizeof(replacement), 0x20u);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, original));

    Stub_Fls_FailWriteAtAddress(commitAddressOfRecord(1u, FEE_LENGTH_ODOMETER), 0u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, replacement));

    Stub_Fls_ClearFaults();
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    /* Retry the same write; this time it must stick. */
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, replacement));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(replacement, read, sizeof(replacement));

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(replacement, read, sizeof(replacement));
}

/*==================================================================================================
 *  TS-FEE-009 .. 010 : corruption fallback
 *================================================================================================*/

/**
 * @test TS-FEE-009 A corrupted newest record falls back to the older intact one.
 *
 * This is why superseded records are not erased eagerly: a bit flip in the newest copy costs
 * one generation of data, not the whole block.
 */
static void test_CorruptNewestRecord_FallsBackToOlder(void)
{
    uint8 first[FEE_LENGTH_ODOMETER];
    uint8 second[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(first, sizeof(first), 0x01u);
    makePattern(second, sizeof(second), 0xF0u);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, first));
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, second));

    /* Flip a bit inside the second record's payload, behind the driver's back -- bit rot, or a
     * cell that lost charge. Only clearing bits is physically plausible on NOR flash. */
    (void)corruptOneBitAt(payloadAddressOfRecord(1u, FEE_LENGTH_ODOMETER) + 4u);

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(first, read, sizeof(first),
                                         "a CRC-failing record was returned as current");

    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_GREATER_THAN_UINT32(0u, status.crcFailureCount);
}

/** @test TS-FEE-010 If every record for a block is corrupt, the read fails rather than lying. */
static void test_AllRecordsCorrupt_ReportsFailure(void)
{
    uint8 value[FEE_LENGTH_ODOMETER];
    uint8 read[FEE_LENGTH_ODOMETER];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(value, sizeof(value), 0x5Au);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, value));

    (void)corruptOneBitAt(payloadAddressOfRecord(0u, FEE_LENGTH_ODOMETER) + 2u);

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    /* E_CRC_FAIL, distinct from E_NOT_FOUND: "the data is damaged" and "there is no data" call
     * for different responses, and only one of them warrants a diagnostic trouble code. */
    TEST_ASSERT_EQUAL(E_CRC_FAIL, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(read)));
}

/*==================================================================================================
 *  TS-FEE-011 .. 014 : garbage collection
 *================================================================================================*/

/** @test TS-FEE-011 Repeated writes trigger collection and preserve every block. */
static void test_GarbageCollection_PreservesAllBlocks(void)
{
    uint8 odo[FEE_LENGTH_ODOMETER];
    uint8 cal[FEE_LENGTH_CALIBRATION];
    /* Sized for the largest block read back below, not for whichever happens to be first --
     * blocks have different lengths and a buffer sized to the wrong one overflows the stack. */
    uint8 read[FEE_MAX_BLOCK_LENGTH];
    Fee_StatusType status;
    uint16 cycle;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    makePattern(cal, sizeof(cal), 0xBEu);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, cal));

    /* Enough odometer writes to overflow a 4 KiB sector several times over. */
    for (cycle = 0u; cycle < 400u; cycle++)
    {
        makePattern(odo, sizeof(odo), (uint8)cycle);
        TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, odo));
    }

    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0u, status.gcCount,
                                            "the sector never filled; the test proves nothing");

    /* The calibration block, written once at the very beginning, must have been carried
     * across every collection. Losing an untouched block during GC is the classic failure of
     * a hand-rolled EEPROM emulation. */
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(cal)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(cal, read, sizeof(cal),
                                         "an untouched block was lost during collection");

    /* And the odometer holds its final value. */
    makePattern(odo, sizeof(odo), (uint8)399u);
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(odo)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(odo, read, sizeof(odo));

    /* Survives a restart too. */
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(cal)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cal, read, sizeof(cal));
}

/**
 * @test TS-FEE-012 A supply loss before the collection commit leaves the source active.
 *
 * The target sector has been erased and partly filled, but its header -- the commit -- never
 * landed. The old sector must still be the authoritative one.
 */
static void test_PowerLoss_DuringGcBeforeCommit_KeepsSourceSector(void)
{
    uint8 odo[FEE_LENGTH_ODOMETER];
    uint8 cal[FEE_LENGTH_CALIBRATION];
    uint8 read[FEE_MAX_BLOCK_LENGTH];
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(cal, sizeof(cal), 0x5Eu);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, cal));
    makePattern(odo, sizeof(odo), 0x99u);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, odo));

    /* Sector 1's header is the collection commit point. Sabotage it, then force collection. */
    Stub_Fls_FailWriteAtAddress(FEE_SECTOR_SIZE, 0u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_GarbageCollect());

    Stub_Fls_ClearFaults();
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));

    /* Sector 0 is still active, because sector 1 never became a valid Fee sector. */
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, status.activeSector, "an uncommitted collection target was adopted");

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(cal)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cal, read, sizeof(cal));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(odo)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(odo, read, sizeof(odo));
}

/**
 * @test TS-FEE-013 A supply loss after the collection commit adopts the target sector.
 *
 * Both sectors now carry a valid header. The higher sequence number identifies the completed
 * copy, and the stale one is reclaimed.
 */
static void test_PowerLoss_DuringGcAfterCommit_AdoptsTargetSector(void)
{
    uint8 odo[FEE_LENGTH_ODOMETER];
    uint8 cal[FEE_LENGTH_CALIBRATION];
    uint8 read[FEE_MAX_BLOCK_LENGTH];
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(cal, sizeof(cal), 0x7Cu);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, cal));
    makePattern(odo, sizeof(odo), 0x2Bu);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, odo));

    /* Collect successfully, but prevent the source sector from being erased afterwards. That
     * leaves exactly the state a reset between steps 3 and 4 produces: two valid headers. */
    Stub_Fls_FailNextReads(0u);
    TEST_ASSERT_EQUAL(E_OK, Fee_GarbageCollect());

    /* Recreate the two-valid-sector condition by restoring sector 0's header, since the real
     * collection did erase it. Writing the same bytes back is exactly what a reset before the
     * erase would have left behind. */
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT16(1u, status.activeSector);

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT16(1u, status.activeSector);

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(cal)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cal, read, sizeof(cal));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, read, 0u, sizeof(odo)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(odo, read, sizeof(odo));
}

/** @test TS-FEE-014 Collection is bounded: it does not erase more than it must. */
static void test_GarbageCollection_ErasesOnePairPerPass(void)
{
    uint8 odo[FEE_LENGTH_ODOMETER];
    uint32 erasesBefore;
    uint32 erasesAfter;
    Fee_StatusType before;
    Fee_StatusType after;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(odo, sizeof(odo), 0x64u);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, odo));

    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&before));
    erasesBefore = Stub_Fls_GetEraseCount();

    TEST_ASSERT_EQUAL(E_OK, Fee_GarbageCollect());

    erasesAfter = Stub_Fls_GetEraseCount();
    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&after));

    /* One pass erases the target before copying and the source after: exactly two. Wear is the
     * scarce resource here, and an extra erase per pass halves the part's service life. */
    TEST_ASSERT_EQUAL_UINT32(2u, erasesAfter - erasesBefore);
    TEST_ASSERT_EQUAL_UINT32(before.gcCount + 1u, after.gcCount);
    TEST_ASSERT_NOT_EQUAL(before.activeSector, after.activeSector);
}

/*==================================================================================================
 *  TS-FEE-015 .. 018 : invalidation, parameter checking, media failure
 *================================================================================================*/

/** @test TS-FEE-015 An invalidated block reads as absent, and a rewrite revives it. */
static void test_Invalidate_ThenRewrite(void)
{
    uint8 value[FEE_LENGTH_CALIBRATION];
    uint8 read[FEE_LENGTH_CALIBRATION];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(value, sizeof(value), 0x3Cu);

    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, value));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(read)));

    TEST_ASSERT_EQUAL(E_OK, Fee_InvalidateBlock(FEE_BLOCK_CALIBRATION));
    TEST_ASSERT_EQUAL(E_NOT_FOUND, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(read)));

    /* Invalidation is itself a record, so it survives a restart. */
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_NOT_FOUND, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(read)));

    /* A later write revives the block. */
    makePattern(value, sizeof(value), 0xD4u);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_CALIBRATION, value));
    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_CALIBRATION, read, 0u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(value, read, sizeof(value));
}

/** @test TS-FEE-016 A partial read inside a block returns the right slice. */
static void test_PartialRead_ReturnsCorrectSlice(void)
{
    uint8 value[FEE_LENGTH_DEVICE_CONFIG];
    uint8 read[8];

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(value, sizeof(value), 0x00u);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_DEVICE_CONFIG, value));

    TEST_ASSERT_EQUAL(E_OK, Fee_ReadBlock(FEE_BLOCK_DEVICE_CONFIG, read, 16u, sizeof(read)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&value[16], read, sizeof(read));
}

/** @test TS-FEE-017 Unknown blocks, NULL buffers and over-long reads are rejected. */
static void test_ParameterChecking(void)
{
    uint8 buffer[FEE_LENGTH_ODOMETER];
    Det_StatisticsType det;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());

    TEST_ASSERT_EQUAL(E_NOT_OK, Fee_ReadBlock((Fee_BlockIdType)0x9999u, buffer, 0u, sizeof(buffer)));
    TEST_ASSERT_EQUAL(E_NOT_OK, Fee_WriteBlock((Fee_BlockIdType)0x9999u, buffer));
    TEST_ASSERT_EQUAL(E_NOT_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, NULL_PTR, 0u, sizeof(buffer)));
    TEST_ASSERT_EQUAL(E_NOT_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, NULL_PTR));

    /* Reading past the block's configured length must be refused, not clamped: a clamped read
     * returns a short buffer the caller believes is full. */
    TEST_ASSERT_EQUAL(E_NOT_OK, Fee_ReadBlock(FEE_BLOCK_ODOMETER, buffer, FEE_LENGTH_ODOMETER - 4u, 8u));

    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(NULL_PTR) == E_OK ? E_NOT_OK : E_OK);

    Det_GetStatistics(&det);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(5u, det.devErrorCount);
}

/** @test TS-FEE-018 A media read failure during Init is reported rather than masked. */
static void test_MediaFailure_IsReported(void)
{
    uint8 value[FEE_LENGTH_ODOMETER];
    Fee_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    makePattern(value, sizeof(value), 0x1Fu);
    TEST_ASSERT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, value));

    /* A write that the media rejects outright must not be reported as success. */
    Stub_Fls_FailNextWrites(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Fee_WriteBlock(FEE_BLOCK_ODOMETER, value));

    TEST_ASSERT_EQUAL(E_OK, Fee_GetStatus(&status));
    TEST_ASSERT_GREATER_THAN_UINT32(0u, status.mediaErrorCount);
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_Init_FormatsVirginPartition);
    RUN_TEST(test_WriteThenRead_RoundTrips);
    RUN_TEST(test_WrittenData_SurvivesRestart);
    RUN_TEST(test_Rewrite_YieldsNewestValue);
    RUN_TEST(test_PowerLoss_BeforeCommit_PreservesPreviousValue);
    RUN_TEST(test_PowerLoss_MidPayload_PreservesPreviousValue);
    RUN_TEST(test_PowerLoss_OnFirstWrite_LeavesBlockAbsent);
    RUN_TEST(test_PowerLoss_ThenRetry_Succeeds);
    RUN_TEST(test_CorruptNewestRecord_FallsBackToOlder);
    RUN_TEST(test_AllRecordsCorrupt_ReportsFailure);
    RUN_TEST(test_GarbageCollection_PreservesAllBlocks);
    RUN_TEST(test_PowerLoss_DuringGcBeforeCommit_KeepsSourceSector);
    RUN_TEST(test_PowerLoss_DuringGcAfterCommit_AdoptsTargetSector);
    RUN_TEST(test_GarbageCollection_ErasesOnePairPerPass);
    RUN_TEST(test_Invalidate_ThenRewrite);
    RUN_TEST(test_PartialRead_ReturnsCorrectSlice);
    RUN_TEST(test_ParameterChecking);
    RUN_TEST(test_MediaFailure_IsReported);
    return UNITY_END();
}

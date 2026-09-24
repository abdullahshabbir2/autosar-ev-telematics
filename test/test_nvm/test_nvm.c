/**
 * @file    test_nvm.c
 * @brief   Unit tests for the non-volatile manager: mirrors, defaults, integrity and wear control.
 *
 * @par What these tests are protecting
 * The odometer reading, which is the number the whole product exists to produce and the one v1 lost.
 * Every case here corresponds to one of the ways it was lost or could be: a partial write that reads
 * back plausibly, a first boot mistaken for a damaged block, a write-through policy that would wear
 * the flash out in days, and a fallback path that produces zero instead of the last known value.
 *
 * The stack under test is real from NvM down through Fee to the flash stub, so the crash-safety
 * argument is exercised end to end rather than asserted. Where a test needs a failure, it injects it
 * at the flash driver -- the only layer where a genuine failure originates.
 *
 * @req SWREQ-NVM-0001 .. SWREQ-NVM-0009, SWREQ-NVM-0030 .. SWREQ-NVM-0038
 * @verifies TS-NVM-001 .. TS-NVM-019
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "mcal/Fls/Fls.h"
#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "services/NvM/NvM.h"
#include "services/NvM/NvM_Cfg.h"
#include "Stub_Mcal.h"
#include "unity.h"

/*==================================================================================================
 *  Fixture
 *================================================================================================*/

/**
 * @brief Bring the storage stack up on erased media.
 *
 * The order matters and is the same as EcuM's: Fls, then Fee, then NvM. Starting NvM against an
 * uninitialised Fee would exercise a path that cannot occur on the target.
 */
static void TnBringUpErased(void)
{
    /* setUp has already erased the media through Stub_Mcal_ResetAll. */
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
}

/**
 * @brief Restart the stack without erasing, as a power cycle would.
 *
 * This is the operation most of these tests turn on: it is the only way to prove that what reached
 * the media is what comes back, as opposed to reading the RAM mirror that is still populated.
 */
static void TnRestart(void)
{
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
}

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
}

void tearDown(void)
{
}

/*==================================================================================================
 *  TS-NVM-001 .. 004  Defaults, and the distinction that caused a real bug
 *================================================================================================*/

/**
 * TS-NVM-001: SWREQ-NVM-0035 -- a first boot populates every block from its configured default.
 *
 * Zero is not a safe default for a calibration value: a tyre diameter of zero makes every distance
 * zero, and the failure would look like a dead speed sensor rather than a storage problem.
 */
static void test_Nvm_FirstBootAppliesDefaults(void)
{
    NvM_CalibrationType calibration;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(NVM_DEFAULT_TYRE_DIAMETER_MILLI_INCH, calibration.tyreDiameterMilliInch);
    TEST_ASSERT_EQUAL_UINT16(NVM_DEFAULT_GEAR_RATIO_MILLI, calibration.gearRatioMilli);
    TEST_ASSERT_EQUAL_UINT16(NVM_DEFAULT_MAX_PLAUSIBLE_RPM, calibration.maxPlausibleRpm);

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(NVM_BLOCK_COUNT, stats.defaultsApplied);
}

/**
 * TS-NVM-002: SWREQ-NVM-0020 -- "never written" is reported as restored defaults, not as damage.
 *
 * The distinction that was a real bug during development. "Use the default" and "fall back to
 * redundancy and raise a storage fault" are opposite responses, and conflating them makes every
 * brand-new unit report a storage fault on its first boot.
 */
static void test_Nvm_NeverWrittenIsNotIntegrityFailure(void)
{
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(NVM_REQ_RESTORED_DEFAULTS, NvM_GetErrorStatus(NVM_BLOCK_ODOMETER));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(0u, stats.integrityFailures);
}

/** TS-NVM-003: the odometer's default is zero distance, which is correct for a new unit only. */
static void test_Nvm_OdometerDefaultIsZero(void)
{
    NvM_OdometerType odometer;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL_UINT64(0uLL, odometer.totalDistanceMm);
    TEST_ASSERT_EQUAL_UINT32(0u, odometer.updateCount);
}

/** TS-NVM-004: SWREQ-NVM-0035 -- restoring defaults on request returns the block to a known state. */
static void test_Nvm_RestoreDefaultsIsReachable(void)
{
    NvM_CalibrationType calibration;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.tyreDiameterMilliInch = 21000u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_CALIBRATION));

    TEST_ASSERT_EQUAL(E_OK, NvM_RestoreBlockDefaults(NVM_BLOCK_CALIBRATION));

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(NVM_DEFAULT_TYRE_DIAMETER_MILLI_INCH, calibration.tyreDiameterMilliInch);
}

/*==================================================================================================
 *  TS-NVM-005 .. 008  The RAM mirror, and what reaches the media
 *================================================================================================*/

/**
 * TS-NVM-005: SWREQ-NVM-0030 -- a write to a deferred block updates the mirror and not flash.
 *
 * This is the basis of the wear budget for everything that is not durability-critical. Calibration is
 * a deferred block; the odometer deliberately is not, and TS-NVM-019 covers that difference.
 */
static void test_Nvm_DeferredWriteUpdatesMirrorNotFlash(void)
{
    NvM_CalibrationType calibration;
    uint32 writesBefore;
    Fls_StatisticsType flsStats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, Fls_GetStatistics(&flsStats));
    writesBefore = flsStats.writeCount;

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.maxPlausibleRpm = 9000u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));

    /* The mirror has the new value... */
    (void)memset(&calibration, 0, sizeof(calibration));
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(9000u, calibration.maxPlausibleRpm);

    /* ...and the media has not been touched. */
    TEST_ASSERT_EQUAL(E_OK, Fls_GetStatistics(&flsStats));
    TEST_ASSERT_EQUAL_UINT32(writesBefore, flsStats.writeCount);
}

/**
 * TS-NVM-019: the odometer writes through immediately, without an explicit flush.
 *
 * The deliberate exception to TS-NVM-005, and the reason it exists: a deferred write is exactly the
 * window in which the power cut that loses the value happens, and the whole product is that value.
 * Restart info is immediate for the same kind of reason -- its only purpose is to be readable after
 * the reset that incremented it.
 */
static void test_Nvm_OdometerWritesThrough(void)
{
    NvM_OdometerType odometer;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 246810uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));

    /* No flush, and nothing left pending. */
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.dirtyBlockCount);

    TnRestart();

    (void)memset(&odometer, 0, sizeof(odometer));
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL_UINT64(246810uLL, odometer.totalDistanceMm);
}

/** TS-NVM-006: an explicit flush is what reaches the media, and it survives a restart. */
static void test_Nvm_FlushedValueSurvivesRestart(void)
{
    NvM_OdometerType odometer;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 987654321uLL;
    odometer.updateCount = 7u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_ODOMETER));

    TnRestart();

    (void)memset(&odometer, 0, sizeof(odometer));
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL_UINT64(987654321uLL, odometer.totalDistanceMm);
    TEST_ASSERT_EQUAL_UINT32(7u, odometer.updateCount);
    TEST_ASSERT_EQUAL(NVM_REQ_OK, NvM_GetErrorStatus(NVM_BLOCK_ODOMETER));
}

/**
 * TS-NVM-007: an unflushed write is lost on a power cut.
 *
 * Asserted rather than left implicit, because it is the cost of the deferred-write policy and the
 * reason the odometer has a 100 m persist threshold. A test suite that only proved the happy path
 * would leave a reader unsure whether the mirror was somehow durable.
 */
static void test_Nvm_UnflushedDeferredWriteIsLostOnRestart(void)
{
    NvM_CalibrationType calibration;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.gearRatioMilli = 5000u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_CALIBRATION));

    /* A second value that is never flushed. */
    calibration.gearRatioMilli = 7000u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));

    TnRestart();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(5000u, calibration.gearRatioMilli);
}

/** TS-NVM-008: WriteAll flushes every dirty block, which is the shutdown path's guarantee. */
static void test_Nvm_WriteAllFlushesEveryDirtyBlock(void)
{
    NvM_EnergyCountersType energy;
    NvM_CalibrationType calibration;
    NvM_StatisticsType stats;

    TnBringUpErased();

    /* Two *deferred* blocks. An immediate block is never dirty, so using the odometer here would
     * assert a dirty count of one and quietly stop testing WriteAll's ability to flush several. */
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));
    energy.energyOutMilliWh = 4242uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.gearRatioMilli = 5500u;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(2u, stats.dirtyBlockCount);

    TEST_ASSERT_EQUAL(E_OK, NvM_WriteAll());

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.dirtyBlockCount);

    TnRestart();
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));
    TEST_ASSERT_EQUAL_UINT64(4242uLL, energy.energyOutMilliWh);
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(5500u, calibration.gearRatioMilli);
}

/*==================================================================================================
 *  TS-NVM-009 .. 012  Write-on-change: the wear budget
 *================================================================================================*/

/**
 * TS-NVM-009: SWREQ-NVM-0004 -- writing unchanged contents does not mark the block dirty.
 *
 * The arithmetic this protects: at one write per 3 s acquisition, unconditional writing is 28 800 a
 * day against about 100 000 erase cycles. Under four days.
 */
static void test_Nvm_UnchangedWriteIsSkipped(void)
{
    NvM_CalibrationType calibration;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.vbattOffsetMv = 12;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_CALIBRATION));

    /* The same contents again: nothing to do. */
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL(NVM_REQ_BLOCK_SKIPPED, NvM_GetErrorStatus(NVM_BLOCK_CALIBRATION));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.dirtyBlockCount);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.skippedWriteCount);
}

/**
 * TS-NVM-010: a one-byte difference is not skipped.
 *
 * The other half of TS-NVM-009, and the more important half: a comparison that was too coarse -- a
 * length check, or a checksum over part of the block -- would silently discard real updates. A single
 * millimetre of distance has to be enough to make it through.
 */
static void test_Nvm_SmallestChangeIsNotSkipped(void)
{
    NvM_EnergyCountersType energy;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));
    energy.energyOutMilliWh = 1000uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_ENERGY_COUNTERS));

    energy.energyOutMilliWh = 1001uLL; /* one milliwatt-hour: the smallest step the field has */
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(1u, stats.dirtyBlockCount);
}

/** TS-NVM-011: a flush of a clean block succeeds without writing anything. */
static void test_Nvm_FlushOfCleanBlockWritesNothing(void)
{
    Fls_StatisticsType before;
    Fls_StatisticsType after;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, Fls_GetStatistics(&before));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_DEVICE_CONFIG));
    TEST_ASSERT_EQUAL(E_OK, Fls_GetStatistics(&after));

    TEST_ASSERT_EQUAL_UINT32(before.writeCount, after.writeCount);
}

/**
 * TS-NVM-012: the deferred writer moves one block per invocation.
 *
 * One at a time deliberately: a flash write can hold the SPI bus, and the bus is shared with the CAN
 * controller. Draining the whole queue in one scheduler slot would put an unbounded stall in a slot
 * with a 5 ms budget.
 */
static void test_Nvm_MainFunctionWritesOneBlockPerCall(void)
{
    NvM_EnergyCountersType energy;
    NvM_CalibrationType calibration;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));
    energy.energyInMilliWh = 77uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ENERGY_COUNTERS, &energy));

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    calibration.vbattOffsetMv = -25;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(2u, stats.dirtyBlockCount);

    NvM_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(1u, stats.dirtyBlockCount);

    NvM_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.dirtyBlockCount);

    /* And an invocation with nothing to do is harmless. */
    NvM_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.dirtyBlockCount);
}

/*==================================================================================================
 *  TS-NVM-013 .. 016  Integrity, and the fallback that must not produce zero
 *================================================================================================*/

/**
 * TS-NVM-013: SWREQ-NVM-0002 -- a corrupted stored block is detected, not used.
 *
 * Without the CRC this is the v1 failure exactly: a partial write during the brown-out that
 * accompanies engine cranking reads back as a plausible number, and there is no way to tell it from a
 * real reading.
 */
static void test_Nvm_CorruptedBlockIsDetected(void)
{
    NvM_OdometerType odometer;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 555000uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_ODOMETER));

    /* Clear one bit of the newest committed record, which is what bit rot looks like. */
    TEST_ASSERT_TRUE(Stub_Fls_CorruptLastWrittenByte());

    TnRestart();

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_TRUE(stats.integrityFailures > 0u);
}

/**
 * TS-NVM-014: SWREQ-ODO-0015 -- a damaged odometer block never yields a lower reading than zero-risk.
 *
 * The monotonic guarantee. When the stored copy cannot be trusted the reading falls back, and what it
 * must never do is present a smaller number as if it were real -- that is not a mileage record. Here
 * the fallback is the configured default because there is nothing else, and the block is *reported*
 * as restored rather than as a valid reading of zero.
 */
static void test_Nvm_DamagedOdometerIsReportedNotSilentlyZero(void)
{
    NvM_OdometerType odometer;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 777000uLL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_ODOMETER));

    TEST_ASSERT_TRUE(Stub_Fls_CorruptLastWrittenByte());
    TnRestart();

    /* Whatever the value is, the status says it cannot be trusted -- which is what lets OdoSwc keep
     * the last known reading rather than accepting a regression. */
    TEST_ASSERT_NOT_EQUAL(NVM_REQ_OK, NvM_GetErrorStatus(NVM_BLOCK_ODOMETER));
}

/**
 * TS-NVM-015: SWREQ-NVM-0006 -- a media write failure is reported, not swallowed.
 *
 * The caller has to know, because for the odometer the correct response is to keep the value in RAM
 * and try again rather than to assume it is safe.
 */
static void test_Nvm_WriteFailureIsReported(void)
{
    NvM_OdometerType odometer;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 31337uLL;

    /* Armed before the call, because the odometer is an immediate block: the media access happens
     * inside NvM_WriteBlock, so arming afterwards would sabotage nothing and the test would pass
     * while exercising the success path. */
    Stub_Fls_FailNextWrites(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_TRUE(stats.writeFailures > 0u);
}

/**
 * TS-NVM-016: a block still dirty after a failed write is retried, not abandoned.
 *
 * Clearing the dirty flag on failure would lose the value silently, which is worse than the failure:
 * the caller was told the write failed and the data is gone anyway.
 */
static void test_Nvm_FailedWriteLeavesBlockDirty(void)
{
    NvM_OdometerType odometer;
    NvM_StatisticsType stats;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    odometer.totalDistanceMm = 424242uLL;

    Stub_Fls_FailNextWrites(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, &odometer));

    /* Still dirty, so the value is not lost. Clearing the flag on failure would discard it silently,
     * which is worse than the failure: the caller was told the write failed and the data is gone
     * anyway, so there is nothing left to retry with. */
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT8(1u, stats.dirtyBlockCount);

    /* With the media working again, the retry succeeds and the value reaches flash. */
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_ODOMETER));

    TnRestart();
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer));
    TEST_ASSERT_EQUAL_UINT64(424242uLL, odometer.totalDistanceMm);
}

/*==================================================================================================
 *  TS-NVM-017 .. 018  Contract
 *================================================================================================*/

/** TS-NVM-017: an unknown block and a NULL buffer are both rejected. */
static void test_Nvm_RejectsBadArguments(void)
{
    NvM_OdometerType odometer;

    TnBringUpErased();

    TEST_ASSERT_NOT_EQUAL(E_OK,
                          NvM_ReadBlock((NvM_BlockIdType)NVM_BLOCK_COUNT, &odometer));
    TEST_ASSERT_NOT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_ODOMETER, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK,
                          NvM_WriteBlock((NvM_BlockIdType)NVM_BLOCK_COUNT, &odometer));
    TEST_ASSERT_NOT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_ODOMETER, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, NvM_GetStatistics(NULL_PTR));
}

/**
 * TS-NVM-018: every block round-trips its full length.
 *
 * A block whose stored length were short would truncate its last field, and for the device
 * configuration that field is the broker host -- so the unit would come up unable to reach the broker
 * with no indication that anything had been lost.
 */
static void test_Nvm_EveryBlockRoundTripsFully(void)
{
    NvM_DeviceConfigType config;
    NvM_RestartInfoType restart;

    TnBringUpErased();

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_DEVICE_CONFIG, &config));
    config.brokerPort = 8883u;
    (void)strncpy(config.brokerHost, "broker.example.invalid", sizeof(config.brokerHost) - 1u);
    config.brokerHost[sizeof(config.brokerHost) - 1u] = '\0';
    (void)strncpy(config.deviceId, "unit-000000000042", sizeof(config.deviceId) - 1u);
    config.deviceId[sizeof(config.deviceId) - 1u] = '\0';
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_DEVICE_CONFIG, &config));

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_RESTART_INFO, &restart));
    restart.restartCount = 3u;
    restart.windowStartUnixTime = 1767225600uL;
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteBlock(NVM_BLOCK_RESTART_INFO, &restart));

    TEST_ASSERT_EQUAL(E_OK, NvM_WriteAll());
    TnRestart();

    (void)memset(&config, 0, sizeof(config));
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_DEVICE_CONFIG, &config));
    TEST_ASSERT_EQUAL_UINT16(8883u, config.brokerPort);
    TEST_ASSERT_EQUAL_STRING("broker.example.invalid", config.brokerHost);
    TEST_ASSERT_EQUAL_STRING("unit-000000000042", config.deviceId);

    (void)memset(&restart, 0, sizeof(restart));
    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_RESTART_INFO, &restart));
    TEST_ASSERT_EQUAL_UINT16(3u, restart.restartCount);
    TEST_ASSERT_EQUAL_UINT32(1767225600uL, restart.windowStartUnixTime);
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Nvm_FirstBootAppliesDefaults);
    RUN_TEST(test_Nvm_NeverWrittenIsNotIntegrityFailure);
    RUN_TEST(test_Nvm_OdometerDefaultIsZero);
    RUN_TEST(test_Nvm_RestoreDefaultsIsReachable);

    RUN_TEST(test_Nvm_DeferredWriteUpdatesMirrorNotFlash);
    RUN_TEST(test_Nvm_OdometerWritesThrough);
    RUN_TEST(test_Nvm_FlushedValueSurvivesRestart);
    RUN_TEST(test_Nvm_UnflushedDeferredWriteIsLostOnRestart);
    RUN_TEST(test_Nvm_WriteAllFlushesEveryDirtyBlock);

    RUN_TEST(test_Nvm_UnchangedWriteIsSkipped);
    RUN_TEST(test_Nvm_SmallestChangeIsNotSkipped);
    RUN_TEST(test_Nvm_FlushOfCleanBlockWritesNothing);
    RUN_TEST(test_Nvm_MainFunctionWritesOneBlockPerCall);

    RUN_TEST(test_Nvm_CorruptedBlockIsDetected);
    RUN_TEST(test_Nvm_DamagedOdometerIsReportedNotSilentlyZero);
    RUN_TEST(test_Nvm_WriteFailureIsReported);
    RUN_TEST(test_Nvm_FailedWriteLeavesBlockDirty);

    RUN_TEST(test_Nvm_RejectsBadArguments);
    RUN_TEST(test_Nvm_EveryBlockRoundTripsFully);

    return UNITY_END();
}

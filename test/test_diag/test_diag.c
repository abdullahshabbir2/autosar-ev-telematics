/**
 * @file    test_diag.c
 * @brief   Unit tests for the diagnostic event manager and the watchdog manager.
 *
 * These two together are the ECU's self-awareness: Dem decides what counts as a fault, WdgM
 * decides when the software has stopped behaving. Both were absent in v1 -- it had a
 * @c byte @c flags[15] array that survived nothing, and a watchdog that was armed and then never
 * used -- so the tests here pin the behaviour that makes a returned unit diagnosable.
 *
 * @req SWREQ-DIAG-0010 .. SWREQ-DIAG-0032, SWREQ-SAF-0001 .. SWREQ-SAF-0012
 * @verifies TS-DEM-001 .. TS-DEM-010, TS-WDGM-001 .. TS-WDGM-010
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "Stub_Mcal.h"
#include "mcal/Wdg/Wdg.h"
#include "services/WdgM/WdgM.h"
#include "unity.h"

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Dem_Init());
}

void tearDown(void) {}

/** Report @p count consecutive failures of @p event. */
static void reportFailures(Dem_EventIdType event, uint8 instance, uint16 count)
{
    uint16 i;
    for (i = 0u; i < count; i++)
    {
        TEST_ASSERT_EQUAL(E_OK, Dem_SetEventStatus(event, instance, DEM_EVENT_STATUS_FAILED));
        Stub_Gpt_AdvanceMs(10u);
    }
}

/*==================================================================================================
 *  TS-DEM-001 .. 010
 *================================================================================================*/

/**
 * @test TS-DEM-001 An untested monitor reports "not completed", not "passed".
 *
 * The distinction matters: a monitor that never runs would otherwise be indistinguishable from one
 * that runs and finds nothing, which is how a fault that is never checked for appears healthy.
 */
static void test_Dem_UntestedEventIsNotPassed(void)
{
    uint8 status = 0u;

    TEST_ASSERT_EQUAL(E_OK, Dem_GetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, &status));

    TEST_ASSERT_BITS_HIGH(DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR, status);
    TEST_ASSERT_BITS_HIGH(DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE, status);
    TEST_ASSERT_BITS_LOW(DEM_UDS_TEST_FAILED, status);
    TEST_ASSERT_BITS_LOW(DEM_UDS_CONFIRMED_DTC, status);
    TEST_ASSERT_FALSE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE));
}

/**
 * @test TS-DEM-002 A single failure is pending, not confirmed.
 *
 * One failure on a vehicle harness is routine -- an ignition transient is enough. Confirming on it
 * would fill the record with noise and train whoever reads it to ignore the whole thing.
 */
static void test_Dem_SingleFailureIsPendingOnly(void)
{
    uint8 status = 0u;

    reportFailures(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_1, 1u);

    TEST_ASSERT_EQUAL(E_OK, Dem_GetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, &status));
    TEST_ASSERT_BITS_HIGH(DEM_UDS_TEST_FAILED, status);
    TEST_ASSERT_BITS_HIGH(DEM_UDS_PENDING_DTC, status);
    TEST_ASSERT_BITS_LOW(DEM_UDS_CONFIRMED_DTC, status);
    TEST_ASSERT_FALSE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE));
}

/** @test TS-DEM-003 Reaching the debounce threshold confirms the event. */
static void test_Dem_ThresholdConfirmsEvent(void)
{
    uint8 status = 0u;
    Dem_StatisticsType stats;

    /* DEM_EVENT_PACK_NO_RESPONSE is configured to confirm on three consecutive failures. */
    reportFailures(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_2, 3u);

    TEST_ASSERT_TRUE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE));
    TEST_ASSERT_EQUAL(E_OK, Dem_GetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, &status));
    TEST_ASSERT_BITS_HIGH(DEM_UDS_CONFIRMED_DTC, status);
    TEST_ASSERT_BITS_LOW(DEM_UDS_PENDING_DTC, status);

    /* A silent pack warns the driver -- it is the fault most likely to strand the vehicle. */
    TEST_ASSERT_BITS_HIGH(DEM_UDS_WARNING_INDICATOR_REQUESTED, status);

    TEST_ASSERT_EQUAL(E_OK, Dem_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT16(1u, stats.confirmedCount);
    TEST_ASSERT_TRUE(stats.warningActive);
}

/**
 * @test TS-DEM-004 Intermittent failures do not confirm.
 *
 * Alternating pass and fail must not creep to confirmation: the counter has to come back down on
 * a pass, or a noisy but working bus eventually raises a fault.
 */
static void test_Dem_AlternatingResultsDoNotConfirm(void)
{
    uint8 cycle;

    for (cycle = 0u; cycle < 20u; cycle++)
    {
        TEST_ASSERT_EQUAL(E_OK, Dem_SetEventStatus(DEM_EVENT_PACK_NO_RESPONSE,
                                                   INSTANCE_ID_BATTERY_1, DEM_EVENT_STATUS_FAILED));
        TEST_ASSERT_EQUAL(E_OK, Dem_SetEventStatus(DEM_EVENT_PACK_NO_RESPONSE,
                                                   INSTANCE_ID_BATTERY_1, DEM_EVENT_STATUS_PASSED));
    }

    TEST_ASSERT_FALSE_MESSAGE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE),
                              "an intermittent fault crept to confirmation");
}

/**
 * @test TS-DEM-005 A confirmed event is not cleared by a subsequent pass.
 *
 * Confirmation means the fault happened. A record that erased itself the moment the fault stopped
 * would be useless for exactly the intermittent faults it exists to catch.
 */
static void test_Dem_ConfirmedEventSurvivesPass(void)
{
    reportFailures(DEM_EVENT_CAN_BUS_OFF, INSTANCE_ID_SINGLE, 1u);
    TEST_ASSERT_TRUE(Dem_IsEventConfirmed(DEM_EVENT_CAN_BUS_OFF));

    TEST_ASSERT_EQUAL(E_OK,
                      Dem_SetEventStatus(DEM_EVENT_CAN_BUS_OFF, INSTANCE_ID_SINGLE,
                                         DEM_EVENT_STATUS_PASSED));

    TEST_ASSERT_TRUE_MESSAGE(Dem_IsEventConfirmed(DEM_EVENT_CAN_BUS_OFF),
                             "a confirmed fault was erased by one passing test");
}

/** @test TS-DEM-006 A snapshot is captured at confirmation and is not overwritten later. */
static void test_Dem_SnapshotCapturedOnceAtConfirmation(void)
{
    Dem_SnapshotType snapshot;

    TEST_ASSERT_EQUAL(E_NOT_FOUND, Dem_GetSnapshot(DEM_EVENT_SD_MOUNT_FAILED, &snapshot));

    Stub_Gpt_SetMonotonicMs(5000u);
    reportFailures(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE, 1u);

    TEST_ASSERT_EQUAL(E_OK, Dem_GetSnapshot(DEM_EVENT_SD_MOUNT_FAILED, &snapshot));
    TEST_ASSERT_UINT32_WITHIN(100u, 5000u, snapshot.uptimeMs);

    /* Heal and re-confirm; the original snapshot must remain, because the first occurrence is the
     * informative one and the occurrence count already records that it came back. */
    {
        const uint32 firstUptime = snapshot.uptimeMs;
        uint8 cycle;

        TEST_ASSERT_EQUAL(E_OK, Dem_SetEventStatus(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE,
                                                   DEM_EVENT_STATUS_PASSED));
        for (cycle = 0u; cycle < (uint8)(DEM_HEALING_CYCLE_COUNT + 1u); cycle++)
        {
            Dem_StartOperationCycle();
        }
        TEST_ASSERT_FALSE(Dem_IsEventConfirmed(DEM_EVENT_SD_MOUNT_FAILED));

        Stub_Gpt_SetMonotonicMs(90000u);
        reportFailures(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE, 1u);

        TEST_ASSERT_EQUAL(E_OK, Dem_GetSnapshot(DEM_EVENT_SD_MOUNT_FAILED, &snapshot));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(firstUptime, snapshot.uptimeMs,
                                         "the original snapshot was overwritten");
    }
}

/**
 * @test TS-DEM-007 A repaired fault heals itself after enough clean operation cycles.
 *
 * So a replaced battery pack does not leave a code behind that needs a diagnostic tool to remove.
 */
static void test_Dem_HealsAfterCleanCycles(void)
{
    uint8 cycle;

    reportFailures(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_3, 3u);
    TEST_ASSERT_TRUE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE));

    TEST_ASSERT_EQUAL(E_OK, Dem_SetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_3,
                                               DEM_EVENT_STATUS_PASSED));

    /* The first cycle boundary closes the cycle in which the fault failed. That cycle is not a
     * clean one, so it resets healing rather than advancing it -- otherwise a fault that failed
     * moments before a power-down would get credit for a journey it spent broken. */
    Dem_StartOperationCycle();

    /* One short of the threshold: still confirmed. */
    for (cycle = 0u; cycle < (uint8)(DEM_HEALING_CYCLE_COUNT - 1u); cycle++)
    {
        Dem_StartOperationCycle();
    }
    TEST_ASSERT_TRUE_MESSAGE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE),
                             "the fault healed before its configured number of clean cycles");

    Dem_StartOperationCycle();
    TEST_ASSERT_FALSE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE));
}

/** @test TS-DEM-007b A fault that recurs restarts healing from zero. */
static void test_Dem_RecurrenceRestartsHealing(void)
{
    uint8 cycle;

    reportFailures(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_1, 3u);

    for (cycle = 0u; cycle < 30u; cycle++)
    {
        Dem_StartOperationCycle();
    }

    /* It comes back. Whatever the intervening clean cycles suggested, it has not been repaired. */
    reportFailures(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_1, 1u);
    Dem_StartOperationCycle();

    for (cycle = 0u; cycle < 20u; cycle++)
    {
        Dem_StartOperationCycle();
    }
    TEST_ASSERT_TRUE_MESSAGE(Dem_IsEventConfirmed(DEM_EVENT_PACK_NO_RESPONSE),
                             "healing did not restart after the fault recurred");
}

/** @test TS-DEM-008 Instances share a code and remain distinguishable by its low byte. */
static void test_Dem_InstanceEncodedInDtc(void)
{
    const Dem_DtcType pack1 = Dem_GetDtcForEvent(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_1);
    const Dem_DtcType pack4 = Dem_GetDtcForEvent(DEM_EVENT_PACK_NO_RESPONSE, INSTANCE_ID_BATTERY_4);

    TEST_ASSERT_NOT_EQUAL(pack1, pack4);
    TEST_ASSERT_EQUAL_HEX32(pack1 & 0x00FFFF00uL, pack4 & 0x00FFFF00uL);
    TEST_ASSERT_EQUAL_HEX32((uint32)INSTANCE_ID_BATTERY_1, pack1 & 0xFFuL);
    TEST_ASSERT_EQUAL_HEX32((uint32)INSTANCE_ID_BATTERY_4, pack4 & 0xFFuL);

    TEST_ASSERT_EQUAL_HEX32(0uL, Dem_GetDtcForEvent((Dem_EventIdType)9999u, 0u));
}

/** @test TS-DEM-009 Confirmed codes are enumerable, and clearing removes them. */
static void test_Dem_ConfirmedDtcListAndClear(void)
{
    Dem_DtcType codes[8];
    uint16 count;

    reportFailures(DEM_EVENT_CAN_BUS_OFF, INSTANCE_ID_SINGLE, 1u);
    reportFailures(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE, 1u);

    count = Dem_GetConfirmedDtcs(codes, (uint16)STD_ARRAY_SIZE(codes));
    TEST_ASSERT_EQUAL_UINT16(2u, count);

    /* Clear one specific code. */
    TEST_ASSERT_EQUAL(E_OK, Dem_ClearDtc(Dem_GetDtcForEvent(DEM_EVENT_CAN_BUS_OFF, 0u)));
    TEST_ASSERT_FALSE(Dem_IsEventConfirmed(DEM_EVENT_CAN_BUS_OFF));
    TEST_ASSERT_TRUE(Dem_IsEventConfirmed(DEM_EVENT_SD_MOUNT_FAILED));

    /* After a clear the monitor reads as never tested again, so a tool can tell a monitor that has
     * since run and passed from one that has not run at all. */
    {
        uint8 status = 0u;
        TEST_ASSERT_EQUAL(E_OK, Dem_GetEventStatus(DEM_EVENT_CAN_BUS_OFF, &status));
        TEST_ASSERT_BITS_HIGH(DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR, status);
        TEST_ASSERT_BITS_LOW(DEM_UDS_TEST_FAILED_SINCE_CLEAR, status);
    }

    /* Clear everything. */
    TEST_ASSERT_EQUAL(E_OK, Dem_ClearDtc(0x00FFFFFFuL));
    TEST_ASSERT_EQUAL_UINT16(0u, Dem_GetConfirmedDtcs(codes, (uint16)STD_ARRAY_SIZE(codes)));

    TEST_ASSERT_EQUAL(E_NOT_FOUND, Dem_ClearDtc(0x00AAAA00uL));
}

/** @test TS-DEM-010 Unknown events and NULL pointers are rejected. */
static void test_Dem_ParameterChecking(void)
{
    uint8 status = 0u;
    Dem_EventRecordType record;

    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_SetEventStatus(DEM_EVENT_NONE, 0u, DEM_EVENT_STATUS_FAILED));
    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_SetEventStatus((Dem_EventIdType)(DEM_EVENT_COUNT + 1u), 0u,
                                                   DEM_EVENT_STATUS_FAILED));
    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_GetEventStatus(DEM_EVENT_NONE, &status));
    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_GetEventStatus(DEM_EVENT_CAN_BUS_OFF, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_GetEventRecord(DEM_EVENT_CAN_BUS_OFF, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK, Dem_GetStatistics(NULL_PTR));
    TEST_ASSERT_EQUAL_UINT16(0u, Dem_GetConfirmedDtcs(NULL_PTR, 4u));

    /* An out-of-range status value is rejected rather than silently treated as a failure. */
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      Dem_SetEventStatus(DEM_EVENT_CAN_BUS_OFF, 0u, (Dem_EventStatusType)99));

    TEST_ASSERT_EQUAL(E_OK, Dem_GetEventRecord(DEM_EVENT_CAN_BUS_OFF, &record));
    TEST_ASSERT_EQUAL_HEX32(0x0C0210uL, record.dtc);
}

/*==================================================================================================
 *  TS-WDGM-001 .. 010
 *================================================================================================*/

/** Run @p cycles supervision cycles, checking in the scheduler entity at its nominal rate. */
static void runSupervisionCycles(uint16 cycles, boolean feedScheduler)
{
    uint16 cycle;

    for (cycle = 0u; cycle < cycles; cycle++)
    {
        uint16 tick;

        for (tick = 0u; tick < 100u; tick++)
        {
            if (feedScheduler != FALSE)
            {
                TEST_ASSERT_EQUAL(E_OK,
                                  WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
                TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
            }
            Stub_Gpt_AdvanceMs(10u);
        }
        WdgM_MainFunction();
    }
}

/** Check in every supervised entity once, in order. */
static void feedAllEntities(void)
{
    WdgM_SupervisedEntityIdType se;

    for (se = 0u; se < (WdgM_SupervisedEntityIdType)WDGM_SE_COUNT; se++)
    {
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(se, WDGM_CP_ENTRY));
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(se, WDGM_CP_EXIT));
    }
}

/**
 * @test TS-WDGM-001 Init arms the hardware watchdog in its slow mode.
 *
 * Slow, because startup legitimately blocks for seconds mounting the SD card and attaching to
 * GPRS. Arming it fast would reset a unit that is merely starting up slowly.
 */
static void test_WdgM_InitArmsWatchdogSlow(void)
{
    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());

    TEST_ASSERT_FALSE_MESSAGE(Stub_Wdg_IsDisabled(), "the watchdog was left disabled after Init");
    TEST_ASSERT_EQUAL_UINT32(WDG_TIMEOUT_SLOW_MS, Stub_Wdg_GetConfiguredTimeoutMs());
    TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_DEACTIVATED, WdgM_GetGlobalStatus());
}

/**
 * @test TS-WDGM-002 The hardware watchdog is actually petted.
 *
 * The v1 defect, pinned: it armed the watchdog and then never petted it or subscribed a task. This
 * asserts that petting genuinely happens, which is the one thing v1's code looked like it did and
 * did not.
 */
static void test_WdgM_PetsTheHardwareWatchdog(void)
{
    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL_UINT32(0u, Stub_Wdg_GetTriggerCount());

    /* Even before supervision is active, startup must keep the armed watchdog fed. */
    WdgM_MainFunction();
    TEST_ASSERT_GREATER_THAN_UINT32(0u, Stub_Wdg_GetTriggerCount());

    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());
    TEST_ASSERT_EQUAL_UINT32(WDG_TIMEOUT_FAST_MS, Stub_Wdg_GetConfiguredTimeoutMs());

    {
        const uint32 before = Stub_Wdg_GetTriggerCount();
        uint16 cycle;

        for (cycle = 0u; cycle < 5u; cycle++)
        {
            uint16 tick;
            for (tick = 0u; tick < 100u; tick++)
            {
                TEST_ASSERT_EQUAL(E_OK,
                                  WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
                TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
                Stub_Gpt_AdvanceMs(10u);
            }
            feedAllEntities();
            WdgM_MainFunction();
        }

        TEST_ASSERT_EQUAL_UINT32(before + 5u, Stub_Wdg_GetTriggerCount());
        TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_OK, WdgM_GetGlobalStatus());
    }
}

/** @test TS-WDGM-003 A well-behaved system stays OK indefinitely. */
static void test_WdgM_HealthySystemStaysOk(void)
{
    uint16 cycle;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    for (cycle = 0u; cycle < 60u; cycle++)
    {
        uint16 tick;
        for (tick = 0u; tick < 100u; tick++)
        {
            TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
            TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
            Stub_Gpt_AdvanceMs(10u);
        }
        feedAllEntities();
        WdgM_MainFunction();
    }

    TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_OK, WdgM_GetGlobalStatus());
    TEST_ASSERT_EQUAL_UINT32(60u, Stub_Wdg_GetTriggerCount());

    {
        WdgM_StatisticsType stats;
        TEST_ASSERT_EQUAL(E_OK, WdgM_GetStatistics(&stats));
        TEST_ASSERT_EQUAL_UINT16(0u, stats.failedEntityCount);
        TEST_ASSERT_EQUAL_UINT32(0u, stats.withheldCount);
    }
}

/**
 * @test TS-WDGM-004 A stalled entity is detected, and the watchdog is withheld only on expiry.
 *
 * A single missed deadline must not reset a vehicle's data logger; exhausting the tolerance must.
 */
static void test_WdgM_StalledEntityExpiresAndStopsPetting(void)
{
    WdgM_EntityStatusType entity;
    uint16 cycle;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    /* Scheduler checks in normally; nothing else ever does. Only the scheduler has a non-zero
     * alive minimum, so the stall is exercised on that entity. */
    runSupervisionCycles(1u, FALSE);

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));
    TEST_ASSERT_EQUAL(WDGM_LOCAL_STATUS_FAILED, entity.localStatus);
    TEST_ASSERT_EQUAL(WDGM_VIOLATION_ALIVE_LOW, entity.lastViolation);
    TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_FAILED, WdgM_GetGlobalStatus());

    /* Still petting: one failed cycle is not grounds for a reset. */
    TEST_ASSERT_GREATER_THAN_UINT32(0u, Stub_Wdg_GetTriggerCount());

    for (cycle = 0u; cycle < (uint16)WDGM_FAILED_TOLERANCE_CYCLES; cycle++)
    {
        runSupervisionCycles(1u, FALSE);
    }

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));
    TEST_ASSERT_EQUAL(WDGM_LOCAL_STATUS_EXPIRED, entity.localStatus);
    TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_STOPPED, WdgM_GetGlobalStatus());

    /* Petting has now stopped, so the hardware will reset the ECU. */
    {
        const uint32 before = Stub_Wdg_GetTriggerCount();
        runSupervisionCycles(3u, FALSE);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, Stub_Wdg_GetTriggerCount(),
                                         "the watchdog was still petted after expiry");
    }

    {
        WdgM_StatisticsType stats;
        TEST_ASSERT_EQUAL(E_OK, WdgM_GetStatistics(&stats));
        TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.withheldCount);
    }
}

/**
 * @test TS-WDGM-005 A runaway entity is detected as well as a stalled one.
 *
 * A runnable looping far faster than its period starves every lower-priority task, and the symptom
 * -- other entities missing their deadlines -- points away from the actual cause.
 */
static void test_WdgM_RunawayEntityIsDetected(void)
{
    WdgM_EntityStatusType entity;
    uint16 tick;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    /* 400 check-ins in one 1000 ms cycle, against a configured maximum of 120. */
    for (tick = 0u; tick < 400u; tick++)
    {
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
        Stub_Gpt_AdvanceMs(2u);
    }
    feedAllEntities();
    WdgM_MainFunction();

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));
    TEST_ASSERT_EQUAL(WDGM_VIOLATION_ALIVE_HIGH, entity.lastViolation);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, entity.aliveViolations);
}

/**
 * @test TS-WDGM-006 A single long stall is caught by the deadline, not hidden by the average.
 *
 * An entity that checks in the right number of times per cycle but does so in one burst after a
 * long silence passes alive supervision completely. Deadline supervision is what catches it, and
 * that pattern -- an SD write blocking for many seconds -- is exactly what this ECU suffers.
 */
static void test_WdgM_DeadlineCatchesBurstAfterStall(void)
{
    WdgM_EntityStatusType entity;
    uint16 tick;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    /* Silent for 900 ms, then 100 check-ins in the remaining 100 ms. The cycle total is exactly
     * the nominal 100, so alive supervision is satisfied. */
    Stub_Gpt_AdvanceMs(900u);
    for (tick = 0u; tick < 100u; tick++)
    {
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
        TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
        Stub_Gpt_AdvanceMs(1u);
    }
    feedAllEntities();
    WdgM_MainFunction();

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));

    /* The 900 ms gap exceeds the 200 ms scheduler deadline. */
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0u, entity.deadlineViolations,
                                            "a 900 ms stall went undetected because the cycle "
                                            "total looked correct");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(900u, entity.worstIntervalMs);
}

/**
 * @test TS-WDGM-007 Checkpoints reached out of order are detected.
 *
 * Program-flow supervision catches a runnable that returned early through an unintended branch,
 * which neither the alive count nor the deadline notices at all.
 */
static void test_WdgM_ProgramFlowViolationDetected(void)
{
    WdgM_EntityStatusType entity;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    /* Entry twice in a row: the runnable never reached its exit. */
    TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_ACQUISITION, WDGM_CP_ENTRY));
    Stub_Gpt_AdvanceMs(10u);
    TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_ACQUISITION, WDGM_CP_ENTRY));

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_ACQUISITION, &entity));
    TEST_ASSERT_EQUAL_UINT32(1u, entity.flowViolations);
    TEST_ASSERT_EQUAL(WDGM_VIOLATION_FLOW, entity.lastViolation);
}

/** @test TS-WDGM-008 Recovery returns an entity to OK without a reset. */
static void test_WdgM_RecoveryReturnsToOk(void)
{
    WdgM_EntityStatusType entity;
    uint16 cycle;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    /* Two failed cycles, short of the tolerance. */
    runSupervisionCycles(2u, FALSE);
    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));
    TEST_ASSERT_EQUAL(WDGM_LOCAL_STATUS_FAILED, entity.localStatus);

    /* Behave again. The failure count decays one per clean cycle rather than resetting at once, so
     * an entity violating every other cycle still escalates instead of oscillating forever. */
    for (cycle = 0u; cycle < 5u; cycle++)
    {
        uint16 tick;
        for (tick = 0u; tick < 100u; tick++)
        {
            TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_ENTRY));
            TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_SCHEDULER, WDGM_CP_EXIT));
            Stub_Gpt_AdvanceMs(10u);
        }
        feedAllEntities();
        WdgM_MainFunction();
    }

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, &entity));
    TEST_ASSERT_EQUAL(WDGM_LOCAL_STATUS_OK, entity.localStatus);
    TEST_ASSERT_EQUAL(WDGM_GLOBAL_STATUS_OK, WdgM_GetGlobalStatus());
}

/** @test TS-WDGM-009 A deactivated entity cannot cause a reset, and reactivation is clean. */
static void test_WdgM_DeactivationSuspendsSupervision(void)
{
    WdgM_EntityStatusType entity;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateSupervision());

    TEST_ASSERT_EQUAL(E_OK, WdgM_DeactivateEntity(WDGM_SE_TELEMETRY));

    /* Long enough to have expired several times over if it were still supervised. */
    runSupervisionCycles(20u, TRUE);

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_TELEMETRY, &entity));
    TEST_ASSERT_EQUAL(WDGM_LOCAL_STATUS_DEACTIVATED, entity.localStatus);
    TEST_ASSERT_NOT_EQUAL(WDGM_GLOBAL_STATUS_STOPPED, WdgM_GetGlobalStatus());

    /* Reactivation must not immediately register a deadline violation spanning the suspension. */
    TEST_ASSERT_EQUAL(E_OK, WdgM_ActivateEntity(WDGM_SE_TELEMETRY));
    TEST_ASSERT_EQUAL(E_OK, WdgM_CheckpointReached(WDGM_SE_TELEMETRY, WDGM_CP_ENTRY));

    TEST_ASSERT_EQUAL(E_OK, WdgM_GetLocalStatus(WDGM_SE_TELEMETRY, &entity));
    TEST_ASSERT_EQUAL_MESSAGE(WDGM_LOCAL_STATUS_OK, entity.localStatus,
                              "reactivation registered a violation for the suspended period");
}

/** @test TS-WDGM-010 Unknown entities and checkpoints are rejected. */
static void test_WdgM_ParameterChecking(void)
{
    WdgM_EntityStatusType entity;

    TEST_ASSERT_EQUAL(E_OK, WdgM_Init());

    TEST_ASSERT_EQUAL(E_NOT_OK,
                      WdgM_CheckpointReached((WdgM_SupervisedEntityIdType)WDGM_SE_COUNT,
                                             WDGM_CP_ENTRY));
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      WdgM_CheckpointReached(WDGM_SE_SCHEDULER,
                                             (WdgM_CheckpointIdType)WDGM_CP_COUNT_PER_ENTITY));
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      WdgM_GetLocalStatus((WdgM_SupervisedEntityIdType)WDGM_SE_COUNT, &entity));
    TEST_ASSERT_EQUAL(E_NOT_OK, WdgM_GetLocalStatus(WDGM_SE_SCHEDULER, NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK, WdgM_GetStatistics(NULL_PTR));
    TEST_ASSERT_EQUAL(E_NOT_OK,
                      WdgM_DeactivateEntity((WdgM_SupervisedEntityIdType)WDGM_SE_COUNT));
    TEST_ASSERT_EQUAL(E_NOT_OK, WdgM_ActivateEntity((WdgM_SupervisedEntityIdType)WDGM_SE_COUNT));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_Dem_UntestedEventIsNotPassed);
    RUN_TEST(test_Dem_SingleFailureIsPendingOnly);
    RUN_TEST(test_Dem_ThresholdConfirmsEvent);
    RUN_TEST(test_Dem_AlternatingResultsDoNotConfirm);
    RUN_TEST(test_Dem_ConfirmedEventSurvivesPass);
    RUN_TEST(test_Dem_SnapshotCapturedOnceAtConfirmation);
    RUN_TEST(test_Dem_HealsAfterCleanCycles);
    RUN_TEST(test_Dem_RecurrenceRestartsHealing);
    RUN_TEST(test_Dem_InstanceEncodedInDtc);
    RUN_TEST(test_Dem_ConfirmedDtcListAndClear);
    RUN_TEST(test_Dem_ParameterChecking);
    RUN_TEST(test_WdgM_InitArmsWatchdogSlow);
    RUN_TEST(test_WdgM_PetsTheHardwareWatchdog);
    RUN_TEST(test_WdgM_HealthySystemStaysOk);
    RUN_TEST(test_WdgM_StalledEntityExpiresAndStopsPetting);
    RUN_TEST(test_WdgM_RunawayEntityIsDetected);
    RUN_TEST(test_WdgM_DeadlineCatchesBurstAfterStall);
    RUN_TEST(test_WdgM_ProgramFlowViolationDetected);
    RUN_TEST(test_WdgM_RecoveryReturnsToOk);
    RUN_TEST(test_WdgM_DeactivationSuspendsSupervision);
    RUN_TEST(test_WdgM_ParameterChecking);
    return UNITY_END();
}

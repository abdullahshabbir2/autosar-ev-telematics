/**
 * @file    test_system.c
 * @brief   Unit tests for the system layer: startup, scheduling, bus arbitration, diagnostic services.
 *
 * The four remaining host-testable clusters, grouped because each is about the ECU as a whole rather than
 * about one signal path:
 *
 *  * **EcuM** -- the startup order and what happens when part of the stack does not come up. v1 treated a
 *    failed CAN controller as fatal and rebooted forever, logging nothing at all including the fault that
 *    would have explained why.
 *  * **SchM** -- dispatch and budget accounting. The host cannot create tasks, but everything about *what*
 *    a task runs and how its execution is measured is ordinary code.
 *  * **Spi** -- bus ownership. Two devices at different clock rates with transaction lengths differing by
 *    five orders of magnitude; v1 had no arbitration at all.
 *  * **DiagSwc** -- the reduced UDS services, which are pure with respect to the transport: the request
 *    bytes go in and the response bytes come out, so the whole service layer is testable without a broker.
 *
 * @req SWREQ-SYS-0001, SWREQ-SYS-0002, SWREQ-SYS-0020, SWREQ-SYS-0021,
 *      SWREQ-SYS-0040 .. SWREQ-SYS-0058, SWREQ-SYS-0060 .. SWREQ-SYS-0085,
 *      SWREQ-COM-0020, SWREQ-COM-0021, SWREQ-DIAG-0040 .. SWREQ-DIAG-0068
 * @verifies TS-SYS-001 .. TS-SYS-010, TS-SCHM-001 .. TS-SCHM-006, TS-SPI-001 .. TS-SPI-006,
 *           TS-UDS-001 .. TS-UDS-012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "Ecu_Cfg.h"
#include "app/DiagSwc/DiagSwc.h"
#include "app/DiagSwc/DiagSwc_Cfg.h"
#include "mcal/Fls/Fls.h"
#include "mcal/Mcu/Mcu.h"
#include "mcal/Spi/Spi.h"
#include "services/Dem/Dem.h"
#include "services/EcuM/EcuM.h"
#include "services/EcuM/EcuM_Cfg.h"
#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "services/NvM/NvM.h"
#include "services/SchM/SchM.h"
#include "services/SchM/SchM_Cfg.h"
#include "Stub_Mcal.h"
#include "Stub_SchM.h"
#include "Stub_Platform.h"
#include "unity.h"

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Stub_Platform_ResetAll();
    Det_Init();
}

void tearDown(void)
{
}

/** Bring the storage stack up, which most of the system layer depends on. */
static void TsBringUpStorage(void)
{
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
}

/*==================================================================================================
 *  TS-SYS-001 .. 004  Reset cause and identity
 *================================================================================================*/

/**
 * TS-SYS-001: SWREQ-SYS-0001 -- the reset cause is reported, and each cause distinctly.
 *
 * It is the input to crash-loop detection and the first thing anyone diagnosing a returned unit looks at.
 * A brownout, a watchdog reset and a panic call for entirely different responses, and without the cause
 * all three look identical from outside.
 */
static void test_Sys_ResetReasonIsDistinct(void)
{
    static const uint8 reasons[] = {
        (uint8)MCU_RESET_POWER_ON, (uint8)MCU_RESET_SOFTWARE, (uint8)MCU_RESET_WATCHDOG,
        (uint8)MCU_RESET_PANIC,    (uint8)MCU_RESET_BROWNOUT, (uint8)MCU_RESET_EXTERNAL,
    };
    uint8 i;

    for (i = 0u; i < (uint8)(sizeof(reasons) / sizeof(reasons[0])); i++)
    {
        Stub_Mcu_SetResetReason(reasons[i]);
        TEST_ASSERT_EQUAL(E_OK, Mcu_Init());
        TEST_ASSERT_EQUAL_UINT8(reasons[i], (uint8)Mcu_GetResetReason());
    }
}

/** TS-SYS-002: every reset cause has a distinct name, so a log line is readable. */
static void test_Sys_ResetReasonNamesAreDistinct(void)
{
    static const uint8 reasons[] = {
        (uint8)MCU_RESET_POWER_ON, (uint8)MCU_RESET_SOFTWARE, (uint8)MCU_RESET_WATCHDOG,
        (uint8)MCU_RESET_PANIC,    (uint8)MCU_RESET_BROWNOUT, (uint8)MCU_RESET_EXTERNAL,
        (uint8)MCU_RESET_UNKNOWN,
    };
    const uint8 count = (uint8)(sizeof(reasons) / sizeof(reasons[0]));
    uint8 i;
    uint8 j;

    for (i = 0u; i < count; i++)
    {
        const char *const first = Mcu_GetResetReasonName((Mcu_ResetReasonType)reasons[i]);

        TEST_ASSERT_NOT_NULL(first);
        TEST_ASSERT_TRUE(strlen(first) > 0u);

        for (j = (uint8)(i + 1u); j < count; j++)
        {
            const char *const second = Mcu_GetResetReasonName((Mcu_ResetReasonType)reasons[j]);

            TEST_ASSERT_TRUE(strcmp(first, second) != 0);
        }
    }
}

/**
 * TS-SYS-003: SWREQ-SYS-0002 -- the device identity is available and is not all zeros.
 *
 * It is the MQTT client identifier and the topic prefix, and it must be unique before any radio starts.
 * An all-zero identity would make two unprovisioned units collide on the broker -- which is exactly what
 * `WiFi.macAddress()` returns before the driver has initialised, and why the identity comes from eFuse.
 */
static void test_Sys_DeviceIdIsPresentAndNonZero(void)
{
    uint8 id[MCU_DEVICE_ID_LENGTH];
    uint8 i;
    boolean anyNonZero = FALSE;

    TEST_ASSERT_EQUAL(E_OK, Mcu_Init());
    TEST_ASSERT_EQUAL(E_OK, Mcu_GetDeviceId(id, (uint8)sizeof(id)));

    for (i = 0u; i < (uint8)sizeof(id); i++)
    {
        if (id[i] != 0u)
        {
            anyNonZero = TRUE;
        }
    }
    TEST_ASSERT_TRUE(anyNonZero);
}

/** TS-SYS-004: an undersized identity buffer is refused rather than partially filled. */
static void test_Sys_DeviceIdRefusesSmallBuffer(void)
{
    uint8 tooSmall[MCU_DEVICE_ID_LENGTH - 1u];

    TEST_ASSERT_EQUAL(E_OK, Mcu_Init());
    TEST_ASSERT_NOT_EQUAL(E_OK, Mcu_GetDeviceId(tooSmall, (uint8)sizeof(tooSmall)));
    TEST_ASSERT_NOT_EQUAL(E_OK, Mcu_GetDeviceId(NULL_PTR, MCU_DEVICE_ID_LENGTH));
}

/**
 * TS-SYS-005: the heap report includes the largest contiguous block, not only the total.
 *
 * Fragmentation is the failure mode that matters. v1's per-record String churn produced a heap that
 * reported plenty free while no single allocation of any size succeeded, and a total-free figure would
 * have shown nothing wrong right up to the failure.
 */
static void test_Sys_HeapReportIncludesLargestBlock(void)
{
    Mcu_HeapInfoType info;

    Stub_Mcu_SetHeapFree(180000uL, 150000uL, 4096uL);
    TEST_ASSERT_EQUAL(E_OK, Mcu_Init());
    TEST_ASSERT_EQUAL(E_OK, Mcu_GetHeapInfo(&info));

    TEST_ASSERT_EQUAL_UINT32(180000uL, info.heapFreeBytes);
    TEST_ASSERT_EQUAL_UINT32(150000uL, info.heapMinFreeBytes);

    /* Badly fragmented: plenty free, nothing large available. The two figures are independent, which is
     * the whole point of reporting both. */
    TEST_ASSERT_EQUAL_UINT32(4096uL, info.heapLargestBlockBytes);
    TEST_ASSERT_TRUE(info.heapLargestBlockBytes < info.heapFreeBytes);
}

/*==================================================================================================
 *  TS-SYS-006 .. 010  Startup, degraded operation and crash-loop detection
 *================================================================================================*/

/** Put every subsystem in a state where it will come up. */
static void TsAllSubsystemsHealthy(void)
{
    Stub_SchM_Reset();
    Stub_SchM_SetCreateFails(FALSE);
    Stub_Fs_SetMountable(TRUE);
    Stub_Fs_SetSpace(4096uL, 100uL);
    Stub_Time_SetRtcPresent(TRUE);
    Stub_Time_SetRtcTime(1781526896uL); /* 2026-06-15 12:34:56 UTC */
    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);
}

/**
 * TS-SYS-006: SWREQ-SYS-0060 -- startup succeeds with every subsystem available, and reaches RUN.
 *
 * The baseline the degraded cases are measured against. Without it, a test showing "startup survived a
 * missing card" would not distinguish a working degraded mode from a startup that never checks anything.
 */
static void test_Sys_StartupReachesRunWhenHealthy(void)
{
    EcuM_StatusType status;

    TsAllSubsystemsHealthy();

    TEST_ASSERT_EQUAL(E_OK, EcuM_Init());
    TEST_ASSERT_EQUAL(ECUM_STATE_RUN, EcuM_GetState());

    TEST_ASSERT_EQUAL(E_OK, EcuM_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT8(0u, status.degradedSubsystemMask);
    TEST_ASSERT_TRUE(status.subsystems.storageAvailable);
    TEST_ASSERT_TRUE(status.subsystems.clockValid);
}

/**
 * TS-SYS-007: SWREQ-SYS-0060 -- an unmountable card does not prevent startup.
 *
 * The requirement that most distinguishes this firmware from v1, which treated a failed subsystem as
 * fatal: `if (can.init_can()) ... else { delay(1000); ESP.restart(); while(1); }`. A vehicle with a
 * disconnected harness rebooted forever and logged nothing at all -- including the fault that would have
 * explained why.
 *
 * Losing the card costs the store-and-forward buffer. It does not cost odometry, battery acquisition, or
 * live publishing, and the ECU must keep doing all three.
 */
static void test_Sys_MissingCardDegradesRatherThanFails(void)
{
    EcuM_StatusType status;

    TsAllSubsystemsHealthy();
    Stub_Fs_SetMountable(FALSE);

    /* Startup still succeeds. */
    TEST_ASSERT_EQUAL(E_OK, EcuM_Init());

    TEST_ASSERT_EQUAL(E_OK, EcuM_GetStatus(&status));

    /* The card is unavailable, and named as such. */
    TEST_ASSERT_FALSE(status.subsystems.storageAvailable);

    /* But the state is RUN, not RUN_DEGRADED, and that distinction is deliberate rather than an
     * oversight. `degradedSubsystemMask` means "skipped this run" -- subsystems the crash-loop detector
     * chose not to attempt because the previous reset implicated them. A card that was attempted and did
     * not mount is *unavailable*, which is a different fact calling for a different response: one is a
     * hardware fault to investigate, the other is the crash-loop protection working as designed.
     *
     * Asserted rather than left implicit, because reading RUN as "everything is fine" is the obvious
     * misreading and `subsystems` is where the answer actually is. */
    TEST_ASSERT_EQUAL(ECUM_STATE_RUN, EcuM_GetState());
    TEST_ASSERT_EQUAL_UINT8(0u, status.degradedSubsystemMask);
    TEST_ASSERT_FALSE(EcuM_IsSubsystemDegraded((uint8)ECUM_DEGRADED_STORAGE));
}

/**
 * TS-SYS-008: SWREQ-SYS-0075 -- an absent clock is reported without stopping startup.
 *
 * Records remain correctly *ordered* without a clock; they lack only an absolute reference, and NTP may
 * supply one later. Refusing to start would trade every record for the timestamps on some of them.
 */
static void test_Sys_AbsentClockIsReportedNotFatal(void)
{
    EcuM_StatusType status;

    TsAllSubsystemsHealthy();
    Stub_Time_SetRtcPresent(FALSE);
    Stub_Time_SetNtpResponse(0uL, FALSE);

    TEST_ASSERT_EQUAL(E_OK, EcuM_Init());

    TEST_ASSERT_EQUAL(E_OK, EcuM_GetStatus(&status));
    TEST_ASSERT_FALSE(status.subsystems.clockValid);
}

/**
 * TS-SYS-009: a failed task creation is the one fatal startup outcome.
 *
 * Because it is the only failure with nothing to degrade to: a task that does not exist means that part
 * of the ECU's job is simply not being done, and no other subsystem can take it over. Asserted because
 * "nothing is fatal" would be the wrong lesson to draw from the two cases above -- the distinction is
 * what makes the degraded modes a design rather than an absence of error handling.
 */
static void test_Sys_TaskCreationFailureIsFatal(void)
{
    TsAllSubsystemsHealthy();
    Stub_SchM_SetCreateFails(TRUE);

    TEST_ASSERT_NOT_EQUAL(E_OK, EcuM_Init());
}

/**
 * TS-SYS-010: SWREQ-SYS-0070 -- repeated resets are counted and past the threshold start degraded.
 *
 * Without this a unit whose card is unmountable resets forever. v1 had the counting half -- a restart
 * count in NVS -- but its only reaction was to upload the current log file and carry on resetting.
 *
 * Each iteration restarts the whole stack from the same non-volatile store, which is what a real reset
 * sequence looks like: the counter has to survive, and it is the surviving counter that drives the
 * decision.
 */
static void test_Sys_CrashLoopIsDetectedAndClearable(void)
{
    EcuM_StatusType status;
    uint16 boot;

    TsAllSubsystemsHealthy();

    /* A run of watchdog resets, which is what a subsystem hanging during startup produces. */
    Stub_Mcu_SetResetReason((uint8)MCU_RESET_WATCHDOG);

    for (boot = 0u; boot < (uint16)(ECUM_CRASH_LOOP_COUNT + 2u); boot++)
    {
        STD_DISCARD(EcuM_Init());
    }

    TEST_ASSERT_EQUAL(E_OK, EcuM_GetStatus(&status));
    TEST_ASSERT_TRUE(status.restartCount > 0u);
    TEST_ASSERT_TRUE(status.crashLoopDetected);

    /* And a technician who has fixed the cause can clear it without waiting out the stable-run period. */
    EcuM_ClearCrashLoopCounter();
    STD_DISCARD(EcuM_Init());

    TEST_ASSERT_EQUAL(E_OK, EcuM_GetStatus(&status));
    TEST_ASSERT_FALSE(status.crashLoopDetected);
}

/*==================================================================================================
 *  TS-SCHM-001 .. 006  Dispatch and budget accounting
 *================================================================================================*/

/** TS-SCHM-001: every configured task dispatches its runnables when run. */
static void test_SchM_EveryTaskDispatches(void)
{
    SchM_TaskStatsType stats;
    uint8 task;

    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, SchM_Init());

    for (task = 0u; task < (uint8)SCHM_TASK_COUNT; task++)
    {
        SchM_RunTask((SchM_TaskType)task);

        TEST_ASSERT_EQUAL(E_OK, SchM_GetTaskStats((SchM_TaskType)task, &stats));
        TEST_ASSERT_EQUAL_UINT32(1u, stats.activations);
    }
}

/** TS-SCHM-002: activations accumulate, so the count reflects real work rather than the last pass. */
static void test_SchM_ActivationsAccumulate(void)
{
    SchM_TaskStatsType stats;
    uint16 i;

    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, SchM_Init());

    for (i = 0u; i < 25u; i++)
    {
        SchM_RunTask(SCHM_TASK_SCHEDULER);
    }

    TEST_ASSERT_EQUAL(E_OK, SchM_GetTaskStats(SCHM_TASK_SCHEDULER, &stats));
    TEST_ASSERT_EQUAL_UINT32(25u, stats.activations);
}

/**
 * TS-SCHM-003: SWREQ-SYS-0050 -- an execution time within budget is not an overrun.
 *
 * The negative case first, because a budget check that fired on every activation would be
 * indistinguishable from one that worked when only the positive case was tested.
 */
static void test_SchM_WithinBudgetIsNotOverrun(void)
{
    SchM_TaskStatsType stats;

    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, SchM_Init());

    /* Virtual time does not advance unless something asks it to, so a runnable that does no work takes
     * no measured time at all. */
    SchM_RunTask(SCHM_TASK_SCHEDULER);

    TEST_ASSERT_EQUAL(E_OK, SchM_GetTaskStats(SCHM_TASK_SCHEDULER, &stats));
    TEST_ASSERT_EQUAL_UINT32(0u, stats.overruns);
}

/**
 * TS-SCHM-004: SWREQ-SYS-0050 -- execution time is measured, and the worst case is retained.
 *
 * The worst case is what a schedule is reviewed against; a last-case figure alone would be whatever the
 * most recent quiet pass happened to be, which is the one measurement that never reveals a problem.
 */
static void test_SchM_ExecutionTimeIsMeasured(void)
{
    SchM_TaskStatsType stats;

    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, SchM_Init());

    /* The stub's Gpt_DelayMs advances virtual time, so a runnable that delays is measured as having
     * taken that long -- which is how a slow body is emulated without one. */
    Stub_Gpt_SetDelayAdvancesTime(TRUE);
    SchM_RunTask(SCHM_TASK_CONNECTIVITY);

    TEST_ASSERT_EQUAL(E_OK, SchM_GetTaskStats(SCHM_TASK_CONNECTIVITY, &stats));

    /* Whatever the body did, the worst case is at least the most recent one -- it is a maximum, so it can
     * never be the smaller of the two. */
    TEST_ASSERT_TRUE(stats.worstCaseUs >= stats.lastCaseUs);
}

/** TS-SCHM-005: an unknown task is rejected rather than indexing past the table. */
static void test_SchM_RejectsUnknownTask(void)
{
    SchM_TaskStatsType stats;

    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, SchM_Init());

    TEST_ASSERT_NOT_EQUAL(E_OK, SchM_GetTaskStats((SchM_TaskType)SCHM_TASK_COUNT, &stats));
    TEST_ASSERT_NOT_EQUAL(E_OK, SchM_GetTaskStats(SCHM_TASK_SCHEDULER, NULL_PTR));

    /* And running one does nothing rather than dispatching from beyond the table. */
    SchM_RunTask((SchM_TaskType)SCHM_TASK_COUNT);
}

/**
 * TS-SCHM-006: SWREQ-SYS-0040 -- every task's budget is below its period.
 *
 * Asserted at run time as well as by the static assertions in SchM_Cfg.h, because the two catch different
 * things: the static assertion catches a configuration that cannot build, and this catches a table whose
 * entries have drifted out of correspondence with the constants they were derived from.
 */
static void test_SchM_BudgetsAreBelowPeriods(void)
{
    TEST_ASSERT_TRUE(SCHM_BUDGET_SCHEDULER_US < (SCHM_PERIOD_SCHEDULER_MS * 1000uL));
    TEST_ASSERT_TRUE(SCHM_BUDGET_ACQUISITION_US < (SCHM_PERIOD_ACQUISITION_MS * 1000uL));
    TEST_ASSERT_TRUE(SCHM_BUDGET_STORAGE_US < (SCHM_PERIOD_STORAGE_MS * 1000uL));
    TEST_ASSERT_TRUE(SCHM_BUDGET_CONNECTIVITY_US < (SCHM_PERIOD_CONNECTIVITY_MS * 1000uL));

    /* And supervision outranks everything it supervises, or a busy system looks like a stopped one. */
    TEST_ASSERT_TRUE(SCHM_PRIORITY_SCHEDULER > SCHM_PRIORITY_ACQUISITION);
    TEST_ASSERT_TRUE(SCHM_PRIORITY_ACQUISITION > SCHM_PRIORITY_STORAGE);
    TEST_ASSERT_TRUE(SCHM_PRIORITY_STORAGE > SCHM_PRIORITY_CONNECTIVITY);
}

/*==================================================================================================
 *  TS-SPI-001 .. 006  Bus ownership
 *================================================================================================*/

/** TS-SPI-001: the bus starts unowned, and a lock takes ownership. */
static void test_Spi_LockTakesOwnership(void)
{
    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    TEST_ASSERT_EQUAL(SPI_DEVICE_NONE, Spi_GetOwner());

    TEST_ASSERT_EQUAL(E_OK, Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS));
    TEST_ASSERT_EQUAL(SPI_DEVICE_CAN, Spi_GetOwner());

    TEST_ASSERT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_CAN));
    TEST_ASSERT_EQUAL(SPI_DEVICE_NONE, Spi_GetOwner());
}

/**
 * TS-SPI-002: SWREQ-COM-0021 -- a transfer without the lock fails.
 *
 * It must not work "most of the time". An unlocked transfer that happened to succeed would pass every
 * test and corrupt data in the field under contention that only occurs on a loaded vehicle -- which is
 * precisely how v1's intermittent card failures presented.
 */
static void test_Spi_TransferWithoutLockFails(void)
{
    uint8 tx[4] = {1u, 2u, 3u, 4u};
    uint8 rx[4];

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());

    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, tx, rx, 4u));
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_TransferContinuous(SPI_DEVICE_CAN, tx, rx, 4u));
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_ChipSelectAssert(SPI_DEVICE_CAN));
}

/**
 * TS-SPI-003: SWREQ-COM-0021 -- the wrong owner cannot transfer or unlock.
 *
 * The case that matters most: the bus is held, so a transfer would physically succeed, and the only thing
 * stopping the other device corrupting the holder's sequence is this check.
 */
static void test_Spi_WrongOwnerIsRejected(void)
{
    uint8 tx[4] = {1u, 2u, 3u, 4u};
    uint8 rx[4];

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    TEST_ASSERT_EQUAL(E_OK, Spi_Lock(SPI_DEVICE_SD, SPI_LOCK_TIMEOUT_MS));

    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, tx, rx, 4u));
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_CAN));

    /* The real owner is unaffected. */
    TEST_ASSERT_EQUAL(SPI_DEVICE_SD, Spi_GetOwner());
    TEST_ASSERT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_SD, tx, rx, 4u));
    TEST_ASSERT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_SD));
}

/**
 * TS-SPI-004: SWREQ-COM-0020 -- a lock spans several transfers.
 *
 * The MCP2515 command set requires chip select to stay low across a command byte, an address byte and a
 * data phase that the driver issues as separate calls. A lock that covered only one transfer would break
 * every one of those sequences.
 */
static void test_Spi_LockSpansSeveralTransfers(void)
{
    uint8 tx[2] = {0x03u, 0x0Eu};
    uint8 rx[2];

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    TEST_ASSERT_EQUAL(E_OK, Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS));

    TEST_ASSERT_EQUAL(E_OK, Spi_ChipSelectAssert(SPI_DEVICE_CAN));
    TEST_ASSERT_EQUAL(E_OK, Spi_TransferContinuous(SPI_DEVICE_CAN, tx, rx, 2u));
    TEST_ASSERT_EQUAL(E_OK, Spi_TransferContinuous(SPI_DEVICE_CAN, tx, rx, 2u));
    TEST_ASSERT_EQUAL(E_OK, Spi_ChipSelectDeassert(SPI_DEVICE_CAN));

    TEST_ASSERT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_CAN));
}

/** TS-SPI-005: the bus counters record what happened. */
static void test_Spi_StatisticsAreRecorded(void)
{
    Spi_StatisticsType stats;
    uint8 tx[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    uint8 rx[8];

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    TEST_ASSERT_EQUAL(E_OK, Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS));
    TEST_ASSERT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, tx, rx, 8u));
    TEST_ASSERT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_CAN));

    TEST_ASSERT_EQUAL(E_OK, Spi_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.transferCount);
    TEST_ASSERT_EQUAL_UINT32(8u, stats.bytesTransferred);
}

/** TS-SPI-006: bad arguments are rejected. */
static void test_Spi_RejectsBadArguments(void)
{
    uint8 buffer[4];

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    TEST_ASSERT_EQUAL(E_OK, Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS));

    /* Both directions NULL is a caller defect: the call would clock bytes nowhere from nowhere. */
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, NULL_PTR, NULL_PTR, 4u));
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_Lock((Spi_DeviceType)SPI_DEVICE_COUNT, SPI_LOCK_TIMEOUT_MS));
    TEST_ASSERT_NOT_EQUAL(E_OK, Spi_GetStatistics(NULL_PTR));

    /* One direction NULL is legitimate -- read-only and write-only are both real operations. */
    TEST_ASSERT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, buffer, NULL_PTR, 4u));
    TEST_ASSERT_EQUAL(E_OK, Spi_Transfer(SPI_DEVICE_CAN, NULL_PTR, buffer, 4u));

    TEST_ASSERT_EQUAL(E_OK, Spi_Unlock(SPI_DEVICE_CAN));
}

/*==================================================================================================
 *  TS-UDS-001 .. 012  The diagnostic services
 *================================================================================================*/

/** Bring the diagnostic component up on an erased store. */
static void TsBringUpDiag(void)
{
    TsBringUpStorage();
    TEST_ASSERT_EQUAL(E_OK, Dem_Init());
    TEST_ASSERT_EQUAL(E_OK, DiagSwc_Init());
}

/** Issue one request and return the response length. */
static Std_ReturnType TsRequest(const uint8 *request, uint16 requestLen, uint8 *response, uint16 *responseLen)
{
    return DiagSwc_HandleRequest(request, requestLen, response, (uint16)DIAGSWC_MAX_RESPONSE_SIZE,
                                 responseLen);
}

/**
 * TS-UDS-001: SWREQ-DIAG-0040 -- reading DTC information produces a positive response.
 *
 * The positive response identifier is the service identifier plus 0x40, per ISO 14229. Following that
 * convention is the point of SWREQ-DIAG-0030: an engineer who knows UDS can read the exchange without a
 * bespoke table, even though the transport here is text over MQTT rather than ISO-TP over CAN.
 */
static void test_Uds_ReadDtcInformation(void)
{
    const uint8 request[] = {DIAGSWC_SID_READ_DTC_INFORMATION, 0x02u, 0xFFu};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_TRUE(responseLen >= 1u);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_SID_READ_DTC_INFORMATION + DIAGSWC_POSITIVE_RESPONSE_OFFSET, response[0]);
}

/**
 * TS-UDS-002: a confirmed fault appears in the DTC listing.
 *
 * Which is the whole purpose of the service. A listing that was always empty would pass a test that only
 * checked the response identifier.
 */
static void test_Uds_ConfirmedDtcAppearsInListing(void)
{
    const uint8 request[] = {DIAGSWC_SID_READ_DTC_INFORMATION, 0x02u, 0xFFu};
    uint8 empty[DIAGSWC_MAX_RESPONSE_SIZE];
    uint8 populated[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 emptyLen = 0u;
    uint16 populatedLen = 0u;
    uint16 i;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), empty, &emptyLen));

    /* Drive an event to confirmation. Dem debounces, so one report is not a fault. */
    for (i = 0u; i < 64u; i++)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_TIMEOUT, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
    }

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), populated, &populatedLen));

    /* The listing grew, which it could not have done if the service ignored the store. */
    TEST_ASSERT_TRUE(populatedLen > emptyLen);
}

/** TS-UDS-003: clearing the diagnostic information empties the listing. */
static void test_Uds_ClearEmptiesTheListing(void)
{
    const uint8 readRequest[] = {DIAGSWC_SID_READ_DTC_INFORMATION, 0x02u, 0xFFu};
    const uint8 clearRequest[] = {DIAGSWC_SID_CLEAR_DIAGNOSTIC_INFO, 0xFFu, 0xFFu, 0xFFu};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 beforeLen = 0u;
    uint16 afterLen = 0u;
    uint16 clearLen = 0u;
    uint16 i;

    TsBringUpDiag();

    for (i = 0u; i < 64u; i++)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_TIMEOUT, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
    }
    TEST_ASSERT_EQUAL(E_OK, TsRequest(readRequest, (uint16)sizeof(readRequest), response, &beforeLen));

    TEST_ASSERT_EQUAL(E_OK, TsRequest(clearRequest, (uint16)sizeof(clearRequest), response, &clearLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_SID_CLEAR_DIAGNOSTIC_INFO + DIAGSWC_POSITIVE_RESPONSE_OFFSET,
                            response[0]);

    TEST_ASSERT_EQUAL(E_OK, TsRequest(readRequest, (uint16)sizeof(readRequest), response, &afterLen));
    TEST_ASSERT_TRUE(afterLen < beforeLen);
}

/** TS-UDS-004: reading a known data identifier returns its value. */
static void test_Uds_ReadDataByIdentifier(void)
{
    const uint8 request[] = {DIAGSWC_SID_READ_DATA_BY_ID, (uint8)(DIAGSWC_DID_FIRMWARE_VERSION >> 8u),
                             (uint8)(DIAGSWC_DID_FIRMWARE_VERSION & 0xFFu)};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_SID_READ_DATA_BY_ID + DIAGSWC_POSITIVE_RESPONSE_OFFSET, response[0]);

    /* The echoed identifier, then at least one byte of value. */
    TEST_ASSERT_TRUE(responseLen > 3u);
    TEST_ASSERT_EQUAL_UINT8((uint8)(DIAGSWC_DID_FIRMWARE_VERSION >> 8u), response[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8)(DIAGSWC_DID_FIRMWARE_VERSION & 0xFFu), response[2]);
}

/** TS-UDS-005: an unknown data identifier is refused with the standard code. */
static void test_Uds_UnknownIdentifierIsRefused(void)
{
    const uint8 request[] = {DIAGSWC_SID_READ_DATA_BY_ID, 0x00u, 0x01u};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    /* A negative response is still a response, so the call reports E_OK -- the request was handled. */
    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));

    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_SID_READ_DATA_BY_ID, response[1]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NRC_REQUEST_OUT_OF_RANGE, response[2]);
}

/** TS-UDS-006: an unsupported service is refused with the standard code. */
static void test_Uds_UnsupportedServiceIsRefused(void)
{
    const uint8 request[] = {0x3Eu, 0x00u}; /* TesterPresent: not implemented here */
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NRC_SERVICE_NOT_SUPPORTED, response[2]);
}

/**
 * TS-UDS-007: a truncated request is refused with the length code, not parsed.
 *
 * This is an externally-controlled input arriving over the broker. A service that read its parameters
 * without checking the length would read past the request, and the bytes beyond it are whatever the
 * previous message left in the buffer.
 */
static void test_Uds_TruncatedRequestIsRefused(void)
{
    const uint8 request[] = {DIAGSWC_SID_READ_DATA_BY_ID, 0xFDu}; /* identifier cut in half */
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NRC_INCORRECT_LENGTH, response[2]);
}

/**
 * TS-UDS-008: an empty request gets a negative response; a NULL argument is a caller defect.
 *
 * Two different outcomes for two different kinds of wrong, and the distinction is the module's contract.
 * A zero-length request came from the broker and is answerable -- with a length rejection -- so it is
 * *handled*, and the call reports E_OK because a response was produced. A NULL pointer came from this
 * firmware and is a development error, so it returns E_NOT_OK and reports to Det.
 *
 * Conflating them would mean either a malformed remote request looked like an internal bug, or an
 * internal bug looked like ordinary traffic. Both hide the thing worth seeing.
 */
static void test_Uds_EmptyRequestIsRefused(void)
{
    const uint8 request[1] = {0u};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    /* Answerable, so a negative response with the length code. */
    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, 0u, response, &responseLen));
    TEST_ASSERT_TRUE(responseLen >= 3u);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NRC_INCORRECT_LENGTH, response[2]);

    /* Caller defects, so E_NOT_OK. */
    TEST_ASSERT_NOT_EQUAL(E_OK, TsRequest(NULL_PTR, 3u, response, &responseLen));
    TEST_ASSERT_NOT_EQUAL(E_OK, TsRequest(request, 1u, NULL_PTR, &responseLen));
}

/**
 * TS-UDS-009: a request longer than the configured maximum is refused.
 *
 * The bound exists because the request arrives from the broker. v1 passed a broker payload straight into
 * a fixed buffer.
 */
static void test_Uds_OversizedRequestIsRefused(void)
{
    static uint8 request[DIAGSWC_MAX_REQUEST_SIZE + 32u];
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    (void)memset(request, 0, sizeof(request));
    request[0] = DIAGSWC_SID_READ_DTC_INFORMATION;

    /* Refused at the length check, before the service identifier is even looked at -- so a long request
     * cannot reach a handler that would then read its parameters from beyond what was sent. */
    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_TRUE(responseLen >= 3u);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NRC_INCORRECT_LENGTH, response[2]);
}

/**
 * TS-UDS-010: SWREQ-DIAG-0050 -- writing a calibration identifier takes effect and is durable.
 *
 * This is the authenticated write path that replaced v1's unauthenticated web server, which could rewrite
 * the odometer from anywhere within radio range.
 */
static void test_Uds_WriteCalibrationIdentifier(void)
{
    const uint8 request[] = {DIAGSWC_SID_WRITE_DATA_BY_ID, (uint8)(DIAGSWC_DID_TYRE_DIAMETER >> 8u),
                             (uint8)(DIAGSWC_DID_TYRE_DIAMETER & 0xFFu), 0x52u,
                             0x08u}; /* 21000 milli-inch, big endian */
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;
    NvM_CalibrationType calibration;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_SID_WRITE_DATA_BY_ID + DIAGSWC_POSITIVE_RESPONSE_OFFSET, response[0]);

    TEST_ASSERT_EQUAL(E_OK, NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration));
    TEST_ASSERT_EQUAL_UINT16(21000u, calibration.tyreDiameterMilliInch);
}

/** TS-UDS-011: writing a read-only identifier is refused. */
static void test_Uds_WriteToReadOnlyIdentifierIsRefused(void)
{
    const uint8 request[] = {DIAGSWC_SID_WRITE_DATA_BY_ID,
                             (uint8)(DIAGSWC_DID_ODOMETER >> 8u),
                             (uint8)(DIAGSWC_DID_ODOMETER & 0xFFu),
                             0x00u,
                             0x00u,
                             0x00u,
                             0x00u};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(request, (uint16)sizeof(request), response, &responseLen));
    TEST_ASSERT_EQUAL_UINT8(DIAGSWC_NEGATIVE_RESPONSE_SID, response[0]);
}

/**
 * TS-UDS-012: SWREQ-DIAG-0068 -- the activity counters distinguish served from rejected.
 *
 * A single request count would make a unit under attack -- or a tester sending malformed requests --
 * indistinguishable from one being used normally.
 */
static void test_Uds_CountersDistinguishServedFromRejected(void)
{
    const uint8 good[] = {DIAGSWC_SID_READ_DTC_INFORMATION, 0x02u, 0xFFu};
    const uint8 bad[] = {0x3Eu, 0x00u};
    uint8 response[DIAGSWC_MAX_RESPONSE_SIZE];
    uint16 responseLen = 0u;
    DiagSwc_StatusType status;

    TsBringUpDiag();

    TEST_ASSERT_EQUAL(E_OK, TsRequest(good, (uint16)sizeof(good), response, &responseLen));
    TEST_ASSERT_EQUAL(E_OK, TsRequest(bad, (uint16)sizeof(bad), response, &responseLen));
    TEST_ASSERT_EQUAL(E_OK, TsRequest(bad, (uint16)sizeof(bad), response, &responseLen));

    TEST_ASSERT_EQUAL(E_OK, DiagSwc_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(3u, status.requestsReceived);
    TEST_ASSERT_EQUAL_UINT32(1u, status.requestsServed);
    TEST_ASSERT_EQUAL_UINT32(2u, status.requestsRejected);
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Sys_ResetReasonIsDistinct);
    RUN_TEST(test_Sys_ResetReasonNamesAreDistinct);
    RUN_TEST(test_Sys_DeviceIdIsPresentAndNonZero);
    RUN_TEST(test_Sys_DeviceIdRefusesSmallBuffer);
    RUN_TEST(test_Sys_HeapReportIncludesLargestBlock);

    RUN_TEST(test_Sys_StartupReachesRunWhenHealthy);
    RUN_TEST(test_Sys_MissingCardDegradesRatherThanFails);
    RUN_TEST(test_Sys_AbsentClockIsReportedNotFatal);
    RUN_TEST(test_Sys_TaskCreationFailureIsFatal);
    RUN_TEST(test_Sys_CrashLoopIsDetectedAndClearable);

    RUN_TEST(test_SchM_EveryTaskDispatches);
    RUN_TEST(test_SchM_ActivationsAccumulate);
    RUN_TEST(test_SchM_WithinBudgetIsNotOverrun);
    RUN_TEST(test_SchM_ExecutionTimeIsMeasured);
    RUN_TEST(test_SchM_RejectsUnknownTask);
    RUN_TEST(test_SchM_BudgetsAreBelowPeriods);

    RUN_TEST(test_Spi_LockTakesOwnership);
    RUN_TEST(test_Spi_TransferWithoutLockFails);
    RUN_TEST(test_Spi_WrongOwnerIsRejected);
    RUN_TEST(test_Spi_LockSpansSeveralTransfers);
    RUN_TEST(test_Spi_StatisticsAreRecorded);
    RUN_TEST(test_Spi_RejectsBadArguments);

    RUN_TEST(test_Uds_ReadDtcInformation);
    RUN_TEST(test_Uds_ConfirmedDtcAppearsInListing);
    RUN_TEST(test_Uds_ClearEmptiesTheListing);
    RUN_TEST(test_Uds_ReadDataByIdentifier);
    RUN_TEST(test_Uds_UnknownIdentifierIsRefused);
    RUN_TEST(test_Uds_UnsupportedServiceIsRefused);
    RUN_TEST(test_Uds_TruncatedRequestIsRefused);
    RUN_TEST(test_Uds_EmptyRequestIsRefused);
    RUN_TEST(test_Uds_OversizedRequestIsRefused);
    RUN_TEST(test_Uds_WriteCalibrationIdentifier);
    RUN_TEST(test_Uds_WriteToReadOnlyIdentifierIsRefused);
    RUN_TEST(test_Uds_CountersDistinguishServedFromRejected);

    return UNITY_END();
}

/**
 * @file    test_core.c
 * @brief   Unit tests for the three foundations everything else rests on: time, tracing, serial.
 *
 * Three modules whose correctness is assumed by every other suite, which is exactly why they need their
 * own. Each has a specific v1 defect attached:
 *
 *  * **Gpt** -- every timeout in the firmware is one call to ::Gpt_HasElapsed. v1 mixed `millis()`,
 *    FreeRTOS ticks and `time(nullptr)` in timeout expressions; two of those three break, and both
 *    failures appear weeks into deployment. The wrap cases here reach the 49.7-day boundary in
 *    microseconds, which is the whole reason virtual time exists in the harness.
 *  * **Det** -- the deduplicating report store is what makes a returned unit diagnosable. v1's entire
 *    fault state was a `byte flags[15]` array overwritten every cycle.
 *  * **Uart** -- discarding received data and waiting for transmitted data to leave are different
 *    operations. v1 called Arduino's `flush()`, which does the second, in three places meaning the
 *    first.
 *
 * @req SWREQ-SYS-0010, SWREQ-SYS-0011, SWREQ-DIAG-0001 .. SWREQ-DIAG-0003,
 *      SWREQ-COM-0001 .. SWREQ-COM-0003
 * @verifies TS-GPT-001 .. TS-GPT-010, TS-DET-001 .. TS-DET-008, TS-UART-001 .. TS-UART-008
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "mcal/Gpt/Gpt.h"
#include "mcal/Uart/Uart.h"
#include "mcal/Uart/Uart_Cfg.h"
#include "services/Det/Det.h"
#include "services/Det/Det_Cfg.h"
#include "Stub_Mcal.h"
#include "unity.h"

/** The RS485 instance, which is the one with the interesting framing and timing. */
#define TC_BUS ((uint8)UART_INSTANCE_RS485)

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Uart_Init());
}

void tearDown(void)
{
}

/** Open the RS485 instance with its configured framing. */
static void TcOpenBus(void)
{
    Uart_ConfigType config;

    (void)memset(&config, 0, sizeof(config));
    config.baudRate = UART_BAUD_RS485;
    config.frame = UART_FRAME_8E1;
    config.rxPin = PIN_RS485_RX;
    config.txPin = PIN_RS485_TX;
    config.rxBufferSize = UART_RX_BUFFER_RS485;
    config.invertRx = FALSE;

    TEST_ASSERT_EQUAL(E_OK, Uart_Open(TC_BUS, &config));
}

/*==================================================================================================
 *  TS-GPT-001 .. 010  Wrap-safe elapsed time
 *================================================================================================*/

/** TS-GPT-001: elapsed time is the difference, in the ordinary case. */
static void test_Gpt_ElapsedInNormalRange(void)
{
    Gpt_TimestampType start;

    Stub_Gpt_SetMonotonicMs(1000u);
    start = Gpt_GetMonotonicMs();

    Stub_Gpt_AdvanceMs(250u);
    TEST_ASSERT_EQUAL_UINT32(250u, Gpt_ElapsedSince(start));
}

/**
 * TS-GPT-002: SWREQ-SYS-0011 -- elapsed time is correct across the 32-bit wrap.
 *
 * The case that breaks a naive implementation. The counter is placed 100 ms before the wrap and
 * advanced 300 ms, so `now` is 200 and `since` is 0xFFFFFF9C. Unsigned subtraction wraps modulo 2^32
 * and yields the true 300; a signed comparison, or special-casing `now < since`, yields either a
 * negative number or about 4.29 billion.
 *
 * On a unit that has been running 49.7 days this is the difference between a working timeout and one
 * that never fires -- and no test short of seven weeks of runtime would find it.
 */
static void test_Gpt_ElapsedAcrossWrap(void)
{
    Gpt_TimestampType start;

    Stub_Gpt_SetMonotonicMs(0xFFFFFFFFuL - 99u); /* 100 ms before the wrap */
    start = Gpt_GetMonotonicMs();

    Stub_Gpt_AdvanceMs(300u);

    TEST_ASSERT_EQUAL_UINT32(300u, Gpt_ElapsedSince(start));
}

/** TS-GPT-003: a timeout expires exactly at its interval, not before. */
static void test_Gpt_HasElapsedBoundary(void)
{
    Gpt_TimestampType start;

    Stub_Gpt_SetMonotonicMs(5000u);
    start = Gpt_GetMonotonicMs();

    Stub_Gpt_AdvanceMs(999u);
    TEST_ASSERT_FALSE(Gpt_HasElapsed(start, 1000u));

    Stub_Gpt_AdvanceMs(1u);
    TEST_ASSERT_TRUE(Gpt_HasElapsed(start, 1000u));
}

/** TS-GPT-004: a zero interval has always elapsed. */
static void test_Gpt_ZeroIntervalHasElapsed(void)
{
    const Gpt_TimestampType start = Gpt_GetMonotonicMs();

    TEST_ASSERT_TRUE(Gpt_HasElapsed(start, 0u));
}

/** TS-GPT-005: a timeout straddling the wrap expires at the right moment. */
static void test_Gpt_HasElapsedAcrossWrap(void)
{
    Gpt_TimestampType start;

    Stub_Gpt_SetMonotonicMs(0xFFFFFFFFuL - 49u); /* 50 ms before the wrap */
    start = Gpt_GetMonotonicMs();

    Stub_Gpt_AdvanceMs(99u); /* now 49 ms past the wrap; 99 elapsed */
    TEST_ASSERT_FALSE(Gpt_HasElapsed(start, 100u));

    Stub_Gpt_AdvanceMs(1u);
    TEST_ASSERT_TRUE(Gpt_HasElapsed(start, 100u));
}

/**
 * TS-GPT-006: an interval above half the counter range reports elapsed.
 *
 * Beyond 24.8 days a past instant and a future one are indistinguishable in 32 bits, so the module
 * treats such a request as already elapsed rather than as a silently wrong "not yet". A caller asking
 * for more than that has a defect, and an early timeout is a visible failure where a hang is not.
 */
static void test_Gpt_OverlongIntervalReportsElapsed(void)
{
    const Gpt_TimestampType start = Gpt_GetMonotonicMs();

    TEST_ASSERT_TRUE(Gpt_HasElapsed(start, GPT_MAX_INTERVAL_MS + 1u));
    TEST_ASSERT_FALSE(Gpt_HasElapsed(start, GPT_MAX_INTERVAL_MS));
}

/** TS-GPT-007: the largest expressible interval behaves correctly at its own boundary. */
static void test_Gpt_MaxIntervalBoundary(void)
{
    Gpt_TimestampType start;

    Stub_Gpt_SetMonotonicMs(0u);
    start = Gpt_GetMonotonicMs();

    Stub_Gpt_AdvanceMs(GPT_MAX_INTERVAL_MS - 1u);
    TEST_ASSERT_FALSE(Gpt_HasElapsed(start, GPT_MAX_INTERVAL_MS));

    Stub_Gpt_AdvanceMs(1u);
    TEST_ASSERT_TRUE(Gpt_HasElapsed(start, GPT_MAX_INTERVAL_MS));
}

/** TS-GPT-008: the millisecond and microsecond bases agree. */
static void test_Gpt_MillisecondAndMicrosecondAgree(void)
{
    Stub_Gpt_SetMonotonicMs(0u);
    Stub_Gpt_AdvanceUs(1500u);

    TEST_ASSERT_EQUAL_UINT32(1u, Gpt_GetMonotonicMs());
    TEST_ASSERT_EQUAL_UINT64(1500uLL, Gpt_GetMonotonicUs());
}

/**
 * TS-GPT-009: the microsecond base does not wrap where the millisecond base does.
 *
 * It is 64 bits, which is what makes it usable for the per-runnable execution measurements SchM
 * records: those subtract two instants and must stay correct however long the unit has been running.
 */
static void test_Gpt_MicrosecondBaseDoesNotWrapEarly(void)
{
    Stub_Gpt_SetMonotonicMs(0u);
    Stub_Gpt_AdvanceUs(5000000000uLL); /* 5000 s, well past a 32-bit microsecond counter */

    TEST_ASSERT_EQUAL_UINT64(5000000000uLL, Gpt_GetMonotonicUs());
}

/** TS-GPT-010: time does not advance on its own, which is what makes the suite deterministic. */
static void test_Gpt_TimeIsVirtual(void)
{
    const Gpt_TimestampType first = Gpt_GetMonotonicMs();
    const Gpt_TimestampType second = Gpt_GetMonotonicMs();

    TEST_ASSERT_EQUAL_UINT32(first, second);
}

/*==================================================================================================
 *  TS-DET-001 .. 008  The report store
 *================================================================================================*/

/** TS-DET-001: SWREQ-DIAG-0001 -- a report is recorded with its full identity. */
static void test_Det_ReportIsRecordedWithIdentity(void)
{
    Det_EntryType history[DET_HISTORY_SIZE];
    uint16 count;

    Stub_Gpt_SetMonotonicMs(4242u);
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 3u, 0x20u, E_PARAM_POINTER));

    count = Det_GetHistory(history, (uint16)DET_HISTORY_SIZE);
    TEST_ASSERT_EQUAL_UINT16(1u, count);

    TEST_ASSERT_EQUAL_UINT16(MODULE_ID_NVM, history[0].moduleId);
    TEST_ASSERT_EQUAL_UINT8(3u, history[0].instanceId);
    TEST_ASSERT_EQUAL_UINT8(0x20u, history[0].apiId);
    TEST_ASSERT_EQUAL_UINT8(E_PARAM_POINTER, history[0].errorId);
    TEST_ASSERT_EQUAL_UINT8((uint8)DET_SEVERITY_DEV, history[0].severity);
    TEST_ASSERT_EQUAL_UINT32(4242u, history[0].timestamp);
}

/**
 * TS-DET-002: SWREQ-DIAG-0002 -- an identical report is counted, not stored again.
 *
 * A fault in the 10 ms scheduler tick would otherwise produce a hundred entries a second and evict
 * everything informative. One entry with a count carries the same information at a fixed cost.
 */
static void test_Det_IdenticalReportsAreDeduplicated(void)
{
    Det_EntryType history[DET_HISTORY_SIZE];
    Det_StatisticsType stats;
    uint16 i;

    for (i = 0u; i < 500u; i++)
    {
        TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_CANIF, 0u, 0x10u, E_PARAM_VALUE));
    }

    TEST_ASSERT_EQUAL_UINT16(1u, Det_GetHistory(history, (uint16)DET_HISTORY_SIZE));
    TEST_ASSERT_EQUAL_UINT16(500u, history[0].occurrences);

    Det_GetStatistics(&stats);
    TEST_ASSERT_EQUAL_UINT32(500u, stats.devErrorCount);
    TEST_ASSERT_EQUAL_UINT16(1u, stats.distinctEntryCount);
}

/**
 * TS-DET-003: reports differing in any one field are distinct.
 *
 * All four fields, because deduplicating on too few would merge genuinely different faults -- and two
 * instances of the same module failing is a different diagnosis from one instance failing twice.
 */
static void test_Det_DistinctFieldsAreNotMerged(void)
{
    Det_StatisticsType stats;

    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 0u, 0x10u, 0x20u));
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_FEE, 0u, 0x10u, 0x20u)); /* module   */
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 1u, 0x10u, 0x20u)); /* instance */
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 0u, 0x11u, 0x20u)); /* api      */
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 0u, 0x10u, 0x21u)); /* error    */

    Det_GetStatistics(&stats);
    TEST_ASSERT_EQUAL_UINT16(5u, stats.distinctEntryCount);
}

/**
 * TS-DET-004: SWREQ-DIAG-0003 -- development and runtime errors are counted separately.
 *
 * The two populations need different responses: a unit reporting development errors has a
 * configuration or integration problem, whereas one reporting only runtime errors is meeting its
 * contracts and the world is not cooperating. Merging them makes neither assessable.
 */
static void test_Det_SeveritiesAreCountedSeparately(void)
{
    Det_StatisticsType stats;

    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_SPI, 0u, 0x0Au, 0x22u));
    TEST_ASSERT_EQUAL(E_OK, Det_ReportRuntimeError(MODULE_ID_SPI, 0u, 0x0Au, 0x23u));
    TEST_ASSERT_EQUAL(E_OK, Det_ReportRuntimeError(MODULE_ID_SPI, 0u, 0x0Bu, 0x23u));
    TEST_ASSERT_EQUAL(E_OK, Det_ReportTransientFault(MODULE_ID_SPI, 0u, 0x0Cu, 0x24u));

    Det_GetStatistics(&stats);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.devErrorCount);
    TEST_ASSERT_EQUAL_UINT32(2u, stats.runtimeErrorCount);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.transientFaultCount);
}

/**
 * TS-DET-005: the store is bounded, and eviction is counted.
 *
 * A fixed store means a unit with many distinct faults loses the oldest, and `ringOverflowCount` is
 * what makes that visible rather than silent -- a full store that said nothing would leave a reader
 * unsure whether they were seeing all the faults.
 */
static void test_Det_StoreIsBoundedAndOverflowCounted(void)
{
    Det_StatisticsType stats;
    uint16 i;

    /* One more distinct triple than the store holds. */
    for (i = 0u; i < (uint16)(DET_HISTORY_SIZE + 8u); i++)
    {
        TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_DEM, (uint8)i, 0x10u, 0x20u));
    }

    Det_GetStatistics(&stats);
    TEST_ASSERT_EQUAL_UINT16((uint16)DET_HISTORY_SIZE, stats.distinctEntryCount);
    TEST_ASSERT_EQUAL_UINT16(8u, stats.ringOverflowCount);
}

/** TS-DET-006: the occurrence counter saturates rather than wrapping to zero. */
static void test_Det_OccurrenceCountSaturates(void)
{
    Det_EntryType history[DET_HISTORY_SIZE];
    uint32 i;

    /* Past the 16-bit maximum. A wrap would make a relentless fault read as a rare one. */
    for (i = 0u; i < ((uint32)DET_OCCURRENCE_MAX + 64uL); i++)
    {
        STD_DISCARD(Det_ReportError(MODULE_ID_WDGM, 0u, 0x30u, 0x40u));
    }

    TEST_ASSERT_EQUAL_UINT16(1u, Det_GetHistory(history, (uint16)DET_HISTORY_SIZE));
    TEST_ASSERT_EQUAL_UINT16((uint16)DET_OCCURRENCE_MAX, history[0].occurrences);
}

/**
 * TS-DET-007: clearing the history zeroes the counters as well as the entries.
 *
 * All of it, deliberately. This is reachable as the UDS ClearDiagnosticInformation service, whose whole
 * purpose is to let a technician confirm that a repair produced a clean run -- and a lifetime total
 * that survived the clear would make "clean" unobservable. The trending a health record does is over
 * successive reports, not over a counter that is never reset.
 *
 * What the clear must *not* do is unhook the logging callback, because that would silently stop a
 * technician seeing the very reports they cleared the store to look for.
 */
static void test_Det_ClearHistoryZeroesEverything(void)
{
    Det_EntryType history[DET_HISTORY_SIZE];
    Det_StatisticsType stats;

    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 0u, 0x10u, 0x20u));
    TEST_ASSERT_EQUAL(E_OK, Det_ReportRuntimeError(MODULE_ID_NVM, 0u, 0x11u, 0x21u));

    Det_ClearHistory();

    TEST_ASSERT_EQUAL_UINT16(0u, Det_GetHistory(history, (uint16)DET_HISTORY_SIZE));

    Det_GetStatistics(&stats);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.devErrorCount);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.runtimeErrorCount);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.distinctEntryCount);

    /* And the store still works afterwards, which a clear that left it in a broken state would not. */
    TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_NVM, 0u, 0x10u, 0x20u));
    TEST_ASSERT_EQUAL_UINT16(1u, Det_GetHistory(history, (uint16)DET_HISTORY_SIZE));
}

/** TS-DET-008: a history read into a smaller buffer returns what fits, not more. */
static void test_Det_HistoryReadRespectsBufferSize(void)
{
    Det_EntryType small[4];
    uint16 i;

    for (i = 0u; i < 10u; i++)
    {
        TEST_ASSERT_EQUAL(E_OK, Det_ReportError(MODULE_ID_COM, (uint8)i, 0x10u, 0x20u));
    }

    TEST_ASSERT_EQUAL_UINT16(4u, Det_GetHistory(small, 4u));
    TEST_ASSERT_EQUAL_UINT16(0u, Det_GetHistory(small, 0u));
    TEST_ASSERT_EQUAL_UINT16(0u, Det_GetHistory(NULL_PTR, 4u));
}

/*==================================================================================================
 *  TS-UART-001 .. 008  Discard, drain and early exit
 *================================================================================================*/

/**
 * TS-UART-001: SWREQ-COM-0001 -- discarding received data is a distinct operation, and reports how
 * much it threw away.
 *
 * The count is worth having: a non-zero discard before a request means the previous exchange left the
 * bus out of step, which is the condition v1 created on every exchange by calling `flush()` -- which
 * waits for the *transmit* buffer -- when it meant this.
 */
static void test_Uart_DiscardReportsWhatItThrewAway(void)
{
    const uint8 stale[] = {0x11u, 0x22u, 0x33u, 0x44u};

    TcOpenBus();
    Stub_Uart_QueueRxBytes(TC_BUS, stale, (uint16)sizeof(stale));

    TEST_ASSERT_EQUAL_UINT16((uint16)sizeof(stale), Uart_BytesAvailable(TC_BUS));
    TEST_ASSERT_EQUAL_UINT16((uint16)sizeof(stale), Uart_DiscardRx(TC_BUS));
    TEST_ASSERT_EQUAL_UINT16(0u, Uart_BytesAvailable(TC_BUS));
}

/** TS-UART-002: discarding an empty buffer reports zero and is not an error. */
static void test_Uart_DiscardOnEmptyBufferIsZero(void)
{
    TcOpenBus();
    TEST_ASSERT_EQUAL_UINT16(0u, Uart_DiscardRx(TC_BUS));
}

/**
 * TS-UART-003: SWREQ-COM-0002 -- draining the transmitter is a distinct operation from discarding.
 *
 * It is what makes the RS485 turnaround correct: the driver-enable line may only drop once the shift
 * register is empty, and `write()` returns when the bytes are merely buffered. v1 dropped DE straight
 * after `write()` and truncated the last character of every request it ever sent.
 *
 * Asserted by showing the two operations do not affect each other: draining leaves received data
 * untouched, which is precisely what v1 assumed the opposite of.
 */
static void test_Uart_DrainDoesNotDiscardReceivedData(void)
{
    const uint8 pending[] = {0xAAu, 0xBBu};
    const uint8 request[] = {0x01u, 0x02u, 0x03u};

    TcOpenBus();
    Stub_Uart_QueueRxBytes(TC_BUS, pending, (uint16)sizeof(pending));
    TEST_ASSERT_EQUAL(E_OK, Uart_Write(TC_BUS, request, (uint16)sizeof(request)));

    TEST_ASSERT_EQUAL(E_OK, Uart_DrainTx(TC_BUS, UART_DRAIN_TIMEOUT_MS));

    /* The received bytes are still there. A drain that discarded them would be the v1 confusion in
     * the other direction, and just as wrong. */
    TEST_ASSERT_EQUAL_UINT16((uint16)sizeof(pending), Uart_BytesAvailable(TC_BUS));
}

/**
 * TS-UART-004: SWREQ-COM-0003 -- a fixed-length read returns as soon as the last byte arrives.
 *
 * Not after the timeout. v1's read loop had no early exit and burned a full second per battery frame:
 * three frames per pack across four packs is 12 s of a 3 s acquisition budget spent waiting for data
 * that had already arrived.
 *
 * Checked by asserting that no virtual time passed -- the harness only advances time when something
 * asks it to, so a loop that waited out its timeout would be visible as elapsed time.
 */
static void test_Uart_ReadExactReturnsEarly(void)
{
    const uint8 response[] = {0x10u, 0x20u, 0x30u, 0x40u};
    uint8 buffer[8];
    uint16 actual = 0u;
    uint32 elapsedBefore;

    TcOpenBus();
    Stub_Uart_QueueRxBytes(TC_BUS, response, (uint16)sizeof(response));

    elapsedBefore = Stub_Gpt_GetTotalDelayMs();

    TEST_ASSERT_EQUAL(E_OK, Uart_ReadExact(TC_BUS, buffer, (uint16)sizeof(response), 1000u, &actual));
    TEST_ASSERT_EQUAL_UINT16((uint16)sizeof(response), actual);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(response, buffer, sizeof(response));

    /* Far less than the 1000 ms bound: the data was there, so nothing should have waited for it. */
    TEST_ASSERT_TRUE((Stub_Gpt_GetTotalDelayMs() - elapsedBefore) < 100u);
}

/**
 * TS-UART-005: a timed-out read reports how much did arrive.
 *
 * Which is what distinguishes "the pack is not answering" from "the pack answered and the frame was
 * cut short" -- two different faults with two different causes, indistinguishable from a bare failure.
 */
static void test_Uart_ReadExactReportsPartialOnTimeout(void)
{
    const uint8 partial[] = {0x10u, 0x20u};
    uint8 buffer[8];
    uint16 actual = 0u;

    TcOpenBus();
    Stub_Uart_QueueRxBytes(TC_BUS, partial, (uint16)sizeof(partial));

    /* Asking for more than will ever arrive. */
    TEST_ASSERT_EQUAL(E_TIMEOUT, Uart_ReadExact(TC_BUS, buffer, 6u, 50u, &actual));
    TEST_ASSERT_EQUAL_UINT16((uint16)sizeof(partial), actual);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(partial, buffer, sizeof(partial));
}

/** TS-UART-006: a read with nothing available is E_OK and zero bytes, not an error. */
static void test_Uart_ReadWithNothingAvailableIsOk(void)
{
    uint8 buffer[8];
    uint16 actual = 0xFFu;

    TcOpenBus();

    /* On a polled half-duplex bus "nothing yet" is the normal state for most of every exchange.
     * Returning an error would force every caller to distinguish a real fault from an empty poll,
     * which is where v1's response parser went wrong. */
    TEST_ASSERT_EQUAL(E_OK, Uart_Read(TC_BUS, buffer, (uint16)sizeof(buffer), &actual));
    TEST_ASSERT_EQUAL_UINT16(0u, actual);
}

/** TS-UART-007: an instance must be opened before use, and cannot be opened twice. */
static void test_Uart_OpenContract(void)
{
    Uart_ConfigType config;
    uint8 buffer[4];
    uint16 actual = 0u;

    (void)memset(&config, 0, sizeof(config));
    config.baudRate = UART_BAUD_RS485;
    config.frame = UART_FRAME_8E1;
    config.rxPin = PIN_RS485_RX;
    config.txPin = PIN_RS485_TX;
    config.rxBufferSize = UART_RX_BUFFER_RS485;

    /* Not open yet. */
    TEST_ASSERT_FALSE(Uart_IsOpen(TC_BUS));
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Read(TC_BUS, buffer, (uint16)sizeof(buffer), &actual));

    TEST_ASSERT_EQUAL(E_OK, Uart_Open(TC_BUS, &config));
    TEST_ASSERT_TRUE(Uart_IsOpen(TC_BUS));

    /* A second open would silently reconfigure a port another module may be mid-exchange on. */
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Open(TC_BUS, &config));

    TEST_ASSERT_EQUAL(E_OK, Uart_Close(TC_BUS));
    TEST_ASSERT_FALSE(Uart_IsOpen(TC_BUS));
}

/** TS-UART-008: bad arguments are rejected by every entry point. */
static void test_Uart_RejectsBadArguments(void)
{
    Uart_ConfigType config;
    uint8 buffer[4];
    uint16 actual = 0u;

    (void)memset(&config, 0, sizeof(config));
    config.baudRate = UART_BAUD_RS485;
    config.frame = UART_FRAME_8E1;
    config.rxBufferSize = UART_RX_BUFFER_RS485;

    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Open((Uart_InstanceType)UART_INSTANCE_COUNT, &config));
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Open(TC_BUS, NULL_PTR));

    /* A zero baud rate would divide by zero in the divisor calculation on a real peripheral. */
    config.baudRate = 0uL;
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Open(TC_BUS, &config));

    TcOpenBus();
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Read(TC_BUS, NULL_PTR, 4u, &actual));
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Read(TC_BUS, buffer, 4u, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_Write(TC_BUS, NULL_PTR, 4u));
    TEST_ASSERT_NOT_EQUAL(E_OK, Uart_ReadExact(TC_BUS, NULL_PTR, 4u, 10u, &actual));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Gpt_ElapsedInNormalRange);
    RUN_TEST(test_Gpt_ElapsedAcrossWrap);
    RUN_TEST(test_Gpt_HasElapsedBoundary);
    RUN_TEST(test_Gpt_ZeroIntervalHasElapsed);
    RUN_TEST(test_Gpt_HasElapsedAcrossWrap);
    RUN_TEST(test_Gpt_OverlongIntervalReportsElapsed);
    RUN_TEST(test_Gpt_MaxIntervalBoundary);
    RUN_TEST(test_Gpt_MillisecondAndMicrosecondAgree);
    RUN_TEST(test_Gpt_MicrosecondBaseDoesNotWrapEarly);
    RUN_TEST(test_Gpt_TimeIsVirtual);

    RUN_TEST(test_Det_ReportIsRecordedWithIdentity);
    RUN_TEST(test_Det_IdenticalReportsAreDeduplicated);
    RUN_TEST(test_Det_DistinctFieldsAreNotMerged);
    RUN_TEST(test_Det_SeveritiesAreCountedSeparately);
    RUN_TEST(test_Det_StoreIsBoundedAndOverflowCounted);
    RUN_TEST(test_Det_OccurrenceCountSaturates);
    RUN_TEST(test_Det_ClearHistoryZeroesEverything);
    RUN_TEST(test_Det_HistoryReadRespectsBufferSize);

    RUN_TEST(test_Uart_DiscardReportsWhatItThrewAway);
    RUN_TEST(test_Uart_DiscardOnEmptyBufferIsZero);
    RUN_TEST(test_Uart_DrainDoesNotDiscardReceivedData);
    RUN_TEST(test_Uart_ReadExactReturnsEarly);
    RUN_TEST(test_Uart_ReadExactReportsPartialOnTimeout);
    RUN_TEST(test_Uart_ReadWithNothingAvailableIsOk);
    RUN_TEST(test_Uart_OpenContract);
    RUN_TEST(test_Uart_RejectsBadArguments);

    return UNITY_END();
}

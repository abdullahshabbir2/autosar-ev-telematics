/**
 * @file    test_hmi.c
 * @brief   Unit tests for indication and analogue conditioning.
 *
 * Two related things, both about the boundary between a physical quantity and a meaning:
 *
 *  * **HmiSwc** turns a condition into a blink pattern. The indicators are the only diagnosis
 *    available to a technician standing at the vehicle without a laptop, so an indicator showing the
 *    wrong thing is worse than one showing nothing -- it sends the investigation somewhere else.
 *    v1 addressed five LEDs through a shared integer index and an off-by-one lit the wrong one.
 *  * **Adc** and **IoHwAb** turn a raw count into millivolts. The conditioning -- oversampling with
 *    the extremes discarded, and per-unit calibration -- is what makes the reading mean anything,
 *    and all of it is arithmetic that belongs on the tested side of the platform boundary.
 *
 * @req SWREQ-HMI-0001 .. SWREQ-HMI-0012, SWREQ-SNS-0030 .. SWREQ-SNS-0038
 * @verifies TS-HMI-001 .. TS-HMI-012, TS-SNS-001 .. TS-SNS-010
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "app/HmiSwc/HmiSwc.h"
#include "app/HmiSwc/HmiSwc_Cfg.h"
#include "ecuabs/IoHwAb/IoHwAb.h"
#include "ecuabs/IoHwAb/IoHwAb_Cfg.h"
#include "mcal/Adc/Adc.h"
#include "mcal/Adc/Adc_Cfg.h"
#include "mcal/Dio/Dio.h"
#include "mcal/Dio/Dio_Cfg.h"
#include "mcal/Fls/Fls.h"
#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "services/NvM/NvM.h"
#include "Stub_Mcal.h"
#include "unity.h"

/** The GPIO each indicator drives, so a test can assert on the pin rather than on the abstraction. */
static const uint8 ThIndicatorPin[] = {
    (uint8)IOHWAB_DIO_ACQUISITION, (uint8)IOHWAB_DIO_STORAGE,   (uint8)IOHWAB_DIO_LINK,
    (uint8)IOHWAB_DIO_CLOUD,       (uint8)IOHWAB_DIO_HEARTBEAT,
};

#define TH_INDICATOR_COUNT ((uint8)(sizeof(ThIndicatorPin) / sizeof(ThIndicatorPin[0])))

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();

    /* IoHwAb reads its divider ratio and offset from NVM_BLOCK_CALIBRATION, so the storage stack has
     * to be up first -- the per-unit calibration is exactly what is not allowed to be a compile-time
     * constant (SWREQ-SNS-0035), and the cost of that is this dependency. On erased media NvM_Init
     * applies the configured defaults, which is what these tests run against. */
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());

    TEST_ASSERT_EQUAL(E_OK, IoHwAb_Init());
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_Init());
}

void tearDown(void)
{
}

/** Run the HMI tick @p n times. */
static void ThTick(uint16 n)
{
    uint16 i;

    for (i = 0u; i < n; i++)
    {
        HmiSwc_MainFunction();
    }
}

/** TRUE if @p indicator's GPIO is at its active level. */
static boolean ThIsLit(IoHwAb_IndicatorType indicator)
{
    return (Stub_Dio_GetLevel(ThIndicatorPin[indicator]) == (uint8)IOHWAB_INDICATOR_ON_LEVEL) ? TRUE : FALSE;
}

/*==================================================================================================
 *  TS-HMI-001 .. 004  Independent addressing
 *================================================================================================*/

/**
 * TS-HMI-001: SWREQ-HMI-0001 -- each indicator drives its own pin and no other.
 *
 * The case v1 got wrong. Every indicator is lit in turn and the other four are checked dark, so a
 * shared index, a swapped pair or an off-by-one all fail here. Checking only that the intended one
 * lights would pass with any of those three.
 */
static void test_Hmi_EachIndicatorIsIndependent(void)
{
    uint8 target;

    for (target = 0u; target < TH_INDICATOR_COUNT; target++)
    {
        uint8 other;

        TEST_ASSERT_EQUAL(E_OK, HmiSwc_Init());
        TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern((IoHwAb_IndicatorType)target, HMI_PATTERN_SOLID));
        ThTick(1u);

        TEST_ASSERT_TRUE(ThIsLit((IoHwAb_IndicatorType)target));

        for (other = 0u; other < TH_INDICATOR_COUNT; other++)
        {
            if (other != target)
            {
                TEST_ASSERT_FALSE(ThIsLit((IoHwAb_IndicatorType)other));
            }
        }
    }
}

/** TS-HMI-002: every indicator maps to a distinct GPIO. */
static void test_Hmi_IndicatorPinsAreDistinct(void)
{
    uint8 i;
    uint8 j;

    for (i = 0u; i < TH_INDICATOR_COUNT; i++)
    {
        for (j = (uint8)(i + 1u); j < TH_INDICATOR_COUNT; j++)
        {
            TEST_ASSERT_NOT_EQUAL(ThIndicatorPin[i], ThIndicatorPin[j]);
        }
    }
}

/** TS-HMI-003: Init leaves everything dark, whatever was lit before. */
static void test_Hmi_InitTurnsEverythingOff(void)
{
    uint8 i;

    for (i = 0u; i < TH_INDICATOR_COUNT; i++)
    {
        TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern((IoHwAb_IndicatorType)i, HMI_PATTERN_SOLID));
    }
    ThTick(1u);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_Init());
    ThTick(1u);

    for (i = 0u; i < TH_INDICATOR_COUNT; i++)
    {
        TEST_ASSERT_FALSE(ThIsLit((IoHwAb_IndicatorType)i));
    }
}

/** TS-HMI-004: an out-of-range indicator and an unknown pattern are both rejected. */
static void test_Hmi_RejectsBadArguments(void)
{
    HmiSwc_IndicatorStateType state;

    TEST_ASSERT_NOT_EQUAL(E_OK,
                          HmiSwc_SetPattern((IoHwAb_IndicatorType)TH_INDICATOR_COUNT, HMI_PATTERN_SOLID));
    TEST_ASSERT_NOT_EQUAL(E_OK, HmiSwc_Pulse((IoHwAb_IndicatorType)TH_INDICATOR_COUNT));
    TEST_ASSERT_NOT_EQUAL(E_OK, HmiSwc_GetState((IoHwAb_IndicatorType)TH_INDICATOR_COUNT, &state));
    TEST_ASSERT_NOT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_LINK, NULL_PTR));
}

/*==================================================================================================
 *  TS-HMI-005 .. 009  Patterns
 *================================================================================================*/

/** TS-HMI-005: solid stays lit across many ticks, without toggling. */
static void test_Hmi_SolidDoesNotToggle(void)
{
    HmiSwc_IndicatorStateType state;

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_LINK, HMI_PATTERN_SOLID));
    ThTick(40u);

    TEST_ASSERT_TRUE(ThIsLit(IOHWAB_INDICATOR_LINK));

    /* One transition: off to on. A pattern that re-wrote the level every tick would still read lit,
     * so the transition count is what distinguishes steady from rapidly flickering. */
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_LINK, &state));
    TEST_ASSERT_EQUAL_UINT32(1u, state.transitionCount);
}

/**
 * TS-HMI-006: the slow blink has the configured period.
 *
 * ::HMI_SLOW_HALF_PERIOD_TICKS is 5 at a 100 ms tick, so one full cycle is 10 ticks and 20 cycles are
 * 200 ticks. Counting transitions over a known span checks the period rather than merely that
 * something blinks -- a pattern at the wrong rate still blinks.
 */
static void test_Hmi_SlowBlinkPeriod(void)
{
    HmiSwc_IndicatorStateType state;
    const uint16 cycles = 20u;
    const uint16 ticks = (uint16)(cycles * 2u * HMI_SLOW_HALF_PERIOD_TICKS);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_HEARTBEAT, HMI_PATTERN_BLINK_SLOW));
    ThTick(ticks);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_HEARTBEAT, &state));

    /* Two transitions per cycle. Allowing +-1 for where the phase happens to stop. */
    TEST_ASSERT_UINT32_WITHIN(1u, (uint32)(cycles * 2u), state.transitionCount);
}

/**
 * TS-HMI-007: each blink rate matches its own configured half-period.
 *
 * Both counts are compared against a figure computed from the configuration -- transitions over a
 * span of N ticks is N / halfPeriod -- rather than against each other. A ratio between two measured
 * counts would compound the +-1 phase error of each, so the tolerance would have to grow with the
 * ratio and the assertion would get weaker exactly as the rates diverge.
 *
 * The two rates being *different* is not the property worth testing; a technician has to be able to
 * tell them apart by eye, which means each has to be near its intended rate.
 */
static void test_Hmi_BlinkRatesMatchConfiguration(void)
{
    HmiSwc_IndicatorStateType slow;
    HmiSwc_IndicatorStateType fast;
    const uint16 ticks = 200u;

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_LINK, HMI_PATTERN_BLINK_SLOW));
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_CLOUD, HMI_PATTERN_BLINK_FAST));
    ThTick(ticks);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_LINK, &slow));
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_CLOUD, &fast));

    TEST_ASSERT_UINT32_WITHIN(2u, (uint32)ticks / (uint32)HMI_SLOW_HALF_PERIOD_TICKS, slow.transitionCount);
    TEST_ASSERT_UINT32_WITHIN(2u, (uint32)ticks / (uint32)HMI_FAST_HALF_PERIOD_TICKS, fast.transitionCount);

    /* And the fast one is genuinely faster, which the two assertions above imply but which is the
     * property a reader of this test is actually looking for. */
    TEST_ASSERT_TRUE(fast.transitionCount > slow.transitionCount);
}

/** TS-HMI-008: off means dark, and stays dark. */
static void test_Hmi_OffStaysDark(void)
{
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_STORAGE, HMI_PATTERN_SOLID));
    ThTick(2u);
    TEST_ASSERT_TRUE(ThIsLit(IOHWAB_INDICATOR_STORAGE));

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_STORAGE, HMI_PATTERN_OFF));
    ThTick(40u);
    TEST_ASSERT_FALSE(ThIsLit(IOHWAB_INDICATOR_STORAGE));
}

/**
 * TS-HMI-009: a pulse flashes once and restores the previous pattern.
 *
 * The restore is the part worth testing. A pulse on the storage indicator happens once per record
 * while the link indicator is solid; if the pulse left the indicator in its flashed state, "a record
 * was written" would become indistinguishable from "storage is dark".
 */
static void test_Hmi_PulseRestoresPreviousPattern(void)
{
    HmiSwc_IndicatorStateType state;

    /* Establish a solid pattern, then pulse over it. */
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_ACQUISITION, HMI_PATTERN_SOLID));
    ThTick(2u);
    TEST_ASSERT_TRUE(ThIsLit(IOHWAB_INDICATOR_ACQUISITION));

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_Pulse(IOHWAB_INDICATOR_ACQUISITION));

    /* Once the pulse has run its length, the solid pattern is back. */
    ThTick((uint16)(HMI_PULSE_TICKS + 2u));

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_ACQUISITION, &state));
    TEST_ASSERT_EQUAL(HMI_PATTERN_SOLID, state.pattern);
    TEST_ASSERT_TRUE(ThIsLit(IOHWAB_INDICATOR_ACQUISITION));
}

/** TS-HMI-010: a pulse over an off indicator returns it to off, not to lit. */
static void test_Hmi_PulseOverOffReturnsToOff(void)
{
    HmiSwc_IndicatorStateType state;

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_STORAGE, HMI_PATTERN_OFF));
    ThTick(2u);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_Pulse(IOHWAB_INDICATOR_STORAGE));
    ThTick((uint16)(HMI_PULSE_TICKS + 2u));

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_STORAGE, &state));
    TEST_ASSERT_EQUAL(HMI_PATTERN_OFF, state.pattern);
    TEST_ASSERT_FALSE(ThIsLit(IOHWAB_INDICATOR_STORAGE));
}

/*==================================================================================================
 *  TS-HMI-011 .. 012  Liveness
 *================================================================================================*/

/**
 * TS-HMI-011: SWREQ-HMI-0012 -- the heartbeat is independent of every other indicator.
 *
 * It is the one signal distinguishing "the firmware has stopped" from "a subsystem is down", and that
 * distinction decides whether the next step is a power cycle or a diagnostic read. Every other
 * indicator is driven while the heartbeat blinks, and the heartbeat must be unaffected.
 */
static void test_Hmi_HeartbeatIsUnaffectedByOthers(void)
{
    HmiSwc_IndicatorStateType before;
    HmiSwc_IndicatorStateType after;

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_HEARTBEAT, HMI_PATTERN_BLINK_SLOW));
    ThTick(100u);
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_HEARTBEAT, &before));

    /* Now churn every other indicator through every pattern. */
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_ACQUISITION, HMI_PATTERN_BLINK_FAST));
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_STORAGE, HMI_PATTERN_SOLID));
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_SetPattern(IOHWAB_INDICATOR_LINK, HMI_PATTERN_BLINK_SLOW));
    TEST_ASSERT_EQUAL(E_OK, HmiSwc_Pulse(IOHWAB_INDICATOR_CLOUD));
    ThTick(100u);

    TEST_ASSERT_EQUAL(E_OK, HmiSwc_GetState(IOHWAB_INDICATOR_HEARTBEAT, &after));

    /* Same rate over the same span: the heartbeat gained the transitions its own pattern implies. */
    TEST_ASSERT_UINT32_WITHIN(2u, before.transitionCount * 2u, after.transitionCount);
}

/** TS-HMI-012: the self test exercises every indicator and leaves them all off. */
static void test_Hmi_SelfTestLeavesEverythingOff(void)
{
    uint8 i;

    HmiSwc_SelfTest();

    for (i = 0u; i < TH_INDICATOR_COUNT; i++)
    {
        /* Each was driven at least once -- otherwise a dead indicator would pass a self test. */
        TEST_ASSERT_TRUE(Stub_Dio_GetWriteCount(ThIndicatorPin[i]) > 0u);
    }

    ThTick(1u);
    for (i = 0u; i < TH_INDICATOR_COUNT; i++)
    {
        TEST_ASSERT_FALSE(ThIsLit((IoHwAb_IndicatorType)i));
    }
}

/*==================================================================================================
 *  TS-SNS-001 .. 006  Analogue conditioning
 *================================================================================================*/

/** TS-SNS-001: a steady input reads back as itself, once initialised. */
static void test_Sns_SteadyInputReadsBack(void)
{
    Adc_ValueType value = 0u;

    TEST_ASSERT_EQUAL(E_OK, Adc_Init());
    Stub_Adc_SetRaw((uint8)ADC_CHANNEL_VBATT, 2048u);

    TEST_ASSERT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, &value));
    TEST_ASSERT_EQUAL_UINT16(2048u, value);
}

/**
 * TS-SNS-002: SWREQ-SNS-0031 -- a failed conversion fails the whole reading.
 *
 * Not averaged over the remaining samples. A divider that has come loose reads plausibly low, and
 * averaging over fewer samples would hide exactly that.
 */
static void test_Sns_FailedConversionFailsTheReading(void)
{
    Adc_ValueType value = 0u;

    TEST_ASSERT_EQUAL(E_OK, Adc_Init());
    Stub_Adc_SetRaw((uint8)ADC_CHANNEL_VBATT, 2048u);

    Stub_Adc_FailNextReads(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, &value));
}

/** TS-SNS-003: reading before Init is refused rather than returning a stale or zero count. */
static void test_Sns_ReadBeforeInitIsRefused(void)
{
    Adc_ValueType value = 0u;

    TEST_ASSERT_NOT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, &value));
}

/** TS-SNS-004: an unknown channel and a NULL destination are refused. */
static void test_Sns_RejectsBadArguments(void)
{
    Adc_ValueType value = 0u;

    TEST_ASSERT_EQUAL(E_OK, Adc_Init());

    TEST_ASSERT_NOT_EQUAL(E_OK, Adc_ReadChannel((Adc_ChannelType)0x7Fu, &value));
    TEST_ASSERT_NOT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, NULL_PTR));
}

/**
 * TS-SNS-005: the oversampling takes the configured number of samples.
 *
 * ::ADC_OVERSAMPLE_COUNT samples per reading. Counted rather than assumed, because a loop that took
 * one sample would produce an identical answer for a steady input -- so the averaging would look
 * correct while doing nothing, and the outlier rejection in TS-SNS-006 would silently not happen.
 */
static void test_Sns_TakesConfiguredSampleCount(void)
{
    Adc_ValueType value = 0u;
    uint32 before;
    uint32 after;

    TEST_ASSERT_EQUAL(E_OK, Adc_Init());
    Stub_Adc_SetRaw((uint8)ADC_CHANNEL_VBATT, 1000u);

    before = Stub_Adc_GetReadCount();
    TEST_ASSERT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, &value));
    after = Stub_Adc_GetReadCount();

    TEST_ASSERT_EQUAL_UINT32((uint32)ADC_OVERSAMPLE_COUNT, after - before);
}

/**
 * TS-SNS-006: SWREQ-SNS-0030 -- a single extreme outlier is rejected, not averaged in.
 *
 * The behaviour that justifies the trimming. This part produces isolated single-sample outliers near
 * a switching supply; a plain mean of seven samples including one at full scale would be pulled
 * about 290 counts off, which on this divider is well over a volt.
 */
static void test_Sns_SingleOutlierIsRejected(void)
{
    Adc_ValueType value = 0u;
    uint16 samples[ADC_OVERSAMPLE_COUNT];
    uint8 i;

    TEST_ASSERT_EQUAL(E_OK, Adc_Init());

    /* Six samples at 1000, one at full scale. With the highest discarded the answer is exactly 1000;
     * a plain mean would be (6 * 1000 + 4095) / 7 = 1442. */
    for (i = 0u; i < (uint8)ADC_OVERSAMPLE_COUNT; i++)
    {
        samples[i] = 1000u;
    }
    samples[3] = (uint16)ADC_MAX_COUNT;

    Stub_Adc_SetRawSequence((uint8)ADC_CHANNEL_VBATT, samples, (uint8)ADC_OVERSAMPLE_COUNT);

    TEST_ASSERT_EQUAL(E_OK, Adc_ReadChannel(ADC_CHANNEL_VBATT, &value));
    TEST_ASSERT_EQUAL_UINT16(1000u, value);
}

/*==================================================================================================
 *  TS-SNS-007 .. 010  Scaling to physical units
 *================================================================================================*/

/**
 * TS-SNS-007: SWREQ-SNS-0038 -- counts convert to millivolts by the calibrated divider ratio.
 *
 * Derived independently: at 12-bit resolution over a 3300 mV reference, one count is
 * 3300 / 4095 mV at the pin. The divider multiplies that by its ratio, held as ratio x 1000.
 *
 * Half scale (2048 counts) with the default divider of 34.848:
 *     pin  = 2048 * 3300 / 4095       = 1650.5 mV
 *     pack = 1650.5 * 34848 / 1000    = 57 515 mV
 */
static void test_Sns_CountsToMilliVolts(void)
{
    const uint16 result = IoHwAb_CountsToMilliVolts(2048u, 34848u, 0);

    /* Within a few millivolts to allow for the order of the integer operations. */
    TEST_ASSERT_UINT16_WITHIN(20u, 57515u, result);
}

/** TS-SNS-008: zero counts is zero millivolts, whatever the divider. */
static void test_Sns_ZeroCountsIsZeroVolts(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, IoHwAb_CountsToMilliVolts(0u, 34848u, 0));
    TEST_ASSERT_EQUAL_UINT16(0u, IoHwAb_CountsToMilliVolts(0u, 1000u, 0));
}

/**
 * TS-SNS-009: SWREQ-SNS-0035 -- the calibration offset is applied, and both signs work.
 *
 * The offset trims a per-unit error, so it must be able to go either way. A positive-only offset
 * would leave half the population uncorrectable.
 */
static void test_Sns_OffsetIsAppliedBothWays(void)
{
    const uint16 nominal = IoHwAb_CountsToMilliVolts(2048u, 34848u, 0);
    const uint16 raised = IoHwAb_CountsToMilliVolts(2048u, 34848u, 500);
    const uint16 lowered = IoHwAb_CountsToMilliVolts(2048u, 34848u, -500);

    TEST_ASSERT_EQUAL_UINT16((uint16)(nominal + 500u), raised);
    TEST_ASSERT_EQUAL_UINT16((uint16)(nominal - 500u), lowered);
}

/**
 * TS-SNS-010: the conversion saturates rather than wrapping.
 *
 * Full scale with a large divider exceeds 16 bits. A wrap would turn the highest possible reading
 * into a near-zero one -- which is an alarm condition, so the fault would present as the opposite of
 * what happened. A negative offset larger than the reading must likewise clamp at zero, not wrap to
 * 65 535.
 */
static void test_Sns_ConversionSaturates(void)
{
    /* dividerMilli is uint16, so the largest expressible ratio is 65.535:1. At full scale the pin is
     * 3300 mV, and 3300 x 65.535 is about 216 000 mV -- comfortably past what uint16 can hold. */
    TEST_ASSERT_EQUAL_UINT16((uint16)IOHWAB_MAX_MILLIVOLTS,
                             IoHwAb_CountsToMilliVolts((uint16)ADC_MAX_COUNT, 65535u, 0));

    /* And a negative offset cannot drive the result below zero. */
    TEST_ASSERT_EQUAL_UINT16(0u, IoHwAb_CountsToMilliVolts(10u, 1000u, -32768));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Hmi_EachIndicatorIsIndependent);
    RUN_TEST(test_Hmi_IndicatorPinsAreDistinct);
    RUN_TEST(test_Hmi_InitTurnsEverythingOff);
    RUN_TEST(test_Hmi_RejectsBadArguments);

    RUN_TEST(test_Hmi_SolidDoesNotToggle);
    RUN_TEST(test_Hmi_SlowBlinkPeriod);
    RUN_TEST(test_Hmi_BlinkRatesMatchConfiguration);
    RUN_TEST(test_Hmi_OffStaysDark);
    RUN_TEST(test_Hmi_PulseRestoresPreviousPattern);
    RUN_TEST(test_Hmi_PulseOverOffReturnsToOff);

    RUN_TEST(test_Hmi_HeartbeatIsUnaffectedByOthers);
    RUN_TEST(test_Hmi_SelfTestLeavesEverythingOff);

    RUN_TEST(test_Sns_SteadyInputReadsBack);
    RUN_TEST(test_Sns_FailedConversionFailsTheReading);
    RUN_TEST(test_Sns_ReadBeforeInitIsRefused);
    RUN_TEST(test_Sns_RejectsBadArguments);
    RUN_TEST(test_Sns_TakesConfiguredSampleCount);
    RUN_TEST(test_Sns_SingleOutlierIsRejected);

    RUN_TEST(test_Sns_CountsToMilliVolts);
    RUN_TEST(test_Sns_ZeroCountsIsZeroVolts);
    RUN_TEST(test_Sns_OffsetIsAppliedBothWays);
    RUN_TEST(test_Sns_ConversionSaturates);

    return UNITY_END();
}

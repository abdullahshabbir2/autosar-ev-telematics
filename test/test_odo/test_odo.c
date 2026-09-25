/**
 * @file    test_odo.c
 * @brief   Unit tests for the odometry software component.
 *
 * This is the number the product exists to produce, so the tests check it against hand-computed
 * physical quantities rather than against the implementation's own arithmetic, and they cover
 * the long-run behaviour that a few-sample test cannot see.
 *
 * @req SWREQ-ODO-0001 .. SWREQ-ODO-0015
 * @verifies TS-ODO-001 .. TS-ODO-016
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "mcal/Fls/Fls.h"
#include "mcal/Gpt/Gpt.h"
#include "services/NvM/NvM.h"
#include "app/OdoSwc/OdoSwc.h"
#include "Stub_Mcal.h"
#include "unity.h"

/*==================================================================================================
 *  Independently computed reference values
 *
 *  Derived from the physical definition, not from the code:
 *
 *      wheel circumference = pi x 19 in x 25.4 mm/in
 *                          = pi x 482.6 mm = 1516.132615... mm
 *      wheel rpm           = motor rpm / 6
 *      distance            = wheel rpm x circumference x minutes
 *
 *  For 5000 motor rpm over 3 s:
 *      wheel rpm = 833.3333...
 *      mm/min    = 833.3333... x 1516.132615 = 1 263 443.8 mm
 *      over 3 s  = 1 263 443.8 x (3/60)      = 63 172.2 mm
 *
 *  A 200 mm tolerance is allowed, which is 0.3 %. The trapezoidal rule is exact at constant
 *  speed, so the only genuine error is the Q32/Q24 quantisation -- measured at 0.046 mm over ten
 *  thousand steps -- and the tolerance simply avoids encoding the exact rounding of one
 *  particular step count into the test.
 *================================================================================================*/

#define ODO_REF_5000RPM_3S_MM 63172uLL
#define ODO_REF_TOLERANCE_MM 200uLL

/** Wheel circumference for the default calibration, in micrometres, for exact reasoning. */
#define ODO_REF_CIRCUMFERENCE_UM 1516133uLL

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Init());
}

void tearDown(void)
{
}

/** Feed a constant speed for @p seconds at a @p stepMs sample period. */
static void driveConstant(uint16 rpm, uint32 seconds, uint32 stepMs)
{
    const uint32 steps = (seconds * 1000u) / stepMs;
    uint32 i;

    /* The first sample only establishes the reference, so one extra is fed to cover the whole
     * requested duration. */
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(rpm, Gpt_GetMonotonicMs(), NULL_PTR));

    for (i = 0u; i < steps; i++)
    {
        Stub_Gpt_AdvanceMs(stepMs);
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(rpm, Gpt_GetMonotonicMs(), NULL_PTR));
    }
}

/*==================================================================================================
 *  TS-ODO-001 .. 003 : the conversion factor
 *================================================================================================*/

/** @test TS-ODO-001 The default calibration produces the physically correct factor. */
static void test_ConversionFactor_MatchesPhysics(void)
{
    const uint32 factor =
        OdoSwc_ComputeConversionFactorQ32(NVM_DEFAULT_TYRE_DIAMETER_MILLI_INCH, NVM_DEFAULT_GEAR_RATIO_MILLI);

    /* C = pi x 19 x 25.4 / (6 x 60000) = 0.00421147948... mm per rpm-millisecond.
     * In Q32 that is 0.00421147948 x 2^32 = 18 088 165.7, so 18 088 166 after rounding down. */
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(2u, 18088166uL, factor, "conversion factor is wrong");

    /* And it is what Init actually adopted. */
    TEST_ASSERT_EQUAL_UINT32(factor, OdoSwc_GetConversionFactorQ32());
}

/** @test TS-ODO-002 The factor scales correctly with tyre diameter and gear ratio. */
static void test_ConversionFactor_ScalesCorrectly(void)
{
    const uint32 base = OdoSwc_ComputeConversionFactorQ32(19000u, 6000u);
    const uint32 doubleTyre = OdoSwc_ComputeConversionFactorQ32(38000u, 6000u);
    const uint32 doubleGear = OdoSwc_ComputeConversionFactorQ32(19000u, 12000u);

    /* Distance is proportional to tyre diameter and inversely proportional to gear ratio. Both
     * relations hold to within the Q32 rounding of one least significant bit. */
    TEST_ASSERT_UINT32_WITHIN(2u, base * 2u, doubleTyre);
    TEST_ASSERT_UINT32_WITHIN(2u, base / 2u, doubleGear);
}

/** @test TS-ODO-003 Calibrations outside the plausible range are rejected. */
static void test_ConversionFactor_RejectsImplausibleCalibration(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, OdoSwc_ComputeConversionFactorQ32(0u, 6000u));
    TEST_ASSERT_EQUAL_UINT32(0u, OdoSwc_ComputeConversionFactorQ32(19000u, 0u));
    TEST_ASSERT_EQUAL_UINT32(
        0u, OdoSwc_ComputeConversionFactorQ32((uint16)(ODO_MIN_TYRE_MILLI_INCH - 1u), 6000u));
    TEST_ASSERT_EQUAL_UINT32(0u, OdoSwc_ComputeConversionFactorQ32(19000u, 65535u));

    /* The boundaries themselves are accepted. */
    TEST_ASSERT_NOT_EQUAL(
        0u, OdoSwc_ComputeConversionFactorQ32(ODO_MIN_TYRE_MILLI_INCH, ODO_MIN_GEAR_RATIO_MILLI));
    TEST_ASSERT_NOT_EQUAL(
        0u, OdoSwc_ComputeConversionFactorQ32(ODO_MAX_TYRE_MILLI_INCH, ODO_MAX_GEAR_RATIO_MILLI));
}

/*==================================================================================================
 *  TS-ODO-004 .. 007 : integration
 *================================================================================================*/

/** @test TS-ODO-004 A constant speed over a known interval yields the hand-computed distance. */
static void test_Integration_MatchesHandComputedDistance(void)
{
    OdoSwc_StateType state;

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(5000u, Gpt_GetMonotonicMs(), NULL_PTR));
    Stub_Gpt_AdvanceMs(3000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(5000u, Gpt_GetMonotonicMs(), NULL_PTR));

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&state));
    TEST_ASSERT_UINT64_WITHIN(ODO_REF_TOLERANCE_MM, ODO_REF_5000RPM_3S_MM, state.totalDistanceMm);

    /* Trip and total advance together from zero. */
    TEST_ASSERT_EQUAL_UINT64(state.totalDistanceMm, state.tripDistanceMm);
}

/** @test TS-ODO-005 The first sample establishes a reference and adds no distance. */
static void test_Integration_FirstSampleAddsNothing(void)
{
    OdoSwc_StateType state;
    OdoSwc_SampleResultType result;

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(5000u, Gpt_GetMonotonicMs(), &result));
    TEST_ASSERT_EQUAL(ODO_SAMPLE_FIRST, result);

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&state));
    TEST_ASSERT_EQUAL_UINT64(0uLL, state.totalDistanceMm);

    /* But instantaneous speed is available immediately, so a display is not blank for one cycle. */
    TEST_ASSERT_GREATER_THAN_UINT32(0u, state.speedMmPerSec);
}

/**
 * @test TS-ODO-006 Integration uses the measured interval, not an assumed one.
 *
 * v1 multiplied by a compile-time three seconds while its own acquisition loop blocked for far
 * longer, so the distance was wrong by whatever the overrun happened to be. Two runs whose
 * sample periods differ by a factor of two must produce the same distance for the same elapsed
 * time.
 */
static void test_Integration_UsesMeasuredInterval(void)
{
    OdoSwc_StateType fast;
    OdoSwc_StateType slow;

    driveConstant(3000u, 60u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&fast));

    /* Fresh start, same 60 s at the same speed but sampled every 2 s. */
    setUp();
    driveConstant(3000u, 60u, 2000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&slow));

    /* Within 0.1 %: the sample period must not influence the result. */
    TEST_ASSERT_UINT64_WITHIN_MESSAGE(fast.totalDistanceMm / 1000uLL, fast.totalDistanceMm,
                                      slow.totalDistanceMm, "distance depends on the sample period");
}

/**
 * @test TS-ODO-007 Acceleration is integrated by the trapezoidal rule.
 *
 * Ramping linearly from 0 to 6000 rpm over 10 s covers exactly the same distance as 10 s at the
 * mean speed of 3000 rpm. An endpoint rule would be out by half a sample's worth at each step
 * and would not close on the analytic answer.
 */
static void test_Integration_TrapezoidalOnAcceleration(void)
{
    OdoSwc_StateType ramp;
    OdoSwc_StateType flat;
    uint32 step;

    /* 10 s ramp in 1 s steps: 0, 600, 1200, ... 6000. */
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(0u, Gpt_GetMonotonicMs(), NULL_PTR));
    for (step = 1u; step <= 10u; step++)
    {
        Stub_Gpt_AdvanceMs(1000u);
        TEST_ASSERT_EQUAL(E_OK,
                          OdoSwc_ProcessSpeedSample((uint16)(step * 600u), Gpt_GetMonotonicMs(), NULL_PTR));
    }
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&ramp));

    setUp();
    driveConstant(3000u, 10u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&flat));

    /* The two must agree to within the Q32 rounding of the individual steps. */
    TEST_ASSERT_UINT64_WITHIN_MESSAGE(16uLL, flat.totalDistanceMm, ramp.totalDistanceMm,
                                      "a linear ramp did not integrate to its mean speed");
}

/*==================================================================================================
 *  TS-ODO-008 : accumulator exactness
 *================================================================================================*/

/**
 * @test TS-ODO-008 Ten thousand small increments accumulate without drift.
 *
 * This is the v1 defect made visible. v1 accumulated into a float and truncated on every
 * persist; either mechanism loses a fraction of each increment, always downward, so the error
 * grows without bound. Here the sub-millimetre remainder is carried, so the total after 10 000
 * identical steps must equal 10 000 times one step to the millimetre.
 *
 * At 300 rpm over 100 ms each step is only about 0.13 mm -- entirely fractional -- so an
 * implementation that truncated per step would accumulate exactly zero.
 */
static void test_Accumulator_DoesNotDriftOverManySmallSteps(void)
{
    OdoSwc_StateType state;
    uint32 i;
    uint64 expectedUm;

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(300u, Gpt_GetMonotonicMs(), NULL_PTR));
    for (i = 0u; i < 10000u; i++)
    {
        Stub_Gpt_AdvanceMs(100u);
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(300u, Gpt_GetMonotonicMs(), NULL_PTR));
    }

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&state));

    /* Analytic answer: 300 motor rpm / 6 = 50 wheel rpm; 10 000 x 100 ms = 1000 s = 16.6667 min;
     * 50 x 16.6667 = 833.333 wheel revolutions; x 1 516 195 um = 1 263 495 833 um = 1263.5 m. */
    expectedUm = (50uLL * 10000uLL * 100uLL * ODO_REF_CIRCUMFERENCE_UM) / 60000uLL;

    TEST_ASSERT_UINT64_WITHIN_MESSAGE(2uLL, expectedUm / 1000uLL, state.totalDistanceMm,
                                      "the accumulator drifted; the sub-millimetre remainder "
                                      "is not being carried between samples");

    /* And it is not zero, which is what a per-step truncation would have produced. */
    TEST_ASSERT_GREATER_THAN_UINT64(1000000uLL, state.totalDistanceMm);
}

/*==================================================================================================
 *  TS-ODO-009 .. 011 : plausibility
 *================================================================================================*/

/** @test TS-ODO-009 An implausible motor speed is discarded, not integrated. */
static void test_Plausibility_RejectsImpossibleRpm(void)
{
    OdoSwc_StateType before;
    OdoSwc_StateType after;
    OdoSwc_SampleResultType result;

    driveConstant(3000u, 10u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&before));

    /* A corrupted frame decoding to 60 000 rpm. Integrated, it would add hundreds of metres. */
    Stub_Gpt_AdvanceMs(3000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(60000u, Gpt_GetMonotonicMs(), &result));
    TEST_ASSERT_EQUAL(ODO_SAMPLE_REJECTED_RPM, result);

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before.totalDistanceMm, after.totalDistanceMm,
                                     "an implausible sample was integrated");
    TEST_ASSERT_EQUAL_UINT32(1u, after.rejectedRpmSamples);
}

/**
 * @test TS-ODO-010 A long gap between samples is not integrated.
 *
 * Integrating across a dropout would assume the vehicle held its last speed for the whole gap,
 * turning a communications fault into fabricated distance.
 */
static void test_Plausibility_RejectsLongGap(void)
{
    OdoSwc_StateType before;
    OdoSwc_StateType after;
    OdoSwc_SampleResultType result;

    driveConstant(3000u, 10u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&before));

    /* An hour of silence, then a sample. */
    Stub_Gpt_AdvanceMs(3600000uL);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(3000u, Gpt_GetMonotonicMs(), &result));
    TEST_ASSERT_EQUAL(ODO_SAMPLE_REJECTED_GAP, result);

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before.totalDistanceMm, after.totalDistanceMm,
                                     "distance was fabricated across a sample gap");
    TEST_ASSERT_EQUAL_UINT32(1u, after.gapCount);

    /* And the next interval integrates normally: a gap must not disable the odometer. */
    Stub_Gpt_AdvanceMs(1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(3000u, Gpt_GetMonotonicMs(), &result));
    TEST_ASSERT_EQUAL(ODO_SAMPLE_ACCEPTED, result);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_GREATER_THAN_UINT64(before.totalDistanceMm, after.totalDistanceMm);
}

/** @test TS-ODO-011 A repeated timestamp adds nothing. */
static void test_Plausibility_RejectsDuplicateTimestamp(void)
{
    OdoSwc_StateType before;
    OdoSwc_StateType after;
    OdoSwc_SampleResultType result;

    driveConstant(3000u, 5u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&before));

    /* Same instant, no time advance. */
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(3000u, Gpt_GetMonotonicMs(), &result));
    TEST_ASSERT_EQUAL(ODO_SAMPLE_REJECTED_ORDER, result);

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_EQUAL_UINT64(before.totalDistanceMm, after.totalDistanceMm);
}

/*==================================================================================================
 *  TS-ODO-012 .. 014 : persistence
 *================================================================================================*/

/**
 * @test TS-ODO-012 The distance survives a power cycle with its fractional part intact.
 *
 * The headline v1 defect: the value was read back with atol(), which parses an integer, so the
 * fraction was lost on every boot and -- because the truncated value was written straight back --
 * the loss compounded.
 */
static void test_Persistence_SurvivesRestartExactly(void)
{
    OdoSwc_StateType before;
    OdoSwc_StateType after;

    /* Enough distance to cross the persist threshold, and deliberately not a whole number of
     * metres, so a truncating implementation is visible. */
    driveConstant(4000u, 120u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Persist());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&before));
    TEST_ASSERT_GREATER_THAN_UINT64(100000uLL, before.totalDistanceMm);

    /* Power cycle: the flash media survives, everything else is rebuilt. */
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Init());

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before.totalDistanceMm, after.totalDistanceMm,
                                     "distance was not preserved exactly across a restart");
    TEST_ASSERT_EQUAL_UINT64(before.tripDistanceMm, after.tripDistanceMm);
}

/**
 * @test TS-ODO-013 Repeated power cycles do not erode the reading.
 *
 * The compounding part of the v1 defect. Twenty restarts with no driving in between must leave
 * the value bit-identical; a truncating round trip would visibly walk it down.
 */
static void test_Persistence_DoesNotErodeAcrossManyRestarts(void)
{
    OdoSwc_StateType original;
    OdoSwc_StateType current;
    uint8 cycle;

    driveConstant(4000u, 200u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Persist());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&original));

    for (cycle = 0u; cycle < 20u; cycle++)
    {
        TEST_ASSERT_EQUAL(E_OK, Fee_Init());
        TEST_ASSERT_EQUAL(E_OK, NvM_Init());
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_Init());
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_Persist());
    }

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&current));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(original.totalDistanceMm, current.totalDistanceMm,
                                     "the reading eroded across restarts");
}

/**
 * @test TS-ODO-014 A parked vehicle causes no flash writes.
 *
 * Flash endurance is the scarce resource. A stationary vehicle accumulating nothing must not
 * consume any of it, however long it idles.
 */
static void test_Persistence_ParkedVehicleWritesNothing(void)
{
    NvM_StatisticsType before;
    NvM_StatisticsType after;
    uint32 tick;

    driveConstant(3000u, 30u, 1000u);

    /* Bring the vehicle to rest *before* taking the baseline. The first zero-rpm sample still
     * carries real distance -- the trapezoidal rule integrates the deceleration interval from
     * 3000 rpm down to 0, which the vehicle genuinely travelled -- so measuring from before it
     * would count that legitimate write against the parked period. */
    Stub_Gpt_AdvanceMs(1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(0u, Gpt_GetMonotonicMs(), NULL_PTR));
    Stub_Gpt_AdvanceMs(1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(0u, Gpt_GetMonotonicMs(), NULL_PTR));

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Persist());
    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&before));

    /* Two hours parked: zero rpm, main function running throughout. */
    for (tick = 0u; tick < 7200u; tick++)
    {
        Stub_Gpt_AdvanceMs(1000u);
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(0u, Gpt_GetMonotonicMs(), NULL_PTR));
        OdoSwc_MainFunction();
    }

    TEST_ASSERT_EQUAL(E_OK, NvM_GetStatistics(&after));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before.writeCount, after.writeCount,
                                     "a parked vehicle consumed flash endurance");
}

/** @test TS-ODO-014b A moving vehicle persists at the configured distance threshold. */
static void test_Persistence_WritesAtDistanceThreshold(void)
{
    OdoSwc_StateType state;
    uint32 tick;

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&state));
    TEST_ASSERT_EQUAL_UINT32(0u, state.persistCount);

    /* 4000 rpm is about 16.8 m/s, so 100 m arrives in roughly 6 s. */
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(4000u, Gpt_GetMonotonicMs(), NULL_PTR));
    for (tick = 0u; tick < 60u; tick++)
    {
        Stub_Gpt_AdvanceMs(1000u);
        TEST_ASSERT_EQUAL(E_OK, OdoSwc_ProcessSpeedSample(4000u, Gpt_GetMonotonicMs(), NULL_PTR));
        OdoSwc_MainFunction();
    }

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&state));

    /* 60 s at 16.8 m/s is about 1000 m, so roughly ten writes at a 100 m threshold. Checked as a
     * range rather than an exact count, because the precise boundary depends on where the
     * samples fall. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(8u, state.persistCount);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(12u, state.persistCount);

    /* And the unpersisted remainder is always below the threshold. */
    TEST_ASSERT_LESS_THAN_UINT64(ODO_PERSIST_DISTANCE_MM, state.unpersistedMm);
}

/*==================================================================================================
 *  TS-ODO-015 .. 016 : trip and calibration
 *================================================================================================*/

/** @test TS-ODO-015 Resetting the trip leaves the lifetime total untouched. */
static void test_Trip_ResetDoesNotAffectTotal(void)
{
    OdoSwc_StateType before;
    OdoSwc_StateType after;

    driveConstant(3000u, 60u, 1000u);
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&before));
    TEST_ASSERT_GREATER_THAN_UINT64(0uLL, before.tripDistanceMm);

    TEST_ASSERT_EQUAL(E_OK, OdoSwc_ResetTrip());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));

    TEST_ASSERT_EQUAL_UINT64(0uLL, after.tripDistanceMm);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before.totalDistanceMm, after.totalDistanceMm,
                                     "a trip reset changed the lifetime total");

    /* And the reset itself is durable. */
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Init());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_GetState(&after));
    TEST_ASSERT_EQUAL_UINT64(0uLL, after.tripDistanceMm);
    TEST_ASSERT_EQUAL_UINT64(before.totalDistanceMm, after.totalDistanceMm);
}

/** @test TS-ODO-016 A new calibration takes effect and survives a restart. */
static void test_Calibration_AppliesAndPersists(void)
{
    const uint32 original = OdoSwc_GetConversionFactorQ32();

    /* A smaller tyre and a taller gear: distance per revolution must fall. */
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_SetCalibration(16000u, 7000u));
    TEST_ASSERT_NOT_EQUAL(original, OdoSwc_GetConversionFactorQ32());
    TEST_ASSERT_EQUAL_UINT32(OdoSwc_ComputeConversionFactorQ32(16000u, 7000u),
                             OdoSwc_GetConversionFactorQ32());
    TEST_ASSERT_LESS_THAN_UINT32(original, OdoSwc_GetConversionFactorQ32());

    /* Written through to NvM, so a restart keeps it. */
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteImmediate(NVM_BLOCK_CALIBRATION));
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    TEST_ASSERT_EQUAL(E_OK, OdoSwc_Init());

    TEST_ASSERT_EQUAL_UINT32(OdoSwc_ComputeConversionFactorQ32(16000u, 7000u),
                             OdoSwc_GetConversionFactorQ32());

    /* An implausible calibration is refused and leaves the previous one in force. */
    {
        const uint32 current = OdoSwc_GetConversionFactorQ32();
        TEST_ASSERT_EQUAL(E_NOT_OK, OdoSwc_SetCalibration(0u, 6000u));
        TEST_ASSERT_EQUAL_UINT32(current, OdoSwc_GetConversionFactorQ32());
    }
}

/** @test TS-ODO-016b The API rejects use before Init and NULL output pointers. */
static void test_Api_ParameterChecking(void)
{
    TEST_ASSERT_EQUAL(E_NOT_OK, OdoSwc_GetState(NULL_PTR));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ConversionFactor_MatchesPhysics);
    RUN_TEST(test_ConversionFactor_ScalesCorrectly);
    RUN_TEST(test_ConversionFactor_RejectsImplausibleCalibration);
    RUN_TEST(test_Integration_MatchesHandComputedDistance);
    RUN_TEST(test_Integration_FirstSampleAddsNothing);
    RUN_TEST(test_Integration_UsesMeasuredInterval);
    RUN_TEST(test_Integration_TrapezoidalOnAcceleration);
    RUN_TEST(test_Accumulator_DoesNotDriftOverManySmallSteps);
    RUN_TEST(test_Plausibility_RejectsImpossibleRpm);
    RUN_TEST(test_Plausibility_RejectsLongGap);
    RUN_TEST(test_Plausibility_RejectsDuplicateTimestamp);
    RUN_TEST(test_Persistence_SurvivesRestartExactly);
    RUN_TEST(test_Persistence_DoesNotErodeAcrossManyRestarts);
    RUN_TEST(test_Persistence_ParkedVehicleWritesNothing);
    RUN_TEST(test_Persistence_WritesAtDistanceThreshold);
    RUN_TEST(test_Trip_ResetDoesNotAffectTotal);
    RUN_TEST(test_Calibration_AppliesAndPersists);
    RUN_TEST(test_Api_ParameterChecking);
    return UNITY_END();
}

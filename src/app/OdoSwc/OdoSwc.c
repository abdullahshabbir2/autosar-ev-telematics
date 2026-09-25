/**
 * @file    OdoSwc.c
 * @brief   Odometry software component implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "app/OdoSwc/OdoSwc.h"

#include "services/Det/Det.h"
#include "services/NvM/NvM.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean OdoSwc_Initialised = FALSE;

/** Conversion factor, Q32, in millimetres per rpm-millisecond. */
STATIC uint32 OdoSwc_FactorQ32;

/** Calibrated motor speed above which a sample is discarded. */
STATIC uint16 OdoSwc_MaxPlausibleRpm;

STATIC uint64 OdoSwc_TotalMm;
STATIC uint64 OdoSwc_TripMm;

/**
 * @brief Sub-millimetre remainder carried between samples, in units of 2^-32 mm.
 *
 * This single variable is what makes the accumulation exact. Without it, each sample would
 * truncate its own fractional millimetre and the loss -- always downward -- would accumulate at
 * roughly half a millimetre per sample, which at one sample every three seconds is about five
 * metres a day, every day, for the life of the vehicle.
 */
STATIC uint64 OdoSwc_FractionAcc;

STATIC uint16 OdoSwc_LastRpm;
STATIC Gpt_TimestampType OdoSwc_LastSampleTime;
STATIC boolean OdoSwc_HaveLastSample;

STATIC uint32 OdoSwc_SpeedMmPerSec;
STATIC uint32 OdoSwc_AcceptedSamples;
STATIC uint32 OdoSwc_RejectedRpmSamples;
STATIC uint32 OdoSwc_GapCount;
STATIC uint32 OdoSwc_PersistCount;

/** Total at the moment of the last successful persist, for the distance threshold. */
STATIC uint64 OdoSwc_LastPersistedMm;
STATIC Gpt_TimestampType OdoSwc_LastPersistTime;

/*==================================================================================================
 *  Conversion factor
 *================================================================================================*/

uint32 OdoSwc_ComputeConversionFactorQ32(uint16 tyreDiameterMilliInch, uint16 gearRatioMilli)
{
    uint64 numerator;
    uint64 denominator;

    if ((tyreDiameterMilliInch < ODO_MIN_TYRE_MILLI_INCH) || (tyreDiameterMilliInch > ODO_MAX_TYRE_MILLI_INCH)
        || (gearRatioMilli < ODO_MIN_GEAR_RATIO_MILLI) || (gearRatioMilli > ODO_MAX_GEAR_RATIO_MILLI))
    {
        return 0u;
    }

    /* C = pi * D_in * 25.4 / (gearRatio * 60000)  [mm per rpm-millisecond]
     *
     * Substituting D_in = tyreMilliInch / 1000, 25.4 = 254/10 and gearRatio = gearRatioMilli/1000:
     *
     *   C = pi * tyreMilliInch * 254 / (600000 * gearRatioMilli)
     *
     * Wanted in Q32, with pi supplied as ODO_PI_Q24 (pi * 2^24):
     *
     *   C_q32 = ODO_PI_Q24 * tyreMilliInch * 254 * 2^32
     *           ------------------------------------------
     *                2^24 * 600000 * gearRatioMilli
     *
     *         = ODO_PI_Q24 * tyreMilliInch * 254 * 256 / (600000 * gearRatioMilli)
     *
     * Evaluated entirely in 64-bit integers: no floating point is involved anywhere in the
     * odometer, so two units given the same calibration compute bit-identical distances
     * regardless of compiler or optimisation level.
     *
     * Worst-case numerator, at the largest accepted tyre, is 1.03e17 against a uint64 limit of
     * 1.8e19 -- two orders of margin. */
    numerator = (uint64)ODO_PI_Q24 * (uint64)tyreDiameterMilliInch * ODO_MM_PER_INCH_X10
                * (uint64)(1uL << (ODO_FIXED_SHIFT - ODO_PI_SHIFT));
    denominator = 600000uLL * (uint64)gearRatioMilli;

    return (uint32)(numerator / denominator);
}

uint32 OdoSwc_GetConversionFactorQ32(void)
{
    return OdoSwc_FactorQ32;
}

/*==================================================================================================
 *  Persistence helpers
 *================================================================================================*/

/** Write the current totals to NvM. */
STATIC Std_ReturnType OdoSwc_Commit(void)
{
    NvM_OdometerType block;

    if (NvM_ReadBlock(NVM_BLOCK_ODOMETER, &block) != E_OK)
    {
        return E_NOT_OK;
    }

    block.structVersion = NVM_STRUCT_VERSION;
    block.reserved0 = 0u;
    block.totalDistanceMm = OdoSwc_TotalMm;
    block.tripDistanceMm = OdoSwc_TripMm;
    block.updateCount++;

    if (NvM_WriteBlock(NVM_BLOCK_ODOMETER, &block) != E_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_MAIN_FUNCTION,
                                     ODOSWC_E_PERSIST_FAILED);
        return E_NOT_OK;
    }

    OdoSwc_LastPersistedMm = OdoSwc_TotalMm;
    OdoSwc_LastPersistTime = Gpt_GetMonotonicMs();
    OdoSwc_PersistCount++;
    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType OdoSwc_Init(void)
{
    NvM_OdometerType odometer;
    NvM_CalibrationType calibration;

    OdoSwc_FractionAcc = 0uLL;
    OdoSwc_LastRpm = 0u;
    OdoSwc_LastSampleTime = 0u;
    OdoSwc_HaveLastSample = FALSE;
    OdoSwc_SpeedMmPerSec = 0u;
    OdoSwc_AcceptedSamples = 0u;
    OdoSwc_RejectedRpmSamples = 0u;
    OdoSwc_GapCount = 0u;
    OdoSwc_PersistCount = 0u;
    OdoSwc_Initialised = FALSE;

    if ((NvM_ReadBlock(NVM_BLOCK_ODOMETER, &odometer) != E_OK)
        || (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK))
    {
        return E_NOT_OK;
    }

    OdoSwc_TotalMm = odometer.totalDistanceMm;
    OdoSwc_TripMm = odometer.tripDistanceMm;
    OdoSwc_LastPersistedMm = OdoSwc_TotalMm;
    OdoSwc_LastPersistTime = Gpt_GetMonotonicMs();

    OdoSwc_FactorQ32 =
        OdoSwc_ComputeConversionFactorQ32(calibration.tyreDiameterMilliInch, calibration.gearRatioMilli);
    OdoSwc_MaxPlausibleRpm = calibration.maxPlausibleRpm;

    if (OdoSwc_FactorQ32 == 0u)
    {
        /* An unusable calibration means no trustworthy distance can be produced. Accumulating
         * with a guessed factor would corrupt the lifetime total permanently, so the component
         * stays uninitialised and the fault is raised instead. */
        (void)Det_ReportError(MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_INIT,
                              ODOSWC_E_BAD_CALIBRATION);
        return E_NOT_OK;
    }

    if (OdoSwc_MaxPlausibleRpm == 0u)
    {
        OdoSwc_MaxPlausibleRpm = NVM_DEFAULT_MAX_PLAUSIBLE_RPM;
    }

    OdoSwc_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType OdoSwc_ProcessSpeedSample(uint16 motorRpm, Gpt_TimestampType sampleTime,
                                         OdoSwc_SampleResultType *result)
{
    OdoSwc_SampleResultType outcome = ODO_SAMPLE_ACCEPTED;

    DET_CHECK_RETURN(OdoSwc_Initialised != FALSE, MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE,
                     ODOSWC_API_ID_PROCESS_SAMPLE, ODOSWC_E_UNINIT, E_NOT_OK);

    /* Instantaneous speed is derived from every sample, even one that is not integrated, so a
     * display keeps updating while the accumulator is being conservative. mm/s = rpm * C * 1000. */
    if (motorRpm <= OdoSwc_MaxPlausibleRpm)
    {
        OdoSwc_SpeedMmPerSec =
            (uint32)(((uint64)motorRpm * (uint64)OdoSwc_FactorQ32 * 1000uLL) >> ODO_FIXED_SHIFT);
    }

    if (motorRpm > OdoSwc_MaxPlausibleRpm)
    {
        /* A corrupted frame decoding to an impossible speed would add kilometres in one step.
         * Discard it, and resynchronise the reference so the next interval is measured from now
         * rather than spanning the discarded one. */
        OdoSwc_RejectedRpmSamples++;
        OdoSwc_LastSampleTime = sampleTime;
        OdoSwc_HaveLastSample = FALSE;
        (void)Det_ReportRuntimeError(MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_PROCESS_SAMPLE,
                                     ODOSWC_E_IMPLAUSIBLE_RPM);
        outcome = ODO_SAMPLE_REJECTED_RPM;
    }
    else if (OdoSwc_HaveLastSample == FALSE)
    {
        /* Nothing to integrate over yet: record the reference and wait for the next sample. */
        OdoSwc_LastRpm = motorRpm;
        OdoSwc_LastSampleTime = sampleTime;
        OdoSwc_HaveLastSample = TRUE;
        outcome = ODO_SAMPLE_FIRST;
    }
    else
    {
        /* Declared here rather than at the top of the function: it has no meaning outside this branch,
         * and the narrower scope is what stops a later edit reading it on the first-sample path where it
         * would hold nothing. Wrap-safe subtraction, per CS-TIME-01. */
        const uint32 deltaMs = (uint32)(sampleTime - OdoSwc_LastSampleTime);

        if (deltaMs == 0u)
        {
            /* The same instant twice: a duplicate delivery, not elapsed time. */
            OdoSwc_LastRpm = motorRpm;
            outcome = ODO_SAMPLE_REJECTED_ORDER;
        }
        else if (deltaMs > ODO_MAX_SAMPLE_GAP_MS)
        {
            /* Samples were missed. Integrating across the gap would assume the vehicle held its
             * last known speed throughout it, which is how a communications dropout becomes
             * fabricated distance. Discard the interval and resynchronise. */
            OdoSwc_GapCount++;
            OdoSwc_LastRpm = motorRpm;
            OdoSwc_LastSampleTime = sampleTime;
            (void)Det_ReportRuntimeError(MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_PROCESS_SAMPLE,
                                         ODOSWC_E_SAMPLE_GAP);
            outcome = ODO_SAMPLE_REJECTED_GAP;
        }
        else
        {
            /* Trapezoidal rule: the mean of the speeds at both ends of the interval. Exact for
             * any linear change in speed, which is a good approximation of a vehicle over a few
             * seconds, and free of the systematic bias that either endpoint rule carries.
             *
             * The sum of two rpm values is at most 24 000 and the interval at most 10 000 ms, so
             * the product with the factor stays below 7e15 against a uint64 limit of 1.8e19. */
            const uint64 rpmSum = (uint64)OdoSwc_LastRpm + (uint64)motorRpm;
            const uint64 increment = (rpmSum * (uint64)deltaMs * (uint64)OdoSwc_FactorQ32) / 2uLL;

            OdoSwc_FractionAcc += increment;

            /* Carry out whole millimetres and keep the remainder, so nothing is lost to
             * truncation on any individual step. */
            OdoSwc_TotalMm += (OdoSwc_FractionAcc >> ODO_FIXED_SHIFT);
            OdoSwc_TripMm += (OdoSwc_FractionAcc >> ODO_FIXED_SHIFT);
            OdoSwc_FractionAcc &= ODO_FIXED_MASK;

            OdoSwc_LastRpm = motorRpm;
            OdoSwc_LastSampleTime = sampleTime;
            OdoSwc_AcceptedSamples++;
            outcome = ODO_SAMPLE_ACCEPTED;
        }
    }

    if (result != NULL_PTR)
    {
        *result = outcome;
    }
    return E_OK;
}

void OdoSwc_MainFunction(void)
{
    uint64 advanced;

    if (OdoSwc_Initialised == FALSE)
    {
        return;
    }

    advanced = OdoSwc_TotalMm - OdoSwc_LastPersistedMm;

    /* Distance first: on a moving vehicle it is what bounds the loss from an unexpected power
     * cut. The time limit exists only so that a very slow vehicle still records progress. */
    if (advanced >= ODO_PERSIST_DISTANCE_MM)
    {
        (void)OdoSwc_Commit();
    }
    else if ((advanced > 0uLL) && (Gpt_HasElapsed(OdoSwc_LastPersistTime, ODO_PERSIST_INTERVAL_MS) != FALSE))
    {
        (void)OdoSwc_Commit();
    }
    else
    {
        /* Nothing has moved. NvM's write-on-change would suppress the write anyway, but not
         * calling it at all keeps a parked vehicle entirely off the flash. */
    }
}

Std_ReturnType OdoSwc_Persist(void)
{
    DET_CHECK_RETURN(OdoSwc_Initialised != FALSE, MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE,
                     ODOSWC_API_ID_MAIN_FUNCTION, ODOSWC_E_UNINIT, E_NOT_OK);

    return OdoSwc_Commit();
}

Std_ReturnType OdoSwc_GetState(OdoSwc_StateType *state)
{
    DET_CHECK_RETURN(state != NULL_PTR, MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_GET_STATE,
                     ODOSWC_E_PARAM_POINTER, E_NOT_OK);

    state->totalDistanceMm = OdoSwc_TotalMm;
    state->tripDistanceMm = OdoSwc_TripMm;
    state->speedMmPerSec = OdoSwc_SpeedMmPerSec;
    state->lastRpm = OdoSwc_LastRpm;
    state->acceptedSamples = OdoSwc_AcceptedSamples;
    state->rejectedRpmSamples = OdoSwc_RejectedRpmSamples;
    state->gapCount = OdoSwc_GapCount;
    state->persistCount = OdoSwc_PersistCount;
    state->unpersistedMm = OdoSwc_TotalMm - OdoSwc_LastPersistedMm;

    return E_OK;
}

Std_ReturnType OdoSwc_ResetTrip(void)
{
    DET_CHECK_RETURN(OdoSwc_Initialised != FALSE, MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE,
                     ODOSWC_API_ID_RESET_TRIP, ODOSWC_E_UNINIT, E_NOT_OK);

    OdoSwc_TripMm = 0uLL;

    /* Committed at once. A trip reset the driver performed and then lost to a power cut would
     * be more confusing than one that never appeared to take effect. */
    return OdoSwc_Commit();
}

Std_ReturnType OdoSwc_SetCalibration(uint16 tyreDiameterMilliInch, uint16 gearRatioMilli)
{
    NvM_CalibrationType calibration;
    const uint32 factor = OdoSwc_ComputeConversionFactorQ32(tyreDiameterMilliInch, gearRatioMilli);

    DET_CHECK_RETURN(factor != 0u, MODULE_ID_ODOSWC, INSTANCE_ID_SINGLE, ODOSWC_API_ID_SET_CALIBRATION,
                     ODOSWC_E_BAD_CALIBRATION, E_NOT_OK);

    if (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
    {
        return E_NOT_OK;
    }

    calibration.structVersion = NVM_STRUCT_VERSION;
    calibration.tyreDiameterMilliInch = tyreDiameterMilliInch;
    calibration.gearRatioMilli = gearRatioMilli;

    if (NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
    {
        return E_NOT_OK;
    }

    /* The accumulated remainder was computed under the old factor. Carrying it across would
     * apply the previous calibration to a fraction of a millimetre of the new one -- harmless in
     * magnitude, but it makes the accumulator's history ambiguous, so it is discarded. */
    OdoSwc_FractionAcc = 0uLL;
    OdoSwc_FactorQ32 = factor;
    OdoSwc_HaveLastSample = FALSE;

    return E_OK;
}

void OdoSwc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = ODOSWC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_ODOSWC;
        versioninfo->sw_major_version = ODOSWC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = ODOSWC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = ODOSWC_SW_PATCH_VERSION;
    }
}

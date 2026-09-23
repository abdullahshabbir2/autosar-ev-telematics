/**
 * @file    OdoSwc.h
 * @brief   Odometry software component -- distance and speed from motor speed.
 *
 * Integrates motor RPM over measured time to produce accumulated distance. This is the number
 * the whole product exists to produce, so the arithmetic is specified here in full.
 *
 * @par Conversion
 * Wheel revolutions are motor revolutions divided by the gear ratio, and each wheel revolution
 * advances the vehicle by one tyre circumference:
 *
 *     distance = rpm / gearRatio x pi x tyreDiameter x dt
 *
 * with tyre diameter converted from inches to millimetres and dt from milliseconds to minutes.
 * Folding the constants gives a single factor C, in millimetres per rpm-millisecond:
 *
 *     C = pi x tyreDiameter_in x 25.4 / (gearRatio x 60000)
 *
 * @par Four things this component does differently from v1
 *
 * 1. **Integer accumulation, in millimetres.** v1 accumulated into a @c float holding
 *    kilometres. A binary32 carries about 24 significant bits, so past roughly 130 000 km the
 *    representable step exceeds a centimetre and small increments vanish into rounding -- always
 *    downward, so the error never averages out. Here C is held as a Q32 fixed-point integer and
 *    the sub-millimetre remainder is carried between samples, so the accumulation is exact:
 *    integrating 5 mm ten thousand times gives exactly 50 m, not 49.98 m.
 *
 * 2. **Measured time, not assumed time.** v1 multiplied by a compile-time constant of three
 *    seconds. Its acquisition loop also performed a blocking one-second GPS delay and up to
 *    twelve seconds of RS485 exchanges, so the true interval was never three seconds and the
 *    distance was wrong by whatever the overrun happened to be. ::OdoSwc_ProcessSpeedSample
 *    takes the sample's timestamp and integrates over the interval that actually elapsed.
 *
 * 3. **Trapezoidal integration.** Distance over an interval uses the mean of the speed at both
 *    ends rather than the value at one end. Under acceleration a right-endpoint rule
 *    over-reads and a left-endpoint rule under-reads; the trapezoid is exact for any linear
 *    speed change, which is what a vehicle approximately does between samples 3 s apart.
 *
 * 4. **Implausible samples are rejected, not integrated.** A corrupted CAN frame decoding to
 *    60 000 rpm would have added kilometres in a single step. Samples above the calibrated
 *    limit are counted and discarded, and an interval longer than ::ODO_MAX_SAMPLE_GAP_MS is
 *    treated as a gap in knowledge rather than as time spent at the last known speed.
 *
 * @par Persistence
 * The accumulated value is pushed to NvM when it has advanced by ::ODO_PERSIST_DISTANCE_MM or
 * when ::ODO_PERSIST_INTERVAL_MS has passed, whichever comes first, and unconditionally on the
 * shutdown path. Writing on every sample would exhaust the flash endurance in months; the
 * threshold bounds the worst-case loss on an unexpected power cut to 100 m. The arithmetic
 * behind that choice is in OdoSwc_Cfg.h.
 *
 * @req SWREQ-ODO-0001 .. SWREQ-ODO-0015
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ODOSWC_H
#define ODOSWC_H

#include "Autosar_ModuleIds.h"
#include "Gpt.h"
#include "OdoSwc_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ODOSWC_VENDOR_ID 0xFFFEu
#define ODOSWC_SW_MAJOR_VERSION 2u
#define ODOSWC_SW_MINOR_VERSION 0u
#define ODOSWC_SW_PATCH_VERSION 0u

#define ODOSWC_API_ID_INIT 0x00u
#define ODOSWC_API_ID_PROCESS_SAMPLE 0x20u
#define ODOSWC_API_ID_GET_STATE 0x21u
#define ODOSWC_API_ID_RESET_TRIP 0x22u
#define ODOSWC_API_ID_SET_CALIBRATION 0x23u
#define ODOSWC_API_ID_MAIN_FUNCTION 0x0Eu

#define ODOSWC_E_UNINIT E_UNINIT
#define ODOSWC_E_PARAM_POINTER E_PARAM_POINTER
#define ODOSWC_E_IMPLAUSIBLE_RPM 0x20u
#define ODOSWC_E_SAMPLE_GAP 0x21u
#define ODOSWC_E_BAD_CALIBRATION 0x22u
#define ODOSWC_E_PERSIST_FAILED 0x23u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Why a sample was not integrated. */
typedef enum
{
    ODO_SAMPLE_ACCEPTED = 0,      /**< Integrated normally.                              */
    ODO_SAMPLE_FIRST = 1,         /**< No previous sample, so no interval to integrate.   */
    ODO_SAMPLE_REJECTED_RPM = 2,  /**< Motor speed above the calibrated plausible limit.  */
    ODO_SAMPLE_REJECTED_GAP = 3,  /**< Interval too long to attribute to a known speed.   */
    ODO_SAMPLE_REJECTED_ORDER = 4 /**< Timestamp did not advance.                         */
} OdoSwc_SampleResultType;

/** Everything the component knows, published to telemetry and diagnostics. */
typedef struct
{
    uint64 totalDistanceMm;     /**< Lifetime distance, millimetres.                    */
    uint64 tripDistanceMm;      /**< Distance since the trip was last reset.            */
    uint32 speedMmPerSec;       /**< Instantaneous speed from the most recent sample.    */
    uint16 lastRpm;             /**< Most recent accepted motor speed.                   */
    uint32 acceptedSamples;     /**< Samples integrated.                                 */
    uint32 rejectedRpmSamples;  /**< Samples discarded as implausible.                   */
    uint32 gapCount;            /**< Intervals discarded as too long.                    */
    uint32 persistCount;        /**< Times the value has been pushed to NvM.             */
    uint64 unpersistedMm;       /**< Distance accumulated since the last successful push.*/
} OdoSwc_StateType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Load the stored distance and calibration, and derive the conversion factor.
 *
 * Must run after ::NvM_Init. If the stored odometer block was absent or damaged, NvM will
 * already have applied its default of zero -- a fresh unit reads zero rather than refusing to
 * start.
 *
 * @return E_OK on success; E_NOT_OK if the calibration is unusable, in which case no distance
 *         is accumulated rather than accumulating a wrong one.
 */
CHECK_RETURN Std_ReturnType OdoSwc_Init(void);

/**
 * @brief Integrate one motor-speed sample.
 *
 * @param[in]  motorRpm   Motor speed from the drive, in rpm.
 * @param[in]  sampleTime Monotonic timestamp of the sample, from ::Gpt_GetMonotonicMs.
 * @param[out] result     Why the sample was or was not integrated. May be NULL_PTR.
 * @return E_OK if the call was well formed, whatever the sample's fate; E_NOT_OK only before
 *         ::OdoSwc_Init. A rejected sample is a normal event, not a call failure, so the
 *         outcome is reported through @p result rather than through the return value.
 */
CHECK_RETURN Std_ReturnType OdoSwc_ProcessSpeedSample(uint16 motorRpm,
                                                      Gpt_TimestampType sampleTime,
                                                      OdoSwc_SampleResultType *result);

/**
 * @brief Persist the accumulated distance if it has moved enough or waited long enough.
 *
 * Driven cyclically by SchM.
 */
void OdoSwc_MainFunction(void);

/**
 * @brief Push the accumulated distance to NvM now, regardless of the thresholds.
 *
 * Called by EcuM on the shutdown path so that a controlled power-down loses nothing.
 */
CHECK_RETURN Std_ReturnType OdoSwc_Persist(void);

/**
 * @brief Read the component's state.
 * @param[out] state Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType OdoSwc_GetState(OdoSwc_StateType *state);

/** Zero the trip counter. The lifetime total is never affected. */
CHECK_RETURN Std_ReturnType OdoSwc_ResetTrip(void);

/**
 * @brief Apply a new vehicle calibration and recompute the conversion factor.
 *
 * Reachable from the diagnostic channel so a tyre or gearbox change is a field adjustment. The
 * new values are written to NvM, so they survive a restart.
 *
 * @param tyreDiameterMilliInch Tyre diameter in thousandths of an inch.
 * @param gearRatioMilli        Gear ratio x 1000.
 * @return E_OK on success; E_NOT_OK if either value is outside its plausible range, in which
 *         case the previous calibration stays in force.
 */
CHECK_RETURN Std_ReturnType OdoSwc_SetCalibration(uint16 tyreDiameterMilliInch,
                                                  uint16 gearRatioMilli);

/**
 * @brief The Q32 conversion factor currently in force, in millimetres per rpm-millisecond.
 *
 * Exposed so that the calibration arithmetic can be verified directly, and so a diagnostic
 * read can confirm which calibration a unit is actually running.
 */
uint32 OdoSwc_GetConversionFactorQ32(void);

/**
 * @brief Compute the Q32 conversion factor for a calibration, without applying it.
 *
 * Pure function, no module state. Separated so the conversion can be tested across the whole
 * plausible calibration range independently of the accumulator.
 *
 * @return The factor, or 0 if either argument is outside its plausible range.
 */
uint32 OdoSwc_ComputeConversionFactorQ32(uint16 tyreDiameterMilliInch, uint16 gearRatioMilli);

/**
 * @brief Return this component's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void OdoSwc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* ODOSWC_H */

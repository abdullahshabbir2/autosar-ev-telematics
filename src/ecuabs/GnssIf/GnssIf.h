/**
 * @file    GnssIf.h
 * @brief   GNSS receiver interface -- NMEA 0183 parsing and position validation.
 *
 * Consumes NMEA sentences from the receiver's serial link and publishes a validated position.
 * Written as a self-contained parser rather than a wrapper around a third-party library, for three
 * reasons: the parser is the part most worth testing and a vendored library cannot be tested against
 * the ECU's own fault cases; NMEA is a small, stable, text-based format; and the alternative brings
 * in floating point and dynamic behaviour on a path that runs every acquisition cycle.
 *
 * @par Coordinates are integers
 * Positions are held as signed degrees scaled by 10^7, which is the same representation u-blox
 * receivers use internally. One count is about 11 mm at the equator -- far finer than any consumer
 * GNSS fix -- and the value is exact, so a position logged and later re-read is bit-identical.
 * v1 stored @c double latitude and longitude and then formatted them to six decimal places with
 * Arduino's @c String(value, 6), which both allocates and rounds on every record.
 *
 * @par Four defects in the v1 GNSS handling
 *
 * 1. **Uninitialised state.** @c double @c prevLat, @c prevLon = 0; initialises only @c prevLon.
 *    @c prevLat was left indeterminate (it survived only because it happened to be a zeroed global).
 *
 * 2. **Bitwise operators used as logical ones.** @c if (!prevLat & !prevLon) and
 *    @c ((_latChange < 0.1) & (_lonChange < 0.1) & locChanged) use @c & where @c && was meant.
 *    They happen to give the same answer for 0/1 operands, but they defeat short-circuiting and are
 *    exactly the construct that stops being equivalent the moment an operand is not 0 or 1.
 *
 * 3. **A geographic region compiled into the validity test.** A fix was only accepted at boot if it
 *    fell within latitude 24..38 and longitude 60..78 -- a box around Pakistan. A unit shipped
 *    anywhere else would never accept its first fix, and the constraint appeared nowhere in any
 *    configuration. Plausibility here is checked against how fast the position can physically
 *    change, which holds everywhere on Earth.
 *
 * 4. **No checksum validation.** v1 relied on the library's internal handling and surfaced nothing,
 *    so a corrupted sentence was indistinguishable from a good one. Every sentence is checked here
 *    and failures are counted, which is what makes a marginal receiver wiring visible.
 *
 * @req SWREQ-GNS-0001 .. SWREQ-GNS-0014
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef GNSSIF_H
#define GNSSIF_H

#include "base/Autosar_ModuleIds.h"
#include "ecuabs/GnssIf/GnssIf_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNSSIF_VENDOR_ID 0xFFFEu
#define GNSSIF_SW_MAJOR_VERSION 2u
#define GNSSIF_SW_MINOR_VERSION 0u
#define GNSSIF_SW_PATCH_VERSION 0u

#define GNSSIF_API_ID_INIT 0x00u
#define GNSSIF_API_ID_MAIN_FUNCTION 0x0Eu
#define GNSSIF_API_ID_GET_POSITION 0x20u
#define GNSSIF_API_ID_PARSE_SENTENCE 0x21u

#define GNSSIF_E_UNINIT E_UNINIT
#define GNSSIF_E_PARAM_POINTER E_PARAM_POINTER
#define GNSSIF_E_CHECKSUM 0x20u
#define GNSSIF_E_MALFORMED 0x21u
#define GNSSIF_E_NO_FIX 0x22u
#define GNSSIF_E_IMPLAUSIBLE_JUMP 0x23u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Fix quality, as reported in the GGA sentence's quality field. */
typedef enum
{
    GNSSIF_FIX_NONE = 0,      /**< No fix.                                  */
    GNSSIF_FIX_GPS = 1,       /**< Standalone GNSS fix.                     */
    GNSSIF_FIX_DGPS = 2,      /**< Differentially corrected fix.            */
    GNSSIF_FIX_PPS = 3,       /**< Precise positioning service.             */
    GNSSIF_FIX_RTK = 4,       /**< Real-time kinematic, fixed.              */
    GNSSIF_FIX_RTK_FLOAT = 5, /**< Real-time kinematic, float.              */
    GNSSIF_FIX_ESTIMATED = 6  /**< Dead reckoning. Not accepted as a fix.   */
} GnssIf_FixQualityType;

/** A validated position. */
typedef struct
{
    sint32 latitudeE7;     /**< Latitude in degrees x 10^7, positive north.  */
    sint32 longitudeE7;    /**< Longitude in degrees x 10^7, positive east.  */
    sint32 altitudeMm;     /**< Altitude above mean sea level, millimetres.  */
    uint32 speedMmPerSec;  /**< Ground speed from the receiver.               */
    uint16 headingDeciDeg; /**< Course over ground, 0.1 degree per count.     */
    uint8 satellitesUsed;  /**< Satellites contributing to the fix.           */
    uint8 fixQuality;      /**< ::GnssIf_FixQualityType.                      */
    uint16 hdopCentiUnits; /**< Horizontal dilution of precision x 100.       */
    uint32 fixTimestampMs; /**< Monotonic time the fix was accepted.          */
    uint32 fixUnixTime;    /**< UTC of the fix, 0 if the date was absent.     */
    boolean valid;         /**< TRUE if a fix has been accepted.              */
} GnssIf_PositionType;

/** Parser and receiver counters, published as diagnostic data. */
typedef struct
{
    uint32 sentencesReceived;   /**< Complete sentences seen.                    */
    uint32 sentencesAccepted;   /**< Sentences that parsed and passed checks.     */
    uint32 checksumFailures;    /**< Sentences whose XOR checksum did not match.  */
    uint32 malformedSentences;  /**< Sentences with missing or unparsable fields. */
    uint32 noFixSentences;      /**< Valid sentences reporting no fix.            */
    uint32 rejectedJumps;       /**< Fixes rejected as physically impossible.     */
    uint32 overflowedSentences; /**< Sentences longer than the buffer.            */
    uint32 fixCount;            /**< Fixes accepted since Init.                   */
} GnssIf_StatisticsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Open the receiver's serial link and reset the parser.
 * @return E_OK on success; E_NOT_OK if the UART could not be opened.
 */
CHECK_RETURN Std_ReturnType GnssIf_Init(void);

/** Close the link and release the UART. */
void GnssIf_DeInit(void);

/**
 * @brief Read available bytes from the receiver and parse any complete sentences.
 *
 * Driven cyclically by SchM. Bounded by ::GNSSIF_MAX_BYTES_PER_CYCLE so a receiver that floods the
 * link cannot make one scheduler slot run long.
 *
 * @return Number of sentences accepted during this call.
 */
uint8 GnssIf_MainFunction(void);

/**
 * @brief Read the most recent validated position.
 * @param[out] position Destination.
 * @return E_OK on success; E_NOT_OK for a NULL pointer or before Init.
 *
 * @note Returns E_OK with @c valid clear when no fix has been accepted. Absence of a fix is a
 *       normal state -- indoors, in a tunnel -- not a call failure.
 */
CHECK_RETURN Std_ReturnType GnssIf_GetPosition(GnssIf_PositionType *position);

/**
 * @brief Whether the current fix is recent enough to publish.
 * @return TRUE if a fix has been accepted within ::GNSSIF_FIX_TIMEOUT_MS.
 */
boolean GnssIf_IsFixFresh(void);

/*==================================================================================================
 *  Parser -- pure functions, exposed for test
 *================================================================================================*/

/**
 * @brief Verify an NMEA sentence's XOR checksum.
 *
 * @param[in] sentence NUL-terminated sentence including the leading @c $ and the trailing
 *                     @c *hh, with or without a line terminator.
 * @return E_OK if the checksum matches; ::E_CRC_FAIL if it does not; E_NOT_OK if the sentence is
 *         too short or has no checksum field.
 */
CHECK_RETURN Std_ReturnType GnssIf_VerifyChecksum(const char *sentence);

/**
 * @brief Convert an NMEA @c ddmm.mmmm coordinate to degrees x 10^7.
 *
 * @param[in]  field     The coordinate field, without the hemisphere.
 * @param[in]  hemisphere One of 'N', 'S', 'E', 'W'.
 * @param[out] result    Degrees x 10^7, signed by hemisphere.
 * @return E_OK on success; E_NOT_OK if the field is empty or malformed.
 *
 * @note Performed entirely in integer arithmetic. The obvious implementation --
 *       @c degrees + minutes/60.0 in double -- introduces a rounding error that varies with
 *       position, so two receivers reporting the same coordinate could log different values.
 */
CHECK_RETURN Std_ReturnType GnssIf_ParseCoordinate(const char *field, char hemisphere, sint32 *result);

/**
 * @brief Parse one complete sentence into @p position.
 *
 * Recognises @c RMC and @c GGA from any talker (GP, GN, GL, GA). Other sentences are ignored
 * without being counted as malformed -- a receiver emitting GSV and GSA is behaving normally.
 *
 * @param[in]     sentence NUL-terminated sentence.
 * @param[in,out] position Fields present in the sentence are updated; others are left alone.
 * @return E_OK if the sentence was recognised, checksum-valid and carried a usable fix;
 *         ::E_CRC_FAIL on a checksum mismatch; E_NOT_FOUND if the sentence type is not one this
 *         module consumes; E_NOT_OK if it was malformed or reported no fix.
 */
CHECK_RETURN Std_ReturnType GnssIf_ParseSentence(const char *sentence, GnssIf_PositionType *position);

/**
 * @brief Whether moving from @p from to @p to within @p deltaMs is physically possible.
 *
 * Replaces v1's compiled-in geographic box with a test that holds anywhere on Earth: the implied
 * ground speed must not exceed ::GNSSIF_MAX_SPEED_MM_PER_SEC. A receiver briefly reporting a
 * position on another continent -- which happens as a fix converges -- is rejected on that basis
 * rather than on where the vehicle is assumed to be.
 *
 * @return TRUE if the transition is plausible, or if @p from is not yet valid.
 */
boolean GnssIf_IsTransitionPlausible(const GnssIf_PositionType *from, const GnssIf_PositionType *to,
                                     uint32 deltaMs);

/**
 * @brief Read the parser counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType GnssIf_GetStatistics(GnssIf_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void GnssIf_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* GNSSIF_H */

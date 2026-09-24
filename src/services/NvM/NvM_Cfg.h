/**
 * @file    NvM_Cfg.h
 * @brief   NVRAM manager block configuration and persistent data structures.
 *
 * The structures below are the ECU's persistent state. Their layout is part of the product's
 * data compatibility story, so every one is fixed width, explicitly padded, and carries a
 * version field that lets a later firmware migrate rather than misread it.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef NVM_CFG_H
#define NVM_CFG_H

#include "services/Fee/Fee_Cfg.h"
#include "base/Std_Types.h"

#define NVM_DEV_ERROR_DETECT STD_ON

/** Bytes of CRC appended by NvM to every block image before it reaches Fee. */
#define NVM_CRC_SIZE 4u

/*==================================================================================================
 *  Block identifiers
 *================================================================================================*/

#define NVM_BLOCK_ODOMETER ((NvM_BlockIdType)0u)
#define NVM_BLOCK_DEVICE_CONFIG ((NvM_BlockIdType)1u)
#define NVM_BLOCK_CALIBRATION ((NvM_BlockIdType)2u)
#define NVM_BLOCK_RESTART_INFO ((NvM_BlockIdType)3u)
#define NVM_BLOCK_ENERGY_COUNTERS ((NvM_BlockIdType)4u)

#define NVM_BLOCK_COUNT 5u

/*==================================================================================================
 *  Persistent data structures
 *================================================================================================*/

/**
 * @brief Accumulated distance.
 *
 * @par Why integers, and why millimetres
 * v1 held the distance as a @c float in kilometres, accumulated into it every cycle, wrote it
 * to the SD card as text, and read it back with @c atol() -- which parses an *integer*. Every
 * boot therefore discarded the fractional part, and because the truncated value was written
 * straight back, the loss compounded. On a vehicle that restarts a few times a day this
 * silently destroys the one number the product exists to produce.
 *
 * Even without the @c atol() defect the float accumulator was unsound. A binary32 carries about
 * 24 significant bits, so once the total passes roughly 130 000 km the representable step
 * exceeds a centimetre and small increments start disappearing into rounding. The error is
 * one-directional -- always downward -- so it never averages out.
 *
 * A @c uint64 in millimetres removes both problems. It counts to 1.8e13 km, which is not a
 * limit any vehicle approaches, and every increment is exact: ::OdoSwc carries the sub
 * millimetre remainder between samples, so integrating 5 mm ten thousand times yields exactly
 * 50 m rather than drifting.
 */
typedef struct
{
    uint16 structVersion;          /**< Layout version; 1 for this definition.        */
    uint16 reserved0;              /**< Explicit padding, written as zero.            */
    uint64 totalDistanceMm;        /**< Lifetime distance in millimetres.             */
    uint64 tripDistanceMm;         /**< Distance since the trip was last reset.        */
    uint32 updateCount;            /**< Times this block has been committed.           */
    uint32 lastUpdateUnixTime;     /**< Wall-clock time of the last commit, 0 if unset.*/
} NvM_OdometerType;

/** Device identity and cloud endpoint, learned during provisioning. */
typedef struct
{
    uint16 structVersion;   /**< Layout version; 1 for this definition.     */
    uint16 brokerPort;      /**< MQTT broker port.                          */
    char deviceId[24];      /**< NUL-terminated logical device identifier.   */
    char brokerHost[32];    /**< NUL-terminated broker address.              */
} NvM_DeviceConfigType;

/**
 * @brief Vehicle calibration.
 *
 * Held in NvM rather than compiled in, because a tyre change or a gearbox swap must be a field
 * adjustment. v1 had @c GEAR_RATIO and @c TIRE_DIAMETER as macros, so correcting either meant
 * rebuilding and reflashing every unit.
 *
 * Stored as scaled integers so that two units given the same calibration compute bit-identical
 * distances -- a float here would make the odometer depend on the compiler's rounding.
 */
typedef struct
{
    uint16 structVersion;         /**< Layout version; 1 for this definition.            */
    uint16 tyreDiameterMilliInch; /**< Tyre diameter in thousandths of an inch.           */
    uint16 gearRatioMilli;        /**< Gear ratio x 1000 (6.000:1 is 6000).               */
    uint16 vbattDividerMilli;     /**< Battery divider ratio x 1000.                      */
    sint16 vbattOffsetMv;         /**< Per-unit offset trim for the battery reading, mV.  */
    uint16 maxPlausibleRpm;       /**< Motor speed above which a sample is rejected.      */
    uint32 reserved1;             /**< Explicit padding, written as zero.                 */
} NvM_CalibrationType;

/**
 * @brief Crash-loop detector state.
 *
 * A unit that cannot mount its SD card would otherwise reset every ten minutes forever. The
 * counter and the window let EcuM notice that and enter a degraded mode instead.
 */
typedef struct
{
    uint16 structVersion;       /**< Layout version; 1 for this definition.        */
    uint16 restartCount;        /**< Restarts inside the current window.           */
    uint32 windowStartUnixTime; /**< When the window opened.                        */
    uint32 lastResetReason;     /**< ::Mcu_ResetReasonType of the previous start.   */
    uint32 totalRestarts;       /**< Lifetime restart count.                        */
} NvM_RestartInfoType;

/** Lifetime energy throughput, for efficiency reporting. */
typedef struct
{
    uint16 structVersion;    /**< Layout version; 1 for this definition.   */
    uint16 reserved0;        /**< Explicit padding, written as zero.       */
    uint64 energyOutMilliWh; /**< Lifetime energy drawn from the packs.    */
    uint64 energyInMilliWh;  /**< Lifetime energy returned by regeneration.*/
    uint32 reserved1;        /**< Explicit padding, written as zero.       */
} NvM_EnergyCountersType;

/*==================================================================================================
 *  Block lengths
 *
 *  Each must leave room for NvM's own CRC inside the Fee block it is stored in. The static
 *  assertions in NvM.c enforce that, so an over-sized structure is a build failure rather than
 *  a runtime truncation.
 *================================================================================================*/

#define NVM_LENGTH_ODOMETER ((uint16)sizeof(NvM_OdometerType))
#define NVM_LENGTH_DEVICE_CONFIG ((uint16)sizeof(NvM_DeviceConfigType))
#define NVM_LENGTH_CALIBRATION ((uint16)sizeof(NvM_CalibrationType))
#define NVM_LENGTH_RESTART_INFO ((uint16)sizeof(NvM_RestartInfoType))
#define NVM_LENGTH_ENERGY_COUNTERS ((uint16)sizeof(NvM_EnergyCountersType))

/** Longest block payload; sizes NvM's staging buffer. */
#define NVM_MAX_BLOCK_LENGTH 64u

/*==================================================================================================
 *  Default values
 *
 *  Applied when a block is absent or every stored copy fails its CRC.
 *================================================================================================*/

/** Structure layout version written by this firmware. */
#define NVM_STRUCT_VERSION 1u

/** Default tyre diameter: 19.000 inches, matching the v1 build-time constant. */
#define NVM_DEFAULT_TYRE_DIAMETER_MILLI_INCH 19000u

/** Default gear ratio: 6.000:1, matching the v1 build-time constant. */
#define NVM_DEFAULT_GEAR_RATIO_MILLI 6000u

/**
 * @brief Default battery divider ratio x 1000.
 *
 * The v1 code computed the battery voltage as @c (count * 115) / 4095, which implies a full
 * scale of 115 V across the divider. Expressed as a ratio against the ADC's 3.3 V reference
 * that is 115 / 3.3 = 34.85, so 34848. Carried over rather than corrected because the divider
 * on the existing boards is unchanged; it is now adjustable per unit instead of compiled in.
 */
#define NVM_DEFAULT_VBATT_DIVIDER_MILLI 34848u

/** Default per-unit voltage trim, in millivolts. */
#define NVM_DEFAULT_VBATT_OFFSET_MV 0

/**
 * @brief Motor speed above which a sample is treated as noise, in rpm.
 *
 * 12 000. The drive's mechanical limit is well below this; a CAN frame decoding to more is
 * corruption rather than a measurement, and integrating it would add kilometres to the odometer
 * in a single sample.
 */
#define NVM_DEFAULT_MAX_PLAUSIBLE_RPM 12000u

/** Default broker port when provisioning has not yet run. */
#define NVM_DEFAULT_BROKER_PORT 1883u

#endif /* NVM_CFG_H */

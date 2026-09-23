/**
 * @file    OdoSwc_Cfg.h
 * @brief   Odometry configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ODOSWC_CFG_H
#define ODOSWC_CFG_H

#include "Std_Types.h"

#define ODOSWC_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Fixed-point arithmetic
 *================================================================================================*/

/**
 * @brief Fractional bits in the conversion factor.
 *
 * 32. The factor is around 0.0042 mm per rpm-millisecond, so a Q32 representation keeps roughly
 * ten significant decimal digits -- far beyond what the tyre and gear-ratio inputs justify, and
 * enough that the representation contributes nothing measurable to the total error.
 */
#define ODO_FIXED_SHIFT 32u

/** Mask isolating the fractional part carried between samples. */
#define ODO_FIXED_MASK 0xFFFFFFFFuLL

/**
 * @brief pi scaled by 2^24.
 *
 * Q24 rather than Q16: at Q16 the quantisation of pi alone contributes a relative error of
 * 2e-6, which is 200 m over 100 000 km. At Q24 it is 9e-9, which is under a metre over the
 * same distance and comfortably below the uncertainty in the tyre diameter itself.
 */
#define ODO_PI_Q24 52707179uL

/** Fractional bits in ::ODO_PI_Q24. */
#define ODO_PI_SHIFT 24u

/** Micrometres per inch x 1000, i.e. 25.4 mm expressed as an integer scaled by 10. */
#define ODO_MM_PER_INCH_X10 254uL

/*==================================================================================================
 *  Plausibility limits
 *================================================================================================*/

/**
 * @brief Longest interval that may be integrated, in milliseconds.
 *
 * 10 000. The nominal sample period is 3 s, so a 10 s gap means samples were missed -- the CAN
 * bus dropped out, or the scheduler overran. Integrating across it would assume the vehicle
 * held its last known speed throughout, which is exactly the assumption that turns a
 * communications fault into fabricated distance. The interval is discarded and counted instead.
 */
#define ODO_MAX_SAMPLE_GAP_MS 10000uL

/** Smallest tyre diameter accepted by ::OdoSwc_SetCalibration, in thousandths of an inch. */
#define ODO_MIN_TYRE_MILLI_INCH 4000u

/** Largest tyre diameter accepted, in thousandths of an inch. */
#define ODO_MAX_TYRE_MILLI_INCH 60000u

/** Smallest gear ratio accepted, x 1000. A ratio below 1:1 would be an overdrive. */
#define ODO_MIN_GEAR_RATIO_MILLI 500u

/** Largest gear ratio accepted, x 1000. */
#define ODO_MAX_GEAR_RATIO_MILLI 50000u

/*==================================================================================================
 *  Persistence
 *
 *  Wear arithmetic behind the thresholds below.
 *
 *  One odometer record occupies 48 bytes of payload plus a 16-byte header, so 64 bytes. A 4 KiB
 *  Fee sector holds (4096 - 16) / 64 = 63 records, and each time it fills, garbage collection
 *  erases two sectors. Flash endurance is specified at 100 000 erase cycles.
 *
 *  At a 100 m threshold and an average 30 km/h, a record is written every 12 s of driving, so
 *  300 per hour. Ten hours of driving a day is 3000 records, which is 3000/63 = 48 collections
 *  and 96 erases per day. 100 000 / 96 is a little over 1000 days of that duty cycle -- and
 *  that duty cycle is an extreme: a delivery vehicle driven ten hours every single day.
 *
 *  Halving the threshold halves the service life; doubling it doubles the distance at risk from
 *  an unexpected power cut. 100 m sits where a lost interval is smaller than the uncertainty
 *  already present in the tyre calibration, so it costs nothing that is actually knowable.
 *================================================================================================*/

/** Distance advance that triggers a write, in millimetres. */
#define ODO_PERSIST_DISTANCE_MM 100000uLL

/**
 * @brief Longest interval between writes while the vehicle is moving, in milliseconds.
 *
 * 60 s, so a slow-moving vehicle that never accumulates 100 m still records progress. A
 * stationary vehicle accumulates nothing, so NvM's write-on-change comparison suppresses the
 * write entirely and a parked vehicle costs no flash wear at all.
 */
#define ODO_PERSIST_INTERVAL_MS 60000uL

#endif /* ODOSWC_CFG_H */

/**
 * @file    GnssIf_Cfg.h
 * @brief   GNSS interface configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef GNSSIF_CFG_H
#define GNSSIF_CFG_H

#include "base/Std_Types.h"
#include "mcal/Uart/Uart_Cfg.h"

#define GNSSIF_DEV_ERROR_DETECT STD_ON

/** UART instance the receiver is wired to. */
#define GNSSIF_UART_INSTANCE UART_INSTANCE_GNSS

/*==================================================================================================
 *  Parser
 *================================================================================================*/

/**
 * @brief Longest sentence the parser will assemble, including the terminator.
 *
 * 96 bytes. NMEA 0183 caps a sentence at 82 characters including @c $ and CR/LF; 96 leaves headroom
 * for the non-compliant extended sentences some receivers emit, while still being small enough to
 * sit on a task stack. A longer sentence is discarded and counted rather than truncated, because a
 * truncated sentence can still pass a checksum computed over the part that arrived.
 */
#define GNSSIF_SENTENCE_BUFFER_SIZE 96u

/**
 * @brief Bytes read from the receiver per ::GnssIf_MainFunction call.
 *
 * 256. At 9600 baud the receiver produces at most 960 bytes per second, and the function runs every
 * 100 ms, so 256 is well above the steady-state need. The bound exists so that a receiver stuck
 * emitting continuously cannot make one scheduler slot run long.
 */
#define GNSSIF_MAX_BYTES_PER_CYCLE 256u

/** Maximum comma-separated fields the parser will index in one sentence. */
#define GNSSIF_MAX_FIELDS 20u

/*==================================================================================================
 *  Plausibility
 *================================================================================================*/

/**
 * @brief Highest ground speed a position change may imply, in millimetres per second.
 *
 * 70 000 mm/s, which is 252 km/h -- far above this vehicle's capability but below any speed a
 * genuine fix jump produces. The purpose is to reject the transient positions a receiver reports
 * while its fix converges, which can be hundreds of kilometres away and would otherwise be logged
 * as a real movement.
 *
 * This replaces v1's compiled-in latitude 24..38, longitude 60..78 box. A speed limit holds
 * everywhere on Earth; a coordinate box makes the firmware silently wrong outside one region.
 */
#define GNSSIF_MAX_SPEED_MM_PER_SEC 70000uL

/**
 * @brief Smallest interval over which the plausibility test is applied, in milliseconds.
 *
 * 200 ms. Over a shorter interval the permitted distance becomes smaller than the receiver's own
 * noise, and a stationary vehicle's jitter would start being rejected as implausible movement.
 */
#define GNSSIF_MIN_PLAUSIBILITY_INTERVAL_MS 200u

/**
 * @brief Metres per degree of latitude, used by the plausibility test.
 *
 * 111 320 m, the mean meridional degree. The test only needs to be right to within a few percent --
 * it is a sanity bound, not a distance measurement -- so the variation with latitude is ignored and
 * longitude is treated with the same figure. Doing so makes the permitted distance slightly
 * generous near the poles, which errs toward accepting a real fix rather than rejecting one.
 */
#define GNSSIF_METRES_PER_DEGREE 111320uL

/*==================================================================================================
 *  Ageing
 *================================================================================================*/

/**
 * @brief Age beyond which a fix is no longer considered current, in milliseconds.
 *
 * 10 000. The receiver produces a fix every second, so 10 s is ten missed updates. Beyond that the
 * vehicle may have moved arbitrarily far and the stored position should be published as stale rather
 * than as current -- v1 published its last known location indefinitely, so a vehicle that drove into
 * a tunnel and parked appeared to still be at the tunnel entrance.
 */
#define GNSSIF_FIX_TIMEOUT_MS 10000uL

/**
 * @brief Consecutive cycles without a fix before a diagnostic event is raised.
 *
 * 30 cycles at the 3 s acquisition period is 90 s. Generous on purpose: a receiver legitimately has
 * no fix indoors, in a tunnel or under a metal roof, and reporting that as a fault would make the
 * event meaningless.
 */
#define GNSSIF_NO_FIX_REPORT_THRESHOLD 30u

#endif /* GNSSIF_CFG_H */

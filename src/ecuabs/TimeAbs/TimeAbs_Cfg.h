/**
 * @file    TimeAbs_Cfg.h
 * @brief   Wall-clock configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef TIMEABS_CFG_H
#define TIMEABS_CFG_H

#include "base/Std_Types.h"

#define TIMEABS_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Plausibility window
 *
 *  A timestamp outside this window did not come from a working clock. The lower bound is what catches
 *  the case that matters: a DS3231 whose backup cell has died reports 2000-01-01, and v1 happily wrote
 *  that into records, which then sorted to the beginning of the dataset and looked like the oldest
 *  data the fleet had.
 *================================================================================================*/

/** 2024-01-01T00:00:00Z. Earlier than any record this firmware family can legitimately produce. */
#define TIMEABS_MIN_PLAUSIBLE_UNIX 1704067200uL

/** 2099-12-31T23:59:59Z. Catches a clock that has run away or an NTP response that is nonsense. */
#define TIMEABS_MAX_PLAUSIBLE_UNIX 4102444799uL

/*==================================================================================================
 *  Synchronisation
 *================================================================================================*/

/** NTP server. */
#define TIMEABS_NTP_SERVER "pool.ntp.org"

/**
 * @brief Seconds of disagreement between RTC and NTP that triggers a write-back.
 *
 * 5 s. A DS3231 drifts by about 2 ppm, which is under a minute a year, so a disagreement of more than
 * a few seconds means either the RTC is faulty or it was never set. Writing back on every small
 * difference would wear the RTC's EEPROM for no benefit.
 */
#define TIMEABS_RTC_DRIFT_TOLERANCE_S 5

/**
 * @brief Interval between NTP synchronisation attempts while a bearer is up, in milliseconds.
 *
 * 6 hours. Frequent enough that drift never becomes visible in the data, infrequent enough to be
 * negligible against the metered cellular allowance.
 */
#define TIMEABS_SYNC_INTERVAL_MS 21600000uL

/** Bound on one NTP attempt, in milliseconds. */
#define TIMEABS_SYNC_TIMEOUT_MS 5000u

/**
 * @brief Timezone offset applied to stored timestamps, in seconds.
 *
 * Zero: everything is stored and transmitted as UTC. v1 applied a fixed 18 000 s (+5 h) offset inside
 * @c configTime and then also subtracted it when reading the RTC, so the two partially cancelled and
 * the stored timestamps were in neither UTC nor local time. Local time is a presentation concern and
 * belongs wherever the data is displayed, not in the data.
 */
#define TIMEABS_UTC_OFFSET_S 0

#endif /* TIMEABS_CFG_H */

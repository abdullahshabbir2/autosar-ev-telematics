/**
 * @file    BattSwc_Cfg.h
 * @brief   Battery monitoring thresholds.
 *
 * Every threshold here is in the pack's raw protocol units, because the physical scaling of those units
 * is not yet confirmed (see docs/08-protocols.md). That is deliberate: a threshold expressed in raw
 * units is exactly as correct as the measurement it is compared against, whereas one expressed in
 * millivolts would embed the unconfirmed assumption in two places instead of one.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef BATTSWC_CFG_H
#define BATTSWC_CFG_H

#include "ecuabs/Rs485If/Rs485If_Cfg.h"
#include "base/Std_Types.h"

#define BATTSWC_DEV_ERROR_DETECT STD_ON

/**
 * @brief Cell imbalance, in raw units, above which a pack is reported out of balance.
 *
 * 300. On the centi-volt reading the simulation fixtures imply, that is 0.30 V between the strongest and
 * weakest cell -- well past the 0.05 V a healthy pack holds, and the point at which a BMS can no longer
 * balance it passively. Set from the fixtures rather than a datasheet, so it is provisional; the value
 * exists in one place so confirming the scaling is a single edit.
 */
#define BATTSWC_IMBALANCE_WARN_RAW 300u

/**
 * @brief Temperature spread, in raw units, above which a pack is reported as unevenly heated.
 *
 * 1500. On the centi-degree reading the fixtures imply, 15 degC between the hottest and coldest sensor
 * in one pack indicates a cell working much harder than its neighbours, which precedes a thermal fault.
 */
#define BATTSWC_TEMPERATURE_SPREAD_WARN_RAW 1500u

/**
 * @brief Consecutive polling rounds a pack may miss before it is reported faulty.
 *
 * 3. At the 3 s acquisition period that is nine seconds of silence -- long enough to exclude an ignition
 * transient, short enough to be reported within one telemetry interval. Matches
 * RS485IF_FAILURE_LIMIT so the transport and the application agree on what "failed" means.
 */
#define BATTSWC_MISSING_LIMIT RS485IF_FAILURE_LIMIT

/**
 * @brief State of charge, in percent, below which the pack is reported nearly empty.
 *
 * 10 %. Below this the BMS will begin limiting discharge current, which the driver experiences as a loss
 * of performance; reporting it first makes that expected rather than alarming.
 */
#define BATTSWC_SOC_LOW_PERCENT 10u

/**
 * @brief State of health, in percent, below which a pack is reported as worn out.
 *
 * 70 %. The usual end-of-life criterion for a traction pack.
 */
#define BATTSWC_SOH_WORN_PERCENT 70u

#endif /* BATTSWC_CFG_H */

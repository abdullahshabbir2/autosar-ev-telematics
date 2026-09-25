/**
 * @file    ComM_Cfg.h
 * @brief   Bearer arbitration configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COMM_CFG_H
#define COMM_CFG_H

#include "ecuabs/NetIf/NetIf_Cfg.h"
#include "base/Std_Types.h"

#define COMM_DEV_ERROR_DETECT STD_ON

/**
 * @brief Consecutive failures before a bearer is abandoned for the other one.
 *
 * 3. Each attempt already costs up to NETIF_LINK_TIMEOUT_MS plus a backoff, so three failures is on the
 * order of two minutes of trying. Fewer would switch to metered GPRS over a transient WiFi glitch;
 * more would leave a vehicle out of WiFi range not sending anything for a long time.
 */
#define COMM_FAILURE_LIMIT 3u

/**
 * @brief Interval between arbitration decisions, in milliseconds.
 *
 * 1000 ms, matching the connectivity task's period. Arbitration is cheap; the cost is in acting on the
 * decision, which the hysteresis above controls.
 */
#define COMM_ARBITRATION_INTERVAL_MS 1000uL

/**
 * @brief Time with no bearer at all before the no-backhaul event is raised, in milliseconds.
 *
 * 10 minutes. A vehicle is frequently out of coverage and that is not a fault; ten minutes of nothing
 * at all, however, is when records start accumulating on the card faster than they leave.
 */
#define COMM_NO_BEARER_REPORT_MS 600000uL

/**
 * @brief How long to stay on a fallback bearer before giving the preferred one another chance, in ms.
 *
 * 600 000 -- ten minutes. Without this the fallback is permanent for the life of the run: the failure
 * count that triggered it is only cleared by Init, by both bearers being exhausted, or by the preferred
 * bearer carrying traffic -- and the last of those cannot happen while the fallback holds. A vehicle
 * that failed WiFi on the way out of its depot would pay for cellular data beside a healthy access
 * point all day.
 *
 * Ten minutes is chosen against what it costs to be wrong in each direction. Too short and a vehicle
 * genuinely out of WiFi range spends a slice of every interval attempting it, which on the acquisition
 * side is only a few seconds of a bearer being brought up; too long and a vehicle that has returned to
 * coverage keeps paying for cellular. Ten minutes is short against a working day and long against the
 * few seconds an attempt costs.
 *
 * Note this restores the *allowance*, not the bearer: the ordinary attempt-and-fail cycle still applies,
 * so a preferred bearer that is still absent simply falls back again.
 */
#define COMM_PREFERRED_RETRY_MS 600000uL

/**
 * @brief Whether WiFi is preferred over GPRS when both are available.
 *
 * On. WiFi is free and fast; GPRS is metered and slow. A vehicle back at a depot should drain its
 * backlog over WiFi rather than pay to do over cellular what it could do for nothing.
 */
#define COMM_PREFER_WIFI STD_ON

#endif /* COMM_CFG_H */

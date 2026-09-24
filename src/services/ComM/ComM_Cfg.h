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

#include "NetIf_Cfg.h"
#include "Std_Types.h"

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
 * @brief Continuous WiFi availability required before switching back to it, in milliseconds.
 *
 * 30 s. This is the hysteresis that stops a vehicle parked at the edge of WiFi range from oscillating
 * between bearers. Each switch drops the broker session and costs tens of seconds of modem attach time,
 * so an oscillating unit never holds a session long enough to transfer anything -- it looks connected
 * and delivers nothing.
 */
#define COMM_WIFI_STABLE_MS 30000uL

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
 * @brief Whether WiFi is preferred over GPRS when both are available.
 *
 * On. WiFi is free and fast; GPRS is metered and slow. A vehicle back at a depot should drain its
 * backlog over WiFi rather than pay to do over cellular what it could do for nothing.
 */
#define COMM_PREFER_WIFI STD_ON

#endif /* COMM_CFG_H */

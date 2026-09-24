/**
 * @file    Wdg_Cfg.h
 * @brief   Watchdog driver pre-compile configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef WDG_CFG_H
#define WDG_CFG_H

#include "base/Std_Types.h"

#define WDG_DEV_ERROR_DETECT STD_ON

/**
 * @brief Timeout for ::WDG_MODE_FAST, in milliseconds.
 *
 * 12 s. The longest legitimate blocking stretch inside a supervised cycle is an SD
 * write behind a card performing internal wear levelling; datasheets bound that at
 * roughly 250 ms, and the archived v1 device logs show write stalls approaching 2 s on
 * a worn card. 12 s leaves headroom for several such stalls plus a full RS485 round of
 * four packs, while still resetting a genuinely hung ECU inside one telemetry
 * interval.
 */
#define WDG_TIMEOUT_FAST_MS 12000u

/**
 * @brief Timeout for ::WDG_MODE_SLOW, in milliseconds.
 *
 * 60 s, covering the worst-case startup: SD mount, GPRS attach with retries, and the
 * first broker handshake.
 */
#define WDG_TIMEOUT_SLOW_MS 60000u

/**
 * @brief Whether ::WDG_MODE_OFF may be selected at all.
 *
 * On, but only because an OTA flash write can hold the flash bus long enough to trip
 * even the slow timeout. Ota is the single caller, it re-enables the watchdog on every
 * exit path including the failure paths, and WdgM raises a DTC if it ever finds the
 * watchdog disabled outside an active OTA session.
 */
#define WDG_ALLOW_DISABLE STD_ON

/** Reset the whole ECU on expiry, rather than only signalling. */
#define WDG_RESET_ON_EXPIRY STD_ON

#endif /* WDG_CFG_H */

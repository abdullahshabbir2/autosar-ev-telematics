/**
 * @file    Ecu_Cfg.h
 * @brief   Top-level ECU configuration: identity, build variant and feature switches.
 *
 * The one file that says what *this build* is. Per-module tuning lives in each module's own `_Cfg.h`;
 * what belongs here is everything that identifies the build or selects between variants.
 *
 * @par Secrets are not in this file
 * v1 committed a Firebase authentication token and a WiFi password as string literals in
 * `include/config.h`. Both are in the repository's history permanently, and anyone with read access to the
 * source had write access to the fleet's database. Credentials now come from `config/Secrets.h`, which is
 * git-ignored and generated from `Secrets.h.template`; the build fails with an explanatory message if it
 * is missing. See [09-operations.md](docs/09-operations.md) for the provisioning procedure.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ECU_CFG_H
#define ECU_CFG_H

#include "Ecu_PinMap.h"

/*==================================================================================================
 *  Build identity
 *
 *  ECU_FIRMWARE_VERSION is reported over diagnostics (DID 0xF189), published in the health record, and
 *  written to the OTA metadata, so a fleet can be told exactly what each unit is running. The build
 *  timestamp and the git description are injected by the build system rather than edited by hand -- v1
 *  carried a COMMIT_DATE macro that had to be updated manually and, predictably, said "8th August 2024"
 *  in a firmware built months later.
 *================================================================================================*/

#define ECU_FIRMWARE_VERSION "2.0.0"

/** Human-readable description of this release. */
#define ECU_FIRMWARE_DESCRIPTION "AUTOSAR-layered rework with crash-safe odometry"

/** Injected by the build system from `git describe`. Falls back if git is unavailable. */
#ifndef ECU_BUILD_GIT_DESCRIBE
#define ECU_BUILD_GIT_DESCRIBE "unknown"
#endif

/** Injected by the build system as an ISO-8601 UTC timestamp. */
#ifndef ECU_BUILD_TIMESTAMP
#define ECU_BUILD_TIMESTAMP "unknown"
#endif

/**
 * @brief On-media data format version.
 *
 * Bumped whenever the CSV record layout or an NvM structure changes in a way a consumer would misread.
 * Published in the health record so a fleet ingest can reject or migrate rather than silently
 * misinterpret.
 */
#define ECU_DATA_FORMAT_VERSION 2u

/*==================================================================================================
 *  Build variant
 *
 *  Exactly one must be defined, which the assertion at the end of this file enforces. A build that is
 *  neither clearly production nor clearly a bench build is how debug behaviour reaches a vehicle.
 *================================================================================================*/

#if !defined(ECU_VARIANT_PRODUCTION) && !defined(ECU_VARIANT_BENCH) && !defined(ECU_VARIANT_SIMULATION)
/* Default to production, because the safe failure mode for an unspecified build is the one with no
 * debug behaviour in it. */
#define ECU_VARIANT_PRODUCTION
#endif

/*==================================================================================================
 *  Feature switches
 *================================================================================================*/

/**
 * @brief Whether the GNSS receiver is populated on this board.
 *
 * On. A build for a variant without one sets this off, and GnssIf then reports no fix rather than
 * accumulating no-fix diagnostic events for hardware that was never fitted.
 */
#define ECU_FEATURE_GNSS STD_ON

/** Whether the GSM modem is populated. */
#define ECU_FEATURE_GSM STD_ON

/** Whether the microSD card slot is populated. */
#define ECU_FEATURE_SD STD_ON

/**
 * @brief Whether over-the-air firmware update is enabled.
 *
 * On. The partition table reserves two 1.875 MiB application slots for it, and a fleet with no OTA path
 * means every firmware fix requires physical access to every vehicle.
 */
#define ECU_FEATURE_OTA STD_ON

/**
 * @brief Whether the remote diagnostic service is enabled.
 *
 * On. It is the only way to investigate a unit that has entered a degraded mode and is inaccessible.
 */
#define ECU_FEATURE_REMOTE_DIAGNOSTICS STD_ON

/*==================================================================================================
 *  Timing -- the periods every other module's configuration is derived from
 *================================================================================================*/

/**
 * @brief Data acquisition period, in milliseconds.
 *
 * 3000 ms, carried over from v1 so the data rate a deployed fleet expects does not change. Note that v1's
 * own macro was `DATA_ACQUISITION_TIME 3` with a comment saying "milliseconds" and a use that multiplied
 * by 1000 -- the comment was wrong, and the value was in seconds. Stated in milliseconds here, with the
 * unit in the name.
 */
#define ECU_ACQUISITION_PERIOD_MS 3000uL

/** Scheduler tick, in milliseconds. Every task period is a multiple of it. */
#define ECU_SCHEDULER_TICK_MS 10uL

/** Connectivity task period, in milliseconds. */
#define ECU_CONNECTIVITY_PERIOD_MS 1000uL

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((ECU_ACQUISITION_PERIOD_MS % ECU_SCHEDULER_TICK_MS) == 0u,
               "the acquisition period must be a whole number of scheduler ticks");
_Static_assert((ECU_CONNECTIVITY_PERIOD_MS % ECU_SCHEDULER_TICK_MS) == 0u,
               "the connectivity period must be a whole number of scheduler ticks");
#endif

/*==================================================================================================
 *  Crash-loop detection
 *
 *  A unit that cannot mount its SD card, or whose CAN controller never answers, would otherwise reset
 *  every few minutes forever. v1 had the counting half of this -- a restart count in NVS -- but its only
 *  reaction was to upload the current log file and carry on resetting.
 *================================================================================================*/

/**
 * @brief Resets within the window that constitute a crash loop.
 *
 * 5. Fewer would trigger on a vehicle whose ignition is cycled a few times while parking.
 */
#define ECU_CRASH_LOOP_COUNT 5u

/**
 * @brief Window over which resets are counted, in seconds.
 *
 * 600 s. Five resets in ten minutes is unambiguous; the same five spread over a day is normal use.
 */
#define ECU_CRASH_LOOP_WINDOW_S 600uL

/*==================================================================================================
 *  Variant consistency
 *================================================================================================*/

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#if (defined(ECU_VARIANT_PRODUCTION) + defined(ECU_VARIANT_BENCH) + defined(ECU_VARIANT_SIMULATION)) > 1
#error "Ecu_Cfg.h: exactly one ECU_VARIANT_* may be defined"
#endif
#endif

#endif /* ECU_CFG_H */

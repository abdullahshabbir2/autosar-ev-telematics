/**
 * @file    WdgM_Cfg.h
 * @brief   Watchdog manager supervision configuration.
 *
 * One supervised entity per cyclic task, with the alive bounds and the deadline derived from that
 * task's period rather than chosen by feel. The arithmetic for each is stated, because a
 * supervision limit that nobody can justify gets widened the first time it trips and then
 * protects nothing.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef WDGM_CFG_H
#define WDGM_CFG_H

#include "base/Std_Types.h"
#include "mcal/Wdg/Wdg_Cfg.h"

#define WDGM_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Supervision cycle
 *================================================================================================*/

/**
 * @brief Interval at which ::WdgM_MainFunction evaluates every entity, in milliseconds.
 *
 * 1000 ms. It must be long enough that the slowest supervised entity checks in at least once per
 * cycle -- otherwise every cycle records a false alive violation -- and short enough that a
 * genuine stall is caught well inside the hardware timeout. The slowest entity here has a 3000 ms
 * period, so its bounds are expressed over several cycles rather than one; see the comment on
 * ::WDGM_SE_ACQUISITION below.
 */
#define WDGM_SUPERVISION_CYCLE_MS 1000u

/**
 * @brief Consecutive failed cycles an entity may accumulate before expiring.
 *
 * 5, so five seconds of continuous violation. Combined with the 12 s hardware timeout, a
 * genuinely stuck entity resets the ECU about 17 s after it stops behaving -- inside one telemetry
 * interval, and long enough that a single slow SD write cannot trigger it.
 */
#define WDGM_FAILED_TOLERANCE_CYCLES 5u

/*==================================================================================================
 *  Supervised entities
 *================================================================================================*/

#define WDGM_SE_SCHEDULER ((WdgM_SupervisedEntityIdType)0u)   /**< The 10 ms scheduler tick. */
#define WDGM_SE_ACQUISITION ((WdgM_SupervisedEntityIdType)1u) /**< Data acquisition, 3 s.    */
#define WDGM_SE_STORAGE ((WdgM_SupervisedEntityIdType)2u)     /**< SD logging, 3 s.          */
#define WDGM_SE_TELEMETRY ((WdgM_SupervisedEntityIdType)3u)   /**< Backhaul, 1 s.            */

#define WDGM_SE_COUNT 4u

/*==================================================================================================
 *  Checkpoints
 *
 *  Each entity declares an entry and an exit checkpoint. Program-flow supervision then verifies
 *  that a runnable which entered also left -- catching a path that returned early through a
 *  branch nobody intended, which neither alive nor deadline supervision can see.
 *================================================================================================*/

#define WDGM_CP_ENTRY ((WdgM_CheckpointIdType)0u)
#define WDGM_CP_EXIT ((WdgM_CheckpointIdType)1u)

#define WDGM_CP_COUNT_PER_ENTITY 2u

/*==================================================================================================
 *  Per-entity limits
 *
 *  Alive bounds are counted over one supervision cycle of ::WDGM_SUPERVISION_CYCLE_MS. An entity
 *  whose period is longer than the supervision cycle cannot check in every cycle, so its minimum
 *  is zero and the deadline does the real work -- the deadline is the stronger check anyway,
 *  because it catches a single long stall that an averaged count hides completely.
 *================================================================================================*/

/*---- Scheduler tick: 10 ms period, so 100 check-ins per 1000 ms cycle. ------*/
/* Bounds of 80 and 120 allow +/-20 %, which covers the jitter a 10 ms software timer shows when
 * the SPI bus is busy without admitting a genuine stall. */
#define WDGM_ALIVE_MIN_SCHEDULER 80u
#define WDGM_ALIVE_MAX_SCHEDULER 120u
/* 200 ms deadline: twenty missed ticks. Below that, a single SD write would trip it. */
#define WDGM_DEADLINE_SCHEDULER_MS 200u

/*---- Acquisition: 3000 ms period, slower than the supervision cycle. --------*/
#define WDGM_ALIVE_MIN_ACQUISITION 0u
#define WDGM_ALIVE_MAX_ACQUISITION 2u
/**
 * @brief Acquisition deadline, in milliseconds.
 *
 * 9000, three times the nominal period. One acquisition pass polls four battery packs over a
 * 4800 baud bus; the worst case with one retry each is about 2.5 s, and a slow SD write can add
 * another 2 s on top. 9000 ms leaves room for both without admitting the case that matters -- a
 * pack that never answers and a read loop that never returns.
 */
#define WDGM_DEADLINE_ACQUISITION_MS 9000u

/*---- Storage: 3000 ms period, driven by the acquisition cycle. --------------*/
#define WDGM_ALIVE_MIN_STORAGE 0u
#define WDGM_ALIVE_MAX_STORAGE 2u
/* 10 000 ms: an SD card doing internal wear levelling has been observed stalling for 2 s, and
 * three consecutive stalls must not reset the ECU. */
#define WDGM_DEADLINE_STORAGE_MS 10000u

/*---- Telemetry: 1000 ms period, one check-in per supervision cycle. ---------*/
#define WDGM_ALIVE_MIN_TELEMETRY 0u
#define WDGM_ALIVE_MAX_TELEMETRY 3u
/**
 * @brief Telemetry deadline, in milliseconds.
 *
 * 30 000. Deliberately generous: a broker handshake over GPRS can take twenty seconds on a weak
 * signal, and that is normal operation rather than a fault. Resetting the ECU for it would
 * guarantee the data never arrives, which is the opposite of the intent.
 */
#define WDGM_DEADLINE_TELEMETRY_MS 30000u

/*==================================================================================================
 *  Reaction
 *================================================================================================*/

/**
 * @brief Whether an expired entity causes petting to stop, and hence a reset.
 *
 * On. The alternative -- record and continue -- is what v1 effectively did by never arming the
 * watchdog at all, and it leaves a half-dead ECU logging nothing while appearing to be alive.
 */
#define WDGM_EXPIRED_STOPS_TRIGGER STD_ON

/**
 * @brief Whether a deactivated entity is allowed indefinitely.
 *
 * Off. An entity deactivated for longer than ::WDGM_MAX_DEACTIVATION_MS raises a diagnostic
 * event, because a permanent deactivation silently removes the protection and is very easy to
 * introduce by forgetting a matching reactivation on an error path.
 */
#define WDGM_ALLOW_PERMANENT_DEACTIVATION STD_OFF

/** Longest an entity may remain deactivated before it is reported, in milliseconds. */
#define WDGM_MAX_DEACTIVATION_MS 300000uL

#endif /* WDGM_CFG_H */

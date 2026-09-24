/**
 * @file    EcuM_Cfg.h
 * @brief   Startup and crash-loop configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef ECUM_CFG_H
#define ECUM_CFG_H

#include "Ecu_Cfg.h"
#include "base/Std_Types.h"

#define ECUM_DEV_ERROR_DETECT STD_ON

/** Resets within the window that constitute a crash loop. */
#define ECUM_CRASH_LOOP_COUNT ECU_CRASH_LOOP_COUNT

/** Window over which resets are counted, in seconds. */
#define ECUM_CRASH_LOOP_WINDOW_S ECU_CRASH_LOOP_WINDOW_S

/**
 * @brief Uptime after which the run is considered stable and the reset counter is cleared, in milliseconds.
 *
 * 300 s. Long enough that a unit which resets after four minutes of apparently normal operation still
 * accumulates toward the threshold; short enough that a genuinely healthy unit clears its history on every
 * journey and never drifts into a degraded mode through accumulated ignition cycles.
 */
#define ECUM_STABLE_RUN_MS 300000uL

/**
 * @brief Whether a crash loop triggers degraded operation.
 *
 * On. The alternative is resetting forever, which is what v1 did on a CAN initialisation failure -- and a
 * unit in a reboot loop logs nothing at all, including the fault that would explain it.
 */
#define ECUM_DEGRADED_MODE_ENABLED STD_ON

/**
 * @brief Whether startup continues when the CAN controller fails to initialise.
 *
 * On. Battery, GNSS and voltage data are still worth recording without it. v1 called ESP.restart() here.
 */
#define ECUM_CONTINUE_WITHOUT_CAN STD_ON

/** Whether startup continues when no battery pack answers discovery. */
#define ECUM_CONTINUE_WITHOUT_PACKS STD_ON

/**
 * @brief Whether startup continues when the card will not mount.
 *
 * On. Live publishing still works; only the store-and-forward buffer is lost. v1 waited ten minutes and
 * then restarted, which guaranteed nothing was ever logged rather than merely not buffered.
 */
#define ECUM_CONTINUE_WITHOUT_STORAGE STD_ON

/** Whether startup continues when no plausible wall-clock time can be established. */
#define ECUM_CONTINUE_WITHOUT_CLOCK STD_ON

/**
 * @brief Delay between initialising a subsystem and the next, in milliseconds.
 *
 * 50 ms. The MCP2515 and the SIM800L both draw a current step when they start, and on a shared 3.3 V rail
 * initialising them back to back has been observed browning out the card. Sequencing them costs a fraction
 * of a second of startup once per power cycle.
 */
#define ECUM_SUBSYSTEM_SETTLE_MS 50u

#endif /* ECUM_CFG_H */

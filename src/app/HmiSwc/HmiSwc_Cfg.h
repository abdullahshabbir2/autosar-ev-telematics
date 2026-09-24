/**
 * @file    HmiSwc_Cfg.h
 * @brief   Status indication timing configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef HMISWC_CFG_H
#define HMISWC_CFG_H

#include "IoHwAb_Cfg.h"
#include "Std_Types.h"

#define HMISWC_DEV_ERROR_DETECT STD_ON

/**
 * @brief Interval at which ::HmiSwc_MainFunction runs, in milliseconds.
 *
 * 100 ms. It must divide both blink periods exactly, or the patterns drift relative to each other and
 * two LEDs nominally on the same pattern visibly diverge. 100 ms divides both 1000 ms and 250 ms, and
 * is fast enough that a pulse is still perceptible.
 */
#define HMI_TICK_MS 100u

/** Ticks per half-period of a slow (1 Hz) blink: 500 ms on, 500 ms off. */
#define HMI_SLOW_HALF_PERIOD_TICKS 5u

/** Ticks per half-period of a fast (4 Hz) blink: 125 ms nominal, rounded to one tick. */
#define HMI_FAST_HALF_PERIOD_TICKS 1u

/** Ticks a pulse stays lit. Two ticks is 200 ms, which is comfortably visible. */
#define HMI_PULSE_TICKS 2u

/**
 * @brief Duration of the startup self-test, in milliseconds.
 *
 * 1500 ms with every indicator lit. Long enough for an installer to see all five, short enough not to
 * delay the first data record noticeably.
 */
#define HMI_SELFTEST_MS 1500u

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((1000u % HMI_TICK_MS) == 0u,
               "HMI_TICK_MS must divide 1000 ms exactly or the slow blink drifts");
_Static_assert((HMI_SLOW_HALF_PERIOD_TICKS * HMI_TICK_MS) == 500u,
               "the slow blink half-period must be 500 ms");
#endif

#endif /* HMISWC_CFG_H */

/**
 * @file    Det_Cfg.h
 * @brief   Pre-compile configuration for the Default Error Tracer.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DET_CFG_H
#define DET_CFG_H

#include "Std_Types.h"

/**
 * @brief Master switch for parameter checking in every module.
 *
 * Kept STD_ON for production on this ECU; see the rationale in Det.h and
 * @ref docs/adr/0004-keep-det-in-production.md. Overridable from the build system
 * (the unit-test build defines it on the command line).
 */
#ifndef DET_ENABLE_DEV_ERROR_DETECT
#define DET_ENABLE_DEV_ERROR_DETECT STD_ON
#endif

/**
 * @brief Stop at the first development error instead of recording and continuing.
 *
 * STD_ON only for bench and unit-test builds. In the field a halt would take the
 * vehicle's data logger offline over what may be a benign integration wrinkle, so
 * production records and carries on.
 */
#ifndef DET_HALT_ON_ERROR
#define DET_HALT_ON_ERROR STD_OFF
#endif

/**
 * @brief Number of distinct (module, instance, api, error) triples retained.
 *
 * Reports are deduplicated: a repeat increments the entry's occurrence counter
 * rather than consuming a slot. 32 entries is generous against the ~40 distinct
 * error codes the whole stack can emit, and costs 32 x 12 = 384 bytes of DRAM.
 */
#ifndef DET_HISTORY_SIZE
#define DET_HISTORY_SIZE 32u
#endif

/**
 * @brief Saturation value for an entry's occurrence counter.
 *
 * Stops at UINT16_MAX rather than wrapping: "at least 65535 times" is actionable,
 * whereas a wrapped counter reading 3 is actively misleading.
 */
#define DET_OCCURRENCE_MAX 0xFFFFu

#endif /* DET_CFG_H */

/**
 * @file    Log_Cfg.h
 * @brief   Logging configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef LOG_CFG_H
#define LOG_CFG_H

#include "base/Std_Types.h"

/**
 * @brief Level a production unit starts at.
 *
 * Warn. A field unit has no serial connection, so anything below a fault is emitted to nobody at the
 * cost of formatting it. The level can be raised at runtime over the diagnostic channel when a unit
 * needs investigating, which is what makes the low default acceptable.
 */
#ifndef LOG_DEFAULT_LEVEL
#define LOG_DEFAULT_LEVEL LOG_LEVEL_WARN
#endif

/**
 * @brief Largest level this build can emit at all.
 *
 * Debug, so a bench unit can be turned up without a rebuild. Setting this lower lets the optimiser
 * remove the higher-level call sites entirely, which is worth doing on a flash-constrained variant.
 */
#ifndef LOG_MAX_COMPILED_LEVEL
#define LOG_MAX_COMPILED_LEVEL LOG_LEVEL_DEBUG
#endif

/**
 * @brief Buffer for one formatted message, including the terminator.
 *
 * 192 bytes. Long enough for a fault line with a module name, a code and a couple of values; short
 * enough to sit on any task's stack. A longer message is truncated and counted rather than being
 * allowed to grow a buffer.
 */
#define LOG_MESSAGE_BUFFER_SIZE 192u

/** Include the monotonic timestamp in each line. */
#define LOG_INCLUDE_TIMESTAMP STD_ON

/** Include the reporting module's ID in each line. */
#define LOG_INCLUDE_MODULE_ID STD_ON

/** Console baud rate. */
#define LOG_CONSOLE_BAUD 115200uL

#endif /* LOG_CFG_H */

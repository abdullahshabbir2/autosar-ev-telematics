/**
 * @file    Log.h
 * @brief   Structured logging front end.
 *
 * A bounded, level-filtered console logger. The only module in the project permitted to format text for
 * a human, and it is deliberately the only one: v1 called @c log_d, @c log_i and @c log_e from every
 * module with @c CORE_DEBUG_LEVEL=5 compiled in, which on a 3 s cycle produced tens of kilobytes per
 * minute of serial output and a measurable share of the CPU -- on units that have no serial connection
 * in service.
 *
 * @par Three rules
 *  - **Level-filtered at runtime, not just at compile time.** A field unit runs at ::LOG_LEVEL_WARN and
 *    can be raised to ::LOG_LEVEL_DEBUG by a diagnostic command, so a fault can be investigated without
 *    reflashing.
 *  - **Bounded output.** Every message is formatted into a fixed buffer and truncated if it does not
 *    fit. No allocation, and no unbounded @c printf into a stream that may block.
 *  - **Never on a hot path.** Logging is for startup, mode changes and faults. Per-record logging is
 *    what made v1's output useless: the interesting lines were buried in thousands of routine ones.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef LOG_H
#define LOG_H

#include "Autosar_ModuleIds.h"
#include "Log_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_VENDOR_ID 0xFFFEu
#define LOG_SW_MAJOR_VERSION 2u
#define LOG_SW_MINOR_VERSION 0u
#define LOG_SW_PATCH_VERSION 0u

/** Severity levels, ordered so a numeric comparison filters them. */
typedef enum
{
    LOG_LEVEL_NONE = 0,  /**< Nothing is emitted.                    */
    LOG_LEVEL_ERROR = 1, /**< A fault the ECU could not absorb.       */
    LOG_LEVEL_WARN = 2,  /**< A fault it did absorb.                  */
    LOG_LEVEL_INFO = 3,  /**< Startup, mode changes, provisioning.    */
    LOG_LEVEL_DEBUG = 4  /**< Per-cycle detail. Bench use only.       */
} Log_LevelType;

/** Initialise the console and set the level to ::LOG_DEFAULT_LEVEL. */
CHECK_RETURN Std_ReturnType Log_Init(void);

/** Set the runtime level. Reachable from the diagnostic channel. */
void Log_SetLevel(Log_LevelType level);

/** The level currently in force. */
Log_LevelType Log_GetLevel(void);

/**
 * @brief Emit a message if @p level passes the current filter.
 *
 * @param level  Severity.
 * @param module Reporting module ID, from Autosar_ModuleIds.h.
 * @param format printf-style format string.
 *
 * Format checking is enforced by the compiler through ::PRINTF_LIKE, so a mismatched argument is a
 * build error rather than a corrupted line or a crash.
 */
void Log_Print(Log_LevelType level, uint16 module, const char *format, ...) PRINTF_LIKE(3, 4);

/** Messages emitted since Init. */
uint32 Log_GetMessageCount(void);

/** Messages suppressed by the level filter, so the cost of raising the level is knowable. */
uint32 Log_GetSuppressedCount(void);

/** Messages truncated because they exceeded ::LOG_MESSAGE_BUFFER_SIZE. */
uint32 Log_GetTruncatedCount(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Log_GetVersionInfo(Std_VersionInfoType *versioninfo);

/*==================================================================================================
 *  Convenience macros
 *
 *  The level check happens before the call, so a suppressed message costs one comparison rather than a
 *  function call with a variadic argument list.
 *================================================================================================*/

#define LOG_ERROR(module, ...)                          \
    do                                                  \
    {                                                   \
        if (Log_GetLevel() >= LOG_LEVEL_ERROR)          \
        {                                               \
            Log_Print(LOG_LEVEL_ERROR, (module), __VA_ARGS__); \
        }                                               \
    } while (0)

#define LOG_WARN(module, ...)                           \
    do                                                  \
    {                                                   \
        if (Log_GetLevel() >= LOG_LEVEL_WARN)           \
        {                                               \
            Log_Print(LOG_LEVEL_WARN, (module), __VA_ARGS__); \
        }                                               \
    } while (0)

#define LOG_INFO(module, ...)                           \
    do                                                  \
    {                                                   \
        if (Log_GetLevel() >= LOG_LEVEL_INFO)           \
        {                                               \
            Log_Print(LOG_LEVEL_INFO, (module), __VA_ARGS__); \
        }                                               \
    } while (0)

#define LOG_DEBUG(module, ...)                          \
    do                                                  \
    {                                                   \
        if (Log_GetLevel() >= LOG_LEVEL_DEBUG)          \
        {                                               \
            Log_Print(LOG_LEVEL_DEBUG, (module), __VA_ARGS__); \
        }                                               \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* LOG_H */

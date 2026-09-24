/**
 * @file    Log.c
 * @brief   Logging front end implementation.
 *
 * The formatting and filtering are here; emitting the finished line is the platform's job, so the same
 * logic serves the ESP32's UART console and the host test build's stdout.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Log.h"

#include <stdarg.h>
#include <stdio.h>

#include "Gpt.h"
#include "Log_Platform.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC Log_LevelType Log_Level = LOG_DEFAULT_LEVEL;
STATIC uint32 Log_MessageCount;
STATIC uint32 Log_SuppressedCount;
STATIC uint32 Log_TruncatedCount;
STATIC boolean Log_Initialised = FALSE;

/**
 * @brief Single formatting buffer.
 *
 * One buffer rather than one per call, because 192 bytes on four different task stacks is 768 bytes of
 * RAM for a facility that is not on any hot path. Logging happens from several tasks, so a line could in
 * principle be interleaved with another; that is accepted deliberately -- the alternative is a mutex on
 * a diagnostic facility, and a garbled line is a far smaller problem than a logging call that can block
 * a task holding the SPI bus.
 */
STATIC char Log_Buffer[LOG_MESSAGE_BUFFER_SIZE];

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

/** One-character tag for @p level. */
STATIC char Log_LevelTag(Log_LevelType level)
{
    char tag;

    switch (level)
    {
    case LOG_LEVEL_ERROR:
        tag = 'E';
        break;
    case LOG_LEVEL_WARN:
        tag = 'W';
        break;
    case LOG_LEVEL_INFO:
        tag = 'I';
        break;
    case LOG_LEVEL_DEBUG:
        tag = 'D';
        break;
    case LOG_LEVEL_NONE:
    default:
        tag = '?';
        break;
    }

    return tag;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType Log_Init(void)
{
    Log_Level = LOG_DEFAULT_LEVEL;
    Log_MessageCount = 0u;
    Log_SuppressedCount = 0u;
    Log_TruncatedCount = 0u;
    Log_Initialised = TRUE;

    return Log_PlatformInit(LOG_CONSOLE_BAUD);
}

void Log_SetLevel(Log_LevelType level)
{
    /* Clamped to what this build can emit, so a diagnostic command asking for a level the build was
     * compiled without produces the highest available rather than silence. */
    Log_Level = (level > (Log_LevelType)LOG_MAX_COMPILED_LEVEL) ? (Log_LevelType)LOG_MAX_COMPILED_LEVEL
                                                               : level;
}

Log_LevelType Log_GetLevel(void)
{
    return Log_Level;
}

void Log_Print(Log_LevelType level, uint16 module, const char *format, ...)
{
    va_list args;
    int prefixLength;
    int bodyLength;

    if ((Log_Initialised == FALSE) || (level > Log_Level) || (level == LOG_LEVEL_NONE))
    {
        Log_SuppressedCount++;
        return;
    }
    if (format == NULL_PTR)
    {
        return;
    }

#if ((LOG_INCLUDE_TIMESTAMP == STD_ON) && (LOG_INCLUDE_MODULE_ID == STD_ON))
    prefixLength = snprintf(Log_Buffer, sizeof(Log_Buffer), "[%10lu][%c][%04X] ",
                            (unsigned long)Gpt_GetMonotonicMs(), Log_LevelTag(level),
                            (unsigned int)module);
#elif (LOG_INCLUDE_TIMESTAMP == STD_ON)
    prefixLength = snprintf(Log_Buffer, sizeof(Log_Buffer), "[%10lu][%c] ",
                            (unsigned long)Gpt_GetMonotonicMs(), Log_LevelTag(level));
#else
    prefixLength = snprintf(Log_Buffer, sizeof(Log_Buffer), "[%c] ", Log_LevelTag(level));
    COMPILER_UNUSED(module);
#endif

    if ((prefixLength < 0) || ((uint32)prefixLength >= (uint32)sizeof(Log_Buffer)))
    {
        /* The prefix alone filled the buffer, which means the configuration is inconsistent rather than
         * the message being long. Nothing useful can be emitted. */
        Log_TruncatedCount++;
        return;
    }

    va_start(args, format);
    bodyLength = vsnprintf(&Log_Buffer[prefixLength], sizeof(Log_Buffer) - (uint32)prefixLength,
                           format, args);
    va_end(args);

    if (bodyLength < 0)
    {
        Log_TruncatedCount++;
        return;
    }

    /* vsnprintf returns what it *would* have written, so a return at or beyond the remaining space means
     * the line was truncated. Counted, so an operator raising the level can tell whether they are seeing
     * whole messages. */
    if ((uint32)bodyLength >= (sizeof(Log_Buffer) - (uint32)prefixLength))
    {
        Log_TruncatedCount++;
    }

    Log_PlatformWrite(Log_Buffer);
    Log_MessageCount++;
}

uint32 Log_GetMessageCount(void)
{
    return Log_MessageCount;
}

uint32 Log_GetSuppressedCount(void)
{
    return Log_SuppressedCount;
}

uint32 Log_GetTruncatedCount(void)
{
    return Log_TruncatedCount;
}

void Log_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = LOG_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_LOG;
        versioninfo->sw_major_version = LOG_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = LOG_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = LOG_SW_PATCH_VERSION;
    }
}

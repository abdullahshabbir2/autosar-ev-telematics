/**
 * @file    Stub_Log.c
 * @brief   Logging platform leaf for the host build: captures lines instead of emitting them.
 *
 * Captured rather than printed, so a test suite's output stays readable and so a test can assert that a
 * particular fault was reported.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "services/Log/Log_Platform.h"
#include "Stub_Log.h"

static char Stub_LogLines[STUB_LOG_MAX_LINES][STUB_LOG_MAX_LINE];
static uint16 Stub_LogCount;
static boolean Stub_LogEcho;

Std_ReturnType Log_PlatformInit(uint32 baud)
{
    COMPILER_UNUSED(baud);
    return E_OK;
}

void Log_PlatformWrite(const char *line)
{
    if (line == NULL_PTR)
    {
        return;
    }

    /* The ring keeps the most recent lines. A test that cares about an early line asserts on it before
     * generating more, which is simpler than growing the buffer to hold a whole run. */
    if (Stub_LogCount >= STUB_LOG_MAX_LINES)
    {
        uint16 i;
        for (i = 1u; i < STUB_LOG_MAX_LINES; i++)
        {
            (void)memcpy(Stub_LogLines[i - 1u], Stub_LogLines[i], STUB_LOG_MAX_LINE);
        }
        Stub_LogCount = (uint16)(STUB_LOG_MAX_LINES - 1u);
    }

    (void)memset(Stub_LogLines[Stub_LogCount], 0, STUB_LOG_MAX_LINE);
    (void)strncpy(Stub_LogLines[Stub_LogCount], line, STUB_LOG_MAX_LINE - 1u);
    Stub_LogCount++;

    if (Stub_LogEcho != FALSE)
    {
        (void)fputs(line, stdout);
        (void)fputc('\n', stdout);
    }
}

void Stub_Log_Reset(void)
{
    (void)memset(Stub_LogLines, 0, sizeof(Stub_LogLines));
    Stub_LogCount = 0u;
    Stub_LogEcho = FALSE;
}

uint16 Stub_Log_GetLineCount(void)
{
    return Stub_LogCount;
}

const char *Stub_Log_GetLine(uint16 index)
{
    return (index < Stub_LogCount) ? Stub_LogLines[index] : NULL_PTR;
}

boolean Stub_Log_ContainsText(const char *needle)
{
    uint16 i;

    if (needle == NULL_PTR)
    {
        return FALSE;
    }
    for (i = 0u; i < Stub_LogCount; i++)
    {
        if (strstr(Stub_LogLines[i], needle) != NULL_PTR)
        {
            return TRUE;
        }
    }
    return FALSE;
}

void Stub_Log_SetEcho(boolean echo)
{
    Stub_LogEcho = echo;
}

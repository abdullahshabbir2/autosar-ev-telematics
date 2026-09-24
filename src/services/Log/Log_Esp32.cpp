/**
 * @file    Log_Esp32.cpp
 * @brief   ESP32 platform leaf of the logging front end: UART0 console.
 *
 * @par Why this must not block
 * ::Log_PlatformWrite is reachable from every task, including ones holding the SPI bus mutex or mid-way
 * through an RS485 turnaround. Arduino's @c Serial.print blocks once the transmit FIFO fills, which at 115 200
 * baud takes about 9 ms for a 128-byte line -- and blocks indefinitely if the host has stopped reading. A
 * diagnostic facility that can stall the acquisition task is worse than no diagnostics, so a line that does not
 * fit is dropped and counted rather than waited on.
 *
 * Log.c counts the drops and includes the figure in the health record, which is what keeps this from hiding
 * the fact that output was lost.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <string.h>

#include "services/Log/Log_Platform.h"

static boolean Log_ConsoleReady = FALSE;

extern "C" Std_ReturnType Log_PlatformInit(uint32 baud)
{
    Serial.begin(baud);

    /* No wait for the port to open. On a board with a USB-serial bridge there is nothing to wait for, and on
     * one with native USB the wait never ends when no host is attached -- which is every deployed unit. v1's
     * `while (!Serial);` is the reason a field unit with no laptop plugged in never reached its main loop. */
    Log_ConsoleReady = TRUE;

    return E_OK;
}

extern "C" void Log_PlatformWrite(const char *line)
{
    if ((Log_ConsoleReady == FALSE) || (line == NULL_PTR))
    {
        return;
    }

    {
        const size_t length = strlen(line);

        /* +1 for the newline, so a line is either emitted whole or not at all. A partial line would corrupt
         * the one after it in the capture, and the log's value depends on being able to trust that a line
         * that appears is a line that was written. */
        if ((size_t)Serial.availableForWrite() < (length + 1u))
        {
            return;
        }

        (void)Serial.write((const uint8_t *)line, length);
        (void)Serial.write('\n');
    }
}

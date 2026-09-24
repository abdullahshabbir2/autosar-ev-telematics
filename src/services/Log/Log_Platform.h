/**
 * @file    Log_Platform.h
 * @brief   Platform leaf of the logging front end.
 *
 * Implemented by Log_Esp32.cpp on the target (UART0 console) and by test/support/Stub_Log.c on the
 * host, where lines are captured so a test can assert on them.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef LOG_PLATFORM_H
#define LOG_PLATFORM_H

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the console at @p baud. */
CHECK_RETURN Std_ReturnType Log_PlatformInit(uint32 baud);

/**
 * @brief Emit one NUL-terminated line, appending a terminator.
 *
 * Must not block indefinitely: it is called from tasks that may hold the SPI bus, and a console with no
 * reader attached would otherwise stall them. The ESP32 implementation writes to the UART's FIFO and
 * drops the line if it is full, which is the correct trade for a diagnostic facility.
 */
void Log_PlatformWrite(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* LOG_PLATFORM_H */

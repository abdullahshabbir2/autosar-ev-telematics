/**
 * @file    Stub_Log.h
 * @brief   Control surface for the logging test double.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef STUB_LOG_H
#define STUB_LOG_H

#include <stdio.h>

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Lines the double retains. */
#define STUB_LOG_MAX_LINES 64u

/** Bytes per retained line. */
#define STUB_LOG_MAX_LINE 200u

/** Discard every captured line. */
void Stub_Log_Reset(void);

/** Lines captured since the last reset. */
uint16 Stub_Log_GetLineCount(void);

/** Line @p index, or NULL_PTR if out of range. */
const char *Stub_Log_GetLine(uint16 index);

/** TRUE if any captured line contains @p needle. */
boolean Stub_Log_ContainsText(const char *needle);

/** Also print captured lines to stdout. Off by default so suite output stays readable. */
void Stub_Log_SetEcho(boolean echo);

#ifdef __cplusplus
}
#endif

#endif /* STUB_LOG_H */

/**
 * @file    Stub_SchM.h
 * @brief   Control surface for the scheduler test double.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef STUB_SCHM_H
#define STUB_SCHM_H

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reset the double. */
void Stub_SchM_Reset(void);

/** Tasks the scheduler asked the platform to create. */
uint8 Stub_SchM_GetTasksCreated(void);

/** Make task creation fail, to exercise the startup path's reaction. */
void Stub_SchM_SetCreateFails(boolean fails);

/** Set the stack high-water figure the platform reports. */
void Stub_SchM_SetStackHighWaterMark(uint32 bytes);

#ifdef __cplusplus
}
#endif

#endif /* STUB_SCHM_H */

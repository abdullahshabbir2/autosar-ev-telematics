/**
 * @file    SchM_Platform.h
 * @brief   Platform leaf of the scheduler: task creation and stack measurement.
 *
 * Implemented by SchM_Esp32.cpp on the target (FreeRTOS tasks pinned to cores) and by
 * test/support/Stub_SchM.c on the host, where no threads are created and a test drives ::SchM_RunTask
 * directly.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef SCHM_PLATFORM_H
#define SCHM_PLATFORM_H

#include "services/SchM/SchM.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the task that periodically calls ::SchM_RunTask for @p task.
 *
 * The task body must call ::SchM_RunTask and then wait until the next period boundary, measured from the
 * previous boundary rather than from the end of the body. Measuring from the end would let a slow
 * activation push every subsequent one later, so the period would drift with load -- which is what v1's
 * mixture of vTaskDelayUntil and blocking delay() calls produced.
 *
 * @param task       Which task to create.
 * @param name       Short name, for the RTOS's own task list.
 * @param stackBytes Stack size in bytes.
 * @param priority   RTOS priority; higher is more urgent.
 * @param core       Core to pin to.
 * @param periodMs   Activation period.
 * @return E_OK if the task was created.
 */
CHECK_RETURN Std_ReturnType SchM_PlatformCreateTask(SchM_TaskType task, const char *name,
                                                    uint32 stackBytes, uint8 priority, uint8 core,
                                                    uint32 periodMs);

/**
 * @brief Smallest free stack observed on the calling task, in bytes.
 *
 * Reported in the health record so the configured stack sizes can be tightened against evidence instead of
 * being guessed downward. Returns 0 where the platform cannot measure it.
 */
uint32 SchM_PlatformGetStackHighWaterMark(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHM_PLATFORM_H */

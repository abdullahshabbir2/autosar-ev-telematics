/**
 * @file    Stub_SchM.c
 * @brief   Scheduler platform leaf for the host build: no threads are created.
 *
 * A host test drives the schedule by calling ::SchM_RunTask directly, which makes the execution order
 * deterministic and lets virtual time advance under the test's control. Creating real threads would make
 * the tests both slow and non-reproducible, and would test FreeRTOS rather than the schedule.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/SchM/SchM_Platform.h"
#include "Stub_SchM.h"

static uint8 Stub_SchMTasksCreated;
static boolean Stub_SchMCreateFails;
static uint32 Stub_SchMStackHighWater = 2048u;

Std_ReturnType SchM_PlatformCreateTask(SchM_TaskType task, const char *name, uint32 stackBytes,
                                       uint8 priority, uint8 core, uint32 periodMs)
{
    COMPILER_UNUSED(task);
    COMPILER_UNUSED(name);
    COMPILER_UNUSED(stackBytes);
    COMPILER_UNUSED(priority);
    COMPILER_UNUSED(core);
    COMPILER_UNUSED(periodMs);

    if (Stub_SchMCreateFails != FALSE)
    {
        return E_NOT_OK;
    }

    Stub_SchMTasksCreated++;
    return E_OK;
}

uint32 SchM_PlatformGetStackHighWaterMark(void)
{
    return Stub_SchMStackHighWater;
}

void Stub_SchM_Reset(void)
{
    Stub_SchMTasksCreated = 0u;
    Stub_SchMCreateFails = FALSE;
    Stub_SchMStackHighWater = 2048u;
}

uint8 Stub_SchM_GetTasksCreated(void)
{
    return Stub_SchMTasksCreated;
}

void Stub_SchM_SetCreateFails(boolean fails)
{
    Stub_SchMCreateFails = fails;
}

void Stub_SchM_SetStackHighWaterMark(uint32 bytes)
{
    Stub_SchMStackHighWater = bytes;
}

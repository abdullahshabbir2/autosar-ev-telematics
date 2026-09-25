/**
 * @file    SchM_Esp32.cpp
 * @brief   ESP32 platform leaf of the scheduler: FreeRTOS tasks pinned to cores.
 *
 * @par The period is measured from the previous boundary, not from the end of the body
 * The loop uses @c vTaskDelayUntil with a running deadline rather than @c vTaskDelay with a fixed interval.
 * The difference is the whole reason this file is worth reading:
 *
 *   - `vTaskDelay(period)` waits @e period after the body finishes, so the activation rate is
 *     `period + executionTime`. Every slow activation pushes all later ones further out, and the sampling
 *     interval silently becomes a function of load.
 *   - `vTaskDelayUntil(&last, period)` waits until `last + period` and advances `last` by exactly one period,
 *     so a body that overran is followed by a shorter wait and the schedule recovers.
 *
 * That matters here because the odometer integrates rpm over time. An acquisition interval that stretched
 * under load would make the integration interval disagree with the assumed period, and the distance would come
 * out short by exactly the amount of the drift -- a slowly accumulating error that looks like a calibration
 * problem rather than a scheduling one. v1 mixed `vTaskDelay`, bare `delay()` and blocking waits inside the
 * body, and its nominal 3 s loop measured between 3.2 s and 13 s.
 *
 * A body that overruns by more than a whole period is a different matter: @c vTaskDelayUntil would then try to
 * catch up by running back-to-back with no delay at all, starving lower-priority tasks. The deadline is
 * resynchronised in that case and SchM counts it as an overrun.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "services/Det/Det.h"
#include "services/SchM/SchM.h"
#include "services/SchM/SchM_Platform.h"
#include "mcal/Wdg/Wdg.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/**
 * @brief What one created task needs to know about itself.
 *
 * Passed to the task body as its parameter. One entry per task, file-scope, because the structure must outlive
 * ::SchM_PlatformCreateTask -- a stack-local would be gone before the new task first ran, which is a
 * use-after-return that happens to work most of the time and is therefore worth being explicit about.
 */
typedef struct
{
    SchM_TaskType task; /**< Which task this is.  */
    uint32 periodMs;    /**< Activation period.   */
    uint32 stackBytes;  /**< Configured stack.    */
} SchM_TaskContextType;

static SchM_TaskContextType SchM_TaskContext[SCHM_TASK_COUNT];
static TaskHandle_t SchM_TaskHandle[SCHM_TASK_COUNT];

/*==================================================================================================
 *  Task body
 *================================================================================================*/

/**
 * @brief The body every scheduler task runs.
 *
 * Identical for all four tasks; the only difference is the context. A single body is what makes the timing
 * guarantee above hold uniformly -- four hand-written loops would need the same care taken four times.
 */
static void SchM_TaskBody(void *parameter)
{
    SchM_TaskContextType *const context = (SchM_TaskContextType *)parameter;
    const TickType_t period = pdMS_TO_TICKS(context->periodMs);
    TickType_t lastWake;

    /* Subscribed by the task itself, as its first action, because the ESP-IDF task watchdog registers
     * whichever task is currently running. Doing it from the creating task would subscribe the creator --
     * which is exactly the sort of mistake that leaves a watchdog supervising nothing, as v1's did. */
    if (Wdg_SubscribeCurrentTask() != E_OK)
    {
        (void)Det_ReportError(MODULE_ID_SCHM, (uint8)context->task, SCHM_API_ID_INIT, E_NOT_OK);
    }

    lastWake = xTaskGetTickCount();

    for (;;)
    {
        SchM_RunTask(context->task);

        /* The overrun case is detected here rather than taken from the delay function's return value. Newer
         * FreeRTOS offers xTaskDelayUntil, which reports whether the deadline had already passed; the version
         * this framework ships has only vTaskDelayUntil, which returns void. So the comparison is done
         * explicitly, which also makes the reasoning visible instead of hidden behind a return code.
         *
         * The subtraction is modular and therefore wrap-safe: TickType_t wraps, and `now - lastWake` yields
         * the true elapsed count across the wrap where `now < lastWake` would read as a deadline in the
         * future. That is the same argument as Gpt_ElapsedSince, for the same reason.
         *
         * When the body has overrun a whole period, continuing from the stale deadline would run it
         * back-to-back with no delay until it caught up, starving every lower-priority task. On the
         * connectivity task that means the network stack never runs, so the ECU stops publishing at exactly
         * the moment it is busiest. Resynchronising sacrifices the missed activation, which is the right
         * trade: SchM has already counted the overrun, and one lost sample is cheaper than a starved system. */
        {
            const TickType_t now = xTaskGetTickCount();

            if ((TickType_t)(now - lastWake) >= period)
            {
                lastWake = now;
            }
            else
            {
                vTaskDelayUntil(&lastWake, period);
            }
        }
    }
}

/*==================================================================================================
 *  Platform leaf
 *================================================================================================*/

extern "C" Std_ReturnType SchM_PlatformCreateTask(SchM_TaskType task, const char *name, uint32 stackBytes,
                                                  uint8 priority, uint8 core, uint32 periodMs)
{
    BaseType_t created;

    if ((task >= (SchM_TaskType)SCHM_TASK_COUNT) || (name == NULL_PTR) || (periodMs == 0uL))
    {
        (void)Det_ReportError(MODULE_ID_SCHM, (uint8)task, SCHM_API_ID_INIT, E_PARAM_VALUE);
        return E_NOT_OK;
    }

    SchM_TaskContext[task].task = task;
    SchM_TaskContext[task].periodMs = periodMs;
    SchM_TaskContext[task].stackBytes = stackBytes;

    /* xTaskCreatePinnedToCore takes the stack depth in *words* on ESP32, not bytes -- unlike vanilla FreeRTOS
     * on some ports. Passing bytes would create a stack four times larger than intended, which wastes about
     * 21 KiB across the four tasks here and, worse, would hide a genuine stack overflow during development
     * only to expose it when someone later "corrected" the units. */
    created = xTaskCreatePinnedToCore(SchM_TaskBody, name, (uint32_t)(stackBytes / sizeof(StackType_t)),
                                      &SchM_TaskContext[task], (UBaseType_t)priority, &SchM_TaskHandle[task],
                                      (BaseType_t)core);

    if (created != pdPASS)
    {
        /* The one startup failure EcuM treats as fatal. Every other subsystem can be degraded around; a task
         * that does not exist means that part of the ECU's job is simply not being done, and there is nothing
         * to fall back to. */
        (void)Det_ReportError(MODULE_ID_SCHM, (uint8)task, SCHM_API_ID_INIT, E_NO_SPACE);
        SchM_TaskHandle[task] = NULL;
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" uint32 SchM_PlatformGetStackHighWaterMark(void)
{
    /* uxTaskGetStackHighWaterMark reports the smallest free space ever observed, in words. NULL means the
     * calling task, which is what makes this callable from inside a runnable without needing a handle table
     * lookup -- and means the figure always belongs to the task that reports it. */
    return (uint32)uxTaskGetStackHighWaterMark(NULL) * (uint32)sizeof(StackType_t);
}

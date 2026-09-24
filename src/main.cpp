/**
 * @file    main.cpp
 * @brief   Entry point. Delegates to EcuM and then gets out of the way.
 *
 * @par Why this file is nine lines of code
 * Everything about starting this ECU -- the order the layers come up in, what happens when one of them does
 * not, crash-loop detection, the degraded modes -- is in ::EcuM_Init, where it is one readable sequence with
 * its reasoning attached and where the host test suite can reach it. Nothing about that belongs in an Arduino
 * entry point.
 *
 * The @c loop() body is empty on purpose, and that is the most important thing on this page. The four cyclic
 * tasks are FreeRTOS tasks created by SchM, each with its own period, priority, core assignment, execution
 * budget and watchdog supervision. Work placed in @c loop() would run on the Arduino loop task instead: at
 * priority 1, unsupervised, on an unspecified core, with no period and no budget. It would be invisible to
 * every mechanism this firmware uses to know it is healthy.
 *
 * v1 put all of it in @c loop() -- 719 lines of acquisition, storage, upload and web server, with blocking
 * @c delay() calls interleaved -- which is why its nominal 3-second cycle measured between 3.2 and 13 seconds
 * depending on what the network was doing, and why a stall anywhere stopped everything.
 *
 * @par Arduino's loop task
 * There is no way to stop the core creating the loop task, so it is left to run and yields immediately. It
 * costs one context switch per tick and 8 KiB of stack that cannot be reclaimed. Deleting itself with
 * @c vTaskDelete(NULL) would free the stack, and is deliberately not done: the Arduino core's own housekeeping
 * runs on that task on some framework versions, and reclaiming 8 KiB is not worth depending on which.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>

#include "services/EcuM/EcuM.h"
#include "mcal/Mcu/Mcu.h"

void setup(void)
{
    if (EcuM_Init() != E_OK)
    {
        /* The single fatal startup outcome: one or more tasks could not be created, so part of the ECU's job is
         * simply not being done and there is nothing to degrade to. Every other failure -- no CAN controller, no
         * card, no packs answering, no network -- is handled inside EcuM_Init by raising a diagnostic event and
         * continuing, because a data logger that records less is worth far more than one that records nothing.
         *
         * A plain reset, not ::EcuM_ShutdownAndReset: the flush path that one performs runs through modules
         * whose tasks do not exist. EcuM has already persisted the reset reason, which is what the next boot's
         * crash-loop detection needs, and after the threshold that next boot starts in a degraded mode rather
         * than repeating this forever. */
        Mcu_PerformReset();
    }
}

void loop(void)
{
    /* Empty by design -- see the file comment. The delay yields the CPU to the four supervised tasks rather
     * than spinning; without it this task would consume its full time slice doing nothing on every tick. */
    delay(1000);
}

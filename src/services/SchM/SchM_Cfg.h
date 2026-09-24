/**
 * @file    SchM_Cfg.h
 * @brief   Task and runnable schedule.
 *
 * The whole execution model of the ECU, in one table. Every period, priority, core assignment, stack size
 * and budget is here with its justification, because a schedule whose numbers nobody can account for is a
 * schedule that gets widened the first time it trips.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef SCHM_CFG_H
#define SCHM_CFG_H

#include "Ecu_Cfg.h"
#include "base/Std_Types.h"
#include "services/WdgM/WdgM_Cfg.h"

#define SCHM_DEV_ERROR_DETECT STD_ON

/** Tasks the scheduler creates. */
#define SCHM_TASK_COUNT 4u

/** Most runnables any one task may dispatch. */
#define SCHM_MAX_RUNNABLES_PER_TASK 8u

/*==================================================================================================
 *  Periods
 *================================================================================================*/

#define SCHM_PERIOD_SCHEDULER_MS ECU_SCHEDULER_TICK_MS      /* 10 ms   */
#define SCHM_PERIOD_ACQUISITION_MS ECU_ACQUISITION_PERIOD_MS /* 3000 ms */
#define SCHM_PERIOD_STORAGE_MS ECU_ACQUISITION_PERIOD_MS     /* 3000 ms */
#define SCHM_PERIOD_CONNECTIVITY_MS ECU_CONNECTIVITY_PERIOD_MS /* 1000 ms */

/*==================================================================================================
 *  Priorities
 *
 *  FreeRTOS on ESP32: higher number is higher priority, and the idle task is 0. The ordering is what
 *  matters, and it follows from what each task must not be delayed by:
 *
 *    5  Scheduler tick   Supervision must run even when everything else is late, or the watchdog manager
 *                        cannot distinguish "the system is busy" from "the system has stopped".
 *    4  Acquisition      RS485 is a timed half-duplex protocol; being preempted mid-exchange risks a
 *                        turnaround violation that costs the whole frame.
 *    3  Storage          An SD write may block for seconds, so it must not hold off acquisition.
 *    2  Connectivity     The network stack is the most tolerant of delay -- everything it sends is
 *                        already durable on the card.
 *================================================================================================*/

#define SCHM_PRIORITY_SCHEDULER 5u
#define SCHM_PRIORITY_ACQUISITION 4u
#define SCHM_PRIORITY_STORAGE 3u
#define SCHM_PRIORITY_CONNECTIVITY 2u

/*==================================================================================================
 *  Core assignment
 *
 *  The ESP32's WiFi and Bluetooth stacks run on core 0. Putting connectivity there too avoids cross-core
 *  contention on the lwIP locks; keeping acquisition on core 1 keeps the radio's unpredictable latency
 *  away from the RS485 timing, which is the part of this ECU with real deadlines.
 *================================================================================================*/

#define SCHM_CORE_SCHEDULER 1u
#define SCHM_CORE_ACQUISITION 1u
#define SCHM_CORE_STORAGE 1u
#define SCHM_CORE_CONNECTIVITY 0u

/*==================================================================================================
 *  Stack sizes, in bytes
 *
 *  Sized from the largest automatic allocation on each task's deepest call path, plus the ESP-IDF
 *  overhead, plus margin. The numbers are conservative on purpose: a stack overflow on this part corrupts
 *  whatever is adjacent rather than trapping, and the resulting fault appears nowhere near its cause.
 *  ::SchM_GetTaskStats reports the observed high-water mark so these can be tightened with evidence
 *  rather than guessed downward.
 *================================================================================================*/

/** Scheduler tick: shallow, but the CAN driver's SPI transfer buffers sit on it. */
#define SCHM_STACK_SCHEDULER 4096u

/**
 * @brief Acquisition: the deepest path in the system.
 *
 * Rs485If's 81-byte frame buffer is module-static, but the request builders, the UART read path and
 * BattSwc's per-pack health computation are all on the stack, and Adc's oversampling adds a 7-entry array.
 */
#define SCHM_STACK_ACQUISITION 6144u

/**
 * @brief Storage: dominated by record serialisation and the SD driver.
 *
 * Com's record buffer and FsAbs's line buffer are module-static, but the CSV header buffer is 3 KiB on the
 * stack during the once-a-day file creation, and the ESP-IDF FAT driver is not shallow.
 */
#define SCHM_STACK_STORAGE 8192u

/**
 * @brief Connectivity: the largest, because the TLS-capable MQTT client and lwIP are on it.
 *
 * 10 KiB even without TLS enabled, because the WiFi driver's callbacks run on the calling task's stack.
 */
#define SCHM_STACK_CONNECTIVITY 10240u

/*==================================================================================================
 *  Execution budgets
 *
 *  The share of its period a task's body may consume before an overrun is reported. Below 100 % so that
 *  approaching the limit is visible before the period is actually missed -- a task reported at 100 % has
 *  already failed, whereas one reported at 70 % is a warning.
 *================================================================================================*/

/** Scheduler tick: 10 ms period, 5 ms budget. It must never be the reason anything else is late. */
#define SCHM_BUDGET_SCHEDULER_US 5000uL

/**
 * @brief Acquisition: 3000 ms period, 2400 ms budget.
 *
 * A full RS485 round is four packs x two objects x (request + response + gap). At 4800 baud the two
 * responses are 121 ms and 186 ms, so a clean round is about 1.3 s; with one retry on one pack it reaches
 * roughly 1.8 s. 2400 ms leaves headroom for that without allowing the 13 s v1 managed.
 */
#define SCHM_BUDGET_ACQUISITION_US 2400000uL

/** Storage: 3000 ms period, 2000 ms budget. An SD write behind wear levelling has reached 2 s. */
#define SCHM_BUDGET_STORAGE_US 2000000uL

/** Connectivity: 1000 ms period, 800 ms budget. */
#define SCHM_BUDGET_CONNECTIVITY_US 800000uL

/*==================================================================================================
 *  Supervised entity mapping
 *
 *  Each task reports to one WdgM supervised entity, whose alive bounds and deadline are derived from the
 *  period above. Kept as an explicit mapping so the two configurations cannot silently disagree about
 *  which task is which.
 *================================================================================================*/

#define SCHM_SE_SCHEDULER WDGM_SE_SCHEDULER
#define SCHM_SE_ACQUISITION WDGM_SE_ACQUISITION
#define SCHM_SE_STORAGE WDGM_SE_STORAGE
#define SCHM_SE_CONNECTIVITY WDGM_SE_TELEMETRY

/*==================================================================================================
 *  Consistency checks
 *================================================================================================*/

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(SCHM_BUDGET_SCHEDULER_US < (SCHM_PERIOD_SCHEDULER_MS * 1000uL),
               "the scheduler budget must be less than its period");
_Static_assert(SCHM_BUDGET_ACQUISITION_US < (SCHM_PERIOD_ACQUISITION_MS * 1000uL),
               "the acquisition budget must be less than its period");
_Static_assert(SCHM_BUDGET_STORAGE_US < (SCHM_PERIOD_STORAGE_MS * 1000uL),
               "the storage budget must be less than its period");
_Static_assert(SCHM_BUDGET_CONNECTIVITY_US < (SCHM_PERIOD_CONNECTIVITY_MS * 1000uL),
               "the connectivity budget must be less than its period");
/* Supervision must be the highest priority, or a busy system looks like a stopped one. */
_Static_assert(SCHM_PRIORITY_SCHEDULER > SCHM_PRIORITY_ACQUISITION,
               "the supervision task must outrank every task it supervises");
_Static_assert(SCHM_PRIORITY_ACQUISITION > SCHM_PRIORITY_STORAGE,
               "a blocking SD write must not hold off acquisition");
_Static_assert(SCHM_PRIORITY_STORAGE > SCHM_PRIORITY_CONNECTIVITY,
               "the network stack is the most delay-tolerant and must rank lowest");
#endif

#endif /* SCHM_CFG_H */

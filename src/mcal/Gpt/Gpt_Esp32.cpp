/**
 * @file    Gpt_Esp32.cpp
 * @brief   ESP32 platform leaf of the GPT driver: the monotonic time base.
 *
 * Five functions, all of them thin. The wrap-safe elapsed-time arithmetic that every timeout in the project
 * depends on is in Gpt.c, where the host test suite can reach it; this file only reads the counter.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <esp_timer.h>

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"

/**
 * @brief Microsecond count captured at Init, so reported time starts near zero.
 *
 * Not strictly required -- the arithmetic in Gpt.c is wrap-safe whatever the origin -- but a timestamp that
 * reads as time-since-boot is far easier to correlate against a log line than one offset by however long the
 * bootloader took.
 */
static uint64 Gpt_OriginUs = 0u;

extern "C" Std_ReturnType Gpt_Init(void)
{
    Gpt_OriginUs = (uint64)esp_timer_get_time();
    return E_OK;
}

extern "C" Gpt_TimestampType Gpt_GetMonotonicMs(void)
{
    /* esp_timer_get_time() is a 64-bit microsecond counter driven from the high-resolution timer. It is
     * unaffected by an NTP correction or an RTC adjustment, which is the reason this module exists separately
     * from TimeAbs: v1 used time(nullptr) in elapsed-time comparisons, so the first NTP step backwards turned
     * a three-second timeout into a wait as long as the correction.
     *
     * The truncation to 32 bits is the documented contract of Gpt_TimestampType. */
    return (Gpt_TimestampType)(((uint64)esp_timer_get_time() - Gpt_OriginUs) / 1000uLL);
}

extern "C" uint64 Gpt_GetMonotonicUs(void)
{
    return (uint64)esp_timer_get_time() - Gpt_OriginUs;
}

extern "C" void Gpt_DelayMs(uint32 ms)
{
    /* Arduino's delay() on ESP32 routes to vTaskDelay above one tick, so the CPU is yielded rather than spun.
     * That distinction is what keeps a settling delay on the acquisition task from starving storage and
     * connectivity -- which is effectively what v1's delay(1000) after every GNSS read did. */
    delay(ms);
}

extern "C" void Gpt_DelayUs(uint32 us)
{
    /* Busy-waits, because a sub-millisecond yield is not expressible through the RTOS tick. Callers are
     * bounded to hardware settling times -- an RS485 transceiver turnaround, a chip-select setup -- so a
     * request at or above 1 ms is a defect in the caller rather than a timing requirement. It is reported and
     * then honoured through the yielding path, because refusing outright would leave the hardware unsettled
     * and produce a second, more confusing failure downstream. */
    if (us >= 1000uL)
    {
        (void)Det_ReportError(MODULE_ID_GPT, INSTANCE_ID_SINGLE, GPT_API_ID_DELAY_MS, E_PARAM_VALUE);
        delay(us / 1000uL);
        us %= 1000uL;
    }

    delayMicroseconds(us);
}

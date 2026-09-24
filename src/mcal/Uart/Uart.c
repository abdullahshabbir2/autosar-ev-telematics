/**
 * @file    Uart.c
 * @brief   Platform-independent part of the UART driver: bounded exact-length read.
 *
 * The platform leaf (Init/Open/Close/Write/Read/DiscardRx/DrainTx/BytesAvailable and
 * the statistics) lives in Uart_Esp32.cpp for the target and in
 * test/support/Stub_Uart.c for the host.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "mcal/Uart/Uart.h"

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"

/**
 * @brief Poll interval while waiting for bytes, in milliseconds.
 *
 * 1 ms. At the slowest configured rate (4800 baud) a character takes 2.1 ms, so polling
 * every millisecond never misses the arrival of a byte by more than half a character
 * time, while still yielding the CPU on every iteration.
 */
#define UART_POLL_INTERVAL_MS 1u

Std_ReturnType Uart_ReadExact(Uart_InstanceType instance, uint8 *buffer, uint16 length,
                              uint32 timeoutMs, uint16 *actualLength)
{
    Gpt_TimestampType started;
    uint16 received = 0u;

    DET_CHECK_RETURN(buffer != NULL_PTR, MODULE_ID_UART, instance, UART_API_ID_READ,
                     UART_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(actualLength != NULL_PTR, MODULE_ID_UART, instance, UART_API_ID_READ,
                     UART_E_PARAM_POINTER, E_NOT_OK);

    *actualLength = 0u;

    if (length == 0u)
    {
        return E_OK;
    }

    started = Gpt_GetMonotonicMs();

    for (;;)
    {
        uint16 chunk = 0u;

        if (Uart_Read(instance, &buffer[received], (uint16)(length - received), &chunk) != E_OK)
        {
            *actualLength = received;
            return E_NOT_OK;
        }

        received = (uint16)(received + chunk);

        /* Return the instant the last byte lands. v1's equivalent loop had no early
         * exit: it ran the full timeout on every exchange, which cost a whole second per
         * battery frame. Three frames per pack across four packs is 12 s of waiting
         * inside a 3 s acquisition period, so the schedule could never keep up. */
        if (received >= length)
        {
            *actualLength = received;
            return E_OK;
        }

        if (Gpt_HasElapsed(started, timeoutMs) != FALSE)
        {
            /* The partial count is reported on timeout as well as on success. How much
             * of a frame arrived is the difference between "the peer is absent" and "the
             * peer replied but the frame was cut short", and those have different
             * causes. */
            *actualLength = received;
            return E_TIMEOUT;
        }

        /* Only yield when nothing arrived. If bytes are still flowing, loop straight
         * back so a fast burst is drained without inserting artificial delay. */
        if (chunk == 0u)
        {
            Gpt_DelayMs(UART_POLL_INTERVAL_MS);
        }
    }
}

void Uart_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = UART_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_UART;
        versioninfo->sw_major_version = UART_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = UART_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = UART_SW_PATCH_VERSION;
    }
}

/**
 * @file    Gpt.c
 * @brief   Platform-independent part of the GPT driver: wrap-safe time arithmetic.
 *
 * The platform leaf -- ::Gpt_Init, ::Gpt_GetMonotonicMs, ::Gpt_GetMonotonicUs,
 * ::Gpt_DelayMs and ::Gpt_DelayUs -- lives in Gpt_Esp32.cpp for the target and in
 * test/support/Stub_Gpt.c for the host. Everything in this file is derived arithmetic
 * and is therefore exercised by the host test suite against the real implementation
 * rather than a double.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Gpt.h"

uint32 Gpt_ElapsedSince(Gpt_TimestampType since)
{
    const Gpt_TimestampType now = Gpt_GetMonotonicMs();

    /* Unsigned subtraction is defined to wrap modulo 2^32, which is exactly the
     * behaviour needed: when `now` has wrapped past zero and `since` has not, the
     * difference still comes out as the true elapsed interval. Writing this as a
     * signed comparison, or special-casing `now < since`, is what breaks at the
     * 49.7-day mark. The cast is explicit so that the modular intent is visible and
     * so -Wconversion does not have to be silenced. */
    return (uint32)(now - since);
}

boolean Gpt_HasElapsed(Gpt_TimestampType since, uint32 intervalMs)
{
    /* An interval above half the counter range cannot be distinguished from a
     * timestamp in the past, so it is treated as already elapsed rather than as a
     * silently wrong "not yet". A caller asking for more than 24.8 days has a defect,
     * and reporting elapsed makes that defect a visible early timeout instead of a
     * hang. */
    if (intervalMs > GPT_MAX_INTERVAL_MS)
    {
        return TRUE;
    }

    return (Gpt_ElapsedSince(since) >= intervalMs) ? TRUE : FALSE;
}

void Gpt_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = GPT_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_GPT;
        versioninfo->sw_major_version = GPT_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = GPT_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = GPT_SW_PATCH_VERSION;
    }
}

/**
 * @file    TimeAbs_Platform.h
 * @brief   Platform leaf of the wall-clock abstraction.
 *
 * The four operations that genuinely need hardware or a network stack. Everything else about time --
 * the calendar arithmetic, the plausibility rule, the RTC write-back policy -- is in TimeAbs.c and is
 * unit tested.
 *
 * Implemented by TimeAbs_Esp32.cpp on the target (DS3231 over I2C, SNTP over lwIP) and by
 * test/support/Stub_Platform.c on the host.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef TIMEABS_PLATFORM_H
#define TIMEABS_PLATFORM_H

#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the I2C bus and confirm the RTC answers.
 * @return E_OK if the device acknowledged its address; E_NOT_OK if it is absent.
 */
CHECK_RETURN Std_ReturnType TimeAbs_PlatformRtcInit(void);

/**
 * @brief Read the RTC.
 * @param[out] unixTime Seconds since the Unix epoch, UTC.
 * @return E_OK on success; E_NOT_OK on a bus error.
 */
CHECK_RETURN Std_ReturnType TimeAbs_PlatformRtcRead(uint32 *unixTime);

/**
 * @brief Write @p unixTime to the RTC.
 * @return E_OK on success; E_NOT_OK on a bus error.
 */
CHECK_RETURN Std_ReturnType TimeAbs_PlatformRtcWrite(uint32 unixTime);

/**
 * @brief Fetch the time from an NTP server. Requires a bearer to be up.
 * @param[out] unixTime  Seconds since the Unix epoch, UTC.
 * @param[in]  timeoutMs Bound on the attempt.
 * @return E_OK on success; E_TIMEOUT if no response arrived.
 */
CHECK_RETURN Std_ReturnType TimeAbs_PlatformNtpFetch(uint32 *unixTime, uint32 timeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* TIMEABS_PLATFORM_H */

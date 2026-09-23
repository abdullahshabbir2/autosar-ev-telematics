/**
 * @file    Dio.h
 * @brief   AUTOSAR DIO driver (SWS_DIODriver) -- ESP32 adaptation.
 *
 * Digital read and write on logical channels. Callers never name a GPIO number:
 * they name a channel symbol from Dio_Cfg.h, which is the single place the pin
 * assignment lives. Moving a status LED to a different pin is then a one-line
 * change that cannot miss a call site.
 *
 * @req SWREQ-SYS-0020
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DIO_H
#define DIO_H

#include "Autosar_ModuleIds.h"
#include "Dio_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIO_VENDOR_ID 0xFFFEu
#define DIO_AR_RELEASE_MAJOR_VERSION 4u
#define DIO_AR_RELEASE_MINOR_VERSION 4u
#define DIO_SW_MAJOR_VERSION 2u
#define DIO_SW_MINOR_VERSION 0u
#define DIO_SW_PATCH_VERSION 0u

#define DIO_API_ID_READ_CHANNEL 0x00u
#define DIO_API_ID_WRITE_CHANNEL 0x01u
#define DIO_API_ID_FLIP_CHANNEL 0x11u

#define DIO_E_PARAM_INVALID_CHANNEL_ID 0x0Au

/** Logical channel identifier; values are defined in Dio_Cfg.h. */
typedef uint8 Dio_ChannelType;

/** Pin level: ::STD_LOW or ::STD_HIGH. */
typedef uint8 Dio_LevelType;

/**
 * @brief Read the current level of @p channelId.
 * @return ::STD_HIGH or ::STD_LOW; ::STD_LOW for an invalid channel (with a Det report).
 */
Dio_LevelType Dio_ReadChannel(Dio_ChannelType channelId);

/**
 * @brief Drive @p channelId to @p level.
 *
 * Silently ignored, with a Det report, for a channel configured as an input or
 * outside the configured set.
 */
void Dio_WriteChannel(Dio_ChannelType channelId, Dio_LevelType level);

/**
 * @brief Invert the current level of @p channelId and return the new level.
 *
 * Read-modify-write is done under a critical section, so two tasks toggling the
 * same channel cannot lose an update.
 */
Dio_LevelType Dio_FlipChannel(Dio_ChannelType channelId);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Dio_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* DIO_H */

/**
 * @file    Stub_Platform.h
 * @brief   Control surface for the ECU-abstraction platform-leaf test doubles.
 *
 * Where Stub_Mcal.h covers the hardware boundary, this covers the three ECU-abstraction modules whose
 * platform leaves need a network stack, a filesystem or an I2C device. The doubles model enough
 * behaviour to exercise the policy above them: an in-memory filesystem with a real byte layout, a
 * bearer that comes up when a test says so, and an RTC that can be made absent or set to an
 * implausible time.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef STUB_PLATFORM_H
#define STUB_PLATFORM_H

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reset every platform double. Call from a suite's setUp() alongside Stub_Mcal_ResetAll(). */
void Stub_Platform_ResetAll(void);

/*==================================================================================================
 *  TimeAbs -- RTC and NTP
 *================================================================================================*/

/** Make the RTC present or absent at Init. */
void Stub_Time_SetRtcPresent(boolean present);

/** Set what the RTC reports. Use a value below 2024 to model a dead backup cell. */
void Stub_Time_SetRtcTime(uint32 unixTime);

/** The time most recently written to the RTC, or 0 if none. */
uint32 Stub_Time_GetLastRtcWrite(void);

/** Number of RTC writes, so a test can assert the drift-tolerance policy suppresses them. */
uint32 Stub_Time_GetRtcWriteCount(void);

/** Make NTP answer with @p unixTime, or set @p available FALSE to make it time out. */
void Stub_Time_SetNtpResponse(uint32 unixTime, boolean available);

/*==================================================================================================
 *  FsAbs -- in-memory filesystem
 *
 *  Files are stored as real byte arrays, so the record framing, the CRC field and the transfer cursor
 *  are exercised against the same layout the SD card would hold.
 *================================================================================================*/

/** Files the double can hold. */
#define STUB_FS_MAX_FILES 8u

/** Bytes per file. */
#define STUB_FS_MAX_FILE_SIZE 8192u

/** Make the card mount or fail to mount. */
void Stub_Fs_SetMountable(boolean mountable);

/** Set the capacity and used figures the card reports, in MiB. */
void Stub_Fs_SetSpace(uint32 capacityMiB, uint32 usedMiB);

/** Make the next @p count appends fail, to model a card that has stopped accepting writes. */
void Stub_Fs_FailNextAppends(uint32 count);

/** Create @p path with @p length bytes of @p content, replacing it if it exists. */
void Stub_Fs_PutFile(const char *path, const uint8 *content, uint32 length);

/** Read a file's bytes directly, bypassing the abstraction. Returns the length, 0 if absent. */
uint32 Stub_Fs_PeekFile(const char *path, uint8 *buffer, uint32 size);

/** Files currently present. */
uint8 Stub_Fs_GetFileCount(void);

/** TRUE if @p path exists in the double. */
boolean Stub_Fs_HasFile(const char *path);

/**
 * @brief Corrupt one byte of @p path, to model bit rot or an interrupted write.
 * @return TRUE if the byte was changed.
 */
boolean Stub_Fs_CorruptByte(const char *path, uint32 offset, uint8 value);

/** Truncate @p path to @p length bytes, modelling a record cut short by a power loss. */
boolean Stub_Fs_Truncate(const char *path, uint32 length);

/*==================================================================================================
 *  NetIf -- bearers and broker
 *================================================================================================*/

/** Make the WiFi bearer come up (TRUE) or never associate (FALSE). */
void Stub_Net_SetWifiAvailable(boolean available);

/** Make the GSM bearer come up or never attach. */
void Stub_Net_SetGsmAvailable(boolean available);

/** Set the RSSI each bearer reports. */
void Stub_Net_SetRssi(sint8 wifiDbm, sint8 gsmDbm);

/** Make the broker accept a session (TRUE) or refuse it (FALSE). */
void Stub_Net_SetBrokerAvailable(boolean available);

/** Drop an established session, modelling a broker restart. */
void Stub_Net_DropSession(void);

/** Make the next @p count publishes fail. */
void Stub_Net_FailNextPublishes(uint32 count);

/** Payloads the broker has accepted. */
uint32 Stub_Net_GetPublishCount(void);

/** Length of the most recently published payload. */
uint16 Stub_Net_GetLastPayloadLength(void);

/** The most recently published payload. Valid until the next publish or reset. */
const uint8 *Stub_Net_GetLastPayload(void);

/** The topic of the most recent publish, NUL-terminated. */
const char *Stub_Net_GetLastTopic(void);

/** Number of connection attempts, so a test can assert the backoff reduces them. */
uint32 Stub_Net_GetConnectAttemptCount(void);

/** Number of subscribe calls, so re-subscription after a reconnect is observable. */
uint32 Stub_Net_GetSubscribeCount(void);

/** Deliver an inbound message to whatever the module subscribed with. */
void Stub_Net_DeliverMessage(const char *topic, const uint8 *payload, uint16 payloadLen);

#ifdef __cplusplus
}
#endif

#endif /* STUB_PLATFORM_H */

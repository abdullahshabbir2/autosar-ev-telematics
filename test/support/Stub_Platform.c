/**
 * @file    Stub_Platform.c
 * @brief   ECU-abstraction platform-leaf test doubles for the host build.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Stub_Platform.h"

#include <string.h>

#include "ecuabs/FsAbs/FsAbs_Platform.h"
#include "ecuabs/NetIf/NetIf_Cfg.h"
#include "ecuabs/NetIf/NetIf_Platform.h"
#include "ecuabs/TimeAbs/TimeAbs_Platform.h"

/*==================================================================================================
 *  SECTION 1 : TimeAbs
 *================================================================================================*/

static boolean Stub_RtcPresent = TRUE;
static uint32 Stub_RtcTime = 1740000000uL; /* 2025-02-19, comfortably plausible */
static uint32 Stub_RtcLastWrite;
static uint32 Stub_RtcWriteCount;
static uint32 Stub_NtpTime = 1740000000uL;
static boolean Stub_NtpAvailable = TRUE;

Std_ReturnType TimeAbs_PlatformRtcInit(void)
{
    return (Stub_RtcPresent != FALSE) ? E_OK : E_NOT_OK;
}

Std_ReturnType TimeAbs_PlatformRtcRead(uint32 *unixTime)
{
    if ((unixTime == NULL_PTR) || (Stub_RtcPresent == FALSE))
    {
        return E_NOT_OK;
    }
    *unixTime = Stub_RtcTime;
    return E_OK;
}

Std_ReturnType TimeAbs_PlatformRtcWrite(uint32 unixTime)
{
    if (Stub_RtcPresent == FALSE)
    {
        return E_NOT_OK;
    }
    Stub_RtcTime = unixTime;
    Stub_RtcLastWrite = unixTime;
    Stub_RtcWriteCount++;
    return E_OK;
}

Std_ReturnType TimeAbs_PlatformNtpFetch(uint32 *unixTime, uint32 timeoutMs)
{
    COMPILER_UNUSED(timeoutMs);

    if (unixTime == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if (Stub_NtpAvailable == FALSE)
    {
        return E_TIMEOUT;
    }
    *unixTime = Stub_NtpTime;
    return E_OK;
}

void Stub_Time_SetRtcPresent(boolean present)
{
    Stub_RtcPresent = present;
}

void Stub_Time_SetRtcTime(uint32 unixTime)
{
    Stub_RtcTime = unixTime;
}

uint32 Stub_Time_GetLastRtcWrite(void)
{
    return Stub_RtcLastWrite;
}

uint32 Stub_Time_GetRtcWriteCount(void)
{
    return Stub_RtcWriteCount;
}

void Stub_Time_SetNtpResponse(uint32 unixTime, boolean available)
{
    Stub_NtpTime = unixTime;
    Stub_NtpAvailable = available;
}

/*==================================================================================================
 *  SECTION 2 : FsAbs -- in-memory filesystem
 *
 *  Real byte arrays, so the record framing, the CRC field and the cursor arithmetic are exercised
 *  against the same layout the card would hold. A double that stored records as a list of strings
 *  would let a framing bug pass.
 *================================================================================================*/

typedef struct
{
    char path[32];
    uint8 data[STUB_FS_MAX_FILE_SIZE];
    uint32 length;
    boolean used;
} Stub_FsFileType;

static Stub_FsFileType Stub_FsFiles[STUB_FS_MAX_FILES];
static boolean Stub_FsMountable = TRUE;
static boolean Stub_FsMounted;
static uint32 Stub_FsCapacityMiB = 30000uL;
static uint32 Stub_FsUsedMiB = 100uL;
static uint32 Stub_FsFailAppends;

/** Find @p path, or NULL if absent. */
static Stub_FsFileType *Stub_FsFind(const char *path)
{
    uint8 i;

    if (path == NULL_PTR)
    {
        return NULL_PTR;
    }
    for (i = 0u; i < STUB_FS_MAX_FILES; i++)
    {
        if ((Stub_FsFiles[i].used != FALSE) && (strcmp(Stub_FsFiles[i].path, path) == 0))
        {
            return &Stub_FsFiles[i];
        }
    }
    return NULL_PTR;
}

/** Find @p path or allocate a slot for it. */
static Stub_FsFileType *Stub_FsFindOrCreate(const char *path)
{
    Stub_FsFileType *file = Stub_FsFind(path);
    uint8 i;

    if (file != NULL_PTR)
    {
        return file;
    }
    if ((path == NULL_PTR) || (strlen(path) >= sizeof(Stub_FsFiles[0].path)))
    {
        return NULL_PTR;
    }

    for (i = 0u; i < STUB_FS_MAX_FILES; i++)
    {
        if (Stub_FsFiles[i].used == FALSE)
        {
            (void)memset(&Stub_FsFiles[i], 0, sizeof(Stub_FsFiles[i]));
            (void)memcpy(Stub_FsFiles[i].path, path, strlen(path));
            Stub_FsFiles[i].used = TRUE;
            return &Stub_FsFiles[i];
        }
    }
    return NULL_PTR;
}

Std_ReturnType FsAbs_PlatformMount(void)
{
    if (Stub_FsMountable == FALSE)
    {
        Stub_FsMounted = FALSE;
        return E_NOT_OK;
    }
    Stub_FsMounted = TRUE;
    return E_OK;
}

void FsAbs_PlatformUnmount(void)
{
    Stub_FsMounted = FALSE;
}

Std_ReturnType FsAbs_PlatformGetSpace(uint32 *capacityMiB, uint32 *usedMiB)
{
    if ((capacityMiB == NULL_PTR) || (usedMiB == NULL_PTR) || (Stub_FsMounted == FALSE))
    {
        return E_NOT_OK;
    }
    *capacityMiB = Stub_FsCapacityMiB;
    *usedMiB = Stub_FsUsedMiB;
    return E_OK;
}

boolean FsAbs_PlatformExists(const char *path)
{
    return (Stub_FsFind(path) != NULL_PTR) ? TRUE : FALSE;
}

Std_ReturnType FsAbs_PlatformAppend(const char *path, const uint8 *data, uint32 length)
{
    Stub_FsFileType *file;

    if ((data == NULL_PTR) || (Stub_FsMounted == FALSE))
    {
        return E_NOT_OK;
    }
    if (Stub_FsFailAppends > 0u)
    {
        Stub_FsFailAppends--;
        return E_NOT_OK;
    }

    file = Stub_FsFindOrCreate(path);
    if (file == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if ((file->length + length) > (uint32)STUB_FS_MAX_FILE_SIZE)
    {
        return E_NO_SPACE;
    }

    (void)memcpy(&file->data[file->length], data, length);
    file->length += length;
    return E_OK;
}

Std_ReturnType FsAbs_PlatformRead(const char *path, uint32 offset, uint8 *buffer, uint32 size,
                                 uint32 *read)
{
    const Stub_FsFileType *file = Stub_FsFind(path);
    uint32 available;
    uint32 take;

    if ((buffer == NULL_PTR) || (read == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (file == NULL_PTR)
    {
        *read = 0u;
        return E_NOT_FOUND;
    }

    if (offset >= file->length)
    {
        *read = 0u;
        return E_OK;
    }

    available = file->length - offset;
    take = (size < available) ? size : available;
    (void)memcpy(buffer, &file->data[offset], take);
    *read = take;
    return E_OK;
}

Std_ReturnType FsAbs_PlatformSize(const char *path, uint32 *size)
{
    const Stub_FsFileType *file = Stub_FsFind(path);

    if (size == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if (file == NULL_PTR)
    {
        return E_NOT_FOUND;
    }
    *size = file->length;
    return E_OK;
}

Std_ReturnType FsAbs_PlatformRemove(const char *path)
{
    Stub_FsFileType *file = Stub_FsFind(path);

    if (file == NULL_PTR)
    {
        return E_NOT_FOUND;
    }
    (void)memset(file, 0, sizeof(*file));
    return E_OK;
}

Std_ReturnType FsAbs_PlatformFindOldestLog(char *buffer, uint16 size)
{
    const Stub_FsFileType *oldest = NULL_PTR;
    uint8 i;

    if (buffer == NULL_PTR)
    {
        return E_NOT_OK;
    }

    /* Lexicographically smallest name. Log files are "/YYYYMMDD.csv", so that is chronological order --
     * which is exactly why the name format was chosen. */
    for (i = 0u; i < STUB_FS_MAX_FILES; i++)
    {
        if (Stub_FsFiles[i].used == FALSE)
        {
            continue;
        }
        if (strstr(Stub_FsFiles[i].path, ".csv") == NULL_PTR)
        {
            continue;
        }
        if ((oldest == NULL_PTR) || (strcmp(Stub_FsFiles[i].path, oldest->path) < 0))
        {
            oldest = &Stub_FsFiles[i];
        }
    }

    if (oldest == NULL_PTR)
    {
        return E_NOT_FOUND;
    }
    if (strlen(oldest->path) >= size)
    {
        return E_NOT_OK;
    }

    (void)memset(buffer, 0, size);
    (void)memcpy(buffer, oldest->path, strlen(oldest->path));
    return E_OK;
}

void Stub_Fs_SetMountable(boolean mountable)
{
    Stub_FsMountable = mountable;
}

void Stub_Fs_SetSpace(uint32 capacityMiB, uint32 usedMiB)
{
    Stub_FsCapacityMiB = capacityMiB;
    Stub_FsUsedMiB = usedMiB;
}

void Stub_Fs_FailNextAppends(uint32 count)
{
    Stub_FsFailAppends = count;
}

void Stub_Fs_PutFile(const char *path, const uint8 *content, uint32 length)
{
    Stub_FsFileType *file = Stub_FsFindOrCreate(path);

    if ((file == NULL_PTR) || (length > (uint32)STUB_FS_MAX_FILE_SIZE))
    {
        return;
    }
    file->length = 0u;
    if ((content != NULL_PTR) && (length > 0u))
    {
        (void)memcpy(file->data, content, length);
        file->length = length;
    }
}

uint32 Stub_Fs_PeekFile(const char *path, uint8 *buffer, uint32 size)
{
    const Stub_FsFileType *file = Stub_FsFind(path);
    uint32 take;

    if ((file == NULL_PTR) || (buffer == NULL_PTR))
    {
        return 0u;
    }
    take = (size < file->length) ? size : file->length;
    (void)memcpy(buffer, file->data, take);
    return take;
}

uint8 Stub_Fs_GetFileCount(void)
{
    uint8 count = 0u;
    uint8 i;

    for (i = 0u; i < STUB_FS_MAX_FILES; i++)
    {
        if (Stub_FsFiles[i].used != FALSE)
        {
            count++;
        }
    }
    return count;
}

boolean Stub_Fs_HasFile(const char *path)
{
    return (Stub_FsFind(path) != NULL_PTR) ? TRUE : FALSE;
}

boolean Stub_Fs_CorruptByte(const char *path, uint32 offset, uint8 value)
{
    Stub_FsFileType *file = Stub_FsFind(path);

    if ((file == NULL_PTR) || (offset >= file->length))
    {
        return FALSE;
    }
    file->data[offset] = value;
    return TRUE;
}

boolean Stub_Fs_Truncate(const char *path, uint32 length)
{
    Stub_FsFileType *file = Stub_FsFind(path);

    if ((file == NULL_PTR) || (length > file->length))
    {
        return FALSE;
    }
    file->length = length;
    return TRUE;
}

/*==================================================================================================
 *  SECTION 3 : NetIf -- bearers and broker
 *================================================================================================*/

static boolean Stub_WifiAvailable = TRUE;
static boolean Stub_GsmAvailable = TRUE;
static boolean Stub_WifiRequested;
static boolean Stub_GsmRequested;
static sint8 Stub_WifiRssi = -55;
static sint8 Stub_GsmRssi = -70;
static boolean Stub_BrokerAvailable = TRUE;
static boolean Stub_SessionUp;
static uint32 Stub_FailPublishes;
static uint32 Stub_PublishCount;
static uint32 Stub_ConnectAttempts;
static uint32 Stub_SubscribeCount;
static uint8 Stub_LastPayload[NETIF_MAX_PAYLOAD_SIZE];
static uint16 Stub_LastPayloadLen;
static char Stub_LastTopic[NETIF_MAX_TOPIC_SIZE];
static void (*Stub_MqttCallback)(const char *topic, const uint8 *payload, uint16 payloadLen);

Std_ReturnType NetIf_PlatformInit(void)
{
    return E_OK;
}

Std_ReturnType NetIf_PlatformWifiConnect(void)
{
    Stub_WifiRequested = TRUE;
    return E_OK;
}

void NetIf_PlatformWifiDisconnect(void)
{
    Stub_WifiRequested = FALSE;
}

boolean NetIf_PlatformWifiIsUp(void)
{
    return ((Stub_WifiRequested != FALSE) && (Stub_WifiAvailable != FALSE)) ? TRUE : FALSE;
}

sint8 NetIf_PlatformWifiRssi(void)
{
    return (NetIf_PlatformWifiIsUp() != FALSE) ? Stub_WifiRssi : 0;
}

Std_ReturnType NetIf_PlatformGsmConnect(void)
{
    Stub_GsmRequested = TRUE;
    return E_OK;
}

void NetIf_PlatformGsmDisconnect(void)
{
    Stub_GsmRequested = FALSE;
}

boolean NetIf_PlatformGsmIsUp(void)
{
    return ((Stub_GsmRequested != FALSE) && (Stub_GsmAvailable != FALSE)) ? TRUE : FALSE;
}

sint8 NetIf_PlatformGsmRssi(void)
{
    return (NetIf_PlatformGsmIsUp() != FALSE) ? Stub_GsmRssi : 0;
}

Std_ReturnType NetIf_PlatformMqttConnect(const char *host, uint16 port, const char *clientId,
                                        uint16 keepAliveS)
{
    COMPILER_UNUSED(host);
    COMPILER_UNUSED(port);
    COMPILER_UNUSED(clientId);
    COMPILER_UNUSED(keepAliveS);

    Stub_ConnectAttempts++;

    if ((Stub_BrokerAvailable == FALSE) ||
        ((NetIf_PlatformWifiIsUp() == FALSE) && (NetIf_PlatformGsmIsUp() == FALSE)))
    {
        return E_TIMEOUT;
    }

    Stub_SessionUp = TRUE;
    return E_OK;
}

void NetIf_PlatformMqttDisconnect(void)
{
    Stub_SessionUp = FALSE;
}

boolean NetIf_PlatformMqttIsConnected(void)
{
    return Stub_SessionUp;
}

Std_ReturnType NetIf_PlatformMqttPublish(const char *topic, const uint8 *payload, uint16 payloadLen,
                                        uint8 qos, boolean retain)
{
    COMPILER_UNUSED(qos);
    COMPILER_UNUSED(retain);

    if ((Stub_SessionUp == FALSE) || (topic == NULL_PTR) || (payload == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (Stub_FailPublishes > 0u)
    {
        Stub_FailPublishes--;
        return E_NOT_OK;
    }
    if (payloadLen > (uint16)NETIF_MAX_PAYLOAD_SIZE)
    {
        return E_NO_SPACE;
    }

    (void)memcpy(Stub_LastPayload, payload, payloadLen);
    Stub_LastPayloadLen = payloadLen;
    (void)memset(Stub_LastTopic, 0, sizeof(Stub_LastTopic));
    if (strlen(topic) < sizeof(Stub_LastTopic))
    {
        (void)memcpy(Stub_LastTopic, topic, strlen(topic));
    }
    Stub_PublishCount++;

    return E_OK;
}

Std_ReturnType NetIf_PlatformMqttSubscribe(const char *topic, uint8 qos)
{
    COMPILER_UNUSED(qos);

    if ((Stub_SessionUp == FALSE) || (topic == NULL_PTR))
    {
        return E_NOT_OK;
    }
    Stub_SubscribeCount++;
    return E_OK;
}

Std_ReturnType NetIf_PlatformMqttLoop(void)
{
    return (Stub_SessionUp != FALSE) ? E_OK : E_NOT_OK;
}

void NetIf_PlatformMqttSetCallback(void (*callback)(const char *topic, const uint8 *payload,
                                                  uint16 payloadLen))
{
    Stub_MqttCallback = callback;
}

void Stub_Net_SetWifiAvailable(boolean available)
{
    Stub_WifiAvailable = available;
}

void Stub_Net_SetGsmAvailable(boolean available)
{
    Stub_GsmAvailable = available;
}

void Stub_Net_SetRssi(sint8 wifiDbm, sint8 gsmDbm)
{
    Stub_WifiRssi = wifiDbm;
    Stub_GsmRssi = gsmDbm;
}

void Stub_Net_SetBrokerAvailable(boolean available)
{
    Stub_BrokerAvailable = available;
}

void Stub_Net_DropSession(void)
{
    Stub_SessionUp = FALSE;
}

void Stub_Net_FailNextPublishes(uint32 count)
{
    Stub_FailPublishes = count;
}

uint32 Stub_Net_GetPublishCount(void)
{
    return Stub_PublishCount;
}

uint16 Stub_Net_GetLastPayloadLength(void)
{
    return Stub_LastPayloadLen;
}

const uint8 *Stub_Net_GetLastPayload(void)
{
    return Stub_LastPayload;
}

const char *Stub_Net_GetLastTopic(void)
{
    return Stub_LastTopic;
}

uint32 Stub_Net_GetConnectAttemptCount(void)
{
    return Stub_ConnectAttempts;
}

uint32 Stub_Net_GetSubscribeCount(void)
{
    return Stub_SubscribeCount;
}

void Stub_Net_DeliverMessage(const char *topic, const uint8 *payload, uint16 payloadLen)
{
    if (Stub_MqttCallback != NULL_PTR)
    {
        Stub_MqttCallback(topic, payload, payloadLen);
    }
}

/*==================================================================================================
 *  SECTION 4 : Global reset
 *================================================================================================*/

void Stub_Platform_ResetAll(void)
{
    Stub_RtcPresent = TRUE;
    Stub_RtcTime = 1740000000uL;
    Stub_RtcLastWrite = 0u;
    Stub_RtcWriteCount = 0u;
    Stub_NtpTime = 1740000000uL;
    Stub_NtpAvailable = TRUE;

    (void)memset(Stub_FsFiles, 0, sizeof(Stub_FsFiles));
    Stub_FsMountable = TRUE;
    Stub_FsMounted = FALSE;
    Stub_FsCapacityMiB = 30000uL;
    Stub_FsUsedMiB = 100uL;
    Stub_FsFailAppends = 0u;

    Stub_WifiAvailable = TRUE;
    Stub_GsmAvailable = TRUE;
    Stub_WifiRequested = FALSE;
    Stub_GsmRequested = FALSE;
    Stub_WifiRssi = -55;
    Stub_GsmRssi = -70;
    Stub_BrokerAvailable = TRUE;
    Stub_SessionUp = FALSE;
    Stub_FailPublishes = 0u;
    Stub_PublishCount = 0u;
    Stub_ConnectAttempts = 0u;
    Stub_SubscribeCount = 0u;
    Stub_LastPayloadLen = 0u;
    (void)memset(Stub_LastPayload, 0, sizeof(Stub_LastPayload));
    (void)memset(Stub_LastTopic, 0, sizeof(Stub_LastTopic));
    Stub_MqttCallback = NULL_PTR;
}

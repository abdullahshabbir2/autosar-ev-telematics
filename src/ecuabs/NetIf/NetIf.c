/**
 * @file    NetIf.c
 * @brief   Bearer abstraction implementation: one state machine, one retry policy, two bearers.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "NetIf.h"

#include <string.h>

#include "Dem.h"
#include "Det.h"
#include "Gpt.h"
#include "NetIf_Platform.h"
#include "NvM.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

typedef struct
{
    char topic[NETIF_MAX_TOPIC_SIZE];
    NetIf_MessageHandlerType handler;
    boolean active;
} NetIf_SubscriptionType;

STATIC boolean NetIf_Initialised = FALSE;
STATIC NetIf_StatusType NetIf_Status;
STATIC NetIf_BearerType NetIf_RequestedBearer = NETIF_BEARER_NONE;
STATIC Gpt_TimestampType NetIf_StateEnteredMs;
STATIC uint32 NetIf_BackoffMs;
STATIC Gpt_TimestampType NetIf_BearerUpSinceMs;

/**
 * @brief Subscriptions, re-applied automatically after every reconnection.
 *
 * Held here rather than left to the caller because a subscription silently lost on reconnect is
 * invisible until someone notices the ECU has stopped answering backfill requests. v1 re-subscribed
 * from inside its transfer loop, so a subscription could be lost whenever that loop took a different
 * path.
 */
STATIC NetIf_SubscriptionType NetIf_Subscriptions[NETIF_MAX_SUBSCRIPTIONS];

/** Broker endpoint, loaded from NvM at Init. */
STATIC char NetIf_BrokerHost[32];
STATIC uint16 NetIf_BrokerPort;
STATIC char NetIf_ClientId[24];

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

/** Move to @p state, timestamping the transition so timeouts are measured from it. */
STATIC void NetIf_EnterState(NetIf_StateType state)
{
    if (NetIf_Status.state == state)
    {
        return;
    }
    NetIf_Status.state = state;
    NetIf_StateEnteredMs = Gpt_GetMonotonicMs();
}

/** Whether the requested bearer's link is up, according to its platform leaf. */
STATIC boolean NetIf_BearerIsUp(NetIf_BearerType bearer)
{
    boolean up = FALSE;

    switch (bearer)
    {
#if (NETIF_WIFI_ENABLED == STD_ON)
    case NETIF_BEARER_WIFI:
        up = NetIf_PlatformWifiIsUp();
        break;
#endif
#if (NETIF_GSM_ENABLED == STD_ON)
    case NETIF_BEARER_GSM:
        up = NetIf_PlatformGsmIsUp();
        break;
#endif
    case NETIF_BEARER_NONE:
    default:
        up = FALSE;
        break;
    }

    return up;
}

/** Begin bringing @p bearer up. */
STATIC Std_ReturnType NetIf_StartBearer(NetIf_BearerType bearer)
{
    Std_ReturnType status;

    switch (bearer)
    {
#if (NETIF_WIFI_ENABLED == STD_ON)
    case NETIF_BEARER_WIFI:
        status = NetIf_PlatformWifiConnect();
        break;
#endif
#if (NETIF_GSM_ENABLED == STD_ON)
    case NETIF_BEARER_GSM:
        status = NetIf_PlatformGsmConnect();
        break;
#endif
    case NETIF_BEARER_NONE:
    default:
        status = E_NOT_OK;
        break;
    }

    return status;
}

/** Tear @p bearer down. */
STATIC void NetIf_StopBearer(NetIf_BearerType bearer)
{
    switch (bearer)
    {
#if (NETIF_WIFI_ENABLED == STD_ON)
    case NETIF_BEARER_WIFI:
        NetIf_PlatformWifiDisconnect();
        break;
#endif
#if (NETIF_GSM_ENABLED == STD_ON)
    case NETIF_BEARER_GSM:
        NetIf_PlatformGsmDisconnect();
        break;
#endif
    case NETIF_BEARER_NONE:
    default:
        break;
    }
}

/** Signal strength of @p bearer, or 0 if unknown. */
STATIC sint8 NetIf_BearerRssi(NetIf_BearerType bearer)
{
    sint8 rssi = 0;

    switch (bearer)
    {
#if (NETIF_WIFI_ENABLED == STD_ON)
    case NETIF_BEARER_WIFI:
        rssi = NetIf_PlatformWifiRssi();
        break;
#endif
#if (NETIF_GSM_ENABLED == STD_ON)
    case NETIF_BEARER_GSM:
        rssi = NetIf_PlatformGsmRssi();
        break;
#endif
    case NETIF_BEARER_NONE:
    default:
        break;
    }

    return rssi;
}

/**
 * @brief Enter backoff, doubling the interval up to the configured ceiling.
 *
 * The doubling is what makes a long outage cheap. v1's fixed 2 s retry from a 3 s task meant a broker
 * that was down for an hour drew about 1200 connection attempts on a metered link.
 */
STATIC void NetIf_EnterBackoff(void)
{
    if (NetIf_BackoffMs == 0u)
    {
        NetIf_BackoffMs = (uint32)NETIF_RECONNECT_MIN_MS;
    }
    else
    {
        NetIf_BackoffMs *= 2u;
        if (NetIf_BackoffMs > (uint32)NETIF_RECONNECT_MAX_MS)
        {
            NetIf_BackoffMs = (uint32)NETIF_RECONNECT_MAX_MS;
        }
    }

    NetIf_Status.currentBackoffMs = NetIf_BackoffMs;
    NetIf_EnterState(NETIF_STATE_BACKOFF);
}

/** Re-apply every held subscription. Called after a session is established. */
STATIC void NetIf_ReapplySubscriptions(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)NETIF_MAX_SUBSCRIPTIONS; i++)
    {
        if (NetIf_Subscriptions[i].active != FALSE)
        {
            STD_DISCARD(
                NetIf_PlatformMqttSubscribe(NetIf_Subscriptions[i].topic, NETIF_DIAGNOSTIC_QOS));
        }
    }
}

/** Route an inbound message to whichever subscription matches its topic. */
STATIC void NetIf_OnMessage(const char *topic, const uint8 *payload, uint16 payloadLen)
{
    uint8 i;

    if (topic == NULL_PTR)
    {
        return;
    }

    for (i = 0u; i < (uint8)NETIF_MAX_SUBSCRIPTIONS; i++)
    {
        if ((NetIf_Subscriptions[i].active != FALSE) &&
            (NetIf_Subscriptions[i].handler != NULL_PTR) &&
            (strcmp(NetIf_Subscriptions[i].topic, topic) == 0))
        {
            NetIf_Subscriptions[i].handler(topic, payload, payloadLen);
            return;
        }
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType NetIf_Init(void)
{
    NvM_DeviceConfigType config;

    (void)memset(&NetIf_Status, 0, sizeof(NetIf_Status));
    (void)memset(NetIf_Subscriptions, 0, sizeof(NetIf_Subscriptions));
    NetIf_RequestedBearer = NETIF_BEARER_NONE;
    NetIf_BackoffMs = 0u;
    NetIf_BearerUpSinceMs = 0u;
    NetIf_Status.state = NETIF_STATE_DOWN;
    NetIf_StateEnteredMs = Gpt_GetMonotonicMs();

    if (NvM_ReadBlock(NVM_BLOCK_DEVICE_CONFIG, &config) != E_OK)
    {
        return E_NOT_OK;
    }

    (void)memcpy(NetIf_BrokerHost, config.brokerHost, sizeof(NetIf_BrokerHost));
    NetIf_BrokerHost[sizeof(NetIf_BrokerHost) - 1u] = '\0';
    NetIf_BrokerPort = config.brokerPort;
    (void)memcpy(NetIf_ClientId, config.deviceId, sizeof(NetIf_ClientId));
    NetIf_ClientId[sizeof(NetIf_ClientId) - 1u] = '\0';

    if (NetIf_PlatformInit() != E_OK)
    {
        return E_NOT_OK;
    }

    NetIf_PlatformMqttSetCallback(&NetIf_OnMessage);
    NetIf_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType NetIf_RequestBearer(NetIf_BearerType bearer)
{
    DET_CHECK_RETURN(NetIf_Initialised != FALSE, MODULE_ID_NETIF, (uint8)bearer,
                     NETIF_API_ID_REQUEST_BEARER, NETIF_E_UNINIT, E_NOT_OK);

#if (NETIF_WIFI_ENABLED == STD_OFF)
    if (bearer == NETIF_BEARER_WIFI)
    {
        return E_UNSUPPORTED;
    }
#endif
#if (NETIF_GSM_ENABLED == STD_OFF)
    if (bearer == NETIF_BEARER_GSM)
    {
        return E_UNSUPPORTED;
    }
#endif

    DET_CHECK_RETURN(bearer <= NETIF_BEARER_GSM, MODULE_ID_NETIF, (uint8)bearer,
                     NETIF_API_ID_REQUEST_BEARER, NETIF_E_PARAM_BEARER, E_NOT_OK);

    if (NetIf_RequestedBearer == bearer)
    {
        return E_OK;
    }

    /* Switching bearer tears the old one down first. Leaving both radios up would draw current for no
     * benefit and, on this hardware, the SIM800L's transmit bursts disturb the WiFi front end. */
    if (NetIf_RequestedBearer != NETIF_BEARER_NONE)
    {
        NetIf_PlatformMqttDisconnect();
        NetIf_StopBearer(NetIf_RequestedBearer);
    }

    NetIf_RequestedBearer = bearer;
    NetIf_BackoffMs = 0u;
    NetIf_Status.currentBackoffMs = 0u;
    NetIf_Status.activeBearer = NETIF_BEARER_NONE;

    if (bearer == NETIF_BEARER_NONE)
    {
        NetIf_EnterState(NETIF_STATE_DOWN);
        return E_OK;
    }

    NetIf_EnterState(NETIF_STATE_CONNECTING);
    return NetIf_StartBearer(bearer);
}

Std_ReturnType NetIf_ReleaseBearer(void)
{
    DET_CHECK_RETURN(NetIf_Initialised != FALSE, MODULE_ID_NETIF, INSTANCE_ID_SINGLE,
                     NETIF_API_ID_REQUEST_BEARER, NETIF_E_UNINIT, E_NOT_OK);

    NetIf_PlatformMqttDisconnect();
    if (NetIf_RequestedBearer != NETIF_BEARER_NONE)
    {
        NetIf_StopBearer(NetIf_RequestedBearer);
    }

    NetIf_RequestedBearer = NETIF_BEARER_NONE;
    NetIf_Status.activeBearer = NETIF_BEARER_NONE;
    NetIf_Status.signalStrengthDbm = 0;
    NetIf_BackoffMs = 0u;
    NetIf_Status.currentBackoffMs = 0u;
    NetIf_EnterState(NETIF_STATE_DOWN);

    return E_OK;
}

boolean NetIf_IsSessionUp(void)
{
    return (NetIf_Status.state == NETIF_STATE_SESSION_UP) ? TRUE : FALSE;
}

NetIf_BearerType NetIf_GetActiveBearer(void)
{
    return NetIf_Status.activeBearer;
}

Std_ReturnType NetIf_Publish(const char *topic, const uint8 *payload, uint16 payloadLen,
                             boolean retain)
{
    DET_CHECK_RETURN(NetIf_Initialised != FALSE, MODULE_ID_NETIF, INSTANCE_ID_SINGLE,
                     NETIF_API_ID_PUBLISH, NETIF_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN((topic != NULL_PTR) && (payload != NULL_PTR), MODULE_ID_NETIF,
                     INSTANCE_ID_SINGLE, NETIF_API_ID_PUBLISH, NETIF_E_PARAM_POINTER, E_NOT_OK);

    /* Rejected rather than truncated. A truncated telemetry record is worse than a missing one,
     * because it parses and looks like data. */
    if (payloadLen > (uint16)NETIF_MAX_PAYLOAD_SIZE)
    {
        (void)Det_ReportError(MODULE_ID_NETIF, INSTANCE_ID_SINGLE, NETIF_API_ID_PUBLISH,
                              NETIF_E_PAYLOAD_TOO_LARGE);
        return E_NO_SPACE;
    }

    if (NetIf_Status.state != NETIF_STATE_SESSION_UP)
    {
        return E_NOT_OK;
    }

    if (NetIf_PlatformMqttPublish(topic, payload, payloadLen, (uint8)NETIF_TELEMETRY_QOS, retain) !=
        E_OK)
    {
        NetIf_Status.publishFailures++;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_BROKER_UNREACHABLE,
                                       (uint8)NetIf_Status.activeBearer, DEM_EVENT_STATUS_FAILED));
        return E_NOT_OK;
    }

    NetIf_Status.publishCount++;
    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_BROKER_UNREACHABLE, (uint8)NetIf_Status.activeBearer,
                                   DEM_EVENT_STATUS_PASSED));
    return E_OK;
}

Std_ReturnType NetIf_Subscribe(const char *topic, NetIf_MessageHandlerType handler)
{
    uint8 i;

    DET_CHECK_RETURN(NetIf_Initialised != FALSE, MODULE_ID_NETIF, INSTANCE_ID_SINGLE,
                     NETIF_API_ID_SUBSCRIBE, NETIF_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN((topic != NULL_PTR) && (handler != NULL_PTR), MODULE_ID_NETIF,
                     INSTANCE_ID_SINGLE, NETIF_API_ID_SUBSCRIBE, NETIF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(strlen(topic) < (uint16)NETIF_MAX_TOPIC_SIZE, MODULE_ID_NETIF,
                     INSTANCE_ID_SINGLE, NETIF_API_ID_SUBSCRIBE, E_PARAM_VALUE, E_NOT_OK);

    for (i = 0u; i < (uint8)NETIF_MAX_SUBSCRIPTIONS; i++)
    {
        if ((NetIf_Subscriptions[i].active == FALSE) ||
            (strcmp(NetIf_Subscriptions[i].topic, topic) == 0))
        {
            (void)memset(NetIf_Subscriptions[i].topic, 0, sizeof(NetIf_Subscriptions[i].topic));
            (void)memcpy(NetIf_Subscriptions[i].topic, topic, strlen(topic));
            NetIf_Subscriptions[i].handler = handler;
            NetIf_Subscriptions[i].active = TRUE;

            /* Applied now if a session already exists, and re-applied on every future reconnection. */
            if (NetIf_Status.state == NETIF_STATE_SESSION_UP)
            {
                return NetIf_PlatformMqttSubscribe(topic, (uint8)NETIF_DIAGNOSTIC_QOS);
            }
            return E_OK;
        }
    }

    return E_NO_SPACE;
}

void NetIf_MainFunction(void)
{
    if (NetIf_Initialised == FALSE)
    {
        return;
    }

    switch (NetIf_Status.state)
    {
    case NETIF_STATE_DOWN:
        /* Nothing requested. Waiting for ComM to choose a bearer. */
        break;

    case NETIF_STATE_CONNECTING:
        if (NetIf_BearerIsUp(NetIf_RequestedBearer) != FALSE)
        {
            NetIf_Status.activeBearer = NetIf_RequestedBearer;
            NetIf_BearerUpSinceMs = Gpt_GetMonotonicMs();
            NetIf_Status.linkUpCount++;
            NetIf_EnterState(NETIF_STATE_LINK_UP);
        }
        else if (Gpt_HasElapsed(NetIf_StateEnteredMs, NETIF_LINK_TIMEOUT_MS) != FALSE)
        {
            NetIf_StopBearer(NetIf_RequestedBearer);
            STD_DISCARD(Dem_SetEventStatus((NetIf_RequestedBearer == NETIF_BEARER_WIFI)
                                               ? DEM_EVENT_WIFI_UNAVAILABLE
                                               : DEM_EVENT_GPRS_UNAVAILABLE,
                                           (uint8)NetIf_RequestedBearer, DEM_EVENT_STATUS_FAILED));
            NetIf_EnterBackoff();
        }
        else
        {
            /* Still associating. */
        }
        break;

    case NETIF_STATE_LINK_UP:
        if (NetIf_BearerIsUp(NetIf_RequestedBearer) == FALSE)
        {
            NetIf_Status.linkDownCount++;
            NetIf_Status.activeBearer = NETIF_BEARER_NONE;
            NetIf_EnterBackoff();
            break;
        }

        STD_DISCARD(Dem_SetEventStatus((NetIf_RequestedBearer == NETIF_BEARER_WIFI)
                                           ? DEM_EVENT_WIFI_UNAVAILABLE
                                           : DEM_EVENT_GPRS_UNAVAILABLE,
                                       (uint8)NetIf_RequestedBearer, DEM_EVENT_STATUS_PASSED));

        if (NetIf_PlatformMqttConnect(NetIf_BrokerHost, NetIf_BrokerPort, NetIf_ClientId,
                                      (uint16)NETIF_KEEPALIVE_S) == E_OK)
        {
            NetIf_Status.sessionCount++;
            /* A successful session resets the backoff, so a brief outage does not leave the interval
             * inflated for the rest of the journey. */
            NetIf_BackoffMs = 0u;
            NetIf_Status.currentBackoffMs = 0u;
            NetIf_ReapplySubscriptions();
            NetIf_EnterState(NETIF_STATE_SESSION_UP);
        }
        else if (Gpt_HasElapsed(NetIf_StateEnteredMs, NETIF_SESSION_TIMEOUT_MS) != FALSE)
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_BROKER_UNREACHABLE,
                                           (uint8)NetIf_RequestedBearer, DEM_EVENT_STATUS_FAILED));
            NetIf_EnterBackoff();
        }
        else
        {
            /* Handshake still in progress. */
        }
        break;

    case NETIF_STATE_SESSION_UP:
        if (NetIf_BearerIsUp(NetIf_RequestedBearer) == FALSE)
        {
            NetIf_Status.linkDownCount++;
            NetIf_Status.activeBearer = NETIF_BEARER_NONE;
            NetIf_PlatformMqttDisconnect();
            NetIf_EnterBackoff();
            break;
        }

        if (NetIf_PlatformMqttLoop() != E_OK)
        {
            /* The session dropped. Treated as a trigger to back off and reconnect, not as an error to
             * report -- a broker restart is a normal event. */
            NetIf_PlatformMqttDisconnect();
            NetIf_EnterBackoff();
            break;
        }

        NetIf_Status.signalStrengthDbm = NetIf_BearerRssi(NetIf_RequestedBearer);
        NetIf_Status.uptimeOnBearerMs = Gpt_ElapsedSince(NetIf_BearerUpSinceMs);
        break;

    case NETIF_STATE_BACKOFF:
        if (Gpt_HasElapsed(NetIf_StateEnteredMs, NetIf_BackoffMs) != FALSE)
        {
            if (NetIf_RequestedBearer == NETIF_BEARER_NONE)
            {
                NetIf_EnterState(NETIF_STATE_DOWN);
            }
            else
            {
                NetIf_EnterState(NETIF_STATE_CONNECTING);
                STD_DISCARD(NetIf_StartBearer(NetIf_RequestedBearer));
            }
        }
        break;

    default:
        NetIf_EnterState(NETIF_STATE_DOWN);
        break;
    }
}

Std_ReturnType NetIf_GetStatus(NetIf_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_NETIF, INSTANCE_ID_SINGLE,
                     NETIF_API_ID_MAIN_FUNCTION, NETIF_E_PARAM_POINTER, E_NOT_OK);

    *status = NetIf_Status;
    return E_OK;
}

void NetIf_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = NETIF_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_NETIF;
        versioninfo->sw_major_version = NETIF_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = NETIF_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = NETIF_SW_PATCH_VERSION;
    }
}

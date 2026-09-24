/**
 * @file    NetIf.h
 * @brief   IP bearer abstraction over WiFi and GSM/GPRS, plus the MQTT session.
 *
 * Presents one interface for "can I send telemetry, and how". Which physical bearer is carrying it is
 * this module's business; nothing above it needs to know, and ComM decides which to prefer.
 *
 * @par Why one interface over two very different bearers
 * v1 had two parallel code paths. `handleWifiConnection` and `handleGSMConnection` each maintained
 * their own retry counter, their own MQTT client, their own NTP wait loop and their own copy of the
 * connect-with-retry logic, and both wrote the same `flags[]` entries. The publish path then chose
 * between them with `if (flags[wf_f]) ... else if (flags[gsm_f])`, duplicated in two functions. Adding
 * a third bearer, or fixing a retry bug, meant changing the same logic in several places and hoping
 * none was missed.
 *
 * Here the retry policy, the backoff and the session state machine exist once. A bearer contributes
 * only the four operations that genuinely differ: bring the link up, tear it down, report whether it is
 * up, and report its signal quality.
 *
 * @par Exponential backoff, because the alternative is worse than not retrying
 * v1 retried a failed broker connection three times with a fixed 2 s gap, inside a task that ran every
 * three seconds — so a broker that was down produced a connection attempt roughly every second,
 * indefinitely, on a metered cellular link. The backoff here doubles from
 * ::NETIF_RECONNECT_MIN_MS to ::NETIF_RECONNECT_MAX_MS, so an outage of hours costs a handful of
 * attempts rather than thousands.
 *
 * @req SWREQ-COM-0040 .. SWREQ-COM-0060
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef NETIF_H
#define NETIF_H

#include "base/Autosar_ModuleIds.h"
#include "ecuabs/NetIf/NetIf_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETIF_VENDOR_ID 0xFFFEu
#define NETIF_SW_MAJOR_VERSION 2u
#define NETIF_SW_MINOR_VERSION 0u
#define NETIF_SW_PATCH_VERSION 0u

#define NETIF_API_ID_INIT 0x00u
#define NETIF_API_ID_REQUEST_BEARER 0x20u
#define NETIF_API_ID_PUBLISH 0x21u
#define NETIF_API_ID_MAIN_FUNCTION 0x0Eu
#define NETIF_API_ID_SUBSCRIBE 0x22u

#define NETIF_E_UNINIT E_UNINIT
#define NETIF_E_PARAM_POINTER E_PARAM_POINTER
#define NETIF_E_PARAM_BEARER 0x20u
#define NETIF_E_NO_BEARER 0x21u
#define NETIF_E_NO_SESSION 0x22u
#define NETIF_E_PUBLISH_FAILED 0x23u
#define NETIF_E_PAYLOAD_TOO_LARGE 0x24u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Physical bearer. */
typedef enum
{
    NETIF_BEARER_NONE = 0, /**< No bearer selected or available. */
    NETIF_BEARER_WIFI = 1, /**< 802.11 station.                  */
    NETIF_BEARER_GSM = 2   /**< GSM/GPRS modem.                  */
} NetIf_BearerType;

/** State of the bearer and the broker session together. */
typedef enum
{
    NETIF_STATE_DOWN = 0,       /**< No link.                                    */
    NETIF_STATE_CONNECTING = 1, /**< Bringing the link up.                       */
    NETIF_STATE_LINK_UP = 2,    /**< Link up, no broker session.                 */
    NETIF_STATE_SESSION_UP = 3, /**< Broker session established; publishing.     */
    NETIF_STATE_BACKOFF = 4     /**< Waiting out a backoff before retrying.      */
} NetIf_StateType;

/** Link and session status, published in the telemetry health record. */
typedef struct
{
    NetIf_BearerType activeBearer;  /**< Bearer currently carrying traffic.          */
    NetIf_StateType state;          /**< Combined link and session state.            */
    sint8 signalStrengthDbm;        /**< RSSI, 0 if unknown.                         */
    uint32 publishCount;            /**< Payloads accepted by the broker.            */
    uint32 publishFailures;         /**< Publishes the broker or link rejected.      */
    uint32 linkUpCount;             /**< Times a bearer came up.                     */
    uint32 linkDownCount;           /**< Times a bearer went down.                   */
    uint32 sessionCount;            /**< Broker sessions established.                */
    uint32 currentBackoffMs;        /**< Backoff in force, 0 when not backing off.   */
    uint32 uptimeOnBearerMs;        /**< How long the current bearer has been up.    */
} NetIf_StatusType;

/**
 * @brief Callback for an inbound message on a subscribed topic.
 *
 * @param topic      NUL-terminated topic the message arrived on.
 * @param payload    Message bytes. Not NUL-terminated.
 * @param payloadLen Message length.
 *
 * @note Called from the network stack's context, so it must not block. The handler copies what it
 *       needs into a queue and returns; v1 parsed an MQTT payload and allocated heap nodes inside the
 *       callback, which is both slow and unbounded in that context.
 */
typedef void (*NetIf_MessageHandlerType)(const char *topic, const uint8 *payload, uint16 payloadLen);

/*==================================================================================================
 *  API
 *================================================================================================*/

/** Initialise the module. Does not bring any bearer up. */
CHECK_RETURN Std_ReturnType NetIf_Init(void);

/**
 * @brief Ask for @p bearer to be brought up.
 *
 * Asynchronous: returns immediately and the attempt proceeds in ::NetIf_MainFunction. A bearer that is
 * already up is a no-op.
 *
 * @return E_OK if the request was accepted; E_NOT_OK for an unknown bearer or one that is not
 *         configured in this build.
 */
CHECK_RETURN Std_ReturnType NetIf_RequestBearer(NetIf_BearerType bearer);

/** Tear the current bearer down and return to ::NETIF_STATE_DOWN. */
CHECK_RETURN Std_ReturnType NetIf_ReleaseBearer(void);

/** TRUE if a broker session is established and a publish would be attempted. */
boolean NetIf_IsSessionUp(void);

/**
 * @brief Whether @p bearer currently has a usable link, regardless of which bearer is in use.
 *
 * ComM needs this to decide whether returning to a preferred bearer is even possible, and it must be
 * answerable about a bearer that is *not* the active one -- which ::NetIf_GetStatus cannot do, since it
 * reports the active bearer only.
 *
 * Without it, arbitration has to guess. An earlier revision of ComM used the WiFi failure count as a
 * proxy for "WiFi might be back", which made the fallback to cellular permanent for the life of the
 * run: the count reaches its limit because WiFi failed, and nothing clears it while cellular works.
 *
 * @return TRUE if @p bearer is associated and addressable. FALSE for ::NETIF_BEARER_NONE, for a bearer
 *         this build has switched off, and for one that is merely associated without an address.
 */
boolean NetIf_BearerAvailable(NetIf_BearerType bearer);

/** The bearer currently carrying traffic, or ::NETIF_BEARER_NONE. */
NetIf_BearerType NetIf_GetActiveBearer(void);

/**
 * @brief Publish @p payload to @p topic.
 *
 * @param[in] topic      NUL-terminated topic.
 * @param[in] payload    Bytes to publish.
 * @param[in] payloadLen Length of @p payload.
 * @param[in] retain     TRUE to ask the broker to retain the message.
 * @return E_OK if the broker accepted it; ::NETIF_E_NO_SESSION as E_NOT_OK if there is no session;
 *         E_NO_SPACE if @p payloadLen exceeds ::NETIF_MAX_PAYLOAD_SIZE.
 */
CHECK_RETURN Std_ReturnType NetIf_Publish(const char *topic, const uint8 *payload, uint16 payloadLen,
                                          boolean retain);

/**
 * @brief Subscribe to @p topic and route its messages to @p handler.
 *
 * Re-applied automatically after a reconnection, so a caller subscribes once rather than having to
 * notice that the session dropped. v1 re-subscribed from inside its transfer loop, which meant a
 * subscription could be silently lost whenever that loop took a different path.
 */
CHECK_RETURN Std_ReturnType NetIf_Subscribe(const char *topic, NetIf_MessageHandlerType handler);

/**
 * @brief Drive the link and session state machine, and service the network stack.
 *
 * Driven cyclically by SchM from the connectivity task. All the retry timing, the backoff and the
 * reconnection live here, once.
 */
void NetIf_MainFunction(void);

/**
 * @brief Read the link and session status.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType NetIf_GetStatus(NetIf_StatusType *status);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void NetIf_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* NETIF_H */

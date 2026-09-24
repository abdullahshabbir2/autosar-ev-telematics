/**
 * @file    NetIf_Cfg.h
 * @brief   Bearer and MQTT session configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef NETIF_CFG_H
#define NETIF_CFG_H

#include "Com_Cfg.h"
#include "Std_Types.h"

#define NETIF_DEV_ERROR_DETECT STD_ON

/** Bearers compiled into this build. */
#define NETIF_WIFI_ENABLED STD_ON
#define NETIF_GSM_ENABLED STD_ON

/*==================================================================================================
 *  Reconnection policy
 *
 *  v1 retried a failed broker connection three times with a fixed 2 s gap, from inside a task that ran
 *  every three seconds. A broker that was down therefore drew a connection attempt roughly every
 *  second, indefinitely, on a metered cellular link -- which costs money and achieves nothing. Doubling
 *  the interval turns an outage of hours into a handful of attempts.
 *================================================================================================*/

/** First retry interval, in milliseconds. */
#define NETIF_RECONNECT_MIN_MS 2000uL

/**
 * @brief Longest retry interval, in milliseconds.
 *
 * 5 minutes. Long enough that a prolonged outage is nearly free; short enough that a vehicle driving
 * back into coverage starts publishing again within a few minutes rather than an hour.
 */
#define NETIF_RECONNECT_MAX_MS 300000uL

/** Bound on bringing a bearer up, in milliseconds. */
#define NETIF_LINK_TIMEOUT_MS 30000uL

/**
 * @brief Bound on establishing the broker session, in milliseconds.
 *
 * 15 s. A TLS-free MQTT CONNECT over a weak GPRS link has been observed taking 11 s, so a shorter
 * timeout would abandon sessions that were about to succeed.
 */
#define NETIF_SESSION_TIMEOUT_MS 15000uL

/**
 * @brief MQTT keep-alive interval, in seconds.
 *
 * 60. The telemetry period is 3 s, so traffic normally keeps the session alive on its own; the
 * keep-alive matters only while a vehicle is parked with the ignition on and nothing is moving.
 */
#define NETIF_KEEPALIVE_S 60u

/*==================================================================================================
 *  Payload and topics
 *================================================================================================*/

/**
 * @brief Largest payload a single publish may carry, in bytes.
 *
 * Sized from the record buffer plus the framing the broker client adds, so a worst-case record can
 * always be published. A payload larger than this is rejected rather than truncated -- a truncated
 * telemetry record is worse than a missing one, because it parses.
 */
#define NETIF_MAX_PAYLOAD_SIZE (COM_RECORD_BUFFER_SIZE + 64u)

/** Longest topic string, including the terminator. */
#define NETIF_MAX_TOPIC_SIZE 64u

/** Subscriptions the module can hold. Two: the backfill request and the diagnostic request. */
#define NETIF_MAX_SUBSCRIPTIONS 2u

/*==================================================================================================
 *  Quality of service
 *================================================================================================*/

/**
 * @brief MQTT QoS for telemetry publishes.
 *
 * 0. The SD card is the reliability mechanism: a record is only removed from the transfer queue once
 * the publish has been accepted, so an at-most-once publish that is lost is simply retried from the
 * card. QoS 1 would add a broker-side acknowledgement round trip per record for a guarantee the
 * store-and-forward cursor already provides more cheaply.
 */
#define NETIF_TELEMETRY_QOS 0u

/** QoS for diagnostic responses, which have no store-and-forward backing. */
#define NETIF_DIAGNOSTIC_QOS 1u

#endif /* NETIF_CFG_H */

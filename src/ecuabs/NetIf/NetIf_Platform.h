/**
 * @file    NetIf_Platform.h
 * @brief   Platform leaf of the bearer abstraction.
 *
 * The operations that genuinely differ between WiFi and GSM, and the MQTT client. The retry policy,
 * the backoff, the session state machine and the subscription bookkeeping are all in NetIf.c and are
 * shared by both bearers -- which is the whole point of the split. v1 duplicated every one of those in
 * two parallel code paths.
 *
 * Implemented by NetIf_Esp32.cpp on the target and by test/support/Stub_Platform.c on the host.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef NETIF_PLATFORM_H
#define NETIF_PLATFORM_H

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise both bearers' stacks without bringing either up. */
CHECK_RETURN Std_ReturnType NetIf_PlatformInit(void);

/*---------------------------------- WiFi ------------------------------------*/

/** Begin associating with a stored access point. Asynchronous. */
CHECK_RETURN Std_ReturnType NetIf_PlatformWifiConnect(void);

/** Disassociate and power the radio down. */
void NetIf_PlatformWifiDisconnect(void);

/** TRUE once associated and holding an IP address. */
boolean NetIf_PlatformWifiIsUp(void);

/** RSSI in dBm, or 0 if not associated. */
sint8 NetIf_PlatformWifiRssi(void);

/*----------------------------------- GSM ------------------------------------*/

/** Power the modem on and begin a GPRS attach. Asynchronous. */
CHECK_RETURN Std_ReturnType NetIf_PlatformGsmConnect(void);

/** Detach and power the modem down. */
void NetIf_PlatformGsmDisconnect(void);

/** TRUE once attached and holding an IP address. */
boolean NetIf_PlatformGsmIsUp(void);

/** Signal quality in dBm, or 0 if not attached. */
sint8 NetIf_PlatformGsmRssi(void);

/*---------------------------------- MQTT ------------------------------------*/

/**
 * @brief Open a broker session over whichever bearer is currently up.
 *
 * @param host      NUL-terminated broker address.
 * @param port      Broker port.
 * @param clientId  NUL-terminated MQTT client identifier.
 * @param keepAliveS Keep-alive interval in seconds.
 * @return E_OK once CONNACK has been received; E_TIMEOUT otherwise.
 */
CHECK_RETURN Std_ReturnType NetIf_PlatformMqttConnect(const char *host, uint16 port,
                                                     const char *clientId, uint16 keepAliveS);

/** Close the broker session. */
void NetIf_PlatformMqttDisconnect(void);

/** TRUE if the broker session is open. */
boolean NetIf_PlatformMqttIsConnected(void);

/**
 * @brief Publish one payload.
 * @return E_OK if the client accepted it for transmission.
 */
CHECK_RETURN Std_ReturnType NetIf_PlatformMqttPublish(const char *topic, const uint8 *payload,
                                                      uint16 payloadLen, uint8 qos, boolean retain);

/** Subscribe to @p topic at @p qos. */
CHECK_RETURN Std_ReturnType NetIf_PlatformMqttSubscribe(const char *topic, uint8 qos);

/**
 * @brief Service the client: send keep-alives and deliver inbound messages.
 *
 * @return E_OK if the session is still open; E_NOT_OK if it has dropped, which NetIf treats as a
 *         trigger to re-enter its backoff rather than as an error to report.
 */
CHECK_RETURN Std_ReturnType NetIf_PlatformMqttLoop(void);

/**
 * @brief Register the function the platform calls when a subscribed message arrives.
 */
void NetIf_PlatformMqttSetCallback(void (*callback)(const char *topic, const uint8 *payload,
                                                   uint16 payloadLen));

#ifdef __cplusplus
}
#endif

#endif /* NETIF_PLATFORM_H */

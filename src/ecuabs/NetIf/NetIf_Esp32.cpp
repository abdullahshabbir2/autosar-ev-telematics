/**
 * @file    NetIf_Esp32.cpp
 * @brief   ESP32 platform leaf of the bearer abstraction: WiFi, GPRS and one MQTT client over either.
 *
 * @par One client, two bearers
 * @c PubSubClient is given a @c Client reference at connect time, and this file is the only place that decides
 * which one. Both bearers produce something implementing the Arduino @c Client interface -- @c WiFiClient for
 * the radio, @c TinyGsmClient for the modem -- so the MQTT layer above never learns which is in use.
 *
 * That is the whole reason the module is shaped this way. v1 had two parallel implementations of publishing,
 * reconnection and backoff, one per bearer, and they had already drifted: the WiFi path retried three times and
 * the GPRS path retried indefinitely, the WiFi path checked the publish result and the GPRS path did not. Any
 * fix to one had to be remembered for the other, and by the time this rewrite started they no longer agreed
 * about when a session was considered lost.
 *
 * Everything above the bearer -- the retry policy, the exponential backoff, the session state machine, the
 * subscription bookkeeping -- is in NetIf.c, shared, and unit tested against a stub of this interface.
 *
 * @par The modem is driven through TinyGSM
 * Unlike the DS3231, the SIM800L genuinely warrants a library: the AT command set is large, stateful, and full
 * of firmware-revision quirks, and the unsolicited result codes have to be demultiplexed from command replies.
 * Reimplementing that would be a month of work to arrive at something less well tested. The AT transport is
 * still this project's own ::Uart module rather than a bare @c HardwareSerial, so the pin allocation and the
 * framing stay under the pin map's control.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>

/* TinyGSM needs both of these before the header. The modem model selects the command dialect; the buffer is
 * the AT response assembly buffer, and 1024 matches ::UART_RX_BUFFER_GSM so the two cannot disagree about how
 * much of a response can be held. */
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include <PubSubClient.h>
#include <TinyGsmClient.h>
#include <WiFi.h>
#include <string.h>

#include "mcal/Dio/Dio.h"
#include "Ecu_PinMap.h"
#include "mcal/Gpt/Gpt.h"
#include "ecuabs/NetIf/NetIf.h"
#include "ecuabs/NetIf/NetIf_Platform.h"
#include "Secrets.h"
#include "mcal/Uart/Uart_Cfg.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/** How long the modem's PWRKEY must be held low to toggle power, per the SIM800L datasheet. */
#define NETIF_GSM_PWRKEY_PULSE_MS 1200uL

/** How long to wait for the modem to answer AT after a power toggle. */
#define NETIF_GSM_BOOT_TIMEOUT_MS 10000uL

/**
 * @brief The AT transport.
 *
 * @c Serial2 directly rather than through ::Uart, because TinyGSM requires a @c Stream reference and holds it
 * for its lifetime. ::Uart_Open is still what configures the port -- baud, framing, pins and buffer size all
 * come from Uart_Cfg.h -- so the pin map remains authoritative; this reference only carries the bytes.
 */
static TinyGsm NetIf_Modem(Serial2);
static TinyGsmClient NetIf_GsmClient(NetIf_Modem);
static WiFiClient NetIf_WifiClient;
static PubSubClient NetIf_MqttClient;

/** The callback NetIf.c registered, or NULL_PTR. */
static void (*NetIf_InboundCallback)(const char *topic, const uint8 *payload, uint16 payloadLen) = NULL_PTR;

static boolean NetIf_GsmAttached = FALSE;
static boolean NetIf_PlatformInitialised = FALSE;

/*==================================================================================================
 *  MQTT inbound
 *================================================================================================*/

/**
 * @brief PubSubClient's callback, adapted to this project's signature.
 *
 * PubSubClient hands over a non-const @c char* topic and a @c uint8_t* payload that both point into its own
 * receive buffer and are valid only for the duration of the call. The adaptation is therefore a straight
 * forward, with no copy: DiagSwc's parser consumes the payload synchronously and is bounded by
 * ::NETIF_MAX_PAYLOAD_SIZE, which is checked here rather than trusted.
 */
static void NetIf_MqttCallbackAdapter(char *topic, uint8_t *payload, unsigned int length)
{
    if ((NetIf_InboundCallback == NULL_PTR) || (topic == NULL_PTR) || (payload == NULL_PTR))
    {
        return;
    }

    /* A payload longer than the configured maximum cannot happen while PubSubClient's own buffer is sized to
     * match, but the check is here because that sizing is a build-time assumption and this is the boundary at
     * which untrusted data enters the ECU. v1 passed a broker payload straight into a fixed buffer. */
    if (length > (unsigned int)NETIF_MAX_PAYLOAD_SIZE)
    {
        return;
    }

    NetIf_InboundCallback(topic, (const uint8 *)payload, (uint16)length);
}

/*==================================================================================================
 *  Init
 *================================================================================================*/

extern "C" Std_ReturnType NetIf_PlatformInit(void)
{
    /* Station mode only, and autoconnect off. Autoconnect has the radio retry a stored access point on its own
     * schedule, behind NetIf's state machine and its backoff -- so the two fight, and the observable behaviour
     * is a link that comes up during a backoff interval the state machine believes it is still waiting out.
     * Persistent credentials are also disabled: they are written to NVS on every connect, which is a flash write
     * per connection attempt for data this firmware already holds in Secrets.h. */
    WiFi.persistent(false);
    (void)WiFi.setAutoReconnect(false);
    (void)WiFi.mode(WIFI_STA);
    (void)WiFi.disconnect(true);

    NetIf_MqttClient.setBufferSize((uint16_t)NETIF_MAX_PAYLOAD_SIZE);
    NetIf_MqttClient.setKeepAlive((uint16_t)NETIF_KEEPALIVE_S);

    /* The socket timeout is what bounds a publish against a broker that has stopped reading. Without it
     * PubSubClient's write blocks in lwIP until the TCP stack gives up, which on a stalled GPRS link is tens of
     * seconds -- long enough for the connectivity task to blow its execution budget and be reported as hung. */
    NetIf_MqttClient.setSocketTimeout((uint16_t)(NETIF_SESSION_TIMEOUT_MS / 1000uL));
    NetIf_MqttClient.setCallback(NetIf_MqttCallbackAdapter);

    NetIf_PlatformInitialised = TRUE;
    return E_OK;
}

/*==================================================================================================
 *  WiFi
 *================================================================================================*/

extern "C" Std_ReturnType NetIf_PlatformWifiConnect(void)
{
#if (NETIF_WIFI_ENABLED == STD_OFF)
    return E_UNSUPPORTED;
#else
    if (NetIf_PlatformInitialised == FALSE)
    {
        return E_NOT_OK;
    }

    /* Asynchronous by contract: begin() starts the association and returns, and NetIf polls
     * ::NetIf_PlatformWifiIsUp until ::NETIF_LINK_TIMEOUT_MS. A blocking connect here would put a 30-second
     * wait inside the connectivity task's 800 ms budget.
     *
     * Only the first stored access point is attempted. Trying each in turn belongs in NetIf.c with the rest of
     * the retry policy, not here -- this function's job is one attempt. */
    (void)WiFi.begin(SECRETS_WIFI_SSID_1, SECRETS_WIFI_PASSWORD_1);

    return E_OK;
#endif
}

extern "C" void NetIf_PlatformWifiDisconnect(void)
{
    /* true for the argument powers the radio down as well as disassociating. Leaving it up draws about 80 mA
     * for nothing, and on a vehicle running its ECU from the traction pack that is the difference between a
     * unit that survives a long park and one that does not. */
    (void)WiFi.disconnect(true);
}

extern "C" boolean NetIf_PlatformWifiIsUp(void)
{
    /* Both conditions. WL_CONNECTED means associated, which is not the same as having an address: with DHCP
     * there is a window of tens to hundreds of milliseconds in between, and a TCP connect attempted inside it
     * fails in a way that looks like a broker fault. v1 checked only the status and retried the broker for that
     * whole window on every connection. */
    return ((WiFi.status() == WL_CONNECTED) && ((uint32_t)WiFi.localIP() != 0uL)) ? TRUE : FALSE;
}

extern "C" sint8 NetIf_PlatformWifiRssi(void)
{
    if (NetIf_PlatformWifiIsUp() == FALSE)
    {
        return 0;
    }

    {
        const long rssi = WiFi.RSSI();

        /* Clamped into the signed 8-bit range the interface uses. Real values are about -30 to -90 dBm, but the
         * driver returns 0 or a large positive number when the radio is between states, and an unclamped cast
         * would publish that as a plausible-looking signal strength. */
        if (rssi < -128L)
        {
            return -128;
        }
        if (rssi > 0L)
        {
            return 0;
        }

        return (sint8)rssi;
    }
}

/*==================================================================================================
 *  GSM
 *================================================================================================*/

/** Pulse PWRKEY to toggle the modem's power state. */
static void NetIf_GsmPulsePowerKey(void)
{
    /* PWRKEY is active low and toggles: the same pulse turns the modem on or off. The pulse width is the
     * datasheet minimum plus margin; a shorter one is ignored, which presents as a modem that never answers. */
    Dio_WriteChannel(DIO_CHANNEL_GSM_PWRKEY, STD_LOW);
    Gpt_DelayMs(NETIF_GSM_PWRKEY_PULSE_MS);
    Dio_WriteChannel(DIO_CHANNEL_GSM_PWRKEY, STD_HIGH);
}

extern "C" Std_ReturnType NetIf_PlatformGsmConnect(void)
{
#if (NETIF_GSM_ENABLED == STD_OFF)
    return E_UNSUPPORTED;
#else
    if (NetIf_PlatformInitialised == FALSE)
    {
        return E_NOT_OK;
    }

    /* The STATUS line says whether the modem is already running, which is the difference between a cold start
     * and a reconnection after a dropped attach. Pulsing PWRKEY unconditionally would turn a running modem
     * *off* -- the pulse toggles -- and the retry after that would turn it on again, so every other attempt
     * would fail. That alternating pattern is a genuinely confusing symptom, and it is avoided by asking. */
    if (Dio_ReadChannel(DIO_CHANNEL_GSM_STATUS) == STD_LOW)
    {
        NetIf_GsmPulsePowerKey();

        {
            const Gpt_TimestampType start = Gpt_GetMonotonicMs();

            while (Gpt_HasElapsed(start, NETIF_GSM_BOOT_TIMEOUT_MS) == FALSE)
            {
                if (NetIf_Modem.testAT(100))
                {
                    break;
                }
            }

            if (NetIf_Modem.testAT(100) == false)
            {
                return E_TIMEOUT;
            }
        }
    }

    if (NetIf_Modem.init() == false)
    {
        return E_NOT_OK;
    }

    /* strlen of a string literal folds at compile time, so this costs nothing and reads as the condition it is.
     * sizeof would not work here: it cannot appear in a #if. */
    if (strlen(SECRETS_SIM_PIN) > 0u)
    {
        if (NetIf_Modem.getSimStatus() != SIM_READY)
        {
            if (NetIf_Modem.simUnlock(SECRETS_SIM_PIN) == false)
            {
                /* Deliberately not retried. Three wrong PIN attempts permanently lock the SIM and require a
                 * PUK, which means a site visit -- so a failure here is reported once and the bearer stays
                 * down. This is the one place in the ECU where a retry policy would do real harm. */
                return E_NOT_OK;
            }
        }
    }

    /* gprsConnect blocks, which is why ::ComM gives the GSM bearer its own longer budget during startup and why
     * this is never called from the periodic path once a bearer is up. The modem's own attach timeout bounds it. */
    if (NetIf_Modem.gprsConnect(SECRETS_GPRS_APN, SECRETS_GPRS_USER, SECRETS_GPRS_PASSWORD) == false)
    {
        return E_NOT_OK;
    }

    NetIf_GsmAttached = TRUE;
    return E_OK;
#endif
}

extern "C" void NetIf_PlatformGsmDisconnect(void)
{
    if (NetIf_GsmAttached != FALSE)
    {
        (void)NetIf_Modem.gprsDisconnect();
        NetIf_GsmAttached = FALSE;
    }

    /* poweroff() is the graceful path and lets the modem detach from the network properly. If it fails the
     * modem is unresponsive, and PWRKEY is the hardware fallback -- the equivalent of holding the button. */
    if (NetIf_Modem.poweroff() == false)
    {
        NetIf_GsmPulsePowerKey();
    }
}

extern "C" boolean NetIf_PlatformGsmIsUp(void)
{
    if (NetIf_GsmAttached == FALSE)
    {
        return FALSE;
    }

    return (NetIf_Modem.isGprsConnected() != false) ? TRUE : FALSE;
}

extern "C" sint8 NetIf_PlatformGsmRssi(void)
{
    if (NetIf_PlatformGsmIsUp() == FALSE)
    {
        return 0;
    }

    {
        /* getSignalQuality returns the AT+CSQ raw value, 0..31, and 99 for "not detectable". That is not dBm,
         * and reporting it as dBm -- which v1 did -- makes a strong signal of 31 look like a nonsensical
         * +31 dBm. The mapping from the datasheet is linear: 0 is -113 dBm and each step is 2 dB. */
        const int16_t csq = NetIf_Modem.getSignalQuality();

        if ((csq < 0) || (csq > 31))
        {
            return 0;
        }

        return (sint8)(-113 + (2 * csq));
    }
}

/*==================================================================================================
 *  MQTT
 *================================================================================================*/

extern "C" Std_ReturnType NetIf_PlatformMqttConnect(const char *host, uint16 port, const char *clientId,
                                                    uint16 keepAliveS)
{
    if ((host == NULL_PTR) || (clientId == NULL_PTR) || (NetIf_PlatformInitialised == FALSE))
    {
        return E_NOT_OK;
    }

    /* Which bearer carries the session is decided here and nowhere else. WiFi is preferred when up, because it
     * costs nothing per byte; ComM owns the policy that decides *which bearer to bring up*, including the
     * 30-second hysteresis that stops a marginal WiFi link flapping the session. This function only follows
     * whatever is actually available at the moment it is called. */
    if (NetIf_PlatformWifiIsUp() != FALSE)
    {
        NetIf_MqttClient.setClient(NetIf_WifiClient);
    }
    else if (NetIf_PlatformGsmIsUp() != FALSE)
    {
        NetIf_MqttClient.setClient(NetIf_GsmClient);
    }
    else
    {
        /* No bearer. Not an error worth reporting -- NetIf's state machine calls this as it comes up and will
         * call it again once a bearer arrives. */
        return E_NOT_OK;
    }

    NetIf_MqttClient.setKeepAlive(keepAliveS);
    NetIf_MqttClient.setServer(host, port);

    {
        boolean connected;

        /* An empty username means an unauthenticated broker, and PubSubClient's four-argument connect with a
         * NULL user is not the same call as the one-argument form -- it sends a CONNECT with the username flag
         * set and an empty string, which a broker configured to require authentication accepts and one
         * configured to forbid anonymous access rejects. Choosing the form by whether a username was configured
         * keeps both deployments working. */
        if (strlen(SECRETS_MQTT_USER) > 0u)
        {
            connected =
                (NetIf_MqttClient.connect(clientId, SECRETS_MQTT_USER, SECRETS_MQTT_PASSWORD) != false)
                    ? TRUE
                    : FALSE;
        }
        else
        {
            connected = (NetIf_MqttClient.connect(clientId) != false) ? TRUE : FALSE;
        }

        if (connected == FALSE)
        {
            /* E_TIMEOUT rather than E_NOT_OK, because that is what NetIf's backoff distinguishes: a refused
             * connection and an unreachable broker both arrive here, and both are answered by waiting longer.
             * PubSubClient's state() would tell them apart, and nothing above would act differently. */
            return E_TIMEOUT;
        }
    }

    return E_OK;
}

extern "C" void NetIf_PlatformMqttDisconnect(void)
{
    NetIf_MqttClient.disconnect();
}

extern "C" boolean NetIf_PlatformMqttIsConnected(void)
{
    return (NetIf_MqttClient.connected() != false) ? TRUE : FALSE;
}

extern "C" Std_ReturnType NetIf_PlatformMqttPublish(const char *topic, const uint8 *payload,
                                                    uint16 payloadLen, uint8 qos, boolean retain)
{
    if ((topic == NULL_PTR) || (payload == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (payloadLen > (uint16)NETIF_MAX_PAYLOAD_SIZE)
    {
        return E_PARAM_VALUE;
    }
    if (NetIf_MqttClient.connected() == false)
    {
        return E_NOT_OK;
    }

    /* PubSubClient implements QoS 0 for publishing only. The parameter is accepted and ignored rather than
     * rejected, and that is a deliberate, documented deviation: ::NETIF_TELEMETRY_QOS is 0 because the
     * store-and-forward layer in TelemSwc already provides the delivery guarantee -- a record stays on the card
     * until the broker has acknowledged it at application level -- so broker-level QoS 1 would duplicate a
     * guarantee that is already stronger. ::NETIF_DIAGNOSTIC_QOS is declared 1 to document the intent for a
     * future client that supports it. See docs/03-interfaces.md. */
    (void)qos;

    if (NetIf_MqttClient.publish(topic, payload, (unsigned int)payloadLen, (retain != FALSE)) == false)
    {
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" Std_ReturnType NetIf_PlatformMqttSubscribe(const char *topic, uint8 qos)
{
    if (topic == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if (NetIf_MqttClient.connected() == false)
    {
        return E_NOT_OK;
    }

    if (NetIf_MqttClient.subscribe(topic, qos) == false)
    {
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" Std_ReturnType NetIf_PlatformMqttLoop(void)
{
    /* loop() returns false once the session has gone. NetIf treats that as a trigger to re-enter its backoff
     * rather than as an error to report, which is why nothing is logged here: a dropped session on a vehicle
     * moving between cells is normal operation, and reporting each one would bury the genuine faults. */
    if (NetIf_MqttClient.loop() == false)
    {
        return E_NOT_OK;
    }

    return E_OK;
}

extern "C" void NetIf_PlatformMqttSetCallback(void (*callback)(const char *topic, const uint8 *payload,
                                                               uint16 payloadLen))
{
    NetIf_InboundCallback = callback;
}

/**
 * @file    test_net.c
 * @brief   Unit tests for the bearer abstraction and bearer arbitration.
 *
 * @par Why one state machine and not two
 * v1 had two parallel implementations of publishing, reconnection and backoff -- one per bearer -- and
 * they had already drifted before this rewrite began: the WiFi path retried three times and the GPRS
 * path retried indefinitely; the WiFi path checked the publish result and the GPRS path did not. Any fix
 * had to be remembered twice, and by the end they no longer agreed about when a session was lost.
 *
 * So the retry policy, the backoff, the session state machine and the subscription bookkeeping are all
 * in NetIf, shared, and only the operations that genuinely differ are per bearer. These tests exercise
 * that shared machinery against both bearers, which is the only way to show the sharing is real.
 *
 * ComM sits above it and decides *which* bearer is wanted. Its two interesting properties are the
 * failure-driven fallback and the hysteresis that stops a marginal WiFi link flapping the session.
 *
 * @req SWREQ-COM-0040 .. SWREQ-COM-0060, SWREQ-COM-0070 .. SWREQ-COM-0082
 * @verifies TS-NET-001 .. TS-NET-014, TS-COMM-001 .. TS-COMM-008
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "Ecu_Cfg.h"
#include "ecuabs/NetIf/NetIf.h"
#include "mcal/Fls/Fls.h"
#include "services/Fee/Fee.h"
#include "services/NvM/NvM.h"
#include "ecuabs/NetIf/NetIf_Cfg.h"
#include "mcal/Gpt/Gpt.h"
#include "services/ComM/ComM.h"
#include "services/ComM/ComM_Cfg.h"
#include "services/Det/Det.h"
#include "Stub_Mcal.h"
#include "Stub_Platform.h"
#include "unity.h"

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Stub_Platform_ResetAll();
    Det_Init();

    /* NetIf reads the broker host, port and client identifier from NVM_BLOCK_DEVICE_CONFIG, so the
     * storage stack has to be up first. That dependency is deliberate: the endpoint is provisioned per
     * unit and changeable remotely, which is what stops one firmware image being tied to one broker. */
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
}

void tearDown(void)
{
}

/** Run NetIf's cyclic function @p n times, advancing time by its period each pass. */
static void TnRunNetIf(uint16 n)
{
    uint16 i;

    for (i = 0u; i < n; i++)
    {
        NetIf_MainFunction();
        Stub_Gpt_AdvanceMs(ECU_CONNECTIVITY_PERIOD_MS);
    }
}

/** Run ComM's cyclic function @p n times, advancing time by its arbitration interval each pass. */
static void TnRunComM(uint16 n)
{
    uint16 i;

    for (i = 0u; i < n; i++)
    {
        ComM_MainFunction();
        NetIf_MainFunction();
        Stub_Gpt_AdvanceMs(COMM_ARBITRATION_INTERVAL_MS);
    }
}

/**
 * @brief Run both cyclic functions for @p attempts full connection attempts' worth of time.
 *
 * One attempt is a session timeout plus the backoff that follows it, and the backoff doubles -- so
 * driving N attempts means advancing well past N * NETIF_SESSION_TIMEOUT_MS. The step used here is the
 * backoff ceiling, which bounds it: however far the doubling has gone, one step of
 * NETIF_RECONNECT_MAX_MS always expires the current backoff.
 *
 * Virtual time makes this instant. The alternative -- guessing a pass count -- is how the first draft
 * of these cases came to assert that a fallback had not happened when it merely had not happened yet.
 */
static void TnRunAttempts(uint16 attempts)
{
    uint16 i;

    for (i = 0u; i < attempts; i++)
    {
        /* Enough passes within the attempt for the session timeout to expire. */
        uint8 pass;

        for (pass = 0u; pass < 4u; pass++)
        {
            ComM_MainFunction();
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_SESSION_TIMEOUT_MS);
        }

        /* Then expire whatever backoff was entered. */
        ComM_MainFunction();
        NetIf_MainFunction();
        Stub_Gpt_AdvanceMs(NETIF_RECONNECT_MAX_MS);
    }
}

/** Bring a session up over WiFi and assert it reached SESSION_UP. */
static void TnBringSessionUpOverWifi(void)
{
    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));

    TnRunNetIf(8u);
    TEST_ASSERT_TRUE(NetIf_IsSessionUp());
}

/*==================================================================================================
 *  TS-NET-001 .. 006  Bringing a session up, over either bearer
 *================================================================================================*/

/** TS-NET-001: a session comes up over WiFi and reports the bearer carrying it. */
static void test_Net_SessionUpOverWifi(void)
{
    NetIf_StatusType status;

    TnBringSessionUpOverWifi();

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_WIFI, status.activeBearer);
    TEST_ASSERT_EQUAL(NETIF_STATE_SESSION_UP, status.state);
    TEST_ASSERT_EQUAL_UINT32(1u, status.sessionCount);
}

/**
 * TS-NET-002: SWREQ-COM-0040 -- the same state machine brings a session up over GSM.
 *
 * The point of the whole design. Nothing about the sequence differs: the same states, the same
 * transitions, the same counters. If the sharing were not real, one of the two would have its own path
 * here and the two would drift exactly as v1's did.
 */
static void test_Net_SessionUpOverGsm(void)
{
    NetIf_StatusType status;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_GSM));

    TnRunNetIf(8u);

    TEST_ASSERT_TRUE(NetIf_IsSessionUp());
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.activeBearer);
    TEST_ASSERT_EQUAL(NETIF_STATE_SESSION_UP, status.state);
}

/**
 * TS-NET-003: SWREQ-COM-0050 -- a link without an address is not a session.
 *
 * With DHCP there is a window of tens to hundreds of milliseconds between associating and holding an
 * address, and a TCP connect attempted inside it fails in a way that looks like a broker fault. v1
 * checked only the link status and retried the broker for that whole window on every connection.
 */
static void test_Net_LinkWithoutBrokerIsNotSessionUp(void)
{
    NetIf_StatusType status;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(FALSE); /* link up, broker unreachable */

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));
    TnRunNetIf(6u);

    TEST_ASSERT_FALSE(NetIf_IsSessionUp());

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_NOT_EQUAL(NETIF_STATE_SESSION_UP, status.state);
}

/** TS-NET-004: with no bearer at all the state machine stays down and publishes nothing. */
static void test_Net_NoBearerStaysDown(void)
{
    NetIf_StatusType status;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));
    TnRunNetIf(10u);

    TEST_ASSERT_FALSE(NetIf_IsSessionUp());
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Publish("odo/test/record", (const uint8 *)"x", 1u, FALSE));

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(0u, status.publishCount);
}

/** TS-NET-005: a publish over an established session reaches the broker and is counted. */
static void test_Net_PublishOverSession(void)
{
    NetIf_StatusType status;

    TnBringSessionUpOverWifi();

    TEST_ASSERT_EQUAL(E_OK, NetIf_Publish("odo/test/record", (const uint8 *)"payload", 7u, FALSE));

    TEST_ASSERT_EQUAL_UINT32(1u, Stub_Net_GetPublishCount());
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.publishCount);
}

/** TS-NET-006: a rejected publish is reported and counted, not silently dropped. */
static void test_Net_FailedPublishIsReported(void)
{
    NetIf_StatusType status;

    TnBringSessionUpOverWifi();

    Stub_Net_FailNextPublishes(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Publish("odo/test/record", (const uint8 *)"payload", 7u, FALSE));

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.publishFailures);
    TEST_ASSERT_EQUAL_UINT32(0u, status.publishCount);
}

/*==================================================================================================
 *  TS-NET-007 .. 011  Loss, backoff and recovery
 *================================================================================================*/

/**
 * TS-NET-007: a dropped broker session is noticed, and is not reported as a bearer failure.
 *
 * The distinction matters for diagnosis. `linkDownCount` counts the *bearer* going down; a broker
 * restart leaves the bearer perfectly healthy, and counting it as a link failure would make a server-
 * side event look like a radio problem -- which is where the investigation would then go.
 */
static void test_Net_DroppedSessionIsNoticed(void)
{
    NetIf_StatusType before;
    NetIf_StatusType after;

    TnBringSessionUpOverWifi();
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&before));

    Stub_Net_DropSession();
    TnRunNetIf(3u);

    TEST_ASSERT_FALSE(NetIf_IsSessionUp());

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&after));
    TEST_ASSERT_NOT_EQUAL(NETIF_STATE_SESSION_UP, after.state);

    /* The bearer never went down, so its counter must not have moved. */
    TEST_ASSERT_EQUAL_UINT32(before.linkDownCount, after.linkDownCount);
}

/**
 * TS-NET-007b: a bearer going down *is* counted as a link failure.
 *
 * The other half of the distinction above. Without both, a single counter could be right for one case
 * and wrong for the other and no test would show it.
 */
static void test_Net_BearerLossIsCountedAsLinkDown(void)
{
    NetIf_StatusType status;

    TnBringSessionUpOverWifi();

    Stub_Net_SetWifiAvailable(FALSE);
    TnRunNetIf(3u);

    TEST_ASSERT_FALSE(NetIf_IsSessionUp());
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.linkDownCount);
}

/**
 * TS-NET-008: SWREQ-COM-0045 -- retries back off, starting at the configured minimum.
 *
 * A unit parked out of coverage must not retry at full rate: on GPRS that is a measurable data charge
 * for no benefit, and it keeps the modem at full power draw.
 */
static void test_Net_BackoffStartsAtMinimum(void)
{
    NetIf_StatusType status;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));

    /* Backoff is entered only once NETIF_SESSION_TIMEOUT_MS of handshake has gone unanswered, so the
     * time advanced has to exceed that -- not merely a few cyclic passes. */
    {
        uint16 i;

        for (i = 0u; i < 4u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_SESSION_TIMEOUT_MS);
        }
    }

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_TRUE(status.currentBackoffMs >= (uint32)NETIF_RECONNECT_MIN_MS);
}

/**
 * TS-NET-009: the backoff grows on repeated failure and is capped.
 *
 * Both halves matter. Growth is what keeps a long outage cheap; the cap is what bounds how long the
 * unit takes to notice coverage has returned. An uncapped exponential would eventually be measured in
 * hours.
 */
static void test_Net_BackoffGrowsAndIsCapped(void)
{
    NetIf_StatusType early;
    NetIf_StatusType late;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));

    {
        uint16 i;

        for (i = 0u; i < 4u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_SESSION_TIMEOUT_MS);
        }
    }
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&early));

    /* A long run of failures. Virtual time makes this instant. */
    {
        uint16 i;

        for (i = 0u; i < 200u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_RECONNECT_MAX_MS);
        }
    }

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&late));

    TEST_ASSERT_TRUE(late.currentBackoffMs > early.currentBackoffMs);
    TEST_ASSERT_TRUE(late.currentBackoffMs <= (uint32)NETIF_RECONNECT_MAX_MS);
}

/**
 * TS-NET-010: the backoff resets once a session is established.
 *
 * Otherwise a unit that had a bad morning would carry a five-minute backoff for the rest of the day,
 * and a single later drop would cost five minutes of records instead of two seconds.
 */
static void test_Net_BackoffResetsOnSuccess(void)
{
    NetIf_StatusType backingOff;
    NetIf_StatusType recovered;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, NetIf_RequestBearer(NETIF_BEARER_WIFI));

    {
        uint16 i;

        for (i = 0u; i < 40u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_SESSION_TIMEOUT_MS);
        }
    }
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&backingOff));
    TEST_ASSERT_TRUE(backingOff.currentBackoffMs > (uint32)NETIF_RECONNECT_MIN_MS);

    /* The broker comes back. Enough time for the current backoff to expire and a session to form. */
    Stub_Net_SetBrokerAvailable(TRUE);
    {
        uint16 i;

        for (i = 0u; i < 10u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_RECONNECT_MAX_MS);
        }
    }

    TEST_ASSERT_TRUE(NetIf_IsSessionUp());
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&recovered));
    TEST_ASSERT_EQUAL_UINT32(0u, recovered.currentBackoffMs);
}

/** TS-NET-011: a session recovers after a drop without needing a re-request. */
static void test_Net_SessionRecoversAfterDrop(void)
{
    NetIf_StatusType status;

    TnBringSessionUpOverWifi();

    Stub_Net_DropSession();
    TnRunNetIf(3u);
    TEST_ASSERT_FALSE(NetIf_IsSessionUp());

    /* No new NetIf_RequestBearer: the state machine owns recovery, which is what stops every caller
     * having to implement its own retry -- the duplication that let v1's two paths drift. */
    {
        uint16 i;

        for (i = 0u; i < 10u; i++)
        {
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_RECONNECT_MAX_MS);
        }
    }

    TEST_ASSERT_TRUE(NetIf_IsSessionUp());
    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));
    TEST_ASSERT_TRUE(status.sessionCount >= 2u);
}

/*==================================================================================================
 *  TS-NET-012 .. 014  Signal strength, subscription, contract
 *================================================================================================*/

/**
 * TS-NET-012: SWREQ-COM-0060 -- signal strength is reported in dBm for both bearers.
 *
 * `AT+CSQ` returns 0..31, not dBm. v1 published the raw value, so a strong GSM signal of 31 read as a
 * nonsensical +31 dBm and could not be compared against the WiFi figure beside it in the same record.
 */
static void test_Net_SignalStrengthIsDbm(void)
{
    NetIf_StatusType status;

    Stub_Net_SetRssi(-67, -95);
    TnBringSessionUpOverWifi();

    TEST_ASSERT_EQUAL(E_OK, NetIf_GetStatus(&status));

    /* Negative and in a plausible range. A positive value would mean raw CSQ leaked through. */
    TEST_ASSERT_TRUE(status.signalStrengthDbm < 0);
    TEST_ASSERT_TRUE(status.signalStrengthDbm > -120);
}

/** Captures the last message a subscription delivered. */
static char TnLastTopic[64];
static uint16 TnLastLength;
static uint32 TnDeliveryCount;

static void TnMessageHandler(const char *topic, const uint8 *payload, uint16 payloadLen)
{
    if (topic != NULL_PTR)
    {
        (void)strncpy(TnLastTopic, topic, sizeof(TnLastTopic) - 1u);
        TnLastTopic[sizeof(TnLastTopic) - 1u] = '\0';
    }
    COMPILER_UNUSED(payload);
    TnLastLength = payloadLen;
    TnDeliveryCount++;
}

/**
 * TS-NET-013: a subscription is registered and an inbound message reaches its handler.
 *
 * A NULL handler is refused, which is also asserted: a subscription with nowhere to deliver is a
 * message silently discarded, and the diagnostic channel is exactly where that must not happen.
 */
static void test_Net_SubscriptionDeliversMessage(void)
{
    TnDeliveryCount = 0u;
    TnLastTopic[0] = '\0';
    TnLastLength = 0u;

    TnBringSessionUpOverWifi();

    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Subscribe("odo/test/cmd", NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Subscribe(NULL_PTR, &TnMessageHandler));

    TEST_ASSERT_EQUAL(E_OK, NetIf_Subscribe("odo/test/cmd", &TnMessageHandler));
    TEST_ASSERT_TRUE(Stub_Net_GetSubscribeCount() > 0u);

    Stub_Net_DeliverMessage("odo/test/cmd", (const uint8 *)"1,READ_DTC", 10u);
    NetIf_MainFunction();

    TEST_ASSERT_EQUAL_UINT32(1u, TnDeliveryCount);
    TEST_ASSERT_EQUAL_STRING("odo/test/cmd", TnLastTopic);
    TEST_ASSERT_EQUAL_UINT16(10u, TnLastLength);
}

/** TS-NET-014: bad arguments are rejected, including an over-long payload. */
static void test_Net_RejectsBadArguments(void)
{
    static uint8 huge[NETIF_MAX_PAYLOAD_SIZE + 64u];

    TnBringSessionUpOverWifi();

    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Publish(NULL_PTR, (const uint8 *)"x", 1u, FALSE));
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Publish("odo/test/record", NULL_PTR, 1u, FALSE));
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_GetStatus(NULL_PTR));

    /* An over-long payload is refused rather than truncated: a truncated record would arrive at the
     * consumer as a short line it would have to guess about. */
    (void)memset(huge, (int)'x', sizeof(huge));
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_Publish("odo/test/record", huge, (uint16)sizeof(huge), FALSE));
}

/*==================================================================================================
 *  TS-COMM-001 .. 008  Bearer arbitration
 *================================================================================================*/

/** TS-COMM-001: with no mode requested, neither radio is wanted. */
static void test_ComM_NoCommunicationWantsNoBearer(void)
{
    ComM_StatusType status;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetGsmAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_NO_COMMUNICATION));

    TnRunComM(5u);

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_NONE, status.preferredBearer);
    TEST_ASSERT_FALSE(ComM_IsCommunicationAvailable());
}

/**
 * TS-COMM-002: SWREQ-COM-0070 -- WiFi is preferred when both are available.
 *
 * Because it costs nothing per byte. The preference is a configuration switch, so this asserts the
 * configured behaviour rather than a hard-coded one.
 */
static void test_ComM_PrefersWifiWhenBothAvailable(void)
{
    ComM_StatusType status;

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));

    TnRunComM(10u);

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
#if (COMM_PREFER_WIFI == STD_ON)
    TEST_ASSERT_EQUAL(NETIF_BEARER_WIFI, status.preferredBearer);
#else
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.preferredBearer);
#endif
}

/** TS-COMM-003: with only GSM available, GSM is chosen. */
static void test_ComM_FallsBackToGsmWhenWifiAbsent(void)
{
    ComM_StatusType status;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));

    /* WiFi is preferred, so ComM tries it first and only falls back once COMM_FAILURE_LIMIT attempts
     * have failed. Each attempt costs a session timeout plus a backoff, so this needs attempts rather
     * than a pass count. */
    TnRunAttempts((uint16)(COMM_FAILURE_LIMIT + 2u));

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.preferredBearer);
}

/**
 * TS-COMM-004: repeated failures on the preferred bearer drive a switch to the other.
 *
 * The failure limit exists so a unit inside a depot whose WiFi is present but broken does not sit there
 * failing indefinitely while a working GPRS bearer goes unused.
 */
static void test_ComM_SwitchesAfterRepeatedFailures(void)
{
    ComM_StatusType status;

    /* WiFi associates but the broker is unreachable over it; GSM works. */
    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));

    /* Long enough for the failure limit to be reached several times over. */
    {
        uint16 i;

        for (i = 0u; i < 300u; i++)
        {
            ComM_MainFunction();
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(NETIF_RECONNECT_MAX_MS);
        }
    }

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_TRUE(status.bearerSwitchCount > 0u);
}

/**
 * TS-COMM-005: SWREQ-COM-0075 -- the fallback is not abandoned before the retry interval elapses.
 *
 * WiFi becoming available is not by itself a reason to switch: a marginal link at the edge of a depot
 * would otherwise flap the session continuously, and each flap costs a full broker handshake over the
 * bearer that was working. The retry interval is what bounds that -- a switch cannot happen more often
 * than once per COMM_PREFERRED_RETRY_MS however the link behaves, which is why a separate hysteresis
 * window is not needed on top of it.
 */
static void test_ComM_DoesNotReturnBeforeRetryInterval(void)
{
    ComM_StatusType status;

    /* Start on GSM, with WiFi absent. */
    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));
    TnRunAttempts((uint16)(COMM_FAILURE_LIMIT + 2u));

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.preferredBearer);

    /* WiFi comes back, but well inside the retry interval. */
    Stub_Net_SetWifiAvailable(TRUE);
    TnRunComM((uint16)((COMM_PREFERRED_RETRY_MS / COMM_ARBITRATION_INTERVAL_MS) / 4u));

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.preferredBearer);
}

/**
 * TS-COMM-006: the preferred bearer is retried once the interval elapses, and won back if it works.
 *
 * The case whose absence made the fallback permanent. Before the decay existed, `wifiFailures` stayed at
 * its limit for the life of the run -- cleared only by Init, by both bearers being exhausted, or by WiFi
 * carrying traffic, which could not happen while the return was blocked. A vehicle that failed WiFi
 * leaving its depot paid for cellular beside a healthy access point all day, and no test noticed because
 * none of them ran for ten minutes of virtual time.
 */
static void test_ComM_ReturnsToWifiAfterRetryInterval(void)
{
    ComM_StatusType status;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));
    TnRunAttempts((uint16)(COMM_FAILURE_LIMIT + 2u));

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_EQUAL(NETIF_BEARER_GSM, status.preferredBearer);

    /* WiFi returns and stays. Advance past the retry interval, then let the attempt complete. */
    Stub_Net_SetWifiAvailable(TRUE);
    {
        uint16 i;

        for (i = 0u; i < 20u; i++)
        {
            ComM_MainFunction();
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(COMM_PREFERRED_RETRY_MS);
        }
    }

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
#if (COMM_PREFER_WIFI == STD_ON)
    TEST_ASSERT_EQUAL(NETIF_BEARER_WIFI, status.preferredBearer);
#endif
}

/**
 * TS-COMM-006b: with the preferred bearer permanently absent, switching stays bounded.
 *
 * The other side of the retry. A unit genuinely out of WiFi range alternates -- retry, attempt, fail,
 * fall back -- and that is the intended behaviour: it is how the unit notices coverage returning. What
 * matters is the *rate*. Each cycle costs one attempt window per COMM_PREFERRED_RETRY_MS, so over ten
 * intervals the switch count grows by a couple of dozen rather than once per cyclic pass.
 *
 * Which bearer it happens to be on at the end is deliberately not asserted: that is a snapshot of an
 * alternating cycle, and pinning it would make the test depend on how the loop's arithmetic happened to
 * land rather than on the property being claimed.
 */
static void test_ComM_RetryFallsBackWhenStillAbsent(void)
{
    ComM_StatusType first;
    ComM_StatusType second;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));
    TnRunAttempts((uint16)(COMM_FAILURE_LIMIT + 2u));
    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&first));

    /* WiFi never comes back. Several retry intervals pass. */
    {
        uint16 i;

        for (i = 0u; i < 10u; i++)
        {
            ComM_MainFunction();
            NetIf_MainFunction();
            Stub_Gpt_AdvanceMs(COMM_PREFERRED_RETRY_MS);
        }
    }
    TnRunAttempts((uint16)(COMM_FAILURE_LIMIT + 2u));

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&second));

    /* Traffic is still flowing over something, and the switch count grew by a bounded amount rather
     * than once per cyclic pass -- which is what "does not thrash" means in observable terms. */
    TEST_ASSERT_TRUE(second.bearerSwitchCount > first.bearerSwitchCount);
    TEST_ASSERT_TRUE((second.bearerSwitchCount - first.bearerSwitchCount) < 40u);
    TEST_ASSERT_TRUE(second.anyBearerEverUp);
}

/**
 * TS-COMM-007: SWREQ-COM-0082 -- time without any bearer is tracked.
 *
 * It is the figure that distinguishes "this unit is out of coverage" from "this unit's radio is broken",
 * and it is the first thing to look at when records arrive in bursts.
 */
static void test_ComM_TracksTimeWithoutBearer(void)
{
    ComM_StatusType status;

    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_RequestMode(COMM_FULL_COMMUNICATION));

    TnRunComM(30u);

    TEST_ASSERT_EQUAL(E_OK, ComM_GetStatus(&status));
    TEST_ASSERT_TRUE(status.timeWithoutBearerMs > 0uL);
    TEST_ASSERT_FALSE(status.anyBearerEverUp);
}

/** TS-COMM-008: bad arguments are rejected. */
static void test_ComM_RejectsBadArguments(void)
{
    TEST_ASSERT_EQUAL(E_OK, NetIf_Init());
    TEST_ASSERT_EQUAL(E_OK, ComM_Init());

    TEST_ASSERT_NOT_EQUAL(E_OK, ComM_RequestMode((ComM_ModeType)99));
    TEST_ASSERT_NOT_EQUAL(E_OK, ComM_GetStatus(NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, NetIf_RequestBearer((NetIf_BearerType)99));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Net_SessionUpOverWifi);
    RUN_TEST(test_Net_SessionUpOverGsm);
    RUN_TEST(test_Net_LinkWithoutBrokerIsNotSessionUp);
    RUN_TEST(test_Net_NoBearerStaysDown);
    RUN_TEST(test_Net_PublishOverSession);
    RUN_TEST(test_Net_FailedPublishIsReported);

    RUN_TEST(test_Net_DroppedSessionIsNoticed);
    RUN_TEST(test_Net_BearerLossIsCountedAsLinkDown);
    RUN_TEST(test_Net_BackoffStartsAtMinimum);
    RUN_TEST(test_Net_BackoffGrowsAndIsCapped);
    RUN_TEST(test_Net_BackoffResetsOnSuccess);
    RUN_TEST(test_Net_SessionRecoversAfterDrop);

    RUN_TEST(test_Net_SignalStrengthIsDbm);
    RUN_TEST(test_Net_SubscriptionDeliversMessage);
    RUN_TEST(test_Net_RejectsBadArguments);

    RUN_TEST(test_ComM_NoCommunicationWantsNoBearer);
    RUN_TEST(test_ComM_PrefersWifiWhenBothAvailable);
    RUN_TEST(test_ComM_FallsBackToGsmWhenWifiAbsent);
    RUN_TEST(test_ComM_SwitchesAfterRepeatedFailures);
    RUN_TEST(test_ComM_DoesNotReturnBeforeRetryInterval);
    RUN_TEST(test_ComM_ReturnsToWifiAfterRetryInterval);
    RUN_TEST(test_ComM_RetryFallsBackWhenStillAbsent);
    RUN_TEST(test_ComM_TracksTimeWithoutBearer);
    RUN_TEST(test_ComM_RejectsBadArguments);

    return UNITY_END();
}

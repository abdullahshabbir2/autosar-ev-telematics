/**
 * @file    ComM.c
 * @brief   Bearer arbitration implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "ComM.h"

#include <string.h>

#include "Dem.h"
#include "Det.h"
#include "Gpt.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean ComM_Initialised = FALSE;
STATIC ComM_StatusType ComM_Status;

/** When the current bearer attempt began, for failure detection. */
STATIC Gpt_TimestampType ComM_AttemptStartedMs;

/** When WiFi was first seen continuously available, for the switch-back hysteresis. */
STATIC Gpt_TimestampType ComM_WifiStableSinceMs;
STATIC boolean ComM_WifiStableTimerRunning;

/** When the ECU last had any bearer at all, for the no-backhaul event. */
STATIC Gpt_TimestampType ComM_LastBearerUpMs;

/*==================================================================================================
 *  Arbitration
 *================================================================================================*/

/** The bearer that should be tried, given the failure counts and the preference. */
STATIC NetIf_BearerType ComM_ChooseBearer(void)
{
#if (COMM_PREFER_WIFI == STD_ON)
    const NetIf_BearerType first = NETIF_BEARER_WIFI;
    const NetIf_BearerType second = NETIF_BEARER_GSM;
    const uint16 firstFailures = ComM_Status.wifiFailures;
    const uint16 secondFailures = ComM_Status.gsmFailures;
#else
    const NetIf_BearerType first = NETIF_BEARER_GSM;
    const NetIf_BearerType second = NETIF_BEARER_WIFI;
    const uint16 firstFailures = ComM_Status.gsmFailures;
    const uint16 secondFailures = ComM_Status.wifiFailures;
#endif

    if (firstFailures < (uint16)COMM_FAILURE_LIMIT)
    {
        return first;
    }
    if (secondFailures < (uint16)COMM_FAILURE_LIMIT)
    {
        return second;
    }

    /* Both have exhausted their allowance. The counters are cleared and the preferred bearer is tried
     * again rather than giving up permanently: the vehicle moves, and a bearer that was unavailable in
     * one place is often available in the next. Giving up for good would mean a unit that lost coverage
     * once never transmitted again until it was power-cycled. */
    ComM_Status.wifiFailures = 0u;
    ComM_Status.gsmFailures = 0u;
    return first;
}

/** Record a failed attempt against @p bearer. */
STATIC void ComM_RecordFailure(NetIf_BearerType bearer)
{
    if (bearer == NETIF_BEARER_WIFI)
    {
        if (ComM_Status.wifiFailures < 0xFFFFu)
        {
            ComM_Status.wifiFailures++;
        }
    }
    else if (bearer == NETIF_BEARER_GSM)
    {
        if (ComM_Status.gsmFailures < 0xFFFFu)
        {
            ComM_Status.gsmFailures++;
        }
    }
    else
    {
        /* NETIF_BEARER_NONE: nothing to record. */
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType ComM_Init(void)
{
    (void)memset(&ComM_Status, 0, sizeof(ComM_Status));
    ComM_Status.requestedMode = COMM_NO_COMMUNICATION;
    ComM_Status.preferredBearer = NETIF_BEARER_NONE;
    ComM_Status.activeBearer = NETIF_BEARER_NONE;
    ComM_AttemptStartedMs = Gpt_GetMonotonicMs();
    ComM_LastBearerUpMs = Gpt_GetMonotonicMs();
    ComM_WifiStableSinceMs = 0u;
    ComM_WifiStableTimerRunning = FALSE;
    ComM_Initialised = TRUE;

    return E_OK;
}

Std_ReturnType ComM_RequestMode(ComM_ModeType mode)
{
    DET_CHECK_RETURN(ComM_Initialised != FALSE, MODULE_ID_COMM, INSTANCE_ID_SINGLE,
                     COMM_API_ID_REQUEST_MODE, COMM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(mode <= COMM_FULL_COMMUNICATION, MODULE_ID_COMM, INSTANCE_ID_SINGLE,
                     COMM_API_ID_REQUEST_MODE, COMM_E_PARAM_MODE, E_NOT_OK);

    if (ComM_Status.requestedMode == mode)
    {
        return E_OK;
    }

    ComM_Status.requestedMode = mode;

    if (mode == COMM_NO_COMMUNICATION)
    {
        ComM_Status.preferredBearer = NETIF_BEARER_NONE;
        return NetIf_ReleaseBearer();
    }

    /* A fresh request clears the failure history: the application asking again is a reason to try the
     * preferred bearer, not to continue from wherever the last attempt gave up. */
    ComM_Status.wifiFailures = 0u;
    ComM_Status.gsmFailures = 0u;
    ComM_Status.preferredBearer = ComM_ChooseBearer();
    ComM_AttemptStartedMs = Gpt_GetMonotonicMs();

    return NetIf_RequestBearer(ComM_Status.preferredBearer);
}

NetIf_BearerType ComM_GetPreferredBearer(void)
{
    return ComM_Status.preferredBearer;
}

boolean ComM_IsCommunicationAvailable(void)
{
    return NetIf_IsSessionUp();
}

void ComM_MainFunction(void)
{
    NetIf_StatusType netStatus;

    if ((ComM_Initialised == FALSE) || (ComM_Status.requestedMode != COMM_FULL_COMMUNICATION))
    {
        return;
    }

    if (NetIf_GetStatus(&netStatus) != E_OK)
    {
        return;
    }

    ComM_Status.activeBearer = netStatus.activeBearer;

    if (netStatus.state == NETIF_STATE_SESSION_UP)
    {
        /* Working. Clear the failure count for the bearer that is carrying traffic, so a later outage
         * starts from a clean allowance rather than from whatever an earlier one left behind. */
        if (netStatus.activeBearer == NETIF_BEARER_WIFI)
        {
            ComM_Status.wifiFailures = 0u;
        }
        else if (netStatus.activeBearer == NETIF_BEARER_GSM)
        {
            ComM_Status.gsmFailures = 0u;
        }
        else
        {
            /* No bearer named; nothing to clear. */
        }

        ComM_Status.anyBearerEverUp = TRUE;
        ComM_LastBearerUpMs = Gpt_GetMonotonicMs();
        ComM_Status.timeWithoutBearerMs = 0u;
        ComM_AttemptStartedMs = Gpt_GetMonotonicMs();

        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_NO_BACKHAUL, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_PASSED));

#if (COMM_PREFER_WIFI == STD_ON)
        /* Running on GPRS while WiFi has become available: switch back, but only after WiFi has been
         * continuously available for the hysteresis interval. Switching on the first sight of an access
         * point is what makes a vehicle at the edge of range oscillate. */
        if (ComM_Status.preferredBearer == NETIF_BEARER_GSM)
        {
            if (ComM_Status.wifiFailures < (uint16)COMM_FAILURE_LIMIT)
            {
                if (ComM_WifiStableTimerRunning == FALSE)
                {
                    ComM_WifiStableSinceMs = Gpt_GetMonotonicMs();
                    ComM_WifiStableTimerRunning = TRUE;
                }
                else if (Gpt_HasElapsed(ComM_WifiStableSinceMs, COMM_WIFI_STABLE_MS) != FALSE)
                {
                    ComM_Status.preferredBearer = NETIF_BEARER_WIFI;
                    ComM_Status.bearerSwitchCount++;
                    ComM_WifiStableTimerRunning = FALSE;
                    ComM_AttemptStartedMs = Gpt_GetMonotonicMs();
                    STD_DISCARD(NetIf_RequestBearer(NETIF_BEARER_WIFI));
                }
                else
                {
                    /* Still waiting out the hysteresis. */
                }
            }
            else
            {
                ComM_WifiStableTimerRunning = FALSE;
            }
        }
#endif
        return;
    }

    /* Not up. Give the current attempt its full allowance -- one link timeout plus one session timeout
     * plus a margin -- before counting it as a failure, so a slow but successful attach is not counted
     * against the bearer. */
    ComM_Status.timeWithoutBearerMs = Gpt_ElapsedSince(ComM_LastBearerUpMs);

    if (Gpt_HasElapsed(ComM_AttemptStartedMs,
                       NETIF_LINK_TIMEOUT_MS + NETIF_SESSION_TIMEOUT_MS + 5000uL) != FALSE)
    {
        const NetIf_BearerType failed = ComM_Status.preferredBearer;
        NetIf_BearerType next;

        ComM_RecordFailure(failed);
        next = ComM_ChooseBearer();
        ComM_AttemptStartedMs = Gpt_GetMonotonicMs();

        if (next != failed)
        {
            ComM_Status.preferredBearer = next;
            ComM_Status.bearerSwitchCount++;
            ComM_WifiStableTimerRunning = FALSE;
            STD_DISCARD(NetIf_RequestBearer(next));
        }
        /* If the choice is unchanged, NetIf's own backoff is already retrying it; requesting the same
         * bearer again would reset that backoff and defeat it. */
    }

    if (Gpt_HasElapsed(ComM_LastBearerUpMs, COMM_NO_BEARER_REPORT_MS) != FALSE)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_NO_BACKHAUL, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
    }
}

Std_ReturnType ComM_GetStatus(ComM_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_COMM, INSTANCE_ID_SINGLE,
                     COMM_API_ID_GET_STATE, COMM_E_PARAM_POINTER, E_NOT_OK);

    *status = ComM_Status;
    return E_OK;
}

void ComM_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = COMM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_COMM;
        versioninfo->sw_major_version = COMM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = COMM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = COMM_SW_PATCH_VERSION;
    }
}

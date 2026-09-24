/**
 * @file    ComM.h
 * @brief   AUTOSAR Communication Manager (SWS_COMM) -- bearer arbitration.
 *
 * Decides which backhaul the ECU should be using, and when to give up on one and try the other. NetIf
 * knows how to bring a bearer up; ComM knows which one is worth bringing up.
 *
 * @par Why WiFi is preferred
 * WiFi is free and fast; GPRS is metered and slow. A vehicle that returns to a depot with a known
 * access point should drain its backlog over WiFi, and should not spend money doing over GPRS what it
 * could do for nothing an hour later. So WiFi is tried first, GPRS is the fallback, and ComM returns to
 * WiFi whenever it becomes available again.
 *
 * @par Hysteresis, and why it matters more than it sounds
 * Switching bearer is expensive: the modem takes tens of seconds to attach, and each switch drops the
 * broker session. Without hysteresis a vehicle parked at the edge of WiFi range would oscillate
 * between bearers indefinitely, never holding a session long enough to transfer anything. ComM
 * therefore requires ::COMM_WIFI_STABLE_MS of continuous WiFi availability before switching back to it,
 * and ::COMM_FAILURE_LIMIT consecutive failures before abandoning a bearer.
 *
 * @par What v1 did
 * v1 had no arbitration. WiFi and GPRS were separate code paths, each with its own retry counter, and
 * the publish path chose between them with a chain of `if (flags[wf_f]) ... else if (flags[gsm_f])`
 * duplicated in two functions. Whether GPRS was even attempted depended on a compile-time
 * `#ifdef GSM_ENABLE`, and with that defined the modem's `Serial2.begin(9600)` silently reconfigured
 * the port the battery bus was using at 4800 -- so enabling cellular backhaul disabled battery
 * monitoring, with nothing reporting either fact.
 *
 * @req SWREQ-COM-0070 .. SWREQ-COM-0082
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COMM_H
#define COMM_H

#include "Autosar_ModuleIds.h"
#include "ComM_Cfg.h"
#include "NetIf.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMM_VENDOR_ID 0xFFFEu
#define COMM_AR_RELEASE_MAJOR_VERSION 4u
#define COMM_AR_RELEASE_MINOR_VERSION 4u
#define COMM_SW_MAJOR_VERSION 2u
#define COMM_SW_MINOR_VERSION 0u
#define COMM_SW_PATCH_VERSION 0u

#define COMM_API_ID_INIT 0x01u
#define COMM_API_ID_REQUEST_MODE 0x03u
#define COMM_API_ID_GET_STATE 0x04u
#define COMM_API_ID_MAIN_FUNCTION 0x60u

#define COMM_E_UNINIT E_UNINIT
#define COMM_E_PARAM_POINTER E_PARAM_POINTER
#define COMM_E_PARAM_MODE 0x20u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Communication mode requested by the application (SWS_ComM_00669). */
typedef enum
{
    COMM_NO_COMMUNICATION = 0,   /**< Nothing needs a bearer; both radios off.   */
    COMM_SILENT_COMMUNICATION = 1,/**< Receive only; not used on this ECU.       */
    COMM_FULL_COMMUNICATION = 2   /**< A bearer is wanted.                       */
} ComM_ModeType;

/** Arbitration state, published in the telemetry health record. */
typedef struct
{
    ComM_ModeType requestedMode;      /**< Mode the application asked for.            */
    NetIf_BearerType preferredBearer; /**< Bearer ComM currently wants.               */
    NetIf_BearerType activeBearer;    /**< Bearer NetIf reports as carrying traffic.   */
    uint16 wifiFailures;              /**< Consecutive WiFi attempts that failed.      */
    uint16 gsmFailures;               /**< Consecutive GPRS attempts that failed.      */
    uint32 bearerSwitchCount;         /**< Times the preferred bearer changed.         */
    uint32 timeWithoutBearerMs;       /**< How long neither bearer has been up.        */
    boolean anyBearerEverUp;          /**< TRUE once a bearer has come up at least once.*/
} ComM_StatusType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/** Initialise the manager. Requests no communication until the application asks. */
CHECK_RETURN Std_ReturnType ComM_Init(void);

/**
 * @brief Request a communication mode.
 *
 * ::COMM_FULL_COMMUNICATION asks for whichever bearer ComM judges best;
 * ::COMM_NO_COMMUNICATION releases both. Called by BswM's equivalent logic in EcuM at startup and by
 * TelemSwc when it has nothing to send for a long period.
 */
CHECK_RETURN Std_ReturnType ComM_RequestMode(ComM_ModeType mode);

/** The bearer ComM currently prefers. */
NetIf_BearerType ComM_GetPreferredBearer(void);

/** TRUE if a broker session is up and telemetry can be published. */
boolean ComM_IsCommunicationAvailable(void);

/**
 * @brief Drive arbitration: choose a bearer, notice failures, apply hysteresis.
 *
 * Driven cyclically by SchM from the connectivity task, immediately before ::NetIf_MainFunction so that
 * a decision takes effect in the same cycle it is made.
 */
void ComM_MainFunction(void);

/**
 * @brief Read the arbitration state.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType ComM_GetStatus(ComM_StatusType *status);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void ComM_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* COMM_H */

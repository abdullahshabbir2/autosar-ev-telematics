/**
 * @file    TelemSwc.h
 * @brief   Telemetry component -- record assembly, storage and delivery.
 *
 * Gathers one record per acquisition cycle from every other component, writes it to the card, and
 * delivers records to the broker. Also serves backfill requests for historical days.
 *
 * @par Store first, then send
 * The record is written to the SD card **before** any attempt to publish it, and it is only removed from
 * the transfer queue once the broker has accepted it. That ordering is the whole reliability argument: a
 * record that reaches the card survives a power cut, a coverage gap and a broker outage, and the
 * transfer cursor guarantees each record is delivered at least once.
 *
 * v1 had this inverted. `vAcquireData` set `toTransfer` only when WiFi and the broker were both already
 * up, so a record acquired during a coverage gap was never queued for transfer at all -- it existed only
 * on the card, and the card was read by a separate path that ran only when `unsent_bytes` exceeded a
 * threshold. Records acquired while offline were therefore delivered late or, if the threshold was never
 * crossed, not at all.
 *
 * @par Live and backlog share the link, with live taking precedence
 * A vehicle coming back into coverage has both a fresh record to send and a backlog to drain. Live
 * records go first, because a stale live record is worthless while a stale backlog record is merely
 * late; the backlog is drained at ::TELEM_BACKLOG_RECORDS_PER_CYCLE per cycle so that draining it cannot
 * starve the live path or hold the task long enough to trip its deadline.
 *
 * @req SWREQ-TEL-0030 .. SWREQ-TEL-0060
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef TELEMSWC_H
#define TELEMSWC_H

#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"
#include "app/TelemSwc/TelemSwc_Cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELEMSWC_VENDOR_ID 0xFFFEu
#define TELEMSWC_SW_MAJOR_VERSION 2u
#define TELEMSWC_SW_MINOR_VERSION 0u
#define TELEMSWC_SW_PATCH_VERSION 0u

#define TELEMSWC_API_ID_INIT 0x00u
#define TELEMSWC_API_ID_ACQUIRE 0x20u
#define TELEMSWC_API_ID_MAIN_FUNCTION 0x0Eu
#define TELEMSWC_API_ID_GET_STATUS 0x21u

#define TELEMSWC_E_UNINIT E_UNINIT
#define TELEMSWC_E_PARAM_POINTER E_PARAM_POINTER
#define TELEMSWC_E_SERIALISE_FAILED 0x20u
#define TELEMSWC_E_NO_CLOCK 0x21u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Telemetry counters, published in the health record and readable over diagnostics. */
typedef struct
{
    uint32 recordsAcquired;         /**< Records assembled.                             */
    uint32 recordsStored;           /**< Records written to the card.                   */
    uint32 recordsPublishedLive;    /**< Records published as they were acquired.       */
    uint32 recordsPublishedBacklog; /**< Records published from the card's backlog.   */
    uint32 storeFailures;           /**< Records the card would not accept.             */
    uint32 publishFailures;         /**< Publishes the broker or link rejected.         */
    uint32 serialiseFailures;       /**< Records that would not fit their buffer.       */
    uint32 backfillRequests;        /**< Backfill requests served.                      */
    uint32 backfillChunksSent;      /**< Chunks transmitted for backfill.               */
    uint32 droppedNoClock;          /**< Records not stored because the clock was invalid.*/
    uint32 sequenceNumber;          /**< Sequence number of the next record.            */
} TelemSwc_StatusType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Initialise the component and subscribe to the request topics.
 * @return E_OK on success.
 */
CHECK_RETURN Std_ReturnType TelemSwc_Init(void);

/**
 * @brief Assemble one record from every source, and write it to the card.
 *
 * Driven cyclically by SchM from the storage task, once per acquisition cycle and after
 * ::BattSwc_MainFunction has refreshed the pack data.
 *
 * @return E_OK if the record was assembled and stored; E_NOT_OK if the card rejected it, in which case
 *         the record is still published live if a session exists -- losing it entirely because the card
 *         is faulty would be worse than delivering it without a durable copy.
 */
CHECK_RETURN Std_ReturnType TelemSwc_AcquireAndStore(void);

/**
 * @brief Publish the live record if there is one, then drain part of the backlog.
 *
 * Driven cyclically by SchM from the connectivity task.
 */
void TelemSwc_MainFunction(void);

/**
 * @brief Read the telemetry counters.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType TelemSwc_GetStatus(TelemSwc_StatusType *status);

/**
 * @brief Publish the periodic health record.
 *
 * A separate, smaller message carrying the counters from every module: heap, watchdog, DTCs, bearer,
 * flash wear, bus error rates. Published at ::TELEM_HEALTH_INTERVAL_MS rather than with every data
 * record, because it changes slowly and doubling the telemetry volume to carry it would be wasteful on a
 * metered link.
 */
void TelemSwc_PublishHealth(void);

/**
 * @brief Return this component's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void TelemSwc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* TELEMSWC_H */

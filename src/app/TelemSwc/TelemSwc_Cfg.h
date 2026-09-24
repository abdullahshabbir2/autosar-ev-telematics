/**
 * @file    TelemSwc_Cfg.h
 * @brief   Telemetry timing, topics and backlog policy.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef TELEMSWC_CFG_H
#define TELEMSWC_CFG_H

#include "services/Com/Com_Cfg.h"
#include "ecuabs/NetIf/NetIf_Cfg.h"
#include "base/Std_Types.h"

#define TELEMSWC_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Topics
 *
 *  Every topic carries the device identifier, so one broker serves a whole fleet without a subscriber
 *  having to filter payloads. The trailing identifier is appended at runtime from the provisioned device
 *  id rather than compiled in.
 *================================================================================================*/

/** Live and backlog telemetry records. */
#define TELEM_TOPIC_DATA_PREFIX "ev/telemetry/"

/** Periodic health record. */
#define TELEM_TOPIC_HEALTH_PREFIX "ev/health/"

/** Inbound: a request to re-send one or more days of history. */
#define TELEM_TOPIC_BACKFILL_REQUEST_PREFIX "ev/backfill/req/"

/** Outbound: the chunks of a backfill transfer. */
#define TELEM_TOPIC_BACKFILL_DATA_PREFIX "ev/backfill/data/"

/** Inbound: a diagnostic request. */
#define TELEM_TOPIC_DIAG_REQUEST_PREFIX "ev/diag/req/"

/** Outbound: a diagnostic response. */
#define TELEM_TOPIC_DIAG_RESPONSE_PREFIX "ev/diag/rsp/"

/*==================================================================================================
 *  Backlog policy
 *================================================================================================*/

/**
 * @brief Backlog records published per connectivity cycle.
 *
 * 4, at a 1 s cycle. A vehicle that has been offline for a day accumulates about 28 800 records, so at
 * four per second the backlog drains in roughly two hours of connected driving -- fast enough to matter,
 * slow enough that draining it cannot starve the live record or hold the task long enough to approach
 * its 30 s watchdog deadline. Each record is a separate publish, so a larger batch would mean a longer
 * uninterruptible stretch rather than more throughput.
 */
#define TELEM_BACKLOG_RECORDS_PER_CYCLE 4u

/**
 * @brief Backlog records to skip past if they are corrupt, per cycle.
 *
 * 2. A corrupt record is skipped deliberately and counted; bounding how many are skipped per cycle means
 * a long run of damaged data cannot turn into an unbounded loop -- which is what v1's reader did, as its
 * own author noted in a comment.
 */
#define TELEM_MAX_SKIPS_PER_CYCLE 2u

/*==================================================================================================
 *  Health record
 *================================================================================================*/

/**
 * @brief Interval between health records, in milliseconds.
 *
 * 60 s. The health counters change slowly, and publishing them with every data record would roughly
 * double the telemetry volume on a metered link to carry information that is almost always unchanged.
 */
#define TELEM_HEALTH_INTERVAL_MS 60000uL

/** Buffer for the health record, which is far smaller than a data record. */
#define TELEM_HEALTH_BUFFER_SIZE 512u

/*==================================================================================================
 *  Backfill
 *================================================================================================*/

/**
 * @brief Backfill chunks sent per connectivity cycle.
 *
 * 1. A chunk is COM_TRANSFER_CHUNK_SIZE bytes, so one per second is 2 KiB/s -- enough to move a day's
 * log in about twenty minutes, and deliberately slow so that a backfill request cannot displace live
 * telemetry. A backfill is a convenience; the live stream is the product.
 */
#define TELEM_BACKFILL_CHUNKS_PER_CYCLE 1u

/** Buffer for one backfill chunk. */
#define TELEM_BACKFILL_BUFFER_SIZE COM_TRANSFER_CHUNK_SIZE

#endif /* TELEMSWC_CFG_H */

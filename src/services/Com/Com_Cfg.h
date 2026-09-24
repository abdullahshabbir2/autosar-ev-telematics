/**
 * @file    Com_Cfg.h
 * @brief   Communication service configuration: record sizing and transfer chunking.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COM_CFG_H
#define COM_CFG_H

#include "Rs485If_Cfg.h"
#include "Std_Types.h"

#define COM_DEV_ERROR_DETECT STD_ON

/** Battery packs whose data appears in a record. */
#define COM_PACK_COUNT RS485IF_PACK_COUNT

/*==================================================================================================
 *  Record sizing
 *
 *  A record carries, per pack: 14 pack-level fields, 23 cell voltages, a current, 4 temperatures and
 *  2 flag words -- 44 fields. Across four packs that is 176, plus 23 vehicle-level fields, giving
 *  199 fields. The widest field is a 20-digit uint64 plus its separator, but the great majority are
 *  4 to 6 characters, and the measured worst case from the v1 device logs is 1866 bytes.
 *
 *  2304 is used rather than 1866: it is the measured worst case plus about 23 % headroom, and it is a
 *  multiple of 256, which keeps the MQTT client's own buffer arithmetic on a round boundary.
 *================================================================================================*/

/**
 * @brief Fields a full record contains, for the header/record consistency assertion.
 *
 * 23 vehicle-level fields plus 44 per pack across ::COM_PACK_COUNT packs. Stated explicitly rather
 * than computed, so that changing the record layout without updating the header -- the one CSV
 * defect that silently shifts every downstream column -- fails a test instead of shipping.
 */
#define COM_RECORD_FIELD_COUNT (23u + (44u * COM_PACK_COUNT))

/**
 * @brief Buffer size a caller must provide for one serialised record, including the terminator.
 */
#define COM_RECORD_BUFFER_SIZE 2304u

/**
 * @brief Buffer size for the CSV header line.
 *
 * The header names every field, so it is longer than a typical record: roughly 12 characters per
 * field across 198 fields.
 */
#define COM_HEADER_BUFFER_SIZE 3072u

/** Bytes required for "/YYYYMMDD.csv" plus its terminator. */
#define COM_FILENAME_SIZE 16u

/*==================================================================================================
 *  Transfer chunking
 *================================================================================================*/

/**
 * @brief Payload bytes per chunk of a historical file transfer.
 *
 * 2048. The broker's own limit is what matters here, and 2048 sits well inside the default 256 KiB
 * MQTT maximum while being large enough that the per-publish overhead is a small fraction of the
 * transfer. v1 used the same figure.
 */
#define COM_TRANSFER_CHUNK_SIZE 2048u

/**
 * @brief Largest number of dates one backfill request may ask for.
 *
 * 16. A request is a list of days to re-send; sixteen days of history is already far more than an
 * operator would ask for in one message, and the bound is what makes the request path safe against
 * an oversized or hostile payload. v1 allocated one heap node per date with no limit at all.
 */
#define COM_MAX_BACKFILL_DATES 16u

/** Characters in a "YYYYMMDD" date, excluding the terminator. */
#define COM_DATE_LENGTH 8u

/**
 * @brief Largest inbound request payload the parser will scan, in bytes.
 *
 * 256. Sixteen dates at nine characters each is 144; 256 allows for whitespace and a trailing
 * separator while bounding the work a single message can cause.
 */
#define COM_MAX_REQUEST_PAYLOAD 256u

/*==================================================================================================
 *  Field conventions
 *================================================================================================*/

/** Field separator. */
#define COM_FIELD_SEPARATOR ','

/**
 * @brief Text emitted for a value that was not measured this cycle.
 *
 * Empty, so the field contains nothing between its separators. This is the distinction v1 lost: it
 * emitted zeros and empty fields interchangeably, so a pack that had stopped answering looked
 * identical in the log to one genuinely reading zero. Anything downstream that computes an average
 * over the fleet is wrong in a way nobody notices.
 */
#define COM_INVALID_FIELD ""

#endif /* COM_CFG_H */

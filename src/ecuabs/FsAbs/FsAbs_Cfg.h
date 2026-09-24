/**
 * @file    FsAbs_Cfg.h
 * @brief   Filesystem abstraction configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FSABS_CFG_H
#define FSABS_CFG_H

#include "services/Com/Com_Cfg.h"
#include "base/Std_Types.h"

#define FSABS_DEV_ERROR_DETECT STD_ON

/** Bytes for a log file name, including the leading slash and the terminator. */
#define FSABS_FILENAME_SIZE COM_FILENAME_SIZE

/** Longest record the reader will accept, including its CRC field and terminator. */
#define FSABS_MAX_RECORD_SIZE COM_RECORD_BUFFER_SIZE

/**
 * @brief Free space below which housekeeping deletes the oldest transferred file, in MiB.
 *
 * 512 MiB. Generous, and deliberately so: the deletion only ever removes files the transfer cursor has
 * already passed, so a large reserve means the ECU rarely has to choose between new data and old, and
 * when it does there is plenty of already-transferred history to reclaim first.
 */
#define FSABS_LOW_SPACE_LIMIT_MIB 512uL

/**
 * @brief Free space below which a diagnostic event is raised, in MiB.
 *
 * 64 MiB. At the observed ~3 MiB per day of records this is three weeks of headroom, which is enough
 * warning to act on before data starts being lost.
 */
#define FSABS_CRITICAL_SPACE_MIB 64uL

/**
 * @brief Consecutive append failures before the card is declared unusable.
 *
 * 5. A single failure can be a transient on a vehicle harness; five in a row at the 3 s record period
 * is fifteen seconds of a card that is not accepting data.
 */
#define FSABS_WRITE_FAILURE_LIMIT 5u

/**
 * @brief Separator between a record's text and its CRC field.
 *
 * A record is written as "<text>|<crc32 in hex>\n". The pipe cannot appear in the CSV payload, which is
 * comma-separated and numeric, so the split is unambiguous without escaping.
 */
#define FSABS_CRC_SEPARATOR '|'

/** Characters in the hexadecimal CRC field. */
#define FSABS_CRC_FIELD_CHARS 8u

#endif /* FSABS_CFG_H */

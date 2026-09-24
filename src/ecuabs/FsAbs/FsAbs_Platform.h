/**
 * @file    FsAbs_Platform.h
 * @brief   Platform leaf of the filesystem abstraction.
 *
 * The operations that need the SD driver. The record framing, CRC verification, cursor policy and
 * housekeeping rules are in FsAbs.c and do not depend on this interface's implementation.
 *
 * Implemented by FsAbs_Esp32.cpp on the target and by test/support/Stub_Platform.c on the host, where
 * an in-memory filesystem stands in for the card.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FSABS_PLATFORM_H
#define FSABS_PLATFORM_H

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the card. E_NOT_OK if absent or unreadable. */
CHECK_RETURN Std_ReturnType FsAbs_PlatformMount(void);

/** Unmount the card, flushing anything outstanding. */
void FsAbs_PlatformUnmount(void);

/**
 * @brief Report card capacity and usage, in MiB.
 * @return E_OK on success; E_NOT_OK if the card is not mounted.
 */
CHECK_RETURN Std_ReturnType FsAbs_PlatformGetSpace(uint32 *capacityMiB, uint32 *usedMiB);

/** TRUE if @p path exists. */
boolean FsAbs_PlatformExists(const char *path);

/**
 * @brief Append @p length bytes to @p path, creating it if absent.
 * @return E_OK once the data is committed to the card.
 */
CHECK_RETURN Std_ReturnType FsAbs_PlatformAppend(const char *path, const uint8 *data,
                                                 uint32 length);

/**
 * @brief Read up to @p size bytes from @p path at @p offset.
 * @param[in]  path   File to read.
 * @param[in]  offset Byte offset to read from.
 * @param[out] buffer Destination.
 * @param[in]  size   Capacity of @p buffer.
 * @param[out] read   Bytes actually read; 0 at end of file.
 */
CHECK_RETURN Std_ReturnType FsAbs_PlatformRead(const char *path, uint32 offset, uint8 *buffer,
                                               uint32 size, uint32 *read);

/** Size of @p path in bytes. E_NOT_FOUND if it does not exist. */
CHECK_RETURN Std_ReturnType FsAbs_PlatformSize(const char *path, uint32 *size);

/** Delete @p path. E_NOT_FOUND if it does not exist. */
CHECK_RETURN Std_ReturnType FsAbs_PlatformRemove(const char *path);

/**
 * @brief Find the lexicographically smallest log file name.
 *
 * Log files are named "/YYYYMMDD.csv", so lexicographic order is chronological order. That is the whole
 * reason for the name format: it makes "the oldest file" answerable without reading any timestamps, and
 * without the integer conversion v1 used, which would have mis-ordered any name it could not parse.
 *
 * @param[out] buffer Destination for the name.
 * @param[in]  size   Capacity of @p buffer.
 * @return E_OK if a log file was found; E_NOT_FOUND if the card holds none.
 */
CHECK_RETURN Std_ReturnType FsAbs_PlatformFindOldestLog(char *buffer, uint16 size);

/**
 * @brief Find the smallest log file name strictly greater than @p after.
 *
 * Because log files are named "/YYYYMMDD.csv", the lexicographically next name is the chronologically
 * next day -- so this answers "which file does the transfer move to when the current one is drained"
 * without reading a single timestamp.
 *
 * Strictly greater, so a cursor sitting on a drained file cannot select that same file again and
 * livelock. A gap in the sequence is normal: a vehicle parked for a week produces no file for those
 * days, and the next name found simply skips them.
 *
 * @param[in]  after  The current file name. An empty string finds the oldest.
 * @param[out] buffer Destination for the name.
 * @param[in]  size   Capacity of @p buffer.
 * @return E_OK if a later log exists; E_NOT_FOUND if @p after is already the newest.
 */
CHECK_RETURN Std_ReturnType FsAbs_PlatformFindNextLog(const char *after, char *buffer, uint16 size);

#ifdef __cplusplus
}
#endif

#endif /* FSABS_PLATFORM_H */

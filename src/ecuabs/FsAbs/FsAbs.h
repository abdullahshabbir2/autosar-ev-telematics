/**
 * @file    FsAbs.h
 * @brief   Filesystem abstraction over the microSD card -- store-and-forward log storage.
 *
 * The SD card is this ECU's buffer against being out of coverage. A vehicle spends much of its life
 * with no usable backhaul, so records are appended to a daily file and transferred later; the card is
 * what makes "later" possible.
 *
 * @par What the read cursor is, and why v1's was fragile
 * Transfer progress is a cursor: which file, and how far into it. v1 kept that cursor in a text file
 * (`/config.txt`) as two `$`-terminated fields, re-read it by scanning for `$` characters in an
 * unbounded loop, and duplicated the same write-the-cursor-back code in five places. Its record reader
 * contained a loop the author had annotated `//! This is an infinite loop. add a failsafe of jumping to
 * next file`, reachable when a file ended mid-record.
 *
 * The cursor here lives in NvM as a CRC-protected block, so it is crash-safe and cannot be
 * half-written; the record reader is bounded by construction and reports a corrupt record rather than
 * looping on it.
 *
 * @par Records are self-delimiting and checksummed
 * Each record is written as one line ending in a CRC-32 field, so a reader can tell a complete record
 * from a truncated one. v1 wrapped records in `<` and `>` with no checksum, so a record cut short by a
 * power loss mid-write was indistinguishable from a complete one until something downstream tried to
 * parse it.
 *
 * @req SWREQ-STO-0001 .. SWREQ-STO-0020
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FSABS_H
#define FSABS_H

#include "base/Autosar_ModuleIds.h"
#include "ecuabs/FsAbs/FsAbs_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FSABS_VENDOR_ID 0xFFFEu
#define FSABS_SW_MAJOR_VERSION 2u
#define FSABS_SW_MINOR_VERSION 0u
#define FSABS_SW_PATCH_VERSION 0u

#define FSABS_API_ID_INIT 0x00u
#define FSABS_API_ID_APPEND 0x20u
#define FSABS_API_ID_READ_NEXT 0x21u
#define FSABS_API_ID_ADVANCE 0x22u
#define FSABS_API_ID_READ_CHUNK 0x23u
#define FSABS_API_ID_HOUSEKEEP 0x24u

#define FSABS_E_UNINIT E_UNINIT
#define FSABS_E_PARAM_POINTER E_PARAM_POINTER
#define FSABS_E_MOUNT_FAILED 0x20u
#define FSABS_E_WRITE_FAILED 0x21u
#define FSABS_E_READ_FAILED 0x22u
#define FSABS_E_RECORD_CORRUPT 0x23u
#define FSABS_E_SPACE_LOW 0x24u
#define FSABS_E_NO_FILE 0x25u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Card and usage information, published in the telemetry health record. */
typedef struct
{
    boolean mounted;       /**< TRUE if the card is present and mounted.      */
    uint32 capacityMiB;    /**< Total capacity.                               */
    uint32 usedMiB;        /**< Space in use.                                 */
    uint32 freeMiB;        /**< Space available.                              */
    uint32 recordsWritten; /**< Records appended since Init.                  */
    uint32 writeFailures;  /**< Appends the card rejected.                    */
    uint32 recordsRead;    /**< Records handed to the transfer path.          */
    uint32 corruptRecords; /**< Records rejected by their CRC.                */
    uint32 filesDeleted;   /**< Oldest files removed to reclaim space.        */
    uint32 unsentBytes;    /**< Bytes behind the transfer cursor.             */
} FsAbs_StatusType;

/** Where the transfer has reached. */
typedef struct
{
    char fileName[FSABS_FILENAME_SIZE]; /**< File currently being transferred.      */
    uint32 offset;                      /**< Byte offset within it.                 */
    boolean valid;                      /**< FALSE before the first record is written. */
} FsAbs_CursorType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Mount the card, restore the transfer cursor from NvM and check free space.
 * @return E_OK if the card is usable; E_NOT_OK otherwise, in which case live publishing continues
 *         without a store-and-forward buffer rather than the ECU refusing to run.
 */
CHECK_RETURN Std_ReturnType FsAbs_Init(void);

/** TRUE if the card is mounted and writable. */
boolean FsAbs_IsMounted(void);

/**
 * @brief Append one record to today's log file, creating the file and its header if needed.
 *
 * @param[in] dateStamp "YYYYMMDD" naming the file. Supplied by the caller rather than read from the
 *                      clock here, so a record is never filed under a date the caller did not intend.
 * @param[in] record    NUL-terminated record text, without a line terminator.
 * @return E_OK on success; E_NOT_OK on a write failure; E_NO_SPACE if the card is full and
 *         housekeeping could not reclaim room.
 */
CHECK_RETURN Std_ReturnType FsAbs_AppendRecord(const char *dateStamp, const char *record);

/**
 * @brief Read the record at the transfer cursor.
 *
 * @param[out] buffer  Destination for the record text, NUL-terminated.
 * @param[in]  size    Capacity of @p buffer.
 * @param[out] length  Characters written.
 * @return E_OK if a complete, CRC-valid record was read; E_PENDING if this file was drained and the
 *         cursor has moved to a later one, so the caller should call again; E_NOT_FOUND if the cursor
 *         has reached the end of all available data; ::FSABS_E_RECORD_CORRUPT as E_CRC_FAIL if the
 *         record at the cursor failed its checksum, in which case ::FsAbs_SkipCorruptRecord moves
 *         past it.
 *
 * @note E_PENDING rather than transparently reading on into the next file. A caller draining a backlog
 *       runs on a budgeted cyclic task, and crossing a file boundary costs a directory walk plus an
 *       open -- so it is made one unit of work rather than something that can happen in the middle of
 *       what looked like a single record read. The CSV header of each file is skipped silently and is
 *       never reported as a corrupt record.
 */
CHECK_RETURN Std_ReturnType FsAbs_ReadRecordAtCursor(char *buffer, uint16 size, uint16 *length);

/**
 * @brief Advance the cursor past the record just read, and persist it.
 *
 * Called only after the record has been successfully transferred, so an interrupted transfer resumes
 * from the same record rather than skipping it. The cursor is written through to NvM, which is
 * crash-safe, so a reset cannot leave it half-updated.
 */
CHECK_RETURN Std_ReturnType FsAbs_AdvanceCursor(void);

/**
 * @brief Move the cursor past a record that failed its CRC.
 *
 * Separate from ::FsAbs_AdvanceCursor so that skipping damaged data is always a deliberate act and is
 * counted. v1 had no equivalent: a corrupt record stalled the transfer permanently, which is what its
 * author's "this is an infinite loop" comment was about.
 */
CHECK_RETURN Std_ReturnType FsAbs_SkipCorruptRecord(void);

/**
 * @brief Read a byte range from a named file, for a historical backfill transfer.
 *
 * @param[in]  fileName File to read, as produced by Com_FormatLogFileName().
 * @param[in]  offset   Byte offset.
 * @param[out] buffer   Destination.
 * @param[in]  size     Bytes to read.
 * @param[out] read     Bytes actually read, which may be fewer at the end of the file.
 * @return E_OK on success; E_NOT_FOUND if the file does not exist.
 */
CHECK_RETURN Std_ReturnType FsAbs_ReadFileChunk(const char *fileName, uint32 offset, uint8 *buffer,
                                                uint32 size, uint32 *read);

/**
 * @brief Size of a named file, in bytes.
 * @return E_OK on success; E_NOT_FOUND if the file does not exist.
 */
CHECK_RETURN Std_ReturnType FsAbs_GetFileSize(const char *fileName, uint32 *size);

/**
 * @brief Reclaim space and refresh the free-space figures.
 *
 * Driven cyclically. Deletes the oldest log file when free space falls below
 * ::FSABS_LOW_SPACE_LIMIT_MIB, and never deletes a file the transfer cursor has not passed -- deleting
 * unsent data to make room for new data loses information that cannot be recovered, which is the wrong
 * trade for a store-and-forward buffer.
 */
void FsAbs_MainFunction(void);

/**
 * @brief Read the transfer cursor.
 * @param[out] cursor Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType FsAbs_GetCursor(FsAbs_CursorType *cursor);

/**
 * @brief Read the card status.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType FsAbs_GetStatus(FsAbs_StatusType *status);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void FsAbs_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* FSABS_H */

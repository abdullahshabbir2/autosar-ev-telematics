/**
 * @file    FsAbs.c
 * @brief   Filesystem abstraction implementation: record framing, cursor policy, housekeeping.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "ecuabs/FsAbs/FsAbs.h"

#include <string.h>

#include "services/Com/Com.h"
#include "services/Crc/Crc.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "ecuabs/FsAbs/FsAbs_Platform.h"
#include "services/NvM/NvM.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean FsAbs_Initialised = FALSE;
STATIC FsAbs_StatusType FsAbs_Status;
STATIC FsAbs_CursorType FsAbs_Cursor;
STATIC uint16 FsAbs_ConsecutiveWriteFailures;

/**
 * @brief Length of the record the cursor is currently pointing at, including its terminator.
 *
 * Set by ::FsAbs_ReadRecordAtCursor and consumed by ::FsAbs_AdvanceCursor, so that advancing does not
 * have to re-read and re-measure the record. Zero when no record has been read.
 */
STATIC uint32 FsAbs_PendingRecordSpan;

/** Scratch buffer for one record plus its CRC field. */
STATIC char FsAbs_LineBuffer[FSABS_MAX_RECORD_SIZE + FSABS_CRC_FIELD_CHARS + 4u];

/**
 * @brief Peek buffer for the header check, separate from the line buffer.
 *
 * FsAbs_ReadRecordAtCursor calls the header check before filling FsAbs_LineBuffer, so sharing the
 * buffer would work today and break the moment the call order changed -- the kind of coupling that is
 * invisible in a diff. Sized for a header line only; a longer first line is left to the read path.
 */
STATIC char FsAbs_HeaderPeek[COM_HEADER_BUFFER_SIZE];

/*==================================================================================================
 *  Cursor persistence
 *
 *  The cursor lives in an NvM block, which gives it a CRC and crash-safe writes. v1 kept it in a text
 *  file with two '$'-terminated fields, re-read by scanning for '$' in an unbounded loop, and rewrote it
 *  from five separate places. A reset between the two fields left a cursor that named one file and an
 *  offset belonging to another.
 *================================================================================================*/

/** On-media layout of the cursor block, matching FEE_LENGTH_TELEMETRY_CURSOR. */
typedef struct
{
    uint16 structVersion;               /**< Layout version; 1 for this definition.     */
    uint16 reserved0;                   /**< Explicit padding, written as zero.         */
    uint32 offset;                      /**< Byte offset within @c fileName.            */
    char fileName[FSABS_FILENAME_SIZE]; /**< NUL-terminated, or empty if never set.     */
    uint32 reserved1;                   /**< Explicit padding, written as zero.         */
} FsAbs_CursorBlockType;

STATIC Std_ReturnType FsAbs_LoadCursor(void)
{
    /* The cursor shares the calibration block's NvM slot family but has its own Fee block. It is read
     * through a local image so a partially-populated structure is never published. */
    FsAbs_CursorBlockType block;

    (void)memset(&block, 0, sizeof(block));

    /* NvM does not own this block directly -- it is read from Fee, because the cursor changes far too
     * often to justify a RAM mirror and an end-to-end CRC on every advance. Fee's own per-record CRC
     * and crash-safe commit are exactly the guarantees the cursor needs. */
    if (Fee_ReadBlock(FEE_BLOCK_TELEMETRY_CURSOR, (uint8 *)&block, 0u, (uint16)sizeof(block)) != E_OK)
    {
        FsAbs_Cursor.valid = FALSE;
        FsAbs_Cursor.offset = 0u;
        FsAbs_Cursor.fileName[0] = '\0';
        return E_NOT_FOUND;
    }

    if ((block.structVersion != 1u) || (block.fileName[0] == '\0'))
    {
        FsAbs_Cursor.valid = FALSE;
        return E_NOT_FOUND;
    }

    (void)memcpy(FsAbs_Cursor.fileName, block.fileName, sizeof(FsAbs_Cursor.fileName));
    FsAbs_Cursor.fileName[sizeof(FsAbs_Cursor.fileName) - 1u] = '\0';
    FsAbs_Cursor.offset = block.offset;
    FsAbs_Cursor.valid = TRUE;

    return E_OK;
}

STATIC Std_ReturnType FsAbs_StoreCursor(void)
{
    FsAbs_CursorBlockType block;

    (void)memset(&block, 0, sizeof(block));
    block.structVersion = 1u;
    block.offset = FsAbs_Cursor.offset;
    (void)memcpy(block.fileName, FsAbs_Cursor.fileName, sizeof(block.fileName));

    return Fee_WriteBlock(FEE_BLOCK_TELEMETRY_CURSOR, (const uint8 *)&block);
}

/*==================================================================================================
 *  Record framing
 *
 *  A record is written as:  <text>|<crc32, 8 hex digits>\n
 *
 *  The CRC covers the text only. A reader can therefore tell a complete record from one truncated by a
 *  power loss mid-write, which v1 could not: it wrapped records in '<' and '>' with no checksum, so a
 *  record cut short was indistinguishable from a complete one until something downstream tried to parse
 *  it -- by which time the data was in the cloud.
 *================================================================================================*/

STATIC char FsAbs_HexDigit(uint8 nibble)
{
    return (nibble < 10u) ? (char)('0' + (char)nibble) : (char)('A' + (char)(nibble - 10u));
}

/** Append the 8-digit hexadecimal form of @p crc at @p out. */
STATIC void FsAbs_WriteCrcField(char *out, uint32 crc)
{
    uint8 i;

    for (i = 0u; i < FSABS_CRC_FIELD_CHARS; i++)
    {
        const uint8 shift = (uint8)(28u - (i * 4u));
        out[i] = FsAbs_HexDigit((uint8)((crc >> shift) & 0x0FuL));
    }
}

/**
 * @brief Advance the cursor past a file's CSV header line, if it is sitting on one.
 *
 * The header is identified by position and shape together: it is the first line of a file, and it has
 * no CRC field. Both conditions are required. Position alone would skip a genuinely damaged first
 * record, and shape alone would skip a corrupt record anywhere in the file -- and a corrupt record must
 * be *reported*, because that is the signal a card is failing.
 *
 * Silent, and does not touch the corruption counter. A header is expected content, not damage.
 */
STATIC void FsAbs_SkipCsvHeader(void)
{
    uint32 read = 0u;
    uint32 i;
    boolean hasSeparator = FALSE;
    uint32 lineEnd = 0u;
    boolean foundTerminator = FALSE;

    if (FsAbs_Cursor.offset != 0u)
    {
        return; /* not at the start of a file, so not on a header */
    }

    if (FsAbs_PlatformRead(FsAbs_Cursor.fileName, 0u, (uint8 *)FsAbs_HeaderPeek,
                           (uint32)sizeof(FsAbs_HeaderPeek) - 1u, &read) != E_OK)
    {
        return;
    }

    for (i = 0u; i < read; i++)
    {
        if (FsAbs_HeaderPeek[i] == '\n')
        {
            lineEnd = i;
            foundTerminator = TRUE;
            break;
        }
        if (FsAbs_HeaderPeek[i] == (char)FSABS_CRC_SEPARATOR)
        {
            hasSeparator = TRUE;
        }
    }

    /* A first line that terminated and carried no separator is the header. A line that did not
     * terminate within the peek is left alone: it is either a record longer than the peek buffer or an
     * interrupted append, and the read path reports both as corruption, which is correct. */
    if ((foundTerminator != FALSE) && (hasSeparator == FALSE))
    {
        FsAbs_Cursor.offset = lineEnd + 1u;
    }
}

/** Parse an 8-digit hexadecimal CRC field. E_NOT_OK on a non-hex character. */
STATIC Std_ReturnType FsAbs_ParseCrcField(const char *text, uint32 *crc)
{
    uint32 value = 0u;
    uint8 i;

    for (i = 0u; i < FSABS_CRC_FIELD_CHARS; i++)
    {
        const char c = text[i];
        uint8 nibble;

        if ((c >= '0') && (c <= '9'))
        {
            nibble = (uint8)(c - '0');
        }
        else if ((c >= 'A') && (c <= 'F'))
        {
            nibble = (uint8)((c - 'A') + 10);
        }
        else
        {
            return E_NOT_OK;
        }
        value = (value << 4u) | (uint32)nibble;
    }

    *crc = value;
    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType FsAbs_Init(void)
{
    (void)memset(&FsAbs_Status, 0, sizeof(FsAbs_Status));
    (void)memset(&FsAbs_Cursor, 0, sizeof(FsAbs_Cursor));
    FsAbs_ConsecutiveWriteFailures = 0u;
    FsAbs_PendingRecordSpan = 0u;
    FsAbs_Initialised = TRUE;

    if (FsAbs_PlatformMount() != E_OK)
    {
        FsAbs_Status.mounted = FALSE;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
        /* Not fatal. Live publishing continues without a store-and-forward buffer, which is far better
         * than refusing to run -- v1 restarted the whole ECU after ten minutes of a failed card, which
         * only guaranteed that nothing was ever logged. */
        return E_NOT_OK;
    }

    FsAbs_Status.mounted = TRUE;
    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_MOUNT_FAILED, INSTANCE_ID_SINGLE,
                                   DEM_EVENT_STATUS_PASSED));

    if (FsAbs_PlatformGetSpace(&FsAbs_Status.capacityMiB, &FsAbs_Status.usedMiB) == E_OK)
    {
        FsAbs_Status.freeMiB = (FsAbs_Status.capacityMiB > FsAbs_Status.usedMiB)
                                   ? (FsAbs_Status.capacityMiB - FsAbs_Status.usedMiB)
                                   : 0u;
    }

    /* A missing cursor is normal on a new unit; it is established by the first append. */
    (void)FsAbs_LoadCursor();

    return E_OK;
}

boolean FsAbs_IsMounted(void)
{
    return FsAbs_Status.mounted;
}

Std_ReturnType FsAbs_AppendRecord(const char *dateStamp, const char *record)
{
    char path[FSABS_FILENAME_SIZE];
    uint16 textLength;
    uint32 crc;
    uint32 lineLength;
    boolean isNewFile;

    DET_CHECK_RETURN(FsAbs_Initialised != FALSE, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_APPEND, FSABS_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN((dateStamp != NULL_PTR) && (record != NULL_PTR), MODULE_ID_FSABS,
                     INSTANCE_ID_SINGLE, FSABS_API_ID_APPEND, FSABS_E_PARAM_POINTER, E_NOT_OK);

    if (FsAbs_Status.mounted == FALSE)
    {
        return E_NOT_OK;
    }

    if (Com_FormatLogFileName(path, (uint16)sizeof(path), dateStamp) != E_OK)
    {
        return E_NOT_OK;
    }

    textLength = (uint16)strlen(record);
    if ((uint32)textLength + FSABS_CRC_FIELD_CHARS + 2u > (uint32)sizeof(FsAbs_LineBuffer))
    {
        (void)Det_ReportError(MODULE_ID_FSABS, INSTANCE_ID_SINGLE, FSABS_API_ID_APPEND,
                              E_PARAM_VALUE);
        return E_NOT_OK;
    }

    isNewFile = (FsAbs_PlatformExists(path) == FALSE) ? TRUE : FALSE;

    if (isNewFile != FALSE)
    {
        /* A new day's file gets the CSV header first, so the file is self-describing. The header is
         * written before the record, so a power loss between them leaves a header-only file that the
         * consumer parses as empty rather than as headerless data. */
        char header[COM_HEADER_BUFFER_SIZE];
        uint16 headerLength = 0u;

        if (Com_FormatCsvHeader(header, (uint16)sizeof(header), &headerLength) == E_OK)
        {
            header[headerLength] = '\n';
            if (FsAbs_PlatformAppend(path, (const uint8 *)header, (uint32)headerLength + 1u) != E_OK)
            {
                FsAbs_Status.writeFailures++;
                return E_NOT_OK;
            }
        }
    }

    (void)memcpy(FsAbs_LineBuffer, record, textLength);
    FsAbs_LineBuffer[textLength] = (char)FSABS_CRC_SEPARATOR;
    crc = Crc_CalculateCRC32((const uint8 *)record, (uint32)textLength, 0u, TRUE);
    FsAbs_WriteCrcField(&FsAbs_LineBuffer[textLength + 1u], crc);
    lineLength = (uint32)textLength + 1u + FSABS_CRC_FIELD_CHARS;
    FsAbs_LineBuffer[lineLength] = '\n';
    lineLength++;

    if (FsAbs_PlatformAppend(path, (const uint8 *)FsAbs_LineBuffer, lineLength) != E_OK)
    {
        FsAbs_Status.writeFailures++;
        if (FsAbs_ConsecutiveWriteFailures < 0xFFFFu)
        {
            FsAbs_ConsecutiveWriteFailures++;
        }
        if (FsAbs_ConsecutiveWriteFailures >= (uint16)FSABS_WRITE_FAILURE_LIMIT)
        {
            FsAbs_Status.mounted = FALSE;
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_WRITE_FAILED, INSTANCE_ID_SINGLE,
                                           DEM_EVENT_STATUS_FAILED));
        }
        return E_NOT_OK;
    }

    FsAbs_ConsecutiveWriteFailures = 0u;
    FsAbs_Status.recordsWritten++;
    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_WRITE_FAILED, INSTANCE_ID_SINGLE,
                                   DEM_EVENT_STATUS_PASSED));

    /* The first record ever written establishes the transfer cursor. */
    if (FsAbs_Cursor.valid == FALSE)
    {
        (void)memcpy(FsAbs_Cursor.fileName, path, sizeof(FsAbs_Cursor.fileName));
        FsAbs_Cursor.fileName[sizeof(FsAbs_Cursor.fileName) - 1u] = '\0';
        FsAbs_Cursor.offset = 0u;
        FsAbs_Cursor.valid = TRUE;
        STD_DISCARD(FsAbs_StoreCursor());
    }

    return E_OK;
}

/**
 * @brief Move the cursor to the next log file, if one exists.
 *
 * Called when the current file is drained. The new cursor is stored immediately, for the same reason
 * ::FsAbs_AdvanceCursor stores its advance: a crossing held only in RAM would be lost on a reset and the
 * transfer would resume on a file it had already finished, re-sending a whole day.
 *
 * @return E_OK if the cursor moved; E_NOT_FOUND if this was the newest file.
 */
STATIC Std_ReturnType FsAbs_AdvanceToNextFile(void)
{
    char next[FSABS_FILENAME_SIZE];

    if (FsAbs_Cursor.valid == FALSE)
    {
        return E_NOT_FOUND;
    }

    if (FsAbs_PlatformFindNextLog(FsAbs_Cursor.fileName, next, (uint16)sizeof(next)) != E_OK)
    {
        return E_NOT_FOUND;
    }

    (void)memcpy(FsAbs_Cursor.fileName, next, sizeof(FsAbs_Cursor.fileName));
    FsAbs_Cursor.fileName[sizeof(FsAbs_Cursor.fileName) - 1u] = '\0';
    FsAbs_Cursor.offset = 0u;
    FsAbs_PendingRecordSpan = 0u;

    STD_DISCARD(FsAbs_StoreCursor());

    return E_OK;
}

Std_ReturnType FsAbs_ReadRecordAtCursor(char *buffer, uint16 size, uint16 *length)
{
    uint32 read = 0u;
    uint32 i;
    uint32 lineEnd = 0u;
    boolean foundTerminator = FALSE;

    DET_CHECK_RETURN(FsAbs_Initialised != FALSE, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_READ_NEXT, FSABS_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN((buffer != NULL_PTR) && (length != NULL_PTR), MODULE_ID_FSABS,
                     INSTANCE_ID_SINGLE, FSABS_API_ID_READ_NEXT, FSABS_E_PARAM_POINTER, E_NOT_OK);

    *length = 0u;
    FsAbs_PendingRecordSpan = 0u;

    if ((FsAbs_Status.mounted == FALSE) || (FsAbs_Cursor.valid == FALSE))
    {
        return E_NOT_FOUND;
    }

    FsAbs_SkipCsvHeader();

    if (FsAbs_PlatformRead(FsAbs_Cursor.fileName, FsAbs_Cursor.offset,
                           (uint8 *)FsAbs_LineBuffer,
                           (uint32)sizeof(FsAbs_LineBuffer) - 1u, &read) != E_OK)
    {
        return E_NOT_FOUND;
    }
    if (read == 0u)
    {
        /* End of this file. If a later one exists the cursor moves to it and the caller retries on its
         * next pass; if not, the transfer has genuinely caught up.
         *
         * This is done here rather than left to housekeeping, which is where a comment in an earlier
         * revision said it belonged and where it was never implemented. The consequence of the gap was
         * severe and entirely silent: the cursor is established on the first record ever written and
         * nothing else moved it, so a vehicle running past midnight drained that first day's file and
         * then stopped transmitting for the life of the unit. Housekeeping then declined to reclaim the
         * file -- correctly, since the cursor had not passed it -- so the card filled to capacity and
         * appends began failing. The visible symptom was a storage fault, which points at the card. */
        if (FsAbs_AdvanceToNextFile() == E_OK)
        {
            return E_PENDING;
        }

        return E_NOT_FOUND;
    }

    /* Bounded by the bytes actually read, so a file whose last line has no terminator ends the scan
     * rather than running past the buffer. This is the loop v1's author annotated as an infinite one. */
    for (i = 0u; i < read; i++)
    {
        if (FsAbs_LineBuffer[i] == '\n')
        {
            lineEnd = i;
            foundTerminator = TRUE;
            break;
        }
    }

    if (foundTerminator == FALSE)
    {
        /* No terminator within the bytes read. Either the record is longer than the buffer -- which the
         * writer makes impossible -- or the file ends mid-record because a power loss interrupted the
         * append. Reported as corrupt so the caller can skip it deliberately. */
        FsAbs_Status.corruptRecords++;
        (void)Det_ReportRuntimeError(MODULE_ID_FSABS, INSTANCE_ID_SINGLE, FSABS_API_ID_READ_NEXT,
                                     FSABS_E_RECORD_CORRUPT);
        FsAbs_PendingRecordSpan = read;
        return E_CRC_FAIL;
    }

    /* Split the line at its CRC separator. */
    {
        uint32 separator = 0u;
        boolean foundSeparator = FALSE;
        uint32 storedCrc = 0u;

        for (i = 0u; i < lineEnd; i++)
        {
            if (FsAbs_LineBuffer[i] == (char)FSABS_CRC_SEPARATOR)
            {
                separator = i;
                foundSeparator = TRUE;
                /* No break: the last separator wins, so a payload that somehow contained one is still
                 * split at the field the writer appended. */
            }
        }

        if ((foundSeparator == FALSE) ||
            ((lineEnd - separator) != (uint32)(FSABS_CRC_FIELD_CHARS + 1u)))
        {
            FsAbs_Status.corruptRecords++;
            FsAbs_PendingRecordSpan = lineEnd + 1u;
            return E_CRC_FAIL;
        }

        if (FsAbs_ParseCrcField(&FsAbs_LineBuffer[separator + 1u], &storedCrc) != E_OK)
        {
            FsAbs_Status.corruptRecords++;
            FsAbs_PendingRecordSpan = lineEnd + 1u;
            return E_CRC_FAIL;
        }

        if (Crc_CalculateCRC32((const uint8 *)FsAbs_LineBuffer, separator, 0u, TRUE) != storedCrc)
        {
            FsAbs_Status.corruptRecords++;
            (void)Det_ReportRuntimeError(MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                                         FSABS_API_ID_READ_NEXT, FSABS_E_RECORD_CORRUPT);
            FsAbs_PendingRecordSpan = lineEnd + 1u;
            return E_CRC_FAIL;
        }

        if (separator >= (uint32)size)
        {
            return E_NOT_OK;
        }

        (void)memcpy(buffer, FsAbs_LineBuffer, separator);
        buffer[separator] = '\0';
        *length = (uint16)separator;
    }

    FsAbs_PendingRecordSpan = lineEnd + 1u;
    FsAbs_Status.recordsRead++;

    return E_OK;
}

Std_ReturnType FsAbs_AdvanceCursor(void)
{
    DET_CHECK_RETURN(FsAbs_Initialised != FALSE, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_ADVANCE, FSABS_E_UNINIT, E_NOT_OK);

    if (FsAbs_PendingRecordSpan == 0u)
    {
        /* Nothing has been read, so there is nothing to advance past. Advancing by a guessed amount
         * here is how a transfer starts skipping records. */
        return E_NOT_OK;
    }

    FsAbs_Cursor.offset += FsAbs_PendingRecordSpan;
    FsAbs_PendingRecordSpan = 0u;

    /* Written through immediately. An interrupted transfer must resume from the record it was on, and a
     * cursor held only in RAM would resend or skip whatever the reset interrupted. */
    return FsAbs_StoreCursor();
}

Std_ReturnType FsAbs_SkipCorruptRecord(void)
{
    DET_CHECK_RETURN(FsAbs_Initialised != FALSE, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_ADVANCE, FSABS_E_UNINIT, E_NOT_OK);

    if (FsAbs_PendingRecordSpan == 0u)
    {
        return E_NOT_OK;
    }

    FsAbs_Cursor.offset += FsAbs_PendingRecordSpan;
    FsAbs_PendingRecordSpan = 0u;

    return FsAbs_StoreCursor();
}

Std_ReturnType FsAbs_ReadFileChunk(const char *fileName, uint32 offset, uint8 *buffer, uint32 size,
                                   uint32 *read)
{
    DET_CHECK_RETURN(FsAbs_Initialised != FALSE, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_READ_CHUNK, FSABS_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN((fileName != NULL_PTR) && (buffer != NULL_PTR) && (read != NULL_PTR),
                     MODULE_ID_FSABS, INSTANCE_ID_SINGLE, FSABS_API_ID_READ_CHUNK,
                     FSABS_E_PARAM_POINTER, E_NOT_OK);

    if (FsAbs_Status.mounted == FALSE)
    {
        return E_NOT_OK;
    }

    return FsAbs_PlatformRead(fileName, offset, buffer, size, read);
}

Std_ReturnType FsAbs_GetFileSize(const char *fileName, uint32 *size)
{
    DET_CHECK_RETURN((fileName != NULL_PTR) && (size != NULL_PTR), MODULE_ID_FSABS,
                     INSTANCE_ID_SINGLE, FSABS_API_ID_READ_CHUNK, FSABS_E_PARAM_POINTER, E_NOT_OK);

    if (FsAbs_Status.mounted == FALSE)
    {
        return E_NOT_OK;
    }

    return FsAbs_PlatformSize(fileName, size);
}

void FsAbs_MainFunction(void)
{
    if ((FsAbs_Initialised == FALSE) || (FsAbs_Status.mounted == FALSE))
    {
        return;
    }

    if (FsAbs_PlatformGetSpace(&FsAbs_Status.capacityMiB, &FsAbs_Status.usedMiB) != E_OK)
    {
        return;
    }
    FsAbs_Status.freeMiB = (FsAbs_Status.capacityMiB > FsAbs_Status.usedMiB)
                               ? (FsAbs_Status.capacityMiB - FsAbs_Status.usedMiB)
                               : 0u;

    /* The backlog behind the cursor, in bytes. Published in the health record, and the field was
     * previously declared and documented but never assigned -- so it read zero always, which is worse
     * than omitting it: it reports an empty buffer on a unit that may be holding a week of records.
     *
     * Only the current file is measured, not every later one. Walking the whole directory to add up the
     * files ahead would make a cyclic function's cost depend on how long the vehicle has been out of
     * coverage, which is exactly when the connectivity task is least able to afford it. The figure is
     * for judging whether the backlog is growing or shrinking, and the current file answers that. */
    {
        uint32 size = 0u;

        if ((FsAbs_Cursor.valid != FALSE) &&
            (FsAbs_PlatformSize(FsAbs_Cursor.fileName, &size) == E_OK))
        {
            FsAbs_Status.unsentBytes =
                (size > FsAbs_Cursor.offset) ? (size - FsAbs_Cursor.offset) : 0u;
        }
        else
        {
            FsAbs_Status.unsentBytes = 0u;
        }
    }

    if (FsAbs_Status.freeMiB < (uint32)FSABS_CRITICAL_SPACE_MIB)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_SPACE_LOW, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
    }
    else
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_SD_SPACE_LOW, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_PASSED));
    }

    if (FsAbs_Status.freeMiB >= (uint32)FSABS_LOW_SPACE_LIMIT_MIB)
    {
        return;
    }

    /* Reclaim the oldest log, but never one the transfer cursor has not yet passed. Deleting unsent
     * data to make room for new data loses information that cannot be recovered, which is the wrong
     * trade for a store-and-forward buffer -- v1 deleted the numerically smallest file name
     * unconditionally, including the one it was still transmitting. */
    {
        char oldest[FSABS_FILENAME_SIZE];

        if (FsAbs_PlatformFindOldestLog(oldest, (uint16)sizeof(oldest)) != E_OK)
        {
            return;
        }

        if ((FsAbs_Cursor.valid != FALSE) && (strcmp(oldest, FsAbs_Cursor.fileName) == 0))
        {
            /* The oldest file is the one being transferred. Nothing can be reclaimed without losing
             * unsent records, so the low-space event stands and the card fills. That is the correct
             * outcome: the alternative is silently discarding data the fleet has not received. */
            return;
        }

        if (FsAbs_PlatformRemove(oldest) == E_OK)
        {
            FsAbs_Status.filesDeleted++;
        }
    }
}

Std_ReturnType FsAbs_GetCursor(FsAbs_CursorType *cursor)
{
    DET_CHECK_RETURN(cursor != NULL_PTR, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_READ_NEXT, FSABS_E_PARAM_POINTER, E_NOT_OK);

    *cursor = FsAbs_Cursor;
    return E_OK;
}

Std_ReturnType FsAbs_GetStatus(FsAbs_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_FSABS, INSTANCE_ID_SINGLE,
                     FSABS_API_ID_HOUSEKEEP, FSABS_E_PARAM_POINTER, E_NOT_OK);

    *status = FsAbs_Status;
    return E_OK;
}

void FsAbs_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = FSABS_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_FSABS;
        versioninfo->sw_major_version = FSABS_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = FSABS_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = FSABS_SW_PATCH_VERSION;
    }
}

/**
 * @file    Fee.h
 * @brief   AUTOSAR Flash EEPROM Emulation (SWS_FEE).
 *
 * Turns two 4 KiB flash sectors into a small set of individually rewritable blocks. NOR
 * flash cannot rewrite a byte in place -- it erases only in whole sectors and programs only
 * by clearing bits -- so a block update is implemented as a new append-only *record*, and
 * the newest valid record for a block is the current value.
 *
 * @par The guarantee this module provides
 * After a supply loss at any instant during a write, the next boot reads either the new
 * value or the previous one. Never a blend of the two, and never nothing.
 *
 * That is the property v1 lacked, and it is why the odometer reading was unreliable. v1
 * stored the distance as text on the SD card and as an NVS double, re-read the text with
 * @c atol() -- which parses an integer and therefore discarded the fraction on every boot,
 * compounding the loss because the truncated value was written straight back -- and had no
 * integrity check on either copy, so a partial write during the brown-out that accompanies
 * engine cranking read back as a plausible number.
 *
 * @par How crash safety is achieved
 * A record is committed by a single write that clears bits in its one-byte state field,
 * *after* both its header and its payload are on the media:
 *
 *   1. Write the record header with @c state left at 0xFF (erased). The header carries its
 *      own CRC-16, the block id, the payload length and a write counter.
 *   2. Write the payload, followed by its CRC-32.
 *   3. Write @c state = 0xFC. This single byte is the commit point.
 *
 * A record interrupted before step 3 is found with @c state = 0xFF and is skipped as
 * incomplete; the previous record for that block is still present and still the newest
 * valid one. Because flash programming can only clear bits, step 3 needs no erase and
 * cannot itself be partially applied in a way that produces a different valid state.
 *
 * @par Garbage collection, also crash safe
 * When the active sector cannot fit another record, the newest valid record of every block
 * is copied to the other sector and the old one is erased. The order matters:
 *
 *   1. Erase the target sector.
 *   2. Copy the records.
 *   3. Write the target's sector header, with a sequence number one higher than the
 *      source's. **This is the commit point.**
 *   4. Erase the source sector.
 *
 * A crash before step 3 leaves the target without a valid header, so the source is still
 * the active sector and nothing is lost. A crash between 3 and 4 leaves two valid sectors,
 * and the higher sequence number identifies the winner; the loser is reclaimed on the next
 * start. At no point is there a window in which neither sector holds the data.
 *
 * @req SWREQ-NVM-0010 .. SWREQ-NVM-0025
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FEE_H
#define FEE_H

#include "base/Autosar_ModuleIds.h"
#include "services/Fee/Fee_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FEE_VENDOR_ID 0xFFFEu
#define FEE_AR_RELEASE_MAJOR_VERSION 4u
#define FEE_AR_RELEASE_MINOR_VERSION 4u
#define FEE_SW_MAJOR_VERSION 2u
#define FEE_SW_MINOR_VERSION 0u
#define FEE_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  API service IDs
 *================================================================================================*/

#define FEE_API_ID_INIT 0x00u
#define FEE_API_ID_READ 0x02u
#define FEE_API_ID_WRITE 0x03u
#define FEE_API_ID_INVALIDATE 0x05u
#define FEE_API_ID_GC 0x20u
#define FEE_API_ID_GET_STATUS 0x21u

/*==================================================================================================
 *  Error codes
 *================================================================================================*/

#define FEE_E_UNINIT E_UNINIT
#define FEE_E_PARAM_POINTER E_PARAM_POINTER
#define FEE_E_INVALID_BLOCK_NO 0x20u   /**< Block id not in the configuration.      */
#define FEE_E_INVALID_LENGTH 0x21u     /**< Length exceeds the block's configured size.*/
#define FEE_E_BLOCK_INVALID 0x22u      /**< Block was explicitly invalidated.       */
#define FEE_E_BLOCK_INCONSISTENT 0x23u /**< Every record for the block failed its CRC.*/
#define FEE_E_NO_SPACE 0x24u           /**< Both sectors full; GC could not help.   */
#define FEE_E_MEDIA_FAILURE 0x25u      /**< The flash driver reported an error.     */
#define FEE_E_LAYOUT_CORRUPT 0x26u     /**< Neither sector has a usable header.     */

/*==================================================================================================
 *  On-media constants
 *
 *  Published because the layout is part of the product's data compatibility story: a
 *  firmware update that changes them silently would make every field unit's odometer
 *  unreadable. ::FEE_FORMAT_VERSION exists so that such a change is detected and handled
 *  rather than misread.
 *================================================================================================*/

/** Identifies a Fee-formatted sector. ASCII "FEE1". */
#define FEE_SECTOR_MAGIC 0x46454531uL

/**
 * @brief On-media layout version.
 *
 * Bump this whenever the record or sector header changes. ::Fee_Init treats a sector whose
 * version it does not recognise as unformatted rather than parsing it with the wrong
 * layout, so old data is discarded cleanly instead of being decoded into nonsense.
 */
#define FEE_FORMAT_VERSION 1u

/** Bytes occupied by a sector header. */
#define FEE_SECTOR_HEADER_SIZE 16u

/** Bytes occupied by a record header. */
#define FEE_RECORD_HEADER_SIZE 16u

/** Record state: the slot has never been written. */
#define FEE_RECORD_STATE_ERASED 0xFFu

/** Record state: header and payload are complete and committed. */
#define FEE_RECORD_STATE_VALID 0xFCu

/** Record state: superseded or explicitly invalidated. */
#define FEE_RECORD_STATE_INVALID 0xF0u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Configured block identifier. Values are defined in Fee_Cfg.h. */
typedef uint16 Fee_BlockIdType;

/** Health and wear information, published as diagnostic data. */
typedef struct
{
    uint32 writeCount;          /**< Records committed since Init.                     */
    uint32 gcCount;             /**< Garbage collection passes performed.              */
    uint32 crcFailureCount;     /**< Records rejected by their payload CRC.            */
    uint32 incompleteRecordCount;/**< Records found uncommitted -- interrupted writes.  */
    uint32 mediaErrorCount;      /**< Flash driver failures encountered.               */
    uint16 activeSector;         /**< Index of the sector currently being appended to.  */
    uint16 activeSequence;       /**< Sequence number of the active sector.            */
    uint32 bytesUsed;            /**< Bytes consumed in the active sector.              */
    uint32 bytesFree;            /**< Bytes still appendable before GC is needed.       */
    boolean layoutRecovered;     /**< TRUE if Init had to repair an interrupted GC.     */
} Fee_StatusType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Mount the emulated EEPROM, choosing and if necessary repairing the active sector.
 *
 * Examines both sector headers. If neither is valid the partition is formatted, which is the
 * expected path on a virgin unit. If both are valid -- the signature of a garbage collection
 * interrupted by a supply loss -- the higher sequence number wins and the loser is erased.
 *
 * @return E_OK if a usable active sector exists afterwards; E_NOT_OK if the media itself is
 *         failing, in which case every later read reports E_NOT_OK and NvM falls back to its
 *         configured defaults rather than publishing uninitialised RAM.
 */
CHECK_RETURN Std_ReturnType Fee_Init(void);

/**
 * @brief Read the current value of @p blockId.
 *
 * Returns the newest committed record whose payload CRC verifies. If the newest record fails
 * its CRC, the next newest is used -- which is what makes a single corrupted write
 * survivable rather than fatal, and is the reason superseded records are not erased eagerly.
 *
 * @param[in]  blockId Configured block.
 * @param[out] buffer  Destination.
 * @param[in]  offset  Byte offset within the block. 0 reads from the start.
 * @param[in]  length  Bytes to read.
 * @return E_OK on success; ::FEE_E_BLOCK_INCONSISTENT as E_NOT_OK if no record verifies;
 *         E_NOT_FOUND if the block has never been written.
 */
CHECK_RETURN Std_ReturnType Fee_ReadBlock(Fee_BlockIdType blockId, uint8 *buffer, uint16 offset,
                                          uint16 length);

/**
 * @brief Append a new record for @p blockId.
 *
 * Runs garbage collection first if the active sector cannot fit the record. Synchronous: the
 * call returns only once the commit byte is on the media, so a caller that sees E_OK knows
 * the value survives a reset. AUTOSAR permits an asynchronous Fee; a synchronous one is
 * chosen here because the whole point of this module is that the caller can reason about
 * durability, and an asynchronous commit would put a window between "returned E_OK" and
 * "actually durable" exactly where the odometer cares about it.
 *
 * @param[in] blockId Configured block.
 * @param[in] buffer  Source, exactly the block's configured length.
 * @return E_OK once committed; ::FEE_E_NO_SPACE as E_NO_SPACE if GC could not free room;
 *         E_NOT_OK on a media failure or a parameter error.
 */
CHECK_RETURN Std_ReturnType Fee_WriteBlock(Fee_BlockIdType blockId, const uint8 *buffer);

/**
 * @brief Mark @p blockId invalid, so later reads report it as absent.
 *
 * Used to retire a calibration that a diagnostic session has cleared. Appends an
 * invalidation record rather than erasing, so the operation is itself crash safe.
 */
CHECK_RETURN Std_ReturnType Fee_InvalidateBlock(Fee_BlockIdType blockId);

/**
 * @brief Run garbage collection now.
 *
 * Normally driven automatically by ::Fee_WriteBlock. Exposed so that a maintenance action can
 * reclaim space at a chosen moment rather than during a write, and so that the tests can
 * drive it directly.
 */
CHECK_RETURN Std_ReturnType Fee_GarbageCollect(void);

/**
 * @brief Read health and wear information.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Fee_GetStatus(Fee_StatusType *status);

/**
 * @brief Erase both sectors and write a fresh sector header.
 *
 * Destroys every stored block. Reachable only from a diagnostic routine that requires an
 * explicit confirmation, and from the tests.
 */
CHECK_RETURN Std_ReturnType Fee_Format(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Fee_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* FEE_H */

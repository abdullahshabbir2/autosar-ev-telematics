/**
 * @file    NvM.h
 * @brief   AUTOSAR NVRAM Manager (SWS_NVM).
 *
 * Presents each persistent quantity as a RAM mirror that the application reads and writes
 * freely, and takes responsibility for getting it to and from flash intact. Sits on Fee,
 * which provides the crash-safe append-only record store.
 *
 * @par What NvM adds on top of Fee
 *  - **A RAM mirror per block.** The application never waits on flash to read a value, and a
 *    read cannot fail part way through a drive.
 *  - **An end-to-end CRC.** Fee already checksums what it stores, but that only proves the
 *    media is intact. NvM appends its own CRC-32 over the payload *before* handing it to Fee
 *    and verifies it *after* reading back, so a corruption anywhere in the path -- including a
 *    RAM bit flip while the mirror sat idle -- is caught rather than persisted.
 *  - **Configured defaults.** A block that has never been written, or whose every stored copy
 *    fails its CRC, is populated from a compiled-in default. Nothing ever publishes
 *    uninitialised memory.
 *  - **Write-on-change.** A block is only pushed to flash when its contents actually differ
 *    from what was last stored. This is what keeps flash wear proportional to real change
 *    rather than to loop rate.
 *
 * @par Immediate versus deferred
 * Most blocks are deferred: marked dirty, then flushed by ::NvM_MainFunction at a rate that
 * suits the media. The odometer is written immediately, because the value that matters is the
 * one that survives the next unexpected power cut, and a deferred write is exactly the window
 * in which that power cut happens.
 *
 * @req SWREQ-NVM-0001 .. SWREQ-NVM-0009, SWREQ-NVM-0030 .. SWREQ-NVM-0038
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef NVM_H
#define NVM_H

#include "Autosar_ModuleIds.h"
#include "NvM_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVM_VENDOR_ID 0xFFFEu
#define NVM_AR_RELEASE_MAJOR_VERSION 4u
#define NVM_AR_RELEASE_MINOR_VERSION 4u
#define NVM_SW_MAJOR_VERSION 2u
#define NVM_SW_MINOR_VERSION 0u
#define NVM_SW_PATCH_VERSION 0u

#define NVM_API_ID_INIT 0x00u
#define NVM_API_ID_READ_BLOCK 0x06u
#define NVM_API_ID_WRITE_BLOCK 0x07u
#define NVM_API_ID_RESTORE_DEFAULTS 0x0Cu
#define NVM_API_ID_WRITE_ALL 0x0Du
#define NVM_API_ID_MAIN_FUNCTION 0x0Eu

#define NVM_E_UNINIT E_UNINIT
#define NVM_E_PARAM_POINTER E_PARAM_POINTER
#define NVM_E_PARAM_BLOCK_ID 0x20u
#define NVM_E_INTEGRITY_FAILED 0x21u
#define NVM_E_WRITE_FAILED 0x22u
#define NVM_E_DEFAULTS_APPLIED 0x23u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Logical NvM block. Values are defined in NvM_Cfg.h. */
typedef uint8 NvM_BlockIdType;

/** Outcome of the most recent operation on a block (SWS_NvM_00470). */
typedef enum
{
    NVM_REQ_OK = 0,              /**< Last operation succeeded.                         */
    NVM_REQ_NOT_OK = 1,          /**< Last operation failed.                            */
    NVM_REQ_PENDING = 2,         /**< A deferred write is queued.                       */
    NVM_REQ_INTEGRITY_FAILED = 3,/**< Stored copy failed its CRC.                       */
    NVM_REQ_BLOCK_SKIPPED = 4,   /**< Nothing to do: contents unchanged.                */
    NVM_REQ_RESTORED_DEFAULTS = 5/**< Block was absent or damaged; defaults are in use.  */
} NvM_RequestResultType;

/** Aggregate counters, published in the telemetry health record. */
typedef struct
{
    uint32 writeCount;         /**< Blocks actually pushed to flash.             */
    uint32 skippedWriteCount;  /**< Writes suppressed because nothing changed.    */
    uint32 readCount;          /**< Blocks loaded from flash at start.            */
    uint32 integrityFailures;  /**< Stored copies that failed their CRC.          */
    uint32 defaultsApplied;    /**< Blocks that fell back to their default.       */
    uint32 writeFailures;      /**< Writes the media rejected.                    */
    uint8 dirtyBlockCount;     /**< Blocks currently awaiting a deferred write.   */
} NvM_StatisticsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Load every configured block into its RAM mirror.
 *
 * Runs once during startup, after ::Fee_Init. A block that is absent or whose stored copy fails
 * its CRC is populated from its configured default and counted, so the condition is visible in
 * diagnostics instead of merely being survived.
 *
 * @return E_OK if every block ended up with usable contents, which includes blocks that fell
 *         back to defaults; E_NOT_OK only if the underlying store is unusable.
 */
CHECK_RETURN Std_ReturnType NvM_Init(void);

/**
 * @brief Copy @p blockId's RAM mirror into @p destination.
 *
 * Never touches flash, so it is cheap enough to call from a cyclic runnable.
 *
 * @param[in]  blockId     Configured block.
 * @param[out] destination Buffer of at least the block's configured length.
 * @return E_OK on success; E_NOT_OK for an unknown block or a NULL pointer.
 */
CHECK_RETURN Std_ReturnType NvM_ReadBlock(NvM_BlockIdType blockId, void *destination);

/**
 * @brief Update @p blockId's RAM mirror from @p source and mark it for writing.
 *
 * If the new contents are identical to what is already stored, the block is not marked dirty
 * and ::NVM_REQ_BLOCK_SKIPPED is recorded. That comparison is the whole wear-management
 * strategy: the odometer runnable may call this every cycle, and only the cycles where the
 * vehicle actually moved reach the flash.
 *
 * For a block configured as immediate, the write happens before this call returns.
 *
 * @return E_OK on success; E_NOT_OK for an unknown block, a NULL pointer, or a failed
 *         immediate write.
 */
CHECK_RETURN Std_ReturnType NvM_WriteBlock(NvM_BlockIdType blockId, const void *source);

/**
 * @brief Flush @p blockId to flash now, regardless of its configured write mode.
 *
 * Used by EcuM during shutdown and by the odometer at the end of a trip.
 *
 * @return E_OK on success, or if the block was already clean; E_NOT_OK on a media failure.
 */
CHECK_RETURN Std_ReturnType NvM_WriteImmediate(NvM_BlockIdType blockId);

/**
 * @brief Flush every dirty block.
 *
 * Called by EcuM on the shutdown path. Attempts all blocks even if one fails, because losing
 * one block is better than abandoning the rest.
 *
 * @return E_OK if every dirty block was written; E_NOT_OK if any failed.
 */
CHECK_RETURN Std_ReturnType NvM_WriteAll(void);

/**
 * @brief Overwrite @p blockId's RAM mirror with its configured default and mark it dirty.
 *
 * Reachable from the diagnostic channel, so a technician can return a miscalibrated unit to a
 * known state without reflashing it.
 */
CHECK_RETURN Std_ReturnType NvM_RestoreBlockDefaults(NvM_BlockIdType blockId);

/**
 * @brief Outcome of the most recent operation on @p blockId.
 */
NvM_RequestResultType NvM_GetErrorStatus(NvM_BlockIdType blockId);

/**
 * @brief Write at most one dirty deferred block.
 *
 * Driven cyclically by SchM. One block per invocation deliberately: a flash write can hold the
 * SPI bus and stall the CAN reader, so spreading the work keeps the worst-case execution time
 * of a single scheduler slot bounded.
 */
void NvM_MainFunction(void);

/**
 * @brief Read the aggregate counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType NvM_GetStatistics(NvM_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void NvM_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* NVM_H */

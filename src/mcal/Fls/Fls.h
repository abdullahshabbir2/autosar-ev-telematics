/**
 * @file    Fls.h
 * @brief   AUTOSAR Flash driver (SWS_FLSDriver) -- raw internal flash partition.
 *
 * Byte-addressable read, page write and sector erase over a dedicated flash partition.
 * The partition is raw: no filesystem, no key-value store. Fee builds EEPROM emulation
 * on top of it and NvM builds redundant, CRC-protected blocks on top of that.
 *
 * @par Why raw flash rather than the NVS key-value store
 * v1 kept the odometer reading in Arduino @c Preferences (ESP-IDF NVS) and a copy in a
 * text file on the SD card, and both paths lost data:
 *
 *  - The SD copy was read back with @c atol(), which parses an integer. A stored
 *    reading of @c 1234.56 came back as @c 1234. Every boot truncated the odometer,
 *    and because the truncated value was then written back, the loss compounded. On a
 *    vehicle rebooting a few times a day this silently destroys the reading that the
 *    entire product exists to produce.
 *  - The NVS copy was written with @c putDouble() and read with @c getDouble() into a
 *    @c float, and @c put_dist_preferences() assigned the *return value* of
 *    @c putDouble -- the number of bytes written -- back over its own @p distance
 *    parameter. Harmless only because the parameter was passed by value.
 *
 *  Neither path had an integrity check, so a partial write during the brown-out that
 *  accompanies engine cranking was indistinguishable from a valid reading.
 *
 * Owning the media outright is what makes the guarantee in NvM possible: two physical
 * copies of every block, each with its own CRC, written in a defined order so that a
 * supply loss at any instant leaves at least one intact. That cannot be expressed
 * through a key-value API that reserves the right to relocate and rewrite entries.
 *
 * @par Flash geometry this driver assumes
 * ESP32 internal flash erases in 4096-byte sectors and programs in pages, and an erased
 * cell reads 0xFF. A write can only clear bits, so a byte may be overwritten from 0xFF
 * to any value, and from any value only to one with fewer set bits, without an erase.
 * Fee's record marking depends on that property.
 *
 * @req SWREQ-NVM-0001 .. SWREQ-NVM-0004
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FLS_H
#define FLS_H

#include "Autosar_ModuleIds.h"
#include "Fls_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLS_VENDOR_ID 0xFFFEu
#define FLS_AR_RELEASE_MAJOR_VERSION 4u
#define FLS_AR_RELEASE_MINOR_VERSION 4u
#define FLS_SW_MAJOR_VERSION 2u
#define FLS_SW_MINOR_VERSION 0u
#define FLS_SW_PATCH_VERSION 0u

#define FLS_API_ID_INIT 0x00u
#define FLS_API_ID_ERASE 0x01u
#define FLS_API_ID_WRITE 0x02u
#define FLS_API_ID_READ 0x07u
#define FLS_API_ID_COMPARE 0x08u
#define FLS_API_ID_BLANK_CHECK 0x0Au

#define FLS_E_UNINIT E_UNINIT
#define FLS_E_PARAM_ADDRESS 0x21u
#define FLS_E_PARAM_LENGTH 0x22u
#define FLS_E_PARAM_DATA 0x23u
#define FLS_E_WRITE_FAILED 0x24u
#define FLS_E_ERASE_FAILED 0x25u
#define FLS_E_READ_FAILED 0x26u
#define FLS_E_UNALIGNED 0x27u
#define FLS_E_VERIFY_FAILED 0x28u

/** Offset within the managed partition, in bytes. Not an absolute flash address. */
typedef uint32 Fls_AddressType;

/** A transfer length in bytes. */
typedef uint32 Fls_LengthType;

/** Driver job result, in the AUTOSAR sense. */
typedef enum
{
    FLS_JOB_OK = 0,      /**< Last job completed successfully. */
    FLS_JOB_PENDING = 1, /**< A job is in progress.            */
    FLS_JOB_FAILED = 2   /**< Last job failed.                 */
} Fls_JobResultType;

/** Wear and error counters, published as diagnostic data. */
typedef struct
{
    uint32 readCount;       /**< Completed read operations.                    */
    uint32 writeCount;      /**< Completed write operations.                   */
    uint32 eraseCount;      /**< Completed sector erases -- the wear figure.    */
    uint32 bytesWritten;    /**< Total bytes programmed since first boot.      */
    uint32 writeFailures;   /**< Writes the media rejected.                    */
    uint32 verifyFailures;  /**< Writes that read back differently.            */
} Fls_StatisticsType;

/**
 * @brief Locate the managed partition and verify its geometry.
 *
 * @return E_OK on success; E_NOT_OK if the partition is missing or its size is not a
 *         whole number of sectors, which is a build configuration fault rather than a
 *         runtime one and is reported as ::FLS_E_PARAM_CONFIG.
 */
CHECK_RETURN Std_ReturnType Fls_Init(void);

/**
 * @brief Read @p length bytes from @p address into @p buffer.
 *
 * Unaligned and arbitrary lengths are permitted: flash reads have no alignment
 * constraint on this part.
 *
 * @return E_OK on success; E_NOT_OK for a range that leaves the partition, a NULL
 *         @p buffer, or a media read error.
 */
CHECK_RETURN Std_ReturnType Fls_Read(Fls_AddressType address, uint8 *buffer,
                                     Fls_LengthType length);

/**
 * @brief Program @p length bytes from @p buffer at @p address.
 *
 * @param address Must be a multiple of ::FLS_WRITE_ALIGNMENT.
 * @param length  Must be a multiple of ::FLS_WRITE_ALIGNMENT.
 *
 * The target range must already be erased. This driver does not erase implicitly:
 * hiding an erase inside a write is how a 4 KiB sector gets destroyed to update two
 * bytes, and Fee needs to control exactly when erases happen.
 *
 * Every write is read back and compared before returning (::FLS_VERIFY_AFTER_WRITE),
 * so a cell that no longer programs is detected at the moment it is written rather than
 * on the next boot.
 *
 * @return E_OK on success; ::FLS_E_UNALIGNED, ::FLS_E_WRITE_FAILED or
 *         ::FLS_E_VERIFY_FAILED as applicable.
 */
CHECK_RETURN Std_ReturnType Fls_Write(Fls_AddressType address, const uint8 *buffer,
                                      Fls_LengthType length);

/**
 * @brief Erase whole sectors, setting every byte to 0xFF.
 *
 * @param address Must be sector-aligned.
 * @param length  Must be a whole multiple of ::FLS_SECTOR_SIZE.
 */
CHECK_RETURN Std_ReturnType Fls_Erase(Fls_AddressType address, Fls_LengthType length);

/**
 * @brief Compare flash content against @p buffer without copying it out.
 * @return E_OK if identical; E_NOT_OK if different or on a parameter error.
 */
CHECK_RETURN Std_ReturnType Fls_Compare(Fls_AddressType address, const uint8 *buffer,
                                        Fls_LengthType length);

/**
 * @brief Check that a range is fully erased.
 *
 * Used by Fee before choosing a sector to write, because programming a non-erased cell
 * succeeds partially and produces data that passes a naive read but fails its CRC.
 *
 * @return E_OK if every byte reads 0xFF; E_NOT_OK otherwise.
 */
CHECK_RETURN Std_ReturnType Fls_BlankCheck(Fls_AddressType address, Fls_LengthType length);

/** Result of the most recent job. */
Fls_JobResultType Fls_GetJobResult(void);

/** Size of the managed partition in bytes, or 0 before ::Fls_Init. */
Fls_LengthType Fls_GetPartitionSize(void);

/**
 * @brief Read the wear and error counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Fls_GetStatistics(Fls_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Fls_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* FLS_H */

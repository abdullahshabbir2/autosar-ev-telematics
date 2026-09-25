/**
 * @file    Crc.h
 * @brief   AUTOSAR CRC Library (SWS_CRCLibrary).
 *
 * Supplies the six CRC profiles required by AUTOSAR. Three of them carry real
 * responsibility in this ECU:
 *
 *  - ::Crc_CalculateCRC16 is the frame check of the RS485 battery protocol
 *    (CRC-16/CCITT-FALSE). See [08-protocols.md.](docs/08-protocols.md.)
 *  - ::Crc_CalculateCRC32 protects every record written to the SD card and every
 *    NvM block, so that a supply brown-out mid-write is detected on the next boot
 *    instead of being read back as plausible garbage.
 *  - ::Crc_CalculateCRC8H2F protects the compact telemetry frame header.
 *
 * @par Streaming use
 * Every routine takes a start value and an @c isFirstCall flag, so a CRC can be
 * accumulated across several buffers without materialising a contiguous copy.
 * For the first block pass @c TRUE (the start value is then ignored and the
 * profile's initial value is used); for each continuation pass @c FALSE together
 * with the value returned by the previous call. This is what lets FsAbs checksum a
 * 4 KiB SD record while streaming it through a 256-byte stack buffer.
 *
 * @par Implementation choice
 * CRC8, CRC8H2F, CRC16 and CRC16ARC are computed bit-at-a-time: their payloads
 * here are at most 81 bytes at 0.33 Hz, so the ~8x cost over a table lookup is
 * immaterial against the 1 KiB of flash a table would cost. CRC32 and CRC32P4 are
 * table-driven because they run over whole SD records and OTA images, where
 * throughput does matter. Both variants are covered by the same test vectors.
 *
 * @req SWREQ-INT-0010, SWREQ-INT-0011, SWREQ-INT-0012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef CRC_H
#define CRC_H

#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *  Published information
 *================================================================================================*/

#define CRC_VENDOR_ID 0xFFFEu
#define CRC_MODULE_ID MODULE_ID_CRC

#define CRC_AR_RELEASE_MAJOR_VERSION 4u
#define CRC_AR_RELEASE_MINOR_VERSION 4u
#define CRC_AR_RELEASE_REVISION_VERSION 0u

#define CRC_SW_MAJOR_VERSION 2u
#define CRC_SW_MINOR_VERSION 0u
#define CRC_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  Profile parameters
 *
 *  Exposed so that callers and tests can name the initial value instead of
 *  repeating a magic constant, and so that a protocol document can be checked
 *  against the code by grep.
 *================================================================================================*/

#define CRC8_INITIAL_VALUE 0xFFu           /**< SAE J1850, poly 0x1D, final XOR 0xFF.  */
#define CRC8H2F_INITIAL_VALUE 0xFFu        /**< poly 0x2F, final XOR 0xFF.             */
#define CRC16_INITIAL_VALUE 0xFFFFu        /**< CCITT-FALSE, poly 0x1021, no final XOR.*/
#define CRC16ARC_INITIAL_VALUE 0x0000u     /**< ARC, reflected poly 0x8005.            */
#define CRC32_INITIAL_VALUE 0xFFFFFFFFuL   /**< Ethernet, reflected poly 0x04C11DB7. */
#define CRC32P4_INITIAL_VALUE 0xFFFFFFFFuL /**< reflected poly 0xF4ACFB13.           */

/** CRC-16/CCITT-FALSE of an empty buffer -- the value a zero-length frame yields. */
#define CRC16_EMPTY_RESULT 0xFFFFu

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Compute a CRC-8 (SAE J1850) over a byte range.
 *
 * @param[in] dataPtr      First byte. May be NULL_PTR only when @p length is 0.
 * @param[in] length       Number of bytes to process.
 * @param[in] startValue   Accumulated CRC from the previous call; ignored when
 *                         @p isFirstCall is TRUE.
 * @param[in] isFirstCall  TRUE to begin a new CRC, FALSE to continue one.
 * @return    The CRC over the range.
 *
 * @note Reentrant. No side effects. Safe to call from an ISR.
 */
uint8 Crc_CalculateCRC8(const uint8 *dataPtr, uint32 length, uint8 startValue, boolean isFirstCall);

/**
 * @brief Compute a CRC-8 with polynomial 0x2F ("H2F") over a byte range.
 * @copydetails Crc_CalculateCRC8
 */
uint8 Crc_CalculateCRC8H2F(const uint8 *dataPtr, uint32 length, uint8 startValue, boolean isFirstCall);

/**
 * @brief Compute a CRC-16/CCITT-FALSE over a byte range.
 *
 * This is the RS485 battery-bus frame check. The bus transmits the CRC
 * most-significant byte first; see Rs485If_FrameAppendCrc().
 *
 * @copydetails Crc_CalculateCRC8
 */
uint16 Crc_CalculateCRC16(const uint8 *dataPtr, uint32 length, uint16 startValue, boolean isFirstCall);

/**
 * @brief Compute a CRC-16/ARC (reflected, zero-initialised) over a byte range.
 * @copydetails Crc_CalculateCRC8
 */
uint16 Crc_CalculateCRC16ARC(const uint8 *dataPtr, uint32 length, uint16 startValue, boolean isFirstCall);

/**
 * @brief Compute a CRC-32 (IEEE 802.3) over a byte range.
 *
 * Used for SD record integrity and NvM block integrity.
 * @copydetails Crc_CalculateCRC8
 */
uint32 Crc_CalculateCRC32(const uint8 *dataPtr, uint32 length, uint32 startValue, boolean isFirstCall);

/**
 * @brief Compute a CRC-32/P4 (polynomial 0xF4ACFB13) over a byte range.
 *
 * Preferred over ::Crc_CalculateCRC32 for payloads above roughly 1 KiB because its
 * Hamming distance stays at 6 where the Ethernet polynomial degrades to 4.
 * @copydetails Crc_CalculateCRC8
 */
uint32 Crc_CalculateCRC32P4(const uint8 *dataPtr, uint32 length, uint32 startValue, boolean isFirstCall);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Crc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* CRC_H */

/**
 * @file    Fee_Cfg.h
 * @brief   Flash EEPROM emulation block configuration.
 *
 * Every persistent quantity in the ECU is declared here with a fixed identifier and a fixed
 * length. Both are part of the on-media contract: changing a block's length without bumping
 * ::FEE_FORMAT_VERSION would make existing field units misread that block.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FEE_CFG_H
#define FEE_CFG_H

#include "Fls_Cfg.h"
#include "Std_Types.h"

#define FEE_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Geometry
 *================================================================================================*/

/** Sectors under Fee's control. Two, so garbage collection can ping-pong between them. */
#define FEE_SECTOR_COUNT 2u

/** Bytes per sector, from the flash driver's geometry. */
#define FEE_SECTOR_SIZE FLS_SECTOR_SIZE

/** Write alignment every record must respect. */
#define FEE_ALIGNMENT FLS_WRITE_ALIGNMENT

/*==================================================================================================
 *  Block identifiers
 *
 *  Identifiers are assigned once and never reused. A retired block's id stays reserved,
 *  because an old unit's media may still contain records carrying it and reusing the number
 *  would make those records decode as the new block.
 *================================================================================================*/

#define FEE_BLOCK_ODOMETER ((Fee_BlockIdType)0x0001u)    /**< Accumulated distance + trip.   */
#define FEE_BLOCK_DEVICE_CONFIG ((Fee_BlockIdType)0x0002u)/**< Device id, broker address.     */
#define FEE_BLOCK_CALIBRATION ((Fee_BlockIdType)0x0003u) /**< Gear ratio, tyre size, trims.  */
#define FEE_BLOCK_RESTART_INFO ((Fee_BlockIdType)0x0004u)/**< Crash-loop detector state.     */
#define FEE_BLOCK_DTC_STORE ((Fee_BlockIdType)0x0005u)   /**< Persistent diagnostic records.  */
#define FEE_BLOCK_TELEMETRY_CURSOR ((Fee_BlockIdType)0x0006u)/**< Store-and-forward position. */
#define FEE_BLOCK_ENERGY_COUNTERS ((Fee_BlockIdType)0x0007u)/**< Lifetime Wh in and out.     */

/** Highest identifier in use. Reads and writes outside 1 .. this are rejected. */
#define FEE_BLOCK_ID_MAX FEE_BLOCK_ENERGY_COUNTERS

/** Number of configured blocks. */
#define FEE_BLOCK_COUNT 7u

/*==================================================================================================
 *  Block lengths, in bytes
 *
 *  Each is a multiple of ::FEE_ALIGNMENT so no padding arithmetic is needed at the call site.
 *  Sizes include each structure's own CRC where NvM adds one.
 *================================================================================================*/

#define FEE_LENGTH_ODOMETER 32u
#define FEE_LENGTH_DEVICE_CONFIG 64u
#define FEE_LENGTH_CALIBRATION 32u
#define FEE_LENGTH_RESTART_INFO 16u
#define FEE_LENGTH_DTC_STORE 256u
#define FEE_LENGTH_TELEMETRY_CURSOR 32u
#define FEE_LENGTH_ENERGY_COUNTERS 32u

/** Longest configured block; sizes Fee's internal staging buffer. */
#define FEE_MAX_BLOCK_LENGTH FEE_LENGTH_DTC_STORE

/*==================================================================================================
 *  Garbage collection
 *================================================================================================*/

/**
 * @brief Free bytes below which a write triggers garbage collection pre-emptively.
 *
 * Twice the longest record (header plus payload), so a write of the largest block never finds
 * itself with nowhere to go. Collecting slightly early costs one extra erase over the unit's
 * life and removes a class of "the write failed only when the sector happened to be nearly
 * full" fault that is very hard to reproduce.
 */
#define FEE_GC_THRESHOLD_BYTES ((FEE_MAX_BLOCK_LENGTH + FEE_RECORD_HEADER_SIZE) * 2u)

/**
 * @brief Erase cycles per sector at which a wear warning is raised.
 *
 * 40 000 per sector, half the 100 000 endurance figure with margin. With write-on-change and
 * the odometer as the most frequent writer, the expected rate is a few records a day, so a
 * sector fills roughly monthly -- decades of service. The event exists to catch a
 * configuration mistake that turns write-on-change into write-every-cycle, which would burn
 * through the endurance in weeks.
 */
#define FEE_WEAR_WARNING_CYCLES 40000uL

#endif /* FEE_CFG_H */

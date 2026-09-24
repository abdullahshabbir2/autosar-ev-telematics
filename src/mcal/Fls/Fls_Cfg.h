/**
 * @file    Fls_Cfg.h
 * @brief   Flash driver configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef FLS_CFG_H
#define FLS_CFG_H

#include "base/Std_Types.h"

#define FLS_DEV_ERROR_DETECT STD_ON

/**
 * @brief Name of the flash partition this driver owns.
 *
 * Declared in partitions/odo_partitions.csv as an 8 KiB data partition of custom
 * subtype 0x40. It must not be shared with NVS or SPIFFS: those relocate data as they
 * see fit, which would break Fee's assumption that a written record stays where it was
 * put.
 */
#define FLS_PARTITION_NAME "nvdata"

/** Custom partition subtype reserved for Fee's use. */
#define FLS_PARTITION_SUBTYPE 0x40u

/** Erase granularity of ESP32 internal flash, in bytes. */
#define FLS_SECTOR_SIZE 4096u

/** Sectors in the managed partition. Two, so Fee can ping-pong between them. */
#define FLS_SECTOR_COUNT 2u

/** Total managed size, in bytes. */
#define FLS_PARTITION_SIZE (FLS_SECTOR_SIZE * FLS_SECTOR_COUNT)

/**
 * @brief Write alignment, in bytes.
 *
 * 4. ESP-IDF's @c esp_partition_write requires word-aligned offsets and lengths on this
 * part. Fee pads every record up to this boundary rather than discovering the
 * constraint at runtime.
 */
#define FLS_WRITE_ALIGNMENT 4u

/** Value an erased flash cell reads back as. */
#define FLS_ERASED_VALUE 0xFFu

/**
 * @brief Read back and compare after every write.
 *
 * On. Costs one extra read per write, which is irrelevant at the handful of writes per
 * minute this ECU performs, and converts a worn-out cell from a silent corruption on
 * some future boot into an immediate ::FLS_E_VERIFY_FAILED that NvM can react to by
 * switching to the redundant copy.
 */
#define FLS_VERIFY_AFTER_WRITE STD_ON

/**
 * @brief Erase cycles per sector at which the wear-warning event is raised.
 *
 * 90 000, against a specified endurance of 100 000. Raising the event at 90 % leaves
 * margin for the unit to be serviced before writes actually start failing. Fee's
 * ping-pong layout plus NvM's write-on-change policy mean the expected rate is a few
 * erases per day, so this is decades of service -- the event exists to catch a
 * configuration fault that turns a write-on-change into a write-every-cycle, which is
 * a very easy mistake to make and an expensive one to discover late.
 */
#define FLS_WEAR_WARNING_ERASE_COUNT 90000uL

#endif /* FLS_CFG_H */

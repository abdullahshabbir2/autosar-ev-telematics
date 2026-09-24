/**
 * @file    Dem_Cfg.h
 * @brief   Diagnostic event and trouble-code configuration.
 *
 * Every fault this ECU can report is declared here, once, with its event identifier, its
 * ISO 14229 diagnostic trouble code, how many consecutive occurrences confirm it, and whether it
 * survives a power cycle. A fault that is not in this table cannot be reported, which is what
 * stops the diagnostic record from filling with ad-hoc strings.
 *
 * @par DTC numbering
 * Codes follow the ISO 15031-6 / SAE J2012 layout: a two-bit system group, then a five-nibble
 * code. This is a body/telematics ECU, so everything sits in the manufacturer-defined
 * @c U3xxx (network/communication) and @c P1xxx (powertrain, manufacturer-specific) ranges,
 * grouped by subsystem so that a technician reading a code knows which harness to look at before
 * consulting any table:
 *
 *     0x0C01xx   battery pack communication (U3...)
 *     0x0C02xx   vehicle CAN bus
 *     0x0C03xx   backhaul: WiFi, GPRS, broker
 *     0x0D01xx   storage: SD card, flash
 *     0x0D02xx   sensors: GNSS, RTC, analogue
 *     0x0E01xx   internal: watchdog, scheduler, memory
 *
 * The low byte identifies the instance, so the four battery packs share one code and are
 * distinguished by it -- which keeps the table small and makes "pack 3 is not answering"
 * expressible without four near-duplicate entries.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DEM_CFG_H
#define DEM_CFG_H

#include "base/Std_Types.h"

#define DEM_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Event identifiers
 *
 *  Contiguous from 1, because they index the status array directly. 0 is reserved as "no event".
 *================================================================================================*/

#define DEM_EVENT_NONE ((Dem_EventIdType)0u)

/*---------------------------- Battery pack communication --------------------*/
#define DEM_EVENT_PACK_NO_RESPONSE ((Dem_EventIdType)1u)  /**< A pack stopped answering.    */
#define DEM_EVENT_PACK_CRC_FAILURE ((Dem_EventIdType)2u)  /**< Frame check failures rising. */
#define DEM_EVENT_PACK_MISSING ((Dem_EventIdType)3u)      /**< No pack found at startup.    */

/*-------------------------------- Vehicle CAN -------------------------------*/
#define DEM_EVENT_CAN_INIT_FAILED ((Dem_EventIdType)4u)   /**< Controller would not start.  */
#define DEM_EVENT_CAN_BUS_OFF ((Dem_EventIdType)5u)       /**< Controller went bus-off.     */
#define DEM_EVENT_CAN_TIMEOUT ((Dem_EventIdType)6u)       /**< Expected frames stopped.     */

/*--------------------------------- Backhaul ---------------------------------*/
#define DEM_EVENT_WIFI_UNAVAILABLE ((Dem_EventIdType)7u)  /**< No access point reachable.   */
#define DEM_EVENT_GPRS_UNAVAILABLE ((Dem_EventIdType)8u)  /**< Modem could not attach.      */
#define DEM_EVENT_BROKER_UNREACHABLE ((Dem_EventIdType)9u)/**< Broker refused or timed out. */
#define DEM_EVENT_NO_BACKHAUL ((Dem_EventIdType)10u)      /**< Neither bearer available.    */

/*---------------------------------- Storage ---------------------------------*/
#define DEM_EVENT_SD_MOUNT_FAILED ((Dem_EventIdType)11u)  /**< Card absent or unreadable.   */
#define DEM_EVENT_SD_WRITE_FAILED ((Dem_EventIdType)12u)  /**< Writes failing repeatedly.   */
#define DEM_EVENT_SD_SPACE_LOW ((Dem_EventIdType)13u)     /**< Free space below the limit.  */
#define DEM_EVENT_NVM_INTEGRITY ((Dem_EventIdType)14u)    /**< A stored block failed its CRC.*/
#define DEM_EVENT_FLASH_WEAR ((Dem_EventIdType)15u)       /**< Erase count near endurance.  */

/*---------------------------------- Sensors ---------------------------------*/
#define DEM_EVENT_GNSS_NO_FIX ((Dem_EventIdType)16u)      /**< No position for too long.    */
#define DEM_EVENT_RTC_INVALID ((Dem_EventIdType)17u)      /**< Clock implausible or absent. */
#define DEM_EVENT_VBATT_LOW ((Dem_EventIdType)18u)        /**< Auxiliary supply low.        */
#define DEM_EVENT_VBATT_SENSE_FAULT ((Dem_EventIdType)19u)/**< Analogue channel unreadable. */

/*--------------------------------- Internal ---------------------------------*/
#define DEM_EVENT_WDGM_DEADLINE ((Dem_EventIdType)20u)    /**< A runnable missed its deadline.*/
#define DEM_EVENT_WDGM_ALIVE ((Dem_EventIdType)21u)       /**< A runnable stopped checking in.*/
#define DEM_EVENT_TASK_OVERRUN ((Dem_EventIdType)22u)     /**< A task exceeded its budget.  */
#define DEM_EVENT_CRASH_LOOP ((Dem_EventIdType)23u)       /**< Repeated resets detected.    */
#define DEM_EVENT_HEAP_LOW ((Dem_EventIdType)24u)         /**< Free heap below the limit.   */
#define DEM_EVENT_OTA_FAILED ((Dem_EventIdType)25u)       /**< Firmware update rejected.    */

/** Number of configured events, excluding ::DEM_EVENT_NONE. */
#define DEM_EVENT_COUNT 25u

/*==================================================================================================
 *  Storage
 *================================================================================================*/

/**
 * @brief Events whose status is written to NvM and restored on the next start.
 *
 * Not all of them: a transient WiFi outage is not worth a flash write, whereas a confirmed pack
 * failure must still be reportable after the vehicle has been parked overnight. The per-event
 * flag is in the descriptor table in Dem.c.
 */
#define DEM_PERSISTENT_EVENT_COUNT 12u

/**
 * @brief Snapshot records retained per confirmed event.
 *
 * One. A snapshot captures the conditions at the moment a fault confirmed -- speed, voltage,
 * uptime -- which is what makes an intermittent fault diagnosable. Keeping more than the first
 * would cost NvM space for diminishing value, and the first occurrence is the informative one.
 */
#define DEM_SNAPSHOT_RECORDS_PER_EVENT 1u

/**
 * @brief Operation cycles without a failure before a confirmed event is cleared.
 *
 * 40, following the usual automotive convention. An operation cycle here is one power cycle, so
 * a repaired fault clears itself after 40 clean journeys rather than needing a tool.
 */
#define DEM_HEALING_CYCLE_COUNT 40u

/*==================================================================================================
 *  Debounce
 *================================================================================================*/

/**
 * @brief Default consecutive failures required to confirm an event.
 *
 * Three. A single failure on a vehicle harness is routine -- an ignition transient is enough --
 * so confirming on one would fill the record with noise and train whoever reads it to ignore it.
 * Per-event values are in the descriptor table.
 */
#define DEM_DEFAULT_FAILURE_THRESHOLD 3u

/**
 * @brief Consecutive passes required to mark an event as no longer failing.
 *
 * One. Recovery is believed immediately while confirmation is deliberate, because the asymmetry
 * matches the cost: a fault reported late is worse than a recovery reported early.
 */
#define DEM_DEFAULT_PASS_THRESHOLD 1u

#endif /* DEM_CFG_H */

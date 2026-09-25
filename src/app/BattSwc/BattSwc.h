/**
 * @file    BattSwc.h
 * @brief   Battery monitoring component -- pack health from the raw RS485 measurements.
 *
 * Polls the packs through Rs485If and derives the quantities that indicate a problem before it becomes
 * a failure: cell imbalance, temperature spread, and whether a pack has stopped answering.
 *
 * @par Cell imbalance is the interesting measurement
 * A pack's terminal voltage says almost nothing about its condition — a pack with one cell at 2.8 V and
 * twenty-two at 4.1 V reads much the same as a healthy one. The spread between the highest and lowest
 * cell is what identifies the pack that is about to fail, and it is computed here because it is derived
 * from a measurement rather than being one.
 *
 * v1 logged all 23 cell voltages per pack and computed nothing from them. The data was present in the
 * cloud and nothing looked at it.
 *
 * @par Aggregation across packs
 * Several packs in parallel share the load, so the vehicle-level quantities — total current, lowest
 * state of charge, highest temperature — are what the driver and the fleet care about. They are computed
 * only over packs that answered *this cycle*: including a silent pack's last known values would make a
 * failing pack invisible in the aggregate, which is the opposite of what a monitor is for.
 *
 * @req SWREQ-BAT-0030 .. SWREQ-BAT-0048
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef BATTSWC_H
#define BATTSWC_H

#include "base/Autosar_ModuleIds.h"
#include "app/BattSwc/BattSwc_Cfg.h"
#include "ecuabs/Rs485If/Rs485If.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATTSWC_VENDOR_ID 0xFFFEu
#define BATTSWC_SW_MAJOR_VERSION 2u
#define BATTSWC_SW_MINOR_VERSION 0u
#define BATTSWC_SW_PATCH_VERSION 0u

#define BATTSWC_API_ID_INIT 0x00u
#define BATTSWC_API_ID_MAIN_FUNCTION 0x0Eu
#define BATTSWC_API_ID_GET_AGGREGATE 0x20u
#define BATTSWC_API_ID_GET_PACK_HEALTH 0x21u

#define BATTSWC_E_UNINIT E_UNINIT
#define BATTSWC_E_PARAM_POINTER E_PARAM_POINTER
#define BATTSWC_E_PARAM_SLOT 0x20u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Derived health of one pack. */
typedef struct
{
    uint16 cellVoltageHighest; /**< Highest cell, raw protocol units.            */
    uint16 cellVoltageLowest;  /**< Lowest cell, raw protocol units.             */
    uint16 cellImbalance;      /**< Highest minus lowest. The key indicator.      */
    uint8 weakestCellIndex;    /**< Which cell is lowest, 0-based.                */
    sint16 temperatureHighest; /**< Highest sensor, raw protocol units.           */
    sint16 temperatureLowest;  /**< Lowest sensor, raw protocol units.            */
    uint16 temperatureSpread;  /**< Highest minus lowest.                         */
    boolean imbalanceWarning;  /**< Imbalance above ::BATTSWC_IMBALANCE_WARN_RAW. */
    boolean dataValid;         /**< FALSE if the pack did not answer this cycle.  */
} BattSwc_PackHealthType;

/** Vehicle-level aggregate, computed only over packs that answered this cycle. */
typedef struct
{
    uint8 packsPresent;          /**< Packs discovered at startup.                   */
    uint8 packsResponding;       /**< Packs that answered this cycle.                */
    sint32 totalCurrent;         /**< Sum of responding packs' currents, raw units.   */
    uint16 lowestStateOfCharge;  /**< Lowest SOC among responding packs, percent.     */
    uint16 lowestStateOfHealth;  /**< Lowest SOH among responding packs, percent.     */
    sint16 highestTemperature;   /**< Highest sensor across responding packs.         */
    uint16 worstImbalance;       /**< Largest imbalance across responding packs.      */
    uint8 worstImbalancePack;    /**< Which pack has it, 1-based; 0 if none.          */
    boolean anyImbalanceWarning; /**< TRUE if any responding pack is out of balance.  */
    boolean aggregateValid;      /**< FALSE if no pack answered this cycle.           */
} BattSwc_AggregateType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Initialise the component. Rs485If must already have discovered the packs.
 * @return E_OK on success.
 */
CHECK_RETURN Std_ReturnType BattSwc_Init(void);

/**
 * @brief Poll every present pack and recompute the derived health figures.
 *
 * Driven cyclically by SchM from the acquisition task, at the acquisition period.
 *
 * @return E_OK if every present pack answered; E_NOT_OK if any did not. A partial round is kept, not
 *         discarded: three good packs and one silent one is far more useful than four discarded
 *         readings, and the silent one is reported separately.
 */
CHECK_RETURN Std_ReturnType BattSwc_MainFunction(void);

/**
 * @brief Read the vehicle-level aggregate.
 * @param[out] aggregate Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType BattSwc_GetAggregate(BattSwc_AggregateType *aggregate);

/**
 * @brief Read one pack's derived health.
 * @param[in]  slot   Pack slot, 1 .. ::RS485IF_PACK_COUNT.
 * @param[out] health Destination.
 */
CHECK_RETURN Std_ReturnType BattSwc_GetPackHealth(Rs485If_SlotType slot, BattSwc_PackHealthType *health);

/**
 * @brief Pointer to the cached pack states, for record serialisation.
 *
 * Returns the array Rs485If maintains, so Com can serialise it without copying nearly a kilobyte of pack
 * data per record. Valid for the lifetime of the program; the contents change on every poll.
 */
const Rs485If_PackStateType *BattSwc_GetPackStates(void);

/**
 * @brief Return this component's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void BattSwc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* BATTSWC_H */

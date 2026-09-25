/**
 * @file    BattSwc.c
 * @brief   Battery monitoring implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "app/BattSwc/BattSwc.h"

#include <string.h>

#include "services/Dem/Dem.h"
#include "services/Det/Det.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean BattSwc_Initialised = FALSE;
STATIC BattSwc_PackHealthType BattSwc_Health[RS485IF_PACK_COUNT];
STATIC BattSwc_AggregateType BattSwc_Aggregate;

/** Cached copy of the pack states, refreshed each cycle so Com can serialise without re-reading. */
STATIC Rs485If_PackStateType BattSwc_PackStates[RS485IF_PACK_COUNT];

/*==================================================================================================
 *  Derived health
 *================================================================================================*/

/** Recompute @p index's health from its cell data. */
STATIC void BattSwc_ComputePackHealth(uint8 index, const Rs485If_PackStateType *state)
{
    BattSwc_PackHealthType *health = &BattSwc_Health[index];
    uint8 cell;

    (void)memset(health, 0, sizeof(*health));

    if ((state->present == FALSE) || (state->cellDataValid == FALSE))
    {
        health->dataValid = FALSE;
        return;
    }

    health->cellVoltageHighest = state->cells.cellVoltage[0];
    health->cellVoltageLowest = state->cells.cellVoltage[0];
    health->weakestCellIndex = 0u;

    for (cell = 1u; cell < (uint8)RS485IF_CELLS_PER_PACK; cell++)
    {
        const uint16 v = state->cells.cellVoltage[cell];

        if (v > health->cellVoltageHighest)
        {
            health->cellVoltageHighest = v;
        }
        if (v < health->cellVoltageLowest)
        {
            health->cellVoltageLowest = v;
            health->weakestCellIndex = cell;
        }
    }

    /* The spread between strongest and weakest cell, which is what actually identifies a failing pack.
     * A pack's terminal voltage barely moves when one cell of twenty-three collapses, so the sum tells
     * you almost nothing -- v1 logged all 23 voltages and computed nothing from them. */
    health->cellImbalance = (uint16)(health->cellVoltageHighest - health->cellVoltageLowest);
    health->imbalanceWarning = (health->cellImbalance > (uint16)BATTSWC_IMBALANCE_WARN_RAW) ? TRUE : FALSE;

    health->temperatureHighest = state->cells.temperature[0];
    health->temperatureLowest = state->cells.temperature[0];
    for (cell = 1u; cell < (uint8)RS485IF_TEMPS_PER_PACK; cell++)
    {
        const sint16 t = state->cells.temperature[cell];

        if (t > health->temperatureHighest)
        {
            health->temperatureHighest = t;
        }
        if (t < health->temperatureLowest)
        {
            health->temperatureLowest = t;
        }
    }
    health->temperatureSpread =
        (uint16)((sint32)health->temperatureHighest - (sint32)health->temperatureLowest);

    health->dataValid = TRUE;
}

/** Recompute the vehicle-level aggregate over the packs that answered this cycle. */
STATIC void BattSwc_ComputeAggregate(void)
{
    uint8 index;
    boolean any = FALSE;

    (void)memset(&BattSwc_Aggregate, 0, sizeof(BattSwc_Aggregate));
    BattSwc_Aggregate.lowestStateOfCharge = 100u;
    BattSwc_Aggregate.lowestStateOfHealth = 100u;
    BattSwc_Aggregate.packsPresent = Rs485If_GetPresentPackCount();

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        const Rs485If_PackStateType *state = &BattSwc_PackStates[index];

        /* Only packs that answered *this* cycle contribute. Including a silent pack's last known values
         * would make a failing pack invisible in the aggregate, which is exactly backwards for a
         * monitor: the aggregate would look healthy precisely because one pack had stopped reporting. */
        if ((state->present == FALSE) || (state->packDataValid == FALSE))
        {
            continue;
        }

        any = TRUE;
        BattSwc_Aggregate.packsResponding++;
        BattSwc_Aggregate.totalCurrent += state->pack.current;

        if ((uint16)state->pack.stateOfCharge < BattSwc_Aggregate.lowestStateOfCharge)
        {
            BattSwc_Aggregate.lowestStateOfCharge = (uint16)state->pack.stateOfCharge;
        }
        if ((uint16)state->pack.stateOfHealth < BattSwc_Aggregate.lowestStateOfHealth)
        {
            BattSwc_Aggregate.lowestStateOfHealth = (uint16)state->pack.stateOfHealth;
        }

        if ((BattSwc_Aggregate.packsResponding == 1u)
            || (state->pack.temperatureHigh > BattSwc_Aggregate.highestTemperature))
        {
            BattSwc_Aggregate.highestTemperature = state->pack.temperatureHigh;
        }

        if ((BattSwc_Health[index].dataValid != FALSE)
            && (BattSwc_Health[index].cellImbalance > BattSwc_Aggregate.worstImbalance))
        {
            BattSwc_Aggregate.worstImbalance = BattSwc_Health[index].cellImbalance;
            BattSwc_Aggregate.worstImbalancePack = (uint8)(index + 1u);
        }
        if (BattSwc_Health[index].imbalanceWarning != FALSE)
        {
            BattSwc_Aggregate.anyImbalanceWarning = TRUE;
        }
    }

    BattSwc_Aggregate.aggregateValid = any;

    if (any == FALSE)
    {
        /* No pack answered at all. The lowest-value fields would otherwise read 100 %, which is the most
         * optimistic possible answer at the moment the least is known. */
        BattSwc_Aggregate.lowestStateOfCharge = 0u;
        BattSwc_Aggregate.lowestStateOfHealth = 0u;
    }
}

/** Raise or clear the per-pack and vehicle-level diagnostic events. */
STATIC void BattSwc_ReportEvents(void)
{
    uint8 index;

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        const Rs485If_PackStateType *state = &BattSwc_PackStates[index];
        const uint8 instance = (uint8)(index + 1u);

        if (state->present == FALSE)
        {
            continue;
        }

        if (state->consecutiveFailures >= (uint16)BATTSWC_MISSING_LIMIT)
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, instance, DEM_EVENT_STATUS_FAILED));
        }
        else if (state->packDataValid != FALSE)
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_PACK_NO_RESPONSE, instance, DEM_EVENT_STATUS_PASSED));
        }
        else
        {
            /* Failed, but not enough times yet to confirm. Dem's debounce handles the accumulation, so
             * nothing is reported here. */
        }
    }

    /* No pack found at all is a different fault from a pack that stopped answering: one is an
     * installation problem and the other is a failure in service. */
    if (Rs485If_GetPresentPackCount() == 0u)
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_PACK_MISSING, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
    }
    else
    {
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_PACK_MISSING, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_PASSED));
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType BattSwc_Init(void)
{
    (void)memset(BattSwc_Health, 0, sizeof(BattSwc_Health));
    (void)memset(&BattSwc_Aggregate, 0, sizeof(BattSwc_Aggregate));
    (void)memset(BattSwc_PackStates, 0, sizeof(BattSwc_PackStates));

    BattSwc_Aggregate.packsPresent = Rs485If_GetPresentPackCount();
    BattSwc_Initialised = TRUE;

    return E_OK;
}

Std_ReturnType BattSwc_MainFunction(void)
{
    Std_ReturnType pollStatus;
    uint8 index;

    if (BattSwc_Initialised == FALSE)
    {
        return E_NOT_OK;
    }

    pollStatus = Rs485If_PollAllPacks();

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        const Rs485If_SlotType slot = (Rs485If_SlotType)(index + 1u);

        if (Rs485If_GetPackState(slot, &BattSwc_PackStates[index]) != E_OK)
        {
            (void)memset(&BattSwc_PackStates[index], 0, sizeof(BattSwc_PackStates[index]));
        }
        BattSwc_ComputePackHealth(index, &BattSwc_PackStates[index]);
    }

    BattSwc_ComputeAggregate();
    BattSwc_ReportEvents();

    /* The poll status is returned as Rs485If reported it. A partial round is a real outcome the caller
     * may want to know about, and it is not the same as this function failing. */
    return pollStatus;
}

Std_ReturnType BattSwc_GetAggregate(BattSwc_AggregateType *aggregate)
{
    DET_CHECK_RETURN(aggregate != NULL_PTR, MODULE_ID_BATTSWC, INSTANCE_ID_SINGLE,
                     BATTSWC_API_ID_GET_AGGREGATE, BATTSWC_E_PARAM_POINTER, E_NOT_OK);

    *aggregate = BattSwc_Aggregate;
    return E_OK;
}

Std_ReturnType BattSwc_GetPackHealth(Rs485If_SlotType slot, BattSwc_PackHealthType *health)
{
    DET_CHECK_RETURN(health != NULL_PTR, MODULE_ID_BATTSWC, slot, BATTSWC_API_ID_GET_PACK_HEALTH,
                     BATTSWC_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN((slot >= 1u) && (slot <= (Rs485If_SlotType)RS485IF_PACK_COUNT), MODULE_ID_BATTSWC, slot,
                     BATTSWC_API_ID_GET_PACK_HEALTH, BATTSWC_E_PARAM_SLOT, E_NOT_OK);

    *health = BattSwc_Health[slot - 1u];
    return E_OK;
}

const Rs485If_PackStateType *BattSwc_GetPackStates(void)
{
    return BattSwc_PackStates;
}

void BattSwc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = BATTSWC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_BATTSWC;
        versioninfo->sw_major_version = BATTSWC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = BATTSWC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = BATTSWC_SW_PATCH_VERSION;
    }
}

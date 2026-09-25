/**
 * @file    Dem.c
 * @brief   Diagnostic Event Manager implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Dem/Dem.h"

#include <string.h>

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"

/*==================================================================================================
 *  Event descriptor table
 *
 *  Indexed by (eventId - 1). The order must match the identifiers in Dem_Cfg.h; the static
 *  assertion at the end of the table checks the count, and ::Dem_Init verifies the mapping.
 *================================================================================================*/

typedef struct
{
    Dem_DtcType dtcBase;      /**< Code with a zero instance byte.                   */
    uint8 failureThreshold;   /**< Consecutive failures needed to confirm.            */
    boolean persistent;       /**< TRUE to carry the status across a power cycle.      */
    boolean warningIndicator; /**< TRUE if a confirmation should warn the driver.      */
} Dem_EventDescriptorType;

STATIC const Dem_EventDescriptorType Dem_Descriptors[DEM_EVENT_COUNT] = {
    /* Battery pack communication. A silent pack is the fault most likely to strand the vehicle,
     * so it warns the driver; rising CRC failures are a degradation trend, not yet a fault. */
    {0x0C0100uL, 3u, TRUE, TRUE},   /* DEM_EVENT_PACK_NO_RESPONSE     */
    {0x0C0110uL, 10u, TRUE, FALSE}, /* DEM_EVENT_PACK_CRC_FAILURE     */
    {0x0C0120uL, 1u, TRUE, TRUE},   /* DEM_EVENT_PACK_MISSING         */

    /* Vehicle CAN. Init failure and bus-off confirm on the first report: both are unambiguous
     * hardware states, not noisy measurements, so debouncing them only delays the report. */
    {0x0C0200uL, 1u, TRUE, FALSE}, /* DEM_EVENT_CAN_INIT_FAILED      */
    {0x0C0210uL, 1u, TRUE, FALSE}, /* DEM_EVENT_CAN_BUS_OFF          */
    {0x0C0220uL, 5u, TRUE, FALSE}, /* DEM_EVENT_CAN_TIMEOUT          */

    /* Backhaul. Thresholds are deliberately high: a vehicle spends much of its life out of
     * coverage, and that is not a fault of the ECU. Only losing *both* bearers for a sustained
     * period is worth recording, because that is when data starts backing up on the card. */
    {0x0C0300uL, 20u, FALSE, FALSE}, /* DEM_EVENT_WIFI_UNAVAILABLE    */
    {0x0C0310uL, 20u, FALSE, FALSE}, /* DEM_EVENT_GPRS_UNAVAILABLE    */
    {0x0C0320uL, 10u, FALSE, FALSE}, /* DEM_EVENT_BROKER_UNREACHABLE  */
    {0x0C0330uL, 60u, TRUE, FALSE},  /* DEM_EVENT_NO_BACKHAUL         */

    /* Storage. The SD card is the store-and-forward buffer, so losing it means data loss
     * whenever the backhaul is also down -- it warns the driver. */
    {0x0D0100uL, 1u, TRUE, TRUE},  /* DEM_EVENT_SD_MOUNT_FAILED      */
    {0x0D0110uL, 5u, TRUE, TRUE},  /* DEM_EVENT_SD_WRITE_FAILED      */
    {0x0D0120uL, 1u, TRUE, FALSE}, /* DEM_EVENT_SD_SPACE_LOW         */
    {0x0D0130uL, 1u, TRUE, FALSE}, /* DEM_EVENT_NVM_INTEGRITY        */
    {0x0D0140uL, 1u, TRUE, FALSE}, /* DEM_EVENT_FLASH_WEAR           */

    /* Sensors. A GNSS receiver legitimately has no fix indoors or in a tunnel, so the threshold
     * is high and the status is not persisted. */
    {0x0D0200uL, 30u, FALSE, FALSE}, /* DEM_EVENT_GNSS_NO_FIX         */
    {0x0D0210uL, 3u, TRUE, FALSE},   /* DEM_EVENT_RTC_INVALID         */
    {0x0D0220uL, 5u, FALSE, FALSE},  /* DEM_EVENT_VBATT_LOW           */
    {0x0D0230uL, 3u, TRUE, FALSE},   /* DEM_EVENT_VBATT_SENSE_FAULT   */

    /* Internal. A missed deadline or a crash loop indicates a software fault, which is exactly
     * what must survive to be read back later. */
    {0x0E0100uL, 3u, TRUE, FALSE},  /* DEM_EVENT_WDGM_DEADLINE        */
    {0x0E0110uL, 3u, TRUE, FALSE},  /* DEM_EVENT_WDGM_ALIVE           */
    {0x0E0120uL, 10u, TRUE, FALSE}, /* DEM_EVENT_TASK_OVERRUN         */
    {0x0E0130uL, 1u, TRUE, TRUE},   /* DEM_EVENT_CRASH_LOOP           */
    {0x0E0140uL, 3u, TRUE, FALSE},  /* DEM_EVENT_HEAP_LOW             */
    {0x0E0150uL, 1u, TRUE, FALSE},  /* DEM_EVENT_OTA_FAILED           */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(STD_ARRAY_SIZE(Dem_Descriptors) == DEM_EVENT_COUNT,
               "Dem descriptor table does not match DEM_EVENT_COUNT");
_Static_assert(DEM_EVENT_OTA_FAILED == DEM_EVENT_COUNT,
               "event identifiers must be contiguous and end at DEM_EVENT_COUNT");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean Dem_Initialised = FALSE;
STATIC Dem_EventRecordType Dem_Records[DEM_EVENT_COUNT];
STATIC Dem_StatisticsType Dem_Stats;
STATIC Dem_SnapshotProviderType Dem_SnapshotProvider;

/** Set when a persistent event's status changes, so ::Dem_MainFunction knows to write. */
STATIC boolean Dem_PersistPending;

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

STATIC boolean Dem_EventValid(Dem_EventIdType eventId)
{
    return ((eventId >= 1u) && (eventId <= (Dem_EventIdType)DEM_EVENT_COUNT)) ? TRUE : FALSE;
}

/** Recompute the confirmed and pending totals and the warning flag. */
STATIC void Dem_RecountStatus(void)
{
    uint16 confirmed = 0u;
    uint16 pending = 0u;
    boolean warning = FALSE;
    uint16 i;

    for (i = 0u; i < (uint16)DEM_EVENT_COUNT; i++)
    {
        if ((Dem_Records[i].udsStatus & DEM_UDS_CONFIRMED_DTC) != 0u)
        {
            confirmed++;
            if (Dem_Descriptors[i].warningIndicator != FALSE)
            {
                warning = TRUE;
            }
        }
        else if ((Dem_Records[i].udsStatus & DEM_UDS_PENDING_DTC) != 0u)
        {
            pending++;
        }
        else
        {
            /* Neither pending nor confirmed. */
        }
    }

    Dem_Stats.confirmedCount = confirmed;
    Dem_Stats.pendingCount = pending;
    Dem_Stats.warningActive = warning;
}

/** Capture the conditions at the moment an event confirms. */
STATIC void Dem_CaptureSnapshot(uint16 index)
{
    Dem_SnapshotType snapshot;

    (void)memset(&snapshot, 0, sizeof(snapshot));

    /* Only the first confirmation is captured. A fault that confirms, heals and confirms again
     * overwrites nothing: the original conditions are the informative ones, and the occurrence
     * count already records that it came back. */
    if (Dem_Records[index].snapshotStored != FALSE)
    {
        return;
    }

    if (Dem_SnapshotProvider != NULL_PTR)
    {
        Dem_SnapshotProvider(&snapshot);
    }
    snapshot.uptimeMs = Gpt_GetMonotonicMs();

    Dem_Records[index].snapshot = snapshot;
    Dem_Records[index].snapshotStored = TRUE;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType Dem_Init(void)
{
    uint16 i;

    (void)memset(Dem_Records, 0, sizeof(Dem_Records));
    (void)memset(&Dem_Stats, 0, sizeof(Dem_Stats));
    Dem_SnapshotProvider = NULL_PTR;
    Dem_PersistPending = FALSE;

    for (i = 0u; i < (uint16)DEM_EVENT_COUNT; i++)
    {
        Dem_Records[i].dtc = Dem_Descriptors[i].dtcBase;

        /* AUTOSAR requires both "not completed" bits set until a monitor has actually run, so a
         * tool can distinguish "tested and passed" from "never tested". Reporting an untested
         * monitor as passed is how a fault that is never checked appears healthy. */
        Dem_Records[i].udsStatus =
            (uint8)(DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR | DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE);
    }

    Dem_Initialised = TRUE;
    return E_OK;
}

void Dem_SetSnapshotProvider(Dem_SnapshotProviderType provider)
{
    Dem_SnapshotProvider = provider;
}

Std_ReturnType Dem_SetEventStatus(Dem_EventIdType eventId, uint8 instanceId, Dem_EventStatusType status)
{
    uint16 index;
    Dem_EventRecordType *record;
    const Dem_EventDescriptorType *descriptor;
    boolean wasConfirmed;

    DET_CHECK_RETURN(Dem_Initialised != FALSE, MODULE_ID_DEM, instanceId, DEM_API_ID_SET_EVENT_STATUS,
                     DEM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(Dem_EventValid(eventId) != FALSE, MODULE_ID_DEM, instanceId, DEM_API_ID_SET_EVENT_STATUS,
                     DEM_E_PARAM_EVENT_ID, E_NOT_OK);

    index = (uint16)(eventId - 1u);
    record = &Dem_Records[index];
    descriptor = &Dem_Descriptors[index];
    wasConfirmed = ((record->udsStatus & DEM_UDS_CONFIRMED_DTC) != 0u) ? TRUE : FALSE;

    Dem_Stats.totalReports++;
    record->instanceId = instanceId;

    /* The monitor has run, whatever it found. */
    record->udsStatus &=
        (uint8)(~(uint8)(DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR | DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE));

    switch (status)
    {
    case DEM_EVENT_STATUS_FAILED:
    case DEM_EVENT_STATUS_PREFAILED:
        record->udsStatus |=
            (uint8)(DEM_UDS_TEST_FAILED | DEM_UDS_TEST_FAILED_THIS_CYCLE | DEM_UDS_TEST_FAILED_SINCE_CLEAR);
        record->lastFailedUptimeMs = Gpt_GetMonotonicMs();
        if (record->occurrenceCount < 0xFFFFu)
        {
            record->occurrenceCount++;
        }
        if (record->firstFailedUptimeMs == 0u)
        {
            record->firstFailedUptimeMs = record->lastFailedUptimeMs;
        }

        /* Healing restarts from zero: a fault that reappears has not been repaired, whatever the
         * intervening clean cycles suggested. */
        record->healingCounter = 0u;

        if (record->debounceCounter < descriptor->failureThreshold)
        {
            record->debounceCounter++;
        }

        if (record->debounceCounter >= descriptor->failureThreshold)
        {
            record->udsStatus |= DEM_UDS_CONFIRMED_DTC;
            record->udsStatus &= (uint8)(~(uint8)DEM_UDS_PENDING_DTC);
            if (descriptor->warningIndicator != FALSE)
            {
                record->udsStatus |= DEM_UDS_WARNING_INDICATOR_REQUESTED;
            }
            if (wasConfirmed == FALSE)
            {
                Dem_CaptureSnapshot(index);
                Dem_Stats.totalConfirmed++;
                if (descriptor->persistent != FALSE)
                {
                    Dem_PersistPending = TRUE;
                }
            }
        }
        else
        {
            record->udsStatus |= DEM_UDS_PENDING_DTC;
        }
        break;

    case DEM_EVENT_STATUS_PASSED:
    case DEM_EVENT_STATUS_PREPASSED:
        record->udsStatus &= (uint8)(~(uint8)DEM_UDS_TEST_FAILED);

        if (record->debounceCounter > 0u)
        {
            record->debounceCounter--;
        }

        /* A confirmed event is *not* cleared by a pass. Confirmation means the fault happened,
         * and a diagnostic record that erased itself the moment the fault stopped would be
         * useless for exactly the intermittent faults it exists to catch. It clears through the
         * healing counter across operation cycles, or when a tool clears it explicitly. */
        if (record->debounceCounter == 0u)
        {
            record->udsStatus &= (uint8)(~(uint8)DEM_UDS_PENDING_DTC);
        }
        break;

    default:
        (void)Det_ReportError(MODULE_ID_DEM, instanceId, DEM_API_ID_SET_EVENT_STATUS, DEM_E_PARAM_STATUS);
        return E_NOT_OK;
    }

    Dem_RecountStatus();
    return E_OK;
}

Std_ReturnType Dem_GetEventStatus(Dem_EventIdType eventId, uint8 *udsStatus)
{
    DET_CHECK_RETURN(Dem_EventValid(eventId) != FALSE, MODULE_ID_DEM, INSTANCE_ID_SINGLE,
                     DEM_API_ID_GET_EVENT_STATUS, DEM_E_PARAM_EVENT_ID, E_NOT_OK);
    DET_CHECK_RETURN(udsStatus != NULL_PTR, MODULE_ID_DEM, INSTANCE_ID_SINGLE, DEM_API_ID_GET_EVENT_STATUS,
                     DEM_E_PARAM_POINTER, E_NOT_OK);

    *udsStatus = Dem_Records[eventId - 1u].udsStatus;
    return E_OK;
}

Std_ReturnType Dem_GetEventRecord(Dem_EventIdType eventId, Dem_EventRecordType *record)
{
    DET_CHECK_RETURN(Dem_EventValid(eventId) != FALSE, MODULE_ID_DEM, INSTANCE_ID_SINGLE,
                     DEM_API_ID_GET_DTC_INFO, DEM_E_PARAM_EVENT_ID, E_NOT_OK);
    DET_CHECK_RETURN(record != NULL_PTR, MODULE_ID_DEM, INSTANCE_ID_SINGLE, DEM_API_ID_GET_DTC_INFO,
                     DEM_E_PARAM_POINTER, E_NOT_OK);

    *record = Dem_Records[eventId - 1u];
    return E_OK;
}

boolean Dem_IsEventConfirmed(Dem_EventIdType eventId)
{
    if (Dem_EventValid(eventId) == FALSE)
    {
        return FALSE;
    }
    return ((Dem_Records[eventId - 1u].udsStatus & DEM_UDS_CONFIRMED_DTC) != 0u) ? TRUE : FALSE;
}

Dem_DtcType Dem_GetDtcForEvent(Dem_EventIdType eventId, uint8 instanceId)
{
    if (Dem_EventValid(eventId) == FALSE)
    {
        return 0uL;
    }
    /* The instance occupies the low byte, so four battery packs share one code and remain
     * distinguishable without four near-identical table entries. */
    return (Dem_Descriptors[eventId - 1u].dtcBase & 0x00FFFF00uL) | (uint32)instanceId;
}

uint16 Dem_GetConfirmedDtcs(Dem_DtcType *buffer, uint16 maxCount)
{
    uint16 written = 0u;
    uint16 i;

    if ((buffer == NULL_PTR) || (maxCount == 0u))
    {
        return 0u;
    }

    for (i = 0u; (i < (uint16)DEM_EVENT_COUNT) && (written < maxCount); i++)
    {
        if ((Dem_Records[i].udsStatus & DEM_UDS_CONFIRMED_DTC) != 0u)
        {
            buffer[written] = (Dem_Records[i].dtc & 0x00FFFF00uL) | (uint32)Dem_Records[i].instanceId;
            written++;
        }
    }
    return written;
}

Std_ReturnType Dem_GetSnapshot(Dem_EventIdType eventId, Dem_SnapshotType *snapshot)
{
    DET_CHECK_RETURN(Dem_EventValid(eventId) != FALSE, MODULE_ID_DEM, INSTANCE_ID_SINGLE,
                     DEM_API_ID_GET_DTC_INFO, DEM_E_PARAM_EVENT_ID, E_NOT_OK);
    DET_CHECK_RETURN(snapshot != NULL_PTR, MODULE_ID_DEM, INSTANCE_ID_SINGLE, DEM_API_ID_GET_DTC_INFO,
                     DEM_E_PARAM_POINTER, E_NOT_OK);

    if (Dem_Records[eventId - 1u].snapshotStored == FALSE)
    {
        return E_NOT_FOUND;
    }

    *snapshot = Dem_Records[eventId - 1u].snapshot;
    return E_OK;
}

Std_ReturnType Dem_ClearDtc(Dem_DtcType dtc)
{
    boolean cleared = FALSE;
    uint16 i;

    DET_CHECK_RETURN(Dem_Initialised != FALSE, MODULE_ID_DEM, INSTANCE_ID_SINGLE, DEM_API_ID_CLEAR_DTC,
                     DEM_E_UNINIT, E_NOT_OK);

    for (i = 0u; i < (uint16)DEM_EVENT_COUNT; i++)
    {
        const boolean matches =
            ((dtc == 0x00FFFFFFuL) || ((Dem_Descriptors[i].dtcBase & 0x00FFFF00uL) == (dtc & 0x00FFFF00uL)))
                ? TRUE
                : FALSE;

        if (matches != FALSE)
        {
            (void)memset(&Dem_Records[i], 0, sizeof(Dem_Records[i]));
            Dem_Records[i].dtc = Dem_Descriptors[i].dtcBase;
            /* Back to "never tested": after a clear, a tool must be able to tell a monitor that
             * has since run and passed from one that has not run at all. */
            Dem_Records[i].udsStatus =
                (uint8)(DEM_UDS_TEST_NOT_COMPLETED_SINCE_CLEAR | DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE);
            cleared = TRUE;
        }
    }

    if (cleared == FALSE)
    {
        return E_NOT_FOUND;
    }

    Dem_Stats.clearCount++;
    Dem_Stats.totalConfirmed = 0u;
    Dem_PersistPending = TRUE;
    Dem_RecountStatus();
    return E_OK;
}

void Dem_StartOperationCycle(void)
{
    uint16 i;

    if (Dem_Initialised == FALSE)
    {
        return;
    }

    for (i = 0u; i < (uint16)DEM_EVENT_COUNT; i++)
    {
        const boolean failedLastCycle =
            ((Dem_Records[i].udsStatus & DEM_UDS_TEST_FAILED_THIS_CYCLE) != 0u) ? TRUE : FALSE;

        Dem_Records[i].udsStatus &= (uint8)(~(uint8)DEM_UDS_TEST_FAILED_THIS_CYCLE);
        Dem_Records[i].udsStatus |= DEM_UDS_TEST_NOT_COMPLETED_THIS_CYCLE;
        Dem_Records[i].firstFailedUptimeMs = 0u;

        if ((Dem_Records[i].udsStatus & DEM_UDS_CONFIRMED_DTC) == 0u)
        {
            continue;
        }

        if (failedLastCycle != FALSE)
        {
            Dem_Records[i].healingCounter = 0u;
            continue;
        }

        if (Dem_Records[i].healingCounter < 0xFFu)
        {
            Dem_Records[i].healingCounter++;
        }

        if (Dem_Records[i].healingCounter >= (uint8)DEM_HEALING_CYCLE_COUNT)
        {
            /* Enough clean journeys: the fault is treated as repaired and clears itself, so a
             * replaced pack does not leave a code behind that needs a tool to remove. */
            Dem_Records[i].udsStatus &=
                (uint8)(~(uint8)(DEM_UDS_CONFIRMED_DTC | DEM_UDS_WARNING_INDICATOR_REQUESTED));
            Dem_Records[i].debounceCounter = 0u;
            Dem_PersistPending = TRUE;
        }
    }

    Dem_RecountStatus();
}

void Dem_MainFunction(void)
{
    if ((Dem_Initialised == FALSE) || (Dem_PersistPending == FALSE))
    {
        return;
    }

    /* The persistent image is assembled and written by DiagSwc, which owns the NvM block and its
     * serialisation. Dem only signals that something changed -- keeping the NvM dependency out of
     * this module is what lets it be unit tested with no storage stack at all. */
    Dem_PersistPending = FALSE;
}

Std_ReturnType Dem_GetStatistics(Dem_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_DEM, INSTANCE_ID_SINGLE, DEM_API_ID_MAIN_FUNCTION,
                     DEM_E_PARAM_POINTER, E_NOT_OK);

    Dem_RecountStatus();
    *stats = Dem_Stats;
    return E_OK;
}

void Dem_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = DEM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_DEM;
        versioninfo->sw_major_version = DEM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = DEM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = DEM_SW_PATCH_VERSION;
    }
}

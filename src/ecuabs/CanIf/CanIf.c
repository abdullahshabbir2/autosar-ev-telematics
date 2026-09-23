/**
 * @file    CanIf.c
 * @brief   CAN interface implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "CanIf.h"

#include <string.h>

#include "Dem.h"
#include "Det.h"
#include "Gpt.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean CanIf_Initialised = FALSE;
STATIC CanIf_McuDataType CanIf_McuData;
STATIC CanIf_StatisticsType CanIf_Stats;
STATIC uint16 CanIf_ConsecutiveStale;

/*==================================================================================================
 *  Byte-order helper
 *================================================================================================*/

/**
 * @brief Assemble a 16-bit signal from two frame bytes, in the configured order.
 *
 * @param low  Byte at the lower frame offset.
 * @param high Byte at the higher frame offset.
 */
STATIC uint16 CanIf_Assemble16(uint8 low, uint8 high)
{
#if (CANIF_MCU_BYTE_ORDER_LITTLE_ENDIAN == STD_ON)
    return (uint16)((uint16)low | ((uint16)high << 8u));
#else
    return (uint16)(((uint16)low << 8u) | (uint16)high);
#endif
}

/*==================================================================================================
 *  Frame decoders -- pure, tested against captured frames
 *================================================================================================*/

Std_ReturnType CanIf_DecodeMcuDriveState(const Can_PduType *pdu, CanIf_McuDataType *data)
{
    DET_CHECK_RETURN((pdu != NULL_PTR) && (data != NULL_PTR), MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_DECODE, CANIF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(pdu->dlc >= CANIF_DS_MIN_DLC, MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_DECODE, CANIF_E_INVALID_DLC, E_NOT_OK);

    data->direction =
        (CanIf_DirectionType)(pdu->sdu[CANIF_DS_OFF_FLAGS] & CANIF_DS_MASK_DIRECTION);
    data->speedMode = (CanIf_SpeedModeType)((pdu->sdu[CANIF_DS_OFF_FLAGS] >>
                                             CANIF_DS_SHIFT_SPEED_MODE) &
                                            CANIF_DS_MASK_SPEED_MODE);
    data->motorRpm =
        CanIf_Assemble16(pdu->sdu[CANIF_DS_OFF_RPM_LOW], pdu->sdu[CANIF_DS_OFF_RPM_HIGH]);
    data->faultCode = pdu->sdu[CANIF_DS_OFF_FAULT];

    /* Compared against the specific value the controller uses rather than tested for non-zero: the
     * byte carries a code, and treating any non-zero value as "low power active" would misreport
     * whatever else the controller chooses to put there. */
    data->lowPowerMode =
        (pdu->sdu[CANIF_DS_OFF_LOW_POWER] == (uint8)CANIF_MCU_LOW_POWER_ACTIVE) ? TRUE : FALSE;

    data->driveStateTimestamp = pdu->timestamp;
    data->driveStateValid = TRUE;

    return E_OK;
}

Std_ReturnType CanIf_DecodeMcuCurrentVoltage(const Can_PduType *pdu, CanIf_McuDataType *data)
{
    DET_CHECK_RETURN((pdu != NULL_PTR) && (data != NULL_PTR), MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_DECODE, CANIF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(pdu->dlc >= CANIF_CV_MIN_DLC, MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_DECODE, CANIF_E_INVALID_DLC, E_NOT_OK);

    /* Kept in the protocol's own 0.1 V and 0.1 A units. v1 multiplied by 0.1f here, which turned an
     * exact integer into a float that then had to be formatted back to text for logging -- two
     * conversions, each rounding, for no gain. The scaling belongs at the point of display. */
    data->dcVoltageDeciVolt = CanIf_Assemble16(pdu->sdu[CANIF_CV_OFF_VOLTAGE_LOW],
                                               pdu->sdu[CANIF_CV_OFF_VOLTAGE_HIGH]);
    data->dcCurrentDeciAmp = CanIf_Assemble16(pdu->sdu[CANIF_CV_OFF_CURRENT_LOW],
                                              pdu->sdu[CANIF_CV_OFF_CURRENT_HIGH]);

    data->currentVoltageTimestamp = pdu->timestamp;
    data->currentVoltageValid = TRUE;

    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType CanIf_Init(void)
{
    (void)memset(&CanIf_McuData, 0, sizeof(CanIf_McuData));
    (void)memset(&CanIf_Stats, 0, sizeof(CanIf_Stats));
    CanIf_ConsecutiveStale = 0u;
    CanIf_Initialised = TRUE;
    return E_OK;
}

uint8 CanIf_MainFunction(void)
{
    uint8 decoded = 0u;

    if (CanIf_Initialised == FALSE)
    {
        return 0u;
    }

    for (;;)
    {
        Can_PduType pdu;
        const Std_ReturnType status = Can_Receive(&pdu);

        if (status != E_OK)
        {
            /* E_NOT_FOUND means the queue is empty, which is the normal exit. */
            break;
        }

        CanIf_Stats.framesProcessed++;

        switch (pdu.id & CAN_ID_MASK)
        {
        case CAN_ID_MCU_DRIVE_STATE:
            if (CanIf_DecodeMcuDriveState(&pdu, &CanIf_McuData) == E_OK)
            {
                CanIf_Stats.driveStateFrames++;
                decoded++;
            }
            else
            {
                CanIf_Stats.badDlcFrames++;
            }
            break;

        case CAN_ID_MCU_CURRENT_VOLTAGE:
            if (CanIf_DecodeMcuCurrentVoltage(&pdu, &CanIf_McuData) == E_OK)
            {
                CanIf_Stats.currentVoltageFrames++;
                decoded++;
            }
            else
            {
                CanIf_Stats.badDlcFrames++;
            }
            break;

        default:
            /* The hardware filters should make this unreachable. Counting it rather than ignoring
             * it means a filter configuration mistake shows up as a number instead of as
             * unexplained CPU load. */
            CanIf_Stats.unknownIdFrames++;
            break;
        }
    }

    return decoded;
}

Std_ReturnType CanIf_GetMcuData(CanIf_McuDataType *data)
{
    DET_CHECK_RETURN(CanIf_Initialised != FALSE, MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_GET_MCU_DATA, CANIF_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(data != NULL_PTR, MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_GET_MCU_DATA, CANIF_E_PARAM_POINTER, E_NOT_OK);

    *data = CanIf_McuData;

    if (CanIf_IsMcuDataFresh() == FALSE)
    {
        CanIf_Stats.staleReads++;
        if (CanIf_ConsecutiveStale < 0xFFFFu)
        {
            CanIf_ConsecutiveStale++;
        }
        if (CanIf_ConsecutiveStale == (uint16)CANIF_STALE_REPORT_THRESHOLD)
        {
            /* Reported once on crossing the threshold rather than on every stale read, so a
             * disconnected controller produces one diagnostic event instead of thousands. */
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_TIMEOUT, INSTANCE_ID_SINGLE,
                                           DEM_EVENT_STATUS_FAILED));
        }
    }
    else
    {
        if (CanIf_ConsecutiveStale > 0u)
        {
            STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_TIMEOUT, INSTANCE_ID_SINGLE,
                                           DEM_EVENT_STATUS_PASSED));
        }
        CanIf_ConsecutiveStale = 0u;
    }

    return E_OK;
}

boolean CanIf_IsMcuDataFresh(void)
{
    if ((CanIf_McuData.driveStateValid == FALSE) || (CanIf_McuData.currentVoltageValid == FALSE))
    {
        return FALSE;
    }

    /* Both halves must be fresh. A controller that still sends voltage but has stopped sending
     * speed would otherwise look healthy to the odometer, which is the consumer that matters most. */
    if (Gpt_HasElapsed(CanIf_McuData.driveStateTimestamp, CANIF_MCU_SIGNAL_TIMEOUT_MS) != FALSE)
    {
        return FALSE;
    }
    if (Gpt_HasElapsed(CanIf_McuData.currentVoltageTimestamp, CANIF_MCU_SIGNAL_TIMEOUT_MS) != FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

Std_ReturnType CanIf_GetStatistics(CanIf_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_CANIF, INSTANCE_ID_SINGLE,
                     CANIF_API_ID_MAIN_FUNCTION, CANIF_E_PARAM_POINTER, E_NOT_OK);

    *stats = CanIf_Stats;
    return E_OK;
}

void CanIf_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = CANIF_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_CANIF;
        versioninfo->sw_major_version = CANIF_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = CANIF_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = CANIF_SW_PATCH_VERSION;
    }
}

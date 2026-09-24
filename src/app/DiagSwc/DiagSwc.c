/**
 * @file    DiagSwc.c
 * @brief   Diagnostics implementation: DTC persistence and the UDS-style service layer.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "app/DiagSwc/DiagSwc.h"

#include <string.h>

#include "services/Crc/Crc.h"
#include "services/Det/Det.h"
#include "Ecu_Cfg.h"
#include "services/Fee/Fee.h"
#include "mcal/Gpt/Gpt.h"
#include "ecuabs/IoHwAb/IoHwAb.h"
#include "services/Log/Log.h"
#include "mcal/Mcu/Mcu.h"
#include "services/NvM/NvM.h"
#include "app/OdoSwc/OdoSwc.h"
#include "ecuabs/Rs485If/Rs485If.h"
#include "ecuabs/TimeAbs/TimeAbs.h"

/*==================================================================================================
 *  Persistent DTC record
 *
 *  Stored in its own Fee block. One entry per configured event, so a fault is never dropped for lack of
 *  room, and a CRC over the whole array so a partially-written record is detected rather than restored.
 *================================================================================================*/

/** One persisted event. Packed to 8 bytes so the whole array fits FEE_LENGTH_DTC_STORE. */
typedef struct
{
    uint8 udsStatus;      /**< ISO 14229 status byte at the time of the write.  */
    uint8 instanceId;     /**< Which instance last reported it.                 */
    uint16 occurrences;   /**< Times it has been reported failed.               */
    uint32 firstFailedAt; /**< Wall-clock time of its first failure, 0 if unknown. */
} DiagSwc_PersistedEventType;

/** On-media layout of the DTC block. */
typedef struct
{
    uint16 structVersion; /**< Layout version; 1 for this definition.           */
    uint16 eventCount;    /**< Events the record holds, for forward tolerance.  */
    DiagSwc_PersistedEventType events[DIAGSWC_PERSISTENT_DTC_SLOTS];
    uint32 crc;           /**< CRC-32 over everything above.                    */
} DiagSwc_DtcBlockType;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(DiagSwc_DtcBlockType) <= FEE_LENGTH_DTC_STORE,
               "the persistent DTC record does not fit its Fee block");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean DiagSwc_Initialised = FALSE;
STATIC DiagSwc_StatusType DiagSwc_Status;
STATIC Gpt_TimestampType DiagSwc_LastPersistCheckMs;
STATIC uint16 DiagSwc_LastConfirmedCount;

/*==================================================================================================
 *  Persistence
 *================================================================================================*/

/** CRC over a block image, excluding the CRC field itself. */
STATIC uint32 DiagSwc_BlockCrc(const DiagSwc_DtcBlockType *block)
{
    return Crc_CalculateCRC32((const uint8 *)block,
                              (uint32)(sizeof(*block) - sizeof(block->crc)), 0u, TRUE);
}

Std_ReturnType DiagSwc_Persist(void)
{
    DiagSwc_DtcBlockType block;
    uint16 event;

    DET_CHECK_RETURN(DiagSwc_Initialised != FALSE, MODULE_ID_DIAGSWC, INSTANCE_ID_SINGLE,
                     DIAGSWC_API_ID_PERSIST, DIAGSWC_E_UNINIT, E_NOT_OK);

    (void)memset(&block, 0, sizeof(block));
    block.structVersion = 1u;
    block.eventCount = (uint16)DEM_EVENT_COUNT;

    for (event = 1u; event <= (uint16)DEM_EVENT_COUNT; event++)
    {
        Dem_EventRecordType record;

        if (Dem_GetEventRecord((Dem_EventIdType)event, &record) != E_OK)
        {
            continue;
        }

        /* Only confirmed events are persisted. A pending one has not yet met its debounce threshold, and
         * restoring a partial debounce count across a power cycle would let a fault that fails once per
         * journey eventually confirm -- which is exactly the intermittent noise the debounce exists to
         * filter out. */
        if ((record.udsStatus & DEM_UDS_CONFIRMED_DTC) == 0u)
        {
            continue;
        }

        block.events[event - 1u].udsStatus = record.udsStatus;
        block.events[event - 1u].instanceId = record.instanceId;
        block.events[event - 1u].occurrences = record.occurrenceCount;
        block.events[event - 1u].firstFailedAt = record.snapshot.unixTime;
    }

    block.crc = DiagSwc_BlockCrc(&block);

    if (Fee_WriteBlock(FEE_BLOCK_DTC_STORE, (const uint8 *)&block) != E_OK)
    {
        DiagSwc_Status.persistFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_DIAGSWC, INSTANCE_ID_SINGLE, DIAGSWC_API_ID_PERSIST,
                                     DIAGSWC_E_PERSIST_FAILED);
        return E_NOT_OK;
    }

    DiagSwc_Status.persistCount++;
    return E_OK;
}

/** Restore confirmed events from the persistent record. */
STATIC Std_ReturnType DiagSwc_Restore(void)
{
    DiagSwc_DtcBlockType block;
    uint16 event;
    uint16 restored = 0u;

    if (Fee_ReadBlock(FEE_BLOCK_DTC_STORE, (uint8 *)&block, 0u, (uint16)sizeof(block)) != E_OK)
    {
        /* Never written, or every copy corrupt. A clean record is the right starting point. */
        return E_NOT_FOUND;
    }

    if (block.structVersion != 1u)
    {
        /* A record from an incompatible firmware version. Discarded rather than reinterpreted: the event
         * identifiers may not mean the same thing, and restoring them would attribute faults to the
         * wrong subsystems. */
        return E_NOT_FOUND;
    }

    if (block.crc != DiagSwc_BlockCrc(&block))
    {
        (void)Det_ReportRuntimeError(MODULE_ID_DIAGSWC, INSTANCE_ID_SINGLE, DIAGSWC_API_ID_INIT,
                                     E_CRC_FAIL);
        return E_CRC_FAIL;
    }

    /* Only up to the count the record claims, and never past this build's event table. A record written
     * by a firmware with more events stays readable for the events both versions share. */
    for (event = 1u; (event <= block.eventCount) && (event <= (uint16)DEM_EVENT_COUNT); event++)
    {
        const DiagSwc_PersistedEventType *entry = &block.events[event - 1u];

        if ((entry->udsStatus & DEM_UDS_CONFIRMED_DTC) == 0u)
        {
            continue;
        }

        /* Replayed as repeated failures so that Dem's own debounce drives it to confirmed, rather than
         * writing Dem's internal state from outside. That keeps Dem the only thing that decides what
         * "confirmed" means. */
        {
            uint8 report;
            for (report = 0u; report < (uint8)DEM_DEFAULT_FAILURE_THRESHOLD + 8u; report++)
            {
                STD_DISCARD(Dem_SetEventStatus((Dem_EventIdType)event, entry->instanceId,
                                               DEM_EVENT_STATUS_FAILED));
                if (Dem_IsEventConfirmed((Dem_EventIdType)event) != FALSE)
                {
                    break;
                }
            }
        }
        restored++;
    }

    DiagSwc_Status.restoredDtcCount = restored;
    return E_OK;
}

/*==================================================================================================
 *  Snapshot provider
 *================================================================================================*/

void DiagSwc_ProvideSnapshot(Dem_SnapshotType *snapshot)
{
    OdoSwc_StateType odo;
    IoHwAb_VoltageType voltage;
    uint32 unixTime = 0u;
    boolean timeValid = FALSE;

    if (snapshot == NULL_PTR)
    {
        return;
    }

    (void)memset(snapshot, 0, sizeof(*snapshot));

    STD_DISCARD(TimeAbs_GetUnixTime(&unixTime, &timeValid));
    snapshot->unixTime = (timeValid != FALSE) ? unixTime : 0u;

    if (OdoSwc_GetState(&odo) == E_OK)
    {
        /* Metres rather than millimetres: a snapshot is for correlating a fault with where the vehicle
         * was in its life, and metre resolution is ample for that while keeping the field at 32 bits. */
        snapshot->odometerMetres = (uint32)(odo.totalDistanceMm / 1000uLL);
        snapshot->speedCmPerSec = (uint16)(odo.speedMmPerSec / 10uL);
    }

    if (IoHwAb_ReadAuxVoltage(&voltage) == E_OK)
    {
        snapshot->vbattMilliVolts = voltage.milliVolts;
    }

    snapshot->resetReason = (uint8)Mcu_GetResetReason();
}

/*==================================================================================================
 *  Service layer
 *================================================================================================*/

/** Build a negative response for @p sid with code @p nrc. */
STATIC Std_ReturnType DiagSwc_NegativeResponse(uint8 sid, uint8 nrc, uint8 *response,
                                               uint16 responseCap, uint16 *responseLen)
{
    if (responseCap < 3u)
    {
        return E_NOT_OK;
    }

    response[0] = (uint8)DIAGSWC_NEGATIVE_RESPONSE_SID;
    response[1] = sid;
    response[2] = nrc;
    *responseLen = 3u;

    DiagSwc_Status.requestsRejected++;
    return E_OK;
}

/** Append a big-endian 32-bit value. */
STATIC void DiagSwc_AppendU32(uint8 *out, uint32 value)
{
    out[0] = (uint8)((value >> 24u) & 0xFFuL);
    out[1] = (uint8)((value >> 16u) & 0xFFuL);
    out[2] = (uint8)((value >> 8u) & 0xFFuL);
    out[3] = (uint8)(value & 0xFFuL);
}

/** Read a big-endian 16-bit value. */
STATIC uint16 DiagSwc_ReadU16(const uint8 *in)
{
    return (uint16)(((uint16)in[0] << 8u) | (uint16)in[1]);
}

/** Serve ReadDTCInformation. */
STATIC Std_ReturnType DiagSwc_ServiceReadDtc(uint8 *response, uint16 responseCap,
                                             uint16 *responseLen)
{
    Dem_DtcType codes[DEM_EVENT_COUNT];
    uint16 count;
    uint16 i;
    uint16 offset;

    count = Dem_GetConfirmedDtcs(codes, (uint16)STD_ARRAY_SIZE(codes));

    /* Response: SID+0x40, count, then three bytes of DTC plus one status byte per entry. */
    if (responseCap < (uint16)(2u + (count * 4u)))
    {
        return E_NOT_OK;
    }

    response[0] = (uint8)(DIAGSWC_SID_READ_DTC_INFORMATION + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
    response[1] = (uint8)count;
    offset = 2u;

    for (i = 0u; i < count; i++)
    {
        uint8 status = 0u;

        response[offset] = (uint8)((codes[i] >> 16u) & 0xFFuL);
        response[offset + 1u] = (uint8)((codes[i] >> 8u) & 0xFFuL);
        response[offset + 2u] = (uint8)(codes[i] & 0xFFuL);

        /* The status byte is looked up by walking the event table, because the DTC list does not carry
         * the event id. With 25 events this is cheaper than maintaining a reverse map. */
        {
            uint16 event;
            for (event = 1u; event <= (uint16)DEM_EVENT_COUNT; event++)
            {
                if ((Dem_GetDtcForEvent((Dem_EventIdType)event, 0u) & 0x00FFFF00uL) ==
                    (codes[i] & 0x00FFFF00uL))
                {
                    STD_DISCARD(Dem_GetEventStatus((Dem_EventIdType)event, &status));
                    break;
                }
            }
        }
        response[offset + 3u] = status;
        offset += 4u;
    }

    *responseLen = offset;
    return E_OK;
}

/** Serve ReadDataByIdentifier. */
STATIC Std_ReturnType DiagSwc_ServiceReadDid(uint16 did, uint8 *response, uint16 responseCap,
                                             uint16 *responseLen)
{
    uint16 offset;

    if (responseCap < 8u)
    {
        return E_NOT_OK;
    }

    response[0] = (uint8)(DIAGSWC_SID_READ_DATA_BY_ID + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
    response[1] = (uint8)((did >> 8u) & 0xFFu);
    response[2] = (uint8)(did & 0xFFu);
    offset = 3u;

    switch (did)
    {
    case DIAGSWC_DID_FIRMWARE_VERSION:
    {
        const char *version = ECU_FIRMWARE_VERSION;
        const uint16 length = (uint16)strlen(version);

        if ((uint32)offset + (uint32)length > (uint32)responseCap)
        {
            return E_NOT_OK;
        }
        (void)memcpy(&response[offset], version, length);
        offset = (uint16)(offset + length);
        break;
    }

    case DIAGSWC_DID_DEVICE_ID:
    {
        char id[13];

        if (Mcu_GetDeviceIdString(id, (uint8)sizeof(id)) != E_OK)
        {
            return E_NOT_OK;
        }
        if ((uint32)offset + 12u > (uint32)responseCap)
        {
            return E_NOT_OK;
        }
        (void)memcpy(&response[offset], id, 12u);
        offset = (uint16)(offset + 12u);
        break;
    }

    case DIAGSWC_DID_ODOMETER:
    case DIAGSWC_DID_TRIP:
    {
        OdoSwc_StateType odo;
        uint64 value;

        if (OdoSwc_GetState(&odo) != E_OK)
        {
            return E_NOT_OK;
        }
        value = (did == DIAGSWC_DID_ODOMETER) ? odo.totalDistanceMm : odo.tripDistanceMm;

        if ((uint32)offset + 8u > (uint32)responseCap)
        {
            return E_NOT_OK;
        }
        /* 64 bits, big-endian, so the full millimetre value is reportable without the truncation a
         * 32-bit field would impose past 4295 km. */
        DiagSwc_AppendU32(&response[offset], (uint32)(value >> 32u));
        DiagSwc_AppendU32(&response[offset + 4u], (uint32)(value & 0xFFFFFFFFuLL));
        offset = (uint16)(offset + 8u);
        break;
    }

    case DIAGSWC_DID_RESET_REASON:
        response[offset] = (uint8)Mcu_GetResetReason();
        offset++;
        break;

    case DIAGSWC_DID_TYRE_DIAMETER:
    case DIAGSWC_DID_GEAR_RATIO:
    case DIAGSWC_DID_VBATT_TRIM:
    {
        NvM_CalibrationType calibration;
        uint16 value;

        if (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
        {
            return E_NOT_OK;
        }
        if (did == DIAGSWC_DID_TYRE_DIAMETER)
        {
            value = calibration.tyreDiameterMilliInch;
        }
        else if (did == DIAGSWC_DID_GEAR_RATIO)
        {
            value = calibration.gearRatioMilli;
        }
        else
        {
            value = (uint16)calibration.vbattOffsetMv;
        }
        response[offset] = (uint8)((value >> 8u) & 0xFFu);
        response[offset + 1u] = (uint8)(value & 0xFFu);
        offset = (uint16)(offset + 2u);
        break;
    }

    case DIAGSWC_DID_LOG_LEVEL:
        response[offset] = (uint8)Log_GetLevel();
        offset++;
        break;

    case DIAGSWC_DID_HEALTH_SUMMARY:
    {
        Dem_StatisticsType dem;
        Mcu_HeapInfoType heap;

        if ((Dem_GetStatistics(&dem) != E_OK) || (Mcu_GetHeapInfo(&heap) != E_OK))
        {
            return E_NOT_OK;
        }
        if ((uint32)offset + 12u > (uint32)responseCap)
        {
            return E_NOT_OK;
        }
        DiagSwc_AppendU32(&response[offset], (uint32)dem.confirmedCount);
        DiagSwc_AppendU32(&response[offset + 4u], heap.heapFreeBytes);
        DiagSwc_AppendU32(&response[offset + 8u], Gpt_GetMonotonicMs());
        offset = (uint16)(offset + 12u);
        break;
    }

    default:
        return E_NOT_FOUND;
    }

    *responseLen = offset;
    return E_OK;
}

/** Serve WriteDataByIdentifier. */
STATIC Std_ReturnType DiagSwc_ServiceWriteDid(uint16 did, const uint8 *value, uint16 valueLen,
                                              uint8 *response, uint16 responseCap,
                                              uint16 *responseLen)
{
    NvM_CalibrationType calibration;

    if (responseCap < 3u)
    {
        return E_NOT_OK;
    }

    switch (did)
    {
    case DIAGSWC_DID_TYRE_DIAMETER:
    case DIAGSWC_DID_GEAR_RATIO:
    {
        uint16 newValue;

        if (valueLen != 2u)
        {
            return E_INVALID_PARAM;
        }
        newValue = DiagSwc_ReadU16(value);

        if (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
        {
            return E_NOT_OK;
        }

        /* Applied through OdoSwc_SetCalibration rather than written straight to NvM, because that is
         * where the plausibility check and the conversion-factor recomputation live. Writing the block
         * directly would leave the running factor stale until the next restart. */
        if (did == DIAGSWC_DID_TYRE_DIAMETER)
        {
            if (OdoSwc_SetCalibration(newValue, calibration.gearRatioMilli) != E_OK)
            {
                return E_INVALID_PARAM;
            }
        }
        else
        {
            if (OdoSwc_SetCalibration(calibration.tyreDiameterMilliInch, newValue) != E_OK)
            {
                return E_INVALID_PARAM;
            }
        }
        break;
    }

    case DIAGSWC_DID_VBATT_TRIM:
    {
        if (valueLen != 2u)
        {
            return E_INVALID_PARAM;
        }
        if (NvM_ReadBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
        {
            return E_NOT_OK;
        }
        calibration.vbattOffsetMv = (sint16)DiagSwc_ReadU16(value);
        if (NvM_WriteBlock(NVM_BLOCK_CALIBRATION, &calibration) != E_OK)
        {
            return E_NOT_OK;
        }
        break;
    }

    case DIAGSWC_DID_LOG_LEVEL:
        if (valueLen != 1u)
        {
            return E_INVALID_PARAM;
        }
        Log_SetLevel((Log_LevelType)value[0]);
        break;

    default:
        /* Every other identifier is read-only. Refusing rather than silently ignoring means a tool that
         * tried to write one finds out. */
        return E_NOT_FOUND;
    }

    response[0] = (uint8)(DIAGSWC_SID_WRITE_DATA_BY_ID + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
    response[1] = (uint8)((did >> 8u) & 0xFFu);
    response[2] = (uint8)(did & 0xFFu);
    *responseLen = 3u;

    return E_OK;
}

/** Serve RoutineControl. */
STATIC Std_ReturnType DiagSwc_ServiceRoutine(uint16 routine, uint8 *response, uint16 responseCap,
                                             uint16 *responseLen)
{
    Std_ReturnType status;

    if (responseCap < 4u)
    {
        return E_NOT_OK;
    }

    switch (routine)
    {
    case DIAGSWC_ROUTINE_RESET_TRIP:
        status = OdoSwc_ResetTrip();
        break;

    case DIAGSWC_ROUTINE_REDISCOVER_PACKS:
        /* Discovery switches pack contactors and takes several seconds. Permitted because a pack replaced
         * in service is otherwise invisible until the next power cycle, which on a vehicle that is never
         * switched off could be weeks. */
        status = Rs485If_DiscoverPacks();
        break;

    case DIAGSWC_ROUTINE_SELF_TEST_LEDS:
        IoHwAb_AllIndicatorsOff();
        status = E_OK;
        break;

    default:
        return E_NOT_FOUND;
    }

    if (status != E_OK)
    {
        return E_NOT_OK;
    }

    response[0] = (uint8)(DIAGSWC_SID_ROUTINE_CONTROL + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
    response[1] = 0x01u; /* startRoutine */
    response[2] = (uint8)((routine >> 8u) & 0xFFu);
    response[3] = (uint8)(routine & 0xFFu);
    *responseLen = 4u;

    return E_OK;
}

Std_ReturnType DiagSwc_HandleRequest(const uint8 *request, uint16 requestLen, uint8 *response,
                                     uint16 responseCap, uint16 *responseLen)
{
    uint8 sid;
    Std_ReturnType status;

    DET_CHECK_RETURN((request != NULL_PTR) && (response != NULL_PTR) && (responseLen != NULL_PTR),
                     MODULE_ID_DIAGSWC, INSTANCE_ID_SINGLE, DIAGSWC_API_ID_HANDLE_REQUEST,
                     DIAGSWC_E_PARAM_POINTER, E_NOT_OK);

    *responseLen = 0u;
    DiagSwc_Status.requestsReceived++;

    if ((requestLen == 0u) || (requestLen > (uint16)DIAGSWC_MAX_REQUEST_SIZE))
    {
        return DiagSwc_NegativeResponse(0x00u, (uint8)DIAGSWC_NRC_INCORRECT_LENGTH, response,
                                        responseCap, responseLen);
    }

    sid = request[0];

    switch (sid)
    {
    case DIAGSWC_SID_READ_DTC_INFORMATION:
        status = DiagSwc_ServiceReadDtc(response, responseCap, responseLen);
        break;

    case DIAGSWC_SID_CLEAR_DIAGNOSTIC_INFO:
        /* Clears both the RAM record and the persistent copy, so a technician can confirm a repair
         * produced a genuinely clean run rather than merely an absence of new faults. */
        status = Dem_ClearDtc(0x00FFFFFFuL);
        if (status == E_OK)
        {
            Det_ClearHistory();
            status = DiagSwc_Persist();
        }
        if (status == E_OK)
        {
            if (responseCap < 1u)
            {
                status = E_NOT_OK;
            }
            else
            {
                response[0] =
                    (uint8)(DIAGSWC_SID_CLEAR_DIAGNOSTIC_INFO + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
                *responseLen = 1u;
            }
        }
        break;

    case DIAGSWC_SID_READ_DATA_BY_ID:
        if (requestLen != 3u)
        {
            return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_INCORRECT_LENGTH, response,
                                            responseCap, responseLen);
        }
        status = DiagSwc_ServiceReadDid(DiagSwc_ReadU16(&request[1]), response, responseCap,
                                       responseLen);
        break;

    case DIAGSWC_SID_WRITE_DATA_BY_ID:
        if (requestLen < 4u)
        {
            return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_INCORRECT_LENGTH, response,
                                            responseCap, responseLen);
        }
        status = DiagSwc_ServiceWriteDid(DiagSwc_ReadU16(&request[1]), &request[3],
                                        (uint16)(requestLen - 3u), response, responseCap,
                                        responseLen);
        break;

    case DIAGSWC_SID_ROUTINE_CONTROL:
        if (requestLen != 4u)
        {
            return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_INCORRECT_LENGTH, response,
                                            responseCap, responseLen);
        }
        status = DiagSwc_ServiceRoutine(DiagSwc_ReadU16(&request[2]), response, responseCap,
                                       responseLen);
        break;

    case DIAGSWC_SID_ECU_RESET:
#if (DIAGSWC_ALLOW_REMOTE_RESET == STD_ON)
        /* The response is built and returned first; the caller publishes it and then invokes the
         * shutdown path. Resetting from inside this function would mean the requester never learns
         * whether the request was accepted. */
        if (responseCap < 2u)
        {
            status = E_NOT_OK;
        }
        else
        {
            response[0] = (uint8)(DIAGSWC_SID_ECU_RESET + DIAGSWC_POSITIVE_RESPONSE_OFFSET);
            response[1] = 0x01u; /* hardReset */
            *responseLen = 2u;
            status = E_OK;
        }
#else
        return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_SERVICE_NOT_SUPPORTED, response,
                                        responseCap, responseLen);
#endif
        break;

    default:
        return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_SERVICE_NOT_SUPPORTED, response,
                                        responseCap, responseLen);
    }

    if (status == E_NOT_FOUND)
    {
        return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_REQUEST_OUT_OF_RANGE, response,
                                        responseCap, responseLen);
    }
    if (status == E_INVALID_PARAM)
    {
        return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_REQUEST_OUT_OF_RANGE, response,
                                        responseCap, responseLen);
    }
    if (status != E_OK)
    {
        return DiagSwc_NegativeResponse(sid, (uint8)DIAGSWC_NRC_CONDITIONS_NOT_CORRECT, response,
                                        responseCap, responseLen);
    }

    DiagSwc_Status.requestsServed++;
    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType DiagSwc_Init(void)
{
    Std_ReturnType restoreStatus;

    (void)memset(&DiagSwc_Status, 0, sizeof(DiagSwc_Status));
    DiagSwc_LastPersistCheckMs = Gpt_GetMonotonicMs();
    DiagSwc_LastConfirmedCount = 0u;
    DiagSwc_Initialised = TRUE;

    Dem_SetSnapshotProvider(&DiagSwc_ProvideSnapshot);

    restoreStatus = DiagSwc_Restore();

    /* E_NOT_FOUND is the normal path on a new unit. Only a CRC failure indicates a problem worth
     * reporting, because it means a record existed and was damaged. */
    return (restoreStatus == E_CRC_FAIL) ? E_NOT_OK : E_OK;
}

void DiagSwc_MainFunction(void)
{
    Dem_StatisticsType stats;

    if (DiagSwc_Initialised == FALSE)
    {
        return;
    }

    if (Gpt_HasElapsed(DiagSwc_LastPersistCheckMs, DIAGSWC_PERSIST_CHECK_MS) == FALSE)
    {
        return;
    }
    DiagSwc_LastPersistCheckMs = Gpt_GetMonotonicMs();

    if (Dem_GetStatistics(&stats) != E_OK)
    {
        return;
    }

    /* Written only when the confirmed count has changed. Dem's own MainFunction signals that something
     * changed, but the count is the cheaper and more direct test, and it means a fault reporting on every
     * cycle produces no writes at all once it has confirmed. */
    if (stats.confirmedCount != DiagSwc_LastConfirmedCount)
    {
        DiagSwc_LastConfirmedCount = stats.confirmedCount;
        STD_DISCARD(DiagSwc_Persist());
    }

    Dem_MainFunction();
}

Std_ReturnType DiagSwc_GetStatus(DiagSwc_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_DIAGSWC, INSTANCE_ID_SINGLE,
                     DIAGSWC_API_ID_MAIN_FUNCTION, DIAGSWC_E_PARAM_POINTER, E_NOT_OK);

    *status = DiagSwc_Status;
    return E_OK;
}

void DiagSwc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = DIAGSWC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_DIAGSWC;
        versioninfo->sw_major_version = DIAGSWC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = DIAGSWC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = DIAGSWC_SW_PATCH_VERSION;
    }
}

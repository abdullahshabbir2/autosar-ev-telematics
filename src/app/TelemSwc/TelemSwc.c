/**
 * @file    TelemSwc.c
 * @brief   Telemetry component implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "app/TelemSwc/TelemSwc.h"

#include <stdio.h>
#include <string.h>

#include "app/BattSwc/BattSwc.h"
#include "ecuabs/CanIf/CanIf.h"
#include "services/Com/Com.h"
#include "services/ComM/ComM.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "ecuabs/FsAbs/FsAbs.h"
#include "ecuabs/GnssIf/GnssIf.h"
#include "mcal/Gpt/Gpt.h"
#include "ecuabs/IoHwAb/IoHwAb.h"
#include "mcal/Mcu/Mcu.h"
#include "ecuabs/NetIf/NetIf.h"
#include "services/NvM/NvM.h"
#include "app/OdoSwc/OdoSwc.h"
#include "ecuabs/TimeAbs/TimeAbs.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean TelemSwc_Initialised = FALSE;
STATIC TelemSwc_StatusType TelemSwc_Status;

/**
 * @brief The record acquired this cycle, waiting to be published live.
 *
 * One buffer, because only one record is in flight at a time: the acquisition task fills it and the
 * connectivity task publishes it, and the storage task's write has already completed by then. Holding a
 * queue of them would mean either a large static buffer or dynamic allocation, and the SD card already
 * is the queue.
 */
STATIC char TelemSwc_LiveRecord[COM_RECORD_BUFFER_SIZE];
STATIC uint16 TelemSwc_LiveRecordLength;
STATIC boolean TelemSwc_LivePending;

/** Buffer for a backlog record read back from the card. */
STATIC char TelemSwc_BacklogRecord[COM_RECORD_BUFFER_SIZE];

/** Topics, built once at Init from the provisioned device identifier. */
STATIC char TelemSwc_DataTopic[NETIF_MAX_TOPIC_SIZE];
STATIC char TelemSwc_HealthTopic[NETIF_MAX_TOPIC_SIZE];
STATIC char TelemSwc_BackfillRequestTopic[NETIF_MAX_TOPIC_SIZE];
STATIC char TelemSwc_BackfillDataTopic[NETIF_MAX_TOPIC_SIZE];

STATIC char TelemSwc_DeviceId[24];

STATIC Gpt_TimestampType TelemSwc_LastHealthMs;

/*==================================================================================================
 *  Backfill state
 *
 *  A request names up to COM_MAX_BACKFILL_DATES days. They are served one at a time, one chunk per
 *  cycle, so a request cannot displace live telemetry. The whole state is a fixed array and three
 *  indices -- v1 allocated a heap node per requested date from inside the MQTT callback.
 *================================================================================================*/

typedef struct
{
    Com_BackfillDateType dates[COM_MAX_BACKFILL_DATES];
    uint8 dateCount;
    uint8 currentDate;
    Com_ChunkPlanType plan;
    uint32 currentChunk;
    boolean active;
} TelemSwc_BackfillType;

STATIC TelemSwc_BackfillType TelemSwc_Backfill;

/*==================================================================================================
 *  Topic construction
 *================================================================================================*/

/** Build @p out as @p prefix followed by the device identifier. */
STATIC Std_ReturnType TelemSwc_BuildTopic(char *out, uint16 size, const char *prefix)
{
    const uint16 prefixLength = (uint16)strlen(prefix);
    const uint16 idLength = (uint16)strlen(TelemSwc_DeviceId);

    if (((uint32)prefixLength + (uint32)idLength + 1u) > (uint32)size)
    {
        return E_NO_SPACE;
    }

    (void)memset(out, 0, size);
    (void)memcpy(out, prefix, prefixLength);
    (void)memcpy(&out[prefixLength], TelemSwc_DeviceId, idLength);

    return E_OK;
}

/*==================================================================================================
 *  Inbound request handling
 *================================================================================================*/

/**
 * @brief Handler for a backfill request.
 *
 * Called from the network stack's context, so it does the least possible work: parse into the fixed
 * array and return. v1 parsed the payload and allocated a heap node per date inside this callback, which
 * is both slow in that context and unbounded.
 */
STATIC void TelemSwc_OnBackfillRequest(const char *topic, const uint8 *payload, uint16 payloadLen)
{
    uint8 count = 0u;
    uint8 dropped = 0u;

    COMPILER_UNUSED(topic);

    if (TelemSwc_Backfill.active != FALSE)
    {
        /* A transfer is already running. The new request is refused rather than queued or allowed to
         * replace it: replacing it would abandon a partly-delivered day with no record of how far it
         * got, and queueing would need unbounded storage for something an operator can simply repeat. */
        return;
    }

    if (Com_ParseBackfillRequest(payload, payloadLen, TelemSwc_Backfill.dates,
                                 (uint8)COM_MAX_BACKFILL_DATES, &count, &dropped) != E_OK)
    {
        return;
    }

    TelemSwc_Backfill.dateCount = count;
    TelemSwc_Backfill.currentDate = 0u;
    TelemSwc_Backfill.currentChunk = 0u;
    TelemSwc_Backfill.plan.chunkCount = 0u;
    TelemSwc_Backfill.active = TRUE;
    TelemSwc_Status.backfillRequests++;
}

/*==================================================================================================
 *  Record assembly
 *================================================================================================*/

/** Gather one record from every source into @p record. */
STATIC void TelemSwc_GatherRecord(Com_TelemetryRecordType *record)
{
    CanIf_McuDataType mcu;
    IoHwAb_VoltageType voltage;
    OdoSwc_StateType odo;
    Dem_StatisticsType dem;
    Mcu_HeapInfoType heap;
    STATIC GnssIf_PositionType position;
    uint32 unixTime = 0u;
    boolean timeValid = FALSE;

    (void)memset(record, 0, sizeof(*record));

    record->sequenceNumber = TelemSwc_Status.sequenceNumber;
    record->uptimeMs = Gpt_GetMonotonicMs();
    record->deviceId = TelemSwc_DeviceId;

    STD_DISCARD(TimeAbs_GetUnixTime(&unixTime, &timeValid));
    record->unixTime = (timeValid != FALSE) ? unixTime : 0u;

    if (IoHwAb_ReadAuxVoltage(&voltage) == E_OK)
    {
        record->auxVoltageMilliVolts = voltage.milliVolts;
        record->auxVoltageValid = voltage.valid;
    }

    /* The drive signals are marked valid only if they are *fresh*. v1 published whatever the last
     * received frame contained, indefinitely, so a controller that stopped transmitting produced records
     * showing its final speed forever. */
    if ((CanIf_GetMcuData(&mcu) == E_OK) && (CanIf_IsMcuDataFresh() != FALSE))
    {
        record->motorRpm = mcu.motorRpm;
        record->dcVoltageDeciVolt = mcu.dcVoltageDeciVolt;
        record->dcCurrentDeciAmp = mcu.dcCurrentDeciAmp;
        record->mcuFaultCode = mcu.faultCode;
        record->driveDataValid = TRUE;
    }

    if (OdoSwc_GetState(&odo) == E_OK)
    {
        record->totalDistanceMm = odo.totalDistanceMm;
        record->tripDistanceMm = odo.tripDistanceMm;
        record->speedMmPerSec = odo.speedMmPerSec;
    }

    /* Position is included only while the fix is fresh; a stale one is reported as absent rather than as
     * the vehicle's current location. */
    if ((GnssIf_GetPosition(&position) == E_OK) && (GnssIf_IsFixFresh() != FALSE))
    {
        record->position = &position;
    }
    else
    {
        record->position = NULL_PTR;
    }

    record->packs = BattSwc_GetPackStates();

    if (Dem_GetStatistics(&dem) == E_OK)
    {
        record->confirmedDtcCount = dem.confirmedCount;
    }
    if (Mcu_GetHeapInfo(&heap) == E_OK)
    {
        record->heapFreeBytes = heap.heapFreeBytes;
    }
    record->bearerState = (uint8)NetIf_GetActiveBearer();
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType TelemSwc_Init(void)
{
    NvM_DeviceConfigType config;

    (void)memset(&TelemSwc_Status, 0, sizeof(TelemSwc_Status));
    (void)memset(&TelemSwc_Backfill, 0, sizeof(TelemSwc_Backfill));
    TelemSwc_LivePending = FALSE;
    TelemSwc_LiveRecordLength = 0u;
    TelemSwc_LastHealthMs = Gpt_GetMonotonicMs();

    if (NvM_ReadBlock(NVM_BLOCK_DEVICE_CONFIG, &config) != E_OK)
    {
        return E_NOT_OK;
    }
    (void)memcpy(TelemSwc_DeviceId, config.deviceId, sizeof(TelemSwc_DeviceId));
    TelemSwc_DeviceId[sizeof(TelemSwc_DeviceId) - 1u] = '\0';

    /* A device with no provisioned identifier falls back to its factory MAC, so an unprovisioned unit
     * still publishes under a unique, stable topic rather than sharing one with every other unprovisioned
     * unit in the fleet. */
    if (TelemSwc_DeviceId[0] == '\0')
    {
        if (Mcu_GetDeviceIdString(TelemSwc_DeviceId, (uint8)sizeof(TelemSwc_DeviceId)) != E_OK)
        {
            return E_NOT_OK;
        }
    }

    if ((TelemSwc_BuildTopic(TelemSwc_DataTopic, (uint16)sizeof(TelemSwc_DataTopic),
                             TELEM_TOPIC_DATA_PREFIX) != E_OK) ||
        (TelemSwc_BuildTopic(TelemSwc_HealthTopic, (uint16)sizeof(TelemSwc_HealthTopic),
                             TELEM_TOPIC_HEALTH_PREFIX) != E_OK) ||
        (TelemSwc_BuildTopic(TelemSwc_BackfillRequestTopic,
                             (uint16)sizeof(TelemSwc_BackfillRequestTopic),
                             TELEM_TOPIC_BACKFILL_REQUEST_PREFIX) != E_OK) ||
        (TelemSwc_BuildTopic(TelemSwc_BackfillDataTopic,
                             (uint16)sizeof(TelemSwc_BackfillDataTopic),
                             TELEM_TOPIC_BACKFILL_DATA_PREFIX) != E_OK))
    {
        return E_NOT_OK;
    }

    /* Subscribed once. NetIf re-applies it after every reconnection, so a dropped session cannot
     * silently leave the ECU unable to answer a backfill request. */
    STD_DISCARD(NetIf_Subscribe(TelemSwc_BackfillRequestTopic, &TelemSwc_OnBackfillRequest));

    TelemSwc_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType TelemSwc_AcquireAndStore(void)
{
    Com_TelemetryRecordType record;
    char dateStamp[9];
    Std_ReturnType storeStatus;

    DET_CHECK_RETURN(TelemSwc_Initialised != FALSE, MODULE_ID_TELEMSWC, INSTANCE_ID_SINGLE,
                     TELEMSWC_API_ID_ACQUIRE, TELEMSWC_E_UNINIT, E_NOT_OK);

    TelemSwc_GatherRecord(&record);

    if (Com_SerialiseCsvRecord(&record, TelemSwc_LiveRecord, (uint16)sizeof(TelemSwc_LiveRecord),
                               &TelemSwc_LiveRecordLength) != E_OK)
    {
        TelemSwc_Status.serialiseFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_TELEMSWC, INSTANCE_ID_SINGLE,
                                     TELEMSWC_API_ID_ACQUIRE, TELEMSWC_E_SERIALISE_FAILED);
        return E_NOT_OK;
    }

    TelemSwc_Status.recordsAcquired++;
    TelemSwc_Status.sequenceNumber++;

    /* Queued for live publication regardless of whether a bearer is up. v1 set its transfer variable
     * only when WiFi and the broker were both already connected, so a record acquired during a coverage
     * gap was never queued at all -- it existed only on the card, and reached the cloud only if a
     * separate byte-count threshold happened to be crossed later. */
    TelemSwc_LivePending = TRUE;

    /* The card is written first, and is what makes the record durable. */
    if (TimeAbs_FormatDateStamp(dateStamp, (uint16)sizeof(dateStamp)) != E_OK)
    {
        /* No valid clock, so there is no file the record can honestly be filed under. It is still
         * published live, where its monotonic uptime makes it orderable, but it is not stored -- a file
         * named after a guessed date would be almost impossible to reconcile later. */
        TelemSwc_Status.droppedNoClock++;
        (void)Det_ReportRuntimeError(MODULE_ID_TELEMSWC, INSTANCE_ID_SINGLE,
                                     TELEMSWC_API_ID_ACQUIRE, TELEMSWC_E_NO_CLOCK);
        return E_NOT_OK;
    }

    storeStatus = FsAbs_AppendRecord(dateStamp, TelemSwc_LiveRecord);
    if (storeStatus == E_OK)
    {
        TelemSwc_Status.recordsStored++;
    }
    else
    {
        TelemSwc_Status.storeFailures++;
    }

    return storeStatus;
}

/** Publish the live record if one is pending and a session exists. */
STATIC void TelemSwc_PublishLive(void)
{
    if ((TelemSwc_LivePending == FALSE) || (ComM_IsCommunicationAvailable() == FALSE))
    {
        return;
    }

    if (NetIf_Publish(TelemSwc_DataTopic, (const uint8 *)TelemSwc_LiveRecord,
                      TelemSwc_LiveRecordLength, FALSE) == E_OK)
    {
        TelemSwc_Status.recordsPublishedLive++;
        TelemSwc_LivePending = FALSE;
    }
    else
    {
        TelemSwc_Status.publishFailures++;
        /* Left pending. It is on the card as well, so the backlog path will deliver it even if the live
         * attempt never succeeds -- which is why nothing is lost by simply retrying next cycle. */
    }
}

/** Publish up to ::TELEM_BACKLOG_RECORDS_PER_CYCLE records from the card. */
STATIC void TelemSwc_DrainBacklog(void)
{
    uint8 published = 0u;
    uint8 skipped = 0u;

    if (ComM_IsCommunicationAvailable() == FALSE)
    {
        return;
    }

    while ((published < (uint8)TELEM_BACKLOG_RECORDS_PER_CYCLE) &&
           (skipped < (uint8)TELEM_MAX_SKIPS_PER_CYCLE))
    {
        uint16 length = 0u;
        const Std_ReturnType readStatus = FsAbs_ReadRecordAtCursor(
            TelemSwc_BacklogRecord, (uint16)sizeof(TelemSwc_BacklogRecord), &length);

        if (readStatus == E_NOT_FOUND)
        {
            /* Caught up with the writer. */
            break;
        }

        if (readStatus == E_CRC_FAIL)
        {
            /* A damaged record. Skipped deliberately and counted, with the number of skips per cycle
             * bounded -- v1's reader had no equivalent and stalled the transfer permanently, which is
             * what its author's "this is an infinite loop" comment was about. */
            STD_DISCARD(FsAbs_SkipCorruptRecord());
            skipped++;
            continue;
        }

        if (readStatus != E_OK)
        {
            break;
        }

        if (NetIf_Publish(TelemSwc_DataTopic, (const uint8 *)TelemSwc_BacklogRecord, length,
                          FALSE) != E_OK)
        {
            TelemSwc_Status.publishFailures++;
            /* The cursor is deliberately not advanced, so the record is retried rather than lost. */
            break;
        }

        /* Advanced only after the broker accepted it. This ordering is what makes delivery at-least-once
         * without needing a higher MQTT quality of service. */
        if (FsAbs_AdvanceCursor() != E_OK)
        {
            break;
        }

        TelemSwc_Status.recordsPublishedBacklog++;
        published++;
    }
}

/** Send up to ::TELEM_BACKFILL_CHUNKS_PER_CYCLE chunks of a historical transfer. */
STATIC void TelemSwc_ServeBackfill(void)
{
    uint8 sent = 0u;

    if ((TelemSwc_Backfill.active == FALSE) || (ComM_IsCommunicationAvailable() == FALSE))
    {
        return;
    }

    while (sent < (uint8)TELEM_BACKFILL_CHUNKS_PER_CYCLE)
    {
        char fileName[COM_FILENAME_SIZE];
        STATIC uint8 chunk[TELEM_BACKFILL_BUFFER_SIZE];
        uint32 offset = 0u;
        uint32 length = 0u;
        uint32 read = 0u;

        if (TelemSwc_Backfill.currentDate >= TelemSwc_Backfill.dateCount)
        {
            TelemSwc_Backfill.active = FALSE;
            break;
        }

        if (Com_FormatLogFileName(fileName, (uint16)sizeof(fileName),
                                  TelemSwc_Backfill.dates[TelemSwc_Backfill.currentDate].date) !=
            E_OK)
        {
            TelemSwc_Backfill.currentDate++;
            continue;
        }

        /* The plan is computed once per file, on the first chunk. */
        if (TelemSwc_Backfill.currentChunk == 0u)
        {
            uint32 fileSize = 0u;

            if (FsAbs_GetFileSize(fileName, &fileSize) != E_OK)
            {
                /* The day was never logged. Skipped silently: an operator asking for a day the vehicle
                 * was not in service is a normal request, not a fault. */
                TelemSwc_Backfill.currentDate++;
                continue;
            }
            if (Com_ComputeChunkPlan(fileSize, (uint32)COM_TRANSFER_CHUNK_SIZE,
                                     &TelemSwc_Backfill.plan) != E_OK)
            {
                TelemSwc_Backfill.currentDate++;
                continue;
            }
            /* An empty file yields a zero-chunk plan and is skipped. This is the case that sent v1's
             * transfer loop into roughly four billion iterations. */
            if (TelemSwc_Backfill.plan.chunkCount == 0u)
            {
                TelemSwc_Backfill.currentDate++;
                continue;
            }
        }

        if (Com_GetChunkExtent(&TelemSwc_Backfill.plan, TelemSwc_Backfill.currentChunk,
                               (uint32)COM_TRANSFER_CHUNK_SIZE, &offset, &length) != E_OK)
        {
            /* Past the end of this file: move to the next requested day. */
            TelemSwc_Backfill.currentDate++;
            TelemSwc_Backfill.currentChunk = 0u;
            continue;
        }

        if (FsAbs_ReadFileChunk(fileName, offset, chunk, length, &read) != E_OK)
        {
            TelemSwc_Backfill.currentDate++;
            TelemSwc_Backfill.currentChunk = 0u;
            continue;
        }

        if (NetIf_Publish(TelemSwc_BackfillDataTopic, chunk, (uint16)read, FALSE) != E_OK)
        {
            /* Retried next cycle from the same chunk. */
            break;
        }

        TelemSwc_Status.backfillChunksSent++;
        TelemSwc_Backfill.currentChunk++;
        sent++;
    }
}

void TelemSwc_MainFunction(void)
{
    if (TelemSwc_Initialised == FALSE)
    {
        return;
    }

    /* Live first. A stale live record is worthless, whereas a stale backlog record is merely late. */
    TelemSwc_PublishLive();
    TelemSwc_DrainBacklog();
    TelemSwc_ServeBackfill();

    if (Gpt_HasElapsed(TelemSwc_LastHealthMs, TELEM_HEALTH_INTERVAL_MS) != FALSE)
    {
        TelemSwc_PublishHealth();
        TelemSwc_LastHealthMs = Gpt_GetMonotonicMs();
    }
}

void TelemSwc_PublishHealth(void)
{
    char buffer[TELEM_HEALTH_BUFFER_SIZE];
    Dem_StatisticsType dem;
    Det_StatisticsType det;
    NetIf_StatusType net;
    FsAbs_StatusType fs;
    Mcu_HeapInfoType heap;
    OdoSwc_StateType odo;
    Rs485If_StatisticsType bus;
    int written;

    if ((TelemSwc_Initialised == FALSE) || (ComM_IsCommunicationAvailable() == FALSE))
    {
        return;
    }

    (void)memset(&dem, 0, sizeof(dem));
    (void)memset(&det, 0, sizeof(det));
    (void)memset(&net, 0, sizeof(net));
    (void)memset(&fs, 0, sizeof(fs));
    (void)memset(&heap, 0, sizeof(heap));
    (void)memset(&odo, 0, sizeof(odo));
    (void)memset(&bus, 0, sizeof(bus));

    STD_DISCARD(Dem_GetStatistics(&dem));
    Det_GetStatistics(&det);
    STD_DISCARD(NetIf_GetStatus(&net));
    STD_DISCARD(FsAbs_GetStatus(&fs));
    STD_DISCARD(Mcu_GetHeapInfo(&heap));
    STD_DISCARD(OdoSwc_GetState(&odo));
    STD_DISCARD(Rs485If_GetStatistics(&bus));

    /* A compact key=value line rather than JSON: it is a third of the size on a metered link, and every
     * consumer of it is a script. */
    written = snprintf(buffer, sizeof(buffer),
                       "up=%lu dtc=%u det=%lu rt=%lu heap=%lu hmin=%lu "
                       "bearer=%u rssi=%d pub=%lu pubf=%lu "
                       "sd=%u free=%lu rec=%lu recf=%lu corrupt=%lu "
                       "odo=%llu trip=%llu odoacc=%lu odorej=%lu "
                       "b485s=%lu b485crc=%lu b485to=%lu",
                       (unsigned long)Gpt_GetMonotonicMs(), (unsigned int)dem.confirmedCount,
                       (unsigned long)det.devErrorCount, (unsigned long)det.runtimeErrorCount,
                       (unsigned long)heap.heapFreeBytes, (unsigned long)heap.heapMinFreeBytes,
                       (unsigned int)net.activeBearer, (int)net.signalStrengthDbm,
                       (unsigned long)net.publishCount, (unsigned long)net.publishFailures,
                       (unsigned int)(fs.mounted != FALSE ? 1u : 0u), (unsigned long)fs.freeMiB,
                       (unsigned long)fs.recordsWritten, (unsigned long)fs.writeFailures,
                       (unsigned long)fs.corruptRecords,
                       (unsigned long long)odo.totalDistanceMm,
                       (unsigned long long)odo.tripDistanceMm,
                       (unsigned long)odo.acceptedSamples, (unsigned long)odo.rejectedRpmSamples,
                       (unsigned long)bus.framesSent, (unsigned long)bus.crcFailures,
                       (unsigned long)bus.timeouts);

    if ((written > 0) && ((uint32)written < sizeof(buffer)))
    {
        STD_DISCARD(NetIf_Publish(TelemSwc_HealthTopic, (const uint8 *)buffer, (uint16)written,
                                  TRUE));
    }
}

Std_ReturnType TelemSwc_GetStatus(TelemSwc_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_TELEMSWC, INSTANCE_ID_SINGLE,
                     TELEMSWC_API_ID_GET_STATUS, TELEMSWC_E_PARAM_POINTER, E_NOT_OK);

    *status = TelemSwc_Status;
    return E_OK;
}

void TelemSwc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = TELEMSWC_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_TELEMSWC;
        versioninfo->sw_major_version = TELEMSWC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = TELEMSWC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = TELEMSWC_SW_PATCH_VERSION;
    }
}

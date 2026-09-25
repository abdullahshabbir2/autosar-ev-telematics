/**
 * @file    Com.c
 * @brief   Communication service implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Com/Com.h"

#include <string.h>

#include "services/Det/Det.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC Com_StatisticsType Com_Stats;
STATIC boolean Com_Initialised = FALSE;

/*==================================================================================================
 *  Bounded output writer
 *
 *  A tiny append-only cursor over a caller-supplied buffer. Every write is bounds-checked and the
 *  overflow flag is sticky, so a truncated record is detected once at the end rather than requiring a
 *  check after each of nearly two hundred fields. This is what replaces v1's repeated
 *  `towrite += String(...)`: no allocation, no reallocation, and a definite answer to "did it fit".
 *================================================================================================*/

typedef struct
{
    char *buffer;
    uint16 capacity; /* including the terminator */
    uint16 length;   /* excluding the terminator */
    boolean overflow;
} Com_WriterType;

STATIC void Com_WriterInit(Com_WriterType *w, char *buffer, uint16 capacity)
{
    w->buffer = buffer;
    w->capacity = capacity;
    w->length = 0u;
    w->overflow = FALSE;

    if (capacity > 0u)
    {
        buffer[0] = '\0';
    }
    else
    {
        w->overflow = TRUE;
    }
}

STATIC void Com_WriteChar(Com_WriterType *w, char c)
{
    if (w->overflow != FALSE)
    {
        return;
    }
    /* One byte must always remain for the terminator, so the buffer is never left unterminated. */
    if ((uint32)w->length + 2u > (uint32)w->capacity)
    {
        w->overflow = TRUE;
        return;
    }
    w->buffer[w->length] = c;
    w->length++;
    w->buffer[w->length] = '\0';
}

STATIC void Com_WriteString(Com_WriterType *w, const char *text)
{
    uint16 i = 0u;

    if (text == NULL_PTR)
    {
        return;
    }
    while (text[i] != '\0')
    {
        Com_WriteChar(w, text[i]);
        if (w->overflow != FALSE)
        {
            return;
        }
        i++;
    }
}

/** Append an unsigned 64-bit value in decimal. */
STATIC void Com_WriteU64(Com_WriterType *w, uint64 value)
{
    /* 20 digits is the widest a uint64 can be. Built backwards into a local buffer so no division by
     * a power of ten is needed and no allocation is involved. */
    char digits[21];
    uint8 count = 0u;

    if (value == 0uLL)
    {
        Com_WriteChar(w, '0');
        return;
    }

    while ((value > 0uLL) && (count < (uint8)sizeof(digits)))
    {
        digits[count] = (char)('0' + (char)(value % 10uLL));
        value /= 10uLL;
        count++;
    }

    while (count > 0u)
    {
        count--;
        Com_WriteChar(w, digits[count]);
    }
}

/** Append a signed 32-bit value in decimal. */
STATIC void Com_WriteS32(Com_WriterType *w, sint32 value)
{
    if (value < 0)
    {
        Com_WriteChar(w, '-');
        /* Negated through uint32 so that the most negative value does not overflow: -(-2147483648)
         * is not representable as a sint32. */
        Com_WriteU64(w, (uint64)(~(uint32)value) + 1uLL);
    }
    else
    {
        Com_WriteU64(w, (uint64)(uint32)value);
    }
}

/** Append a field separator. */
STATIC void Com_WriteSeparator(Com_WriterType *w)
{
    Com_WriteChar(w, COM_FIELD_SEPARATOR);
}

/** Append @p value, or nothing at all if @p valid is FALSE. */
STATIC void Com_WriteU64OrBlank(Com_WriterType *w, uint64 value, boolean valid)
{
    if (valid != FALSE)
    {
        Com_WriteU64(w, value);
    }
}

/** Append @p value, or nothing at all if @p valid is FALSE. */
STATIC void Com_WriteS32OrBlank(Com_WriterType *w, sint32 value, boolean valid)
{
    if (valid != FALSE)
    {
        Com_WriteS32(w, value);
    }
}

/*==================================================================================================
 *  Date handling
 *================================================================================================*/

STATIC boolean Com_IsDigitChar(char c)
{
    return ((c >= '0') && (c <= '9')) ? TRUE : FALSE;
}

/** Days in @p month of @p year, 0 if @p month is out of range. */
STATIC uint8 Com_DaysInMonth(uint16 year, uint8 month)
{
    STATIC const uint8 days[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};

    if ((month < 1u) || (month > 12u))
    {
        return 0u;
    }

    if (month == 2u)
    {
        const boolean leap =
            (((year % 4u) == 0u) && (((year % 100u) != 0u) || ((year % 400u) == 0u))) ? TRUE : FALSE;
        return (leap != FALSE) ? 29u : 28u;
    }

    return days[month - 1u];
}

boolean Com_IsValidDate(const char *date)
{
    uint16 year;
    uint8 month;
    uint8 day;
    uint8 i;

    if (date == NULL_PTR)
    {
        return FALSE;
    }

    for (i = 0u; i < (uint8)COM_DATE_LENGTH; i++)
    {
        if (Com_IsDigitChar(date[i]) == FALSE)
        {
            return FALSE;
        }
    }
    if (date[COM_DATE_LENGTH] != '\0')
    {
        return FALSE;
    }

    year = (uint16)(((uint16)(date[0] - '0') * 1000u) + ((uint16)(date[1] - '0') * 100u)
                    + ((uint16)(date[2] - '0') * 10u) + (uint16)(date[3] - '0'));
    month = (uint8)(((uint8)(date[4] - '0') * 10u) + (uint8)(date[5] - '0'));
    day = (uint8)(((uint8)(date[6] - '0') * 10u) + (uint8)(date[7] - '0'));

    /* Bounded below by the year this firmware family was written: a date before that cannot name a
     * log file this ECU produced, and accepting one only creates a request that can never succeed. */
    if ((year < 2024u) || (year > 2099u))
    {
        return FALSE;
    }

    /* The day is checked against the month and the year's leap status. v1 accepted any day from 1 to
     * 31 in any month, so a request for 20240230 produced a file name that could never exist and a
     * transfer that reported "Not Found" -- indistinguishable from a genuinely missing log. */
    if ((day < 1u) || (day > Com_DaysInMonth(year, month)))
    {
        return FALSE;
    }

    return TRUE;
}

Std_ReturnType Com_FormatLogFileName(char *buffer, uint16 size, const char *date)
{
    Com_WriterType writer;

    DET_CHECK_RETURN((buffer != NULL_PTR) && (date != NULL_PTR), MODULE_ID_COM, INSTANCE_ID_SINGLE,
                     COM_API_ID_FORMAT_HEADER, COM_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(Com_IsValidDate(date) != FALSE, MODULE_ID_COM, INSTANCE_ID_SINGLE,
                     COM_API_ID_FORMAT_HEADER, COM_E_MALFORMED_REQUEST, E_NOT_OK);

    Com_WriterInit(&writer, buffer, size);
    Com_WriteChar(&writer, '/');
    Com_WriteString(&writer, date);
    Com_WriteString(&writer, ".csv");

    return (writer.overflow == FALSE) ? E_OK : E_NOT_OK;
}

/*==================================================================================================
 *  Chunking
 *================================================================================================*/

Std_ReturnType Com_ComputeChunkPlan(uint32 fileSize, uint32 chunkSize, Com_ChunkPlanType *plan)
{
    DET_CHECK_RETURN(plan != NULL_PTR, MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_CHUNK_PLAN,
                     COM_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(chunkSize > 0u, MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_CHUNK_PLAN, E_PARAM_VALUE,
                     E_NOT_OK);

    /* An empty file yields a zero-chunk plan, stated explicitly. This is the case that broke v1: it
     * computed a chunk count of 0 and then looped to `count - 1`, which on an unsigned type is
     * SIZE_MAX -- roughly four billion iterations, each publishing a chunk read from past the end of
     * an empty file. Returning every quantity the caller needs means no call site subtracts. */
    if (fileSize == 0u)
    {
        plan->chunkCount = 0u;
        plan->fullChunkCount = 0u;
        plan->lastChunkSize = 0u;
        return E_OK;
    }

    plan->fullChunkCount = fileSize / chunkSize;
    plan->lastChunkSize = fileSize % chunkSize;

    if (plan->lastChunkSize == 0u)
    {
        /* Divides exactly: every chunk is a full one and there is no short tail. */
        plan->chunkCount = plan->fullChunkCount;
    }
    else
    {
        plan->chunkCount = plan->fullChunkCount + 1u;
    }

    return E_OK;
}

Std_ReturnType Com_GetChunkExtent(const Com_ChunkPlanType *plan, uint32 index, uint32 chunkSize,
                                  uint32 *offset, uint32 *length)
{
    DET_CHECK_RETURN((plan != NULL_PTR) && (offset != NULL_PTR) && (length != NULL_PTR), MODULE_ID_COM,
                     INSTANCE_ID_SINGLE, COM_API_ID_CHUNK_PLAN, COM_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(chunkSize > 0u, MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_CHUNK_PLAN, E_PARAM_VALUE,
                     E_NOT_OK);

    if (index >= plan->chunkCount)
    {
        return E_NOT_FOUND;
    }

    *offset = index * chunkSize;

    /* The final chunk is short only when the file did not divide exactly. */
    if ((index == (plan->chunkCount - 1u)) && (plan->lastChunkSize > 0u))
    {
        *length = plan->lastChunkSize;
    }
    else
    {
        *length = chunkSize;
    }

    return E_OK;
}

/*==================================================================================================
 *  Backfill requests
 *================================================================================================*/

Std_ReturnType Com_ParseBackfillRequest(const uint8 *payload, uint16 payloadLen, Com_BackfillDateType *dates,
                                        uint8 maxDates, uint8 *count, uint8 *dropped)
{
    uint16 i;
    uint16 fieldStart = 0u;
    uint8 accepted = 0u;
    uint8 discarded = 0u;
    uint16 scanLimit;

    DET_CHECK_RETURN((payload != NULL_PTR) && (dates != NULL_PTR) && (count != NULL_PTR), MODULE_ID_COM,
                     INSTANCE_ID_SINGLE, COM_API_ID_PARSE_REQUEST, COM_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(maxDates > 0u, MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_PARSE_REQUEST,
                     E_PARAM_VALUE, E_NOT_OK);

    *count = 0u;
    if (dropped != NULL_PTR)
    {
        *dropped = 0u;
    }

    /* The payload comes from the network. Scanning is bounded so that an oversized message costs a
     * fixed amount of work rather than however much the sender chose to send. */
    scanLimit = (payloadLen > (uint16)COM_MAX_REQUEST_PAYLOAD) ? (uint16)COM_MAX_REQUEST_PAYLOAD : payloadLen;

    for (i = 0u; i <= scanLimit; i++)
    {
        const boolean atEnd = (i == scanLimit) ? TRUE : FALSE;
        const boolean isSeparator =
            ((atEnd != FALSE) || (payload[i] == (uint8)',') || (payload[i] == (uint8)'\0')) ? TRUE : FALSE;

        if (isSeparator == FALSE)
        {
            continue;
        }

        {
            const uint16 fieldLen = (uint16)(i - fieldStart);

            if (fieldLen == (uint16)COM_DATE_LENGTH)
            {
                char candidate[COM_DATE_LENGTH + 1u];

                (void)memcpy(candidate, &payload[fieldStart], (uint16)COM_DATE_LENGTH);
                candidate[COM_DATE_LENGTH] = '\0';

                if (Com_IsValidDate(candidate) != FALSE)
                {
                    if (accepted < maxDates)
                    {
                        (void)memcpy(dates[accepted].date, candidate, sizeof(candidate));
                        accepted++;
                    }
                    else
                    {
                        /* Counted and discarded, never allocated. v1 called `new` for every date
                         * with no limit and no allocation check, so one crafted message could
                         * exhaust the heap. */
                        if (discarded < 0xFFu)
                        {
                            discarded++;
                        }
                    }
                }
            }
        }

        fieldStart = (uint16)(i + 1u);
    }

    *count = accepted;
    if (dropped != NULL_PTR)
    {
        *dropped = discarded;
    }
    Com_Stats.datesDropped += (uint32)discarded;

    if (accepted == 0u)
    {
        Com_Stats.requestsRejected++;
        return E_NOT_FOUND;
    }

    Com_Stats.requestsParsed++;
    return E_OK;
}

/*==================================================================================================
 *  CSV header
 *================================================================================================*/

/** Append the per-pack header names for pack @p index. */
STATIC void Com_WritePackHeader(Com_WriterType *w, uint8 index)
{
    STATIC const char *const packFields[] = {
        "V",   "V_HI", "V_LO",   "I",      "T",     "T_HI",  "T_LO",
        "SOC", "SOH",  "CHG_WH", "DIS_WH", "CHG_S", "DIS_S", "FLAGS",
    };
    STATIC const char *const cellTail[] = {"C_I", "C_T1", "C_T2", "C_T3", "C_T4", "C_F1", "C_F2"};
    uint8 f;
    uint8 cell;

    for (f = 0u; f < (uint8)STD_ARRAY_SIZE(packFields); f++)
    {
        Com_WriteSeparator(w);
        Com_WriteString(w, "P");
        Com_WriteU64(w, (uint64)index);
        Com_WriteChar(w, '_');
        Com_WriteString(w, packFields[f]);
    }

    for (cell = 1u; cell <= (uint8)RS485IF_CELLS_PER_PACK; cell++)
    {
        Com_WriteSeparator(w);
        Com_WriteString(w, "P");
        Com_WriteU64(w, (uint64)index);
        Com_WriteString(w, "_CV");
        Com_WriteU64(w, (uint64)cell);
    }

    for (f = 0u; f < (uint8)STD_ARRAY_SIZE(cellTail); f++)
    {
        Com_WriteSeparator(w);
        Com_WriteString(w, "P");
        Com_WriteU64(w, (uint64)index);
        Com_WriteChar(w, '_');
        Com_WriteString(w, cellTail[f]);
    }
}

Std_ReturnType Com_FormatCsvHeader(char *buffer, uint16 size, uint16 *written)
{
    Com_WriterType writer;
    uint8 pack;

    DET_CHECK_RETURN((buffer != NULL_PTR) && (written != NULL_PTR), MODULE_ID_COM, INSTANCE_ID_SINGLE,
                     COM_API_ID_FORMAT_HEADER, COM_E_PARAM_POINTER, E_NOT_OK);

    *written = 0u;
    Com_WriterInit(&writer, buffer, size);

    Com_WriteString(&writer, "SEQ,UNIX_TIME,UPTIME_MS,DEVICE_ID,AUX_MV,RPM,DC_DV,DC_DA,"
                             "SPEED_MMPS,MCU_FAULT,ODO_MM,TRIP_MM,LAT_E7,LON_E7,ALT_MM,"
                             "GNSS_SPEED_MMPS,GNSS_HDG_DDEG,GNSS_SATS,GNSS_FIX,GNSS_HDOP,"
                             "DTC_COUNT,HEAP_FREE,BEARER");

    for (pack = 1u; pack <= (uint8)COM_PACK_COUNT; pack++)
    {
        Com_WritePackHeader(&writer, pack);
    }

    if (writer.overflow != FALSE)
    {
        /* Nothing partial is returned: a truncated header would make the whole file unparsable by
         * the consumer, which is worse than having no file. */
        if (size > 0u)
        {
            buffer[0] = '\0';
        }
        (void)Det_ReportError(MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_FORMAT_HEADER,
                              COM_E_BUFFER_TOO_SMALL);
        return E_NO_SPACE;
    }

    *written = writer.length;
    return E_OK;
}

/*==================================================================================================
 *  CSV record
 *================================================================================================*/

/** Append one pack's fields, blanking everything the pack did not report this cycle. */
STATIC void Com_WritePackRecord(Com_WriterType *w, const Rs485If_PackStateType *pack)
{
    const boolean packValid = (pack != NULL_PTR) ? pack->packDataValid : FALSE;
    const boolean cellValid = (pack != NULL_PTR) ? pack->cellDataValid : FALSE;
    uint8 cell;

    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.voltage : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.voltageHighest : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.voltageLowest : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteS32OrBlank(w, (packValid != FALSE) ? pack->pack.current : 0, packValid);
    Com_WriteSeparator(w);
    Com_WriteS32OrBlank(w, (packValid != FALSE) ? (sint32)pack->pack.temperature : 0, packValid);
    Com_WriteSeparator(w);
    Com_WriteS32OrBlank(w, (packValid != FALSE) ? (sint32)pack->pack.temperatureHigh : 0, packValid);
    Com_WriteSeparator(w);
    Com_WriteS32OrBlank(w, (packValid != FALSE) ? (sint32)pack->pack.temperatureLow : 0, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.stateOfCharge : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.stateOfHealth : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.chargeEnergyWh : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.dischargeEnergyWh : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.chargeTimeSec : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.dischargeTimeSec : 0uLL, packValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (packValid != FALSE) ? (uint64)pack->pack.statusFlags : 0uLL, packValid);

    for (cell = 0u; cell < (uint8)RS485IF_CELLS_PER_PACK; cell++)
    {
        Com_WriteSeparator(w);
        Com_WriteU64OrBlank(w, (cellValid != FALSE) ? (uint64)pack->cells.cellVoltage[cell] : 0uLL,
                            cellValid);
    }

    Com_WriteSeparator(w);
    Com_WriteS32OrBlank(w, (cellValid != FALSE) ? pack->cells.current : 0, cellValid);
    for (cell = 0u; cell < (uint8)RS485IF_TEMPS_PER_PACK; cell++)
    {
        Com_WriteSeparator(w);
        Com_WriteS32OrBlank(w, (cellValid != FALSE) ? (sint32)pack->cells.temperature[cell] : 0, cellValid);
    }
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (cellValid != FALSE) ? (uint64)pack->cells.statusFlags1 : 0uLL, cellValid);
    Com_WriteSeparator(w);
    Com_WriteU64OrBlank(w, (cellValid != FALSE) ? (uint64)pack->cells.statusFlags2 : 0uLL, cellValid);
}

Std_ReturnType Com_SerialiseCsvRecord(const Com_TelemetryRecordType *record, char *buffer, uint16 size,
                                      uint16 *written)
{
    Com_WriterType writer;
    uint8 pack;
    const boolean havePosition =
        ((record != NULL_PTR) && (record->position != NULL_PTR) && (record->position->valid != FALSE))
            ? TRUE
            : FALSE;

    DET_CHECK_RETURN((record != NULL_PTR) && (buffer != NULL_PTR) && (written != NULL_PTR), MODULE_ID_COM,
                     INSTANCE_ID_SINGLE, COM_API_ID_SERIALISE, COM_E_PARAM_POINTER, E_NOT_OK);

    *written = 0u;
    Com_WriterInit(&writer, buffer, size);

    Com_WriteU64(&writer, (uint64)record->sequenceNumber);
    Com_WriteSeparator(&writer);
    /* A zero wall-clock time means the RTC was not trusted. Emitted blank rather than as 0, because
     * a literal 0 decodes as 1 January 1970 and would be plotted as a real timestamp. */
    Com_WriteU64OrBlank(&writer, (uint64)record->unixTime, (record->unixTime > 0u) ? TRUE : FALSE);
    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, (uint64)record->uptimeMs);
    Com_WriteSeparator(&writer);
    Com_WriteString(&writer, record->deviceId);

    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->auxVoltageMilliVolts, record->auxVoltageValid);

    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->motorRpm, record->driveDataValid);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->dcVoltageDeciVolt, record->driveDataValid);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->dcCurrentDeciAmp, record->driveDataValid);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->speedMmPerSec, record->driveDataValid);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (uint64)record->mcuFaultCode, record->driveDataValid);

    /* The odometer is always emitted. It is never "unavailable": if no distance has accumulated the
     * value is genuinely zero, and blanking it would lose the one field that must never have a gap. */
    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, record->totalDistanceMm);
    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, record->tripDistanceMm);

    Com_WriteSeparator(&writer);
    Com_WriteS32OrBlank(&writer, (havePosition != FALSE) ? record->position->latitudeE7 : 0, havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteS32OrBlank(&writer, (havePosition != FALSE) ? record->position->longitudeE7 : 0, havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteS32OrBlank(&writer, (havePosition != FALSE) ? record->position->altitudeMm : 0, havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (havePosition != FALSE) ? (uint64)record->position->speedMmPerSec : 0uLL,
                        havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (havePosition != FALSE) ? (uint64)record->position->headingDeciDeg : 0uLL,
                        havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (havePosition != FALSE) ? (uint64)record->position->satellitesUsed : 0uLL,
                        havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (havePosition != FALSE) ? (uint64)record->position->fixQuality : 0uLL,
                        havePosition);
    Com_WriteSeparator(&writer);
    Com_WriteU64OrBlank(&writer, (havePosition != FALSE) ? (uint64)record->position->hdopCentiUnits : 0uLL,
                        havePosition);

    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, (uint64)record->confirmedDtcCount);
    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, (uint64)record->heapFreeBytes);
    Com_WriteSeparator(&writer);
    Com_WriteU64(&writer, (uint64)record->bearerState);

    for (pack = 0u; pack < (uint8)COM_PACK_COUNT; pack++)
    {
        Com_WritePackRecord(&writer, (record->packs != NULL_PTR) ? &record->packs[pack] : NULL_PTR);
    }

    *written = writer.length;

    if (writer.overflow != FALSE)
    {
        Com_Stats.truncatedRecords++;
        (void)Det_ReportRuntimeError(MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_SERIALISE,
                                     COM_E_TRUNCATED);
        /* The buffer holds a valid NUL-terminated prefix and @p written reports its length, so the
         * caller can log the truncation with its real size instead of guessing. */
        return E_NO_SPACE;
    }

    Com_Stats.recordsSerialised++;
    if (writer.length > (uint16)Com_Stats.longestRecordBytes)
    {
        Com_Stats.longestRecordBytes = (uint32)writer.length;
    }

    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType Com_Init(void)
{
    (void)memset(&Com_Stats, 0, sizeof(Com_Stats));
    Com_Initialised = TRUE;
    return E_OK;
}

Std_ReturnType Com_GetStatistics(Com_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_COM, INSTANCE_ID_SINGLE, COM_API_ID_SERIALISE,
                     COM_E_PARAM_POINTER, E_NOT_OK);

    *stats = Com_Stats;
    return E_OK;
}

void Com_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = COM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_COM;
        versioninfo->sw_major_version = COM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = COM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = COM_SW_PATCH_VERSION;
    }
}

/**
 * @file    Com.h
 * @brief   Communication service -- telemetry record assembly and transfer framing.
 *
 * Owns the format of everything this ECU sends and receives on the telemetry channel: the CSV record
 * written to the SD card and published live, the chunking arithmetic for a historical file transfer,
 * and the parsing of an inbound backfill request.
 *
 * @par No dynamic strings anywhere
 * v1 assembled every record by repeatedly appending to an Arduino @c String. A record is around
 * 1800 characters across roughly 180 fields, so each one performed on the order of 180 reallocations
 * and copies, on a heap shared with the WiFi stack and the MQTT client, once every three seconds
 * forever. The result is progressive fragmentation: the heap has enough free bytes but no
 * contiguous run large enough, and the allocation that eventually fails is whichever unlucky
 * subsystem asked next -- which is why a fragmentation failure of this kind never points at its
 * cause. Every function here writes into a caller-supplied buffer of known size and reports how much
 * it used.
 *
 * @par Three v1 defects this module's interface prevents
 *
 * 1. **An unsigned underflow that produced a four-billion-iteration loop.** v1's @c sendFile
 *    computed @c numOfPackets from the file size and then looped
 *    @c for (packetIndex = 0; packetIndex < numOfPackets - 1; packetIndex++). For an empty file
 *    @c numOfPackets is 0, and @c 0 - 1 on a @c size_t is SIZE_MAX. An empty or missing log file
 *    would have hung the transfer task until the watchdog -- which, being inert, would never have
 *    fired. ::Com_ComputeChunkPlan returns the count and the final chunk's length together, with no
 *    subtraction at any call site.
 *
 * 2. **An unbounded heap queue fed from the network.** v1's @c addDataRequest did
 *    @c new DataRequest for every comma-separated date in an inbound MQTT payload, with no limit and
 *    no allocation check. A single crafted message could exhaust the heap.
 *    ::Com_ParseBackfillRequest fills a caller-supplied fixed array and reports how many entries it
 *    had to drop.
 *
 * 3. **A 2049-byte buffer on a 7 KiB task stack.** v1 declared
 *    @c char buffer[PUSH_PACKET_SIZE + 1] inside the transfer loop. Chunk buffers are supplied by
 *    the caller here, so their placement is a visible decision rather than an accident of scope.
 *
 * @req SWREQ-TEL-0001 .. SWREQ-TEL-0020
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COM_H
#define COM_H

#include "base/Autosar_ModuleIds.h"
#include "services/Com/Com_Cfg.h"
#include "ecuabs/GnssIf/GnssIf.h"
#include "ecuabs/Rs485If/Rs485If.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COM_VENDOR_ID 0xFFFEu
#define COM_AR_RELEASE_MAJOR_VERSION 4u
#define COM_AR_RELEASE_MINOR_VERSION 4u
#define COM_SW_MAJOR_VERSION 2u
#define COM_SW_MINOR_VERSION 0u
#define COM_SW_PATCH_VERSION 0u

#define COM_API_ID_INIT 0x00u
#define COM_API_ID_SERIALISE 0x20u
#define COM_API_ID_CHUNK_PLAN 0x21u
#define COM_API_ID_PARSE_REQUEST 0x22u
#define COM_API_ID_FORMAT_HEADER 0x23u

#define COM_E_UNINIT E_UNINIT
#define COM_E_PARAM_POINTER E_PARAM_POINTER
#define COM_E_BUFFER_TOO_SMALL 0x20u
#define COM_E_TRUNCATED 0x21u
#define COM_E_MALFORMED_REQUEST 0x22u
#define COM_E_REQUEST_OVERFLOW 0x23u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** One acquisition cycle's worth of data, ready to be serialised. */
typedef struct
{
    /* Identity and time. */
    uint32 unixTime;          /**< Wall-clock time of the record, 0 if unknown.   */
    uint32 uptimeMs;          /**< Monotonic time, always available.               */
    uint32 sequenceNumber;    /**< Monotonically increasing per record.            */
    const char *deviceId;     /**< NUL-terminated device identifier.               */

    /* Supply. */
    uint16 auxVoltageMilliVolts; /**< Auxiliary battery, millivolts.               */
    boolean auxVoltageValid;     /**< FALSE if the ADC read failed.                */

    /* Drive. */
    uint16 motorRpm;            /**< Motor speed.                                 */
    uint16 dcVoltageDeciVolt;   /**< DC link voltage, 0.1 V per count.            */
    uint16 dcCurrentDeciAmp;    /**< DC link current, 0.1 A per count.            */
    uint32 speedMmPerSec;       /**< Vehicle speed.                               */
    uint8 mcuFaultCode;         /**< Motor controller fault code.                 */
    boolean driveDataValid;     /**< FALSE if the CAN signals were stale.         */

    /* Odometry. */
    uint64 totalDistanceMm;     /**< Lifetime distance.                           */
    uint64 tripDistanceMm;      /**< Trip distance.                               */

    /* Battery packs. */
    const Rs485If_PackStateType *packs; /**< Array of ::COM_PACK_COUNT entries.    */

    /* Position. */
    const GnssIf_PositionType *position; /**< May be NULL_PTR if no receiver.      */

    /* Health. */
    uint16 confirmedDtcCount;   /**< Confirmed diagnostic trouble codes.          */
    uint32 heapFreeBytes;       /**< Free heap at the time of the record.         */
    uint8 bearerState;          /**< Which backhaul was active.                   */
} Com_TelemetryRecordType;

/** How a file transfer divides into chunks. */
typedef struct
{
    uint32 chunkCount;     /**< Total chunks, 0 for an empty file.               */
    uint32 fullChunkCount; /**< Chunks of exactly @c chunkSize bytes.            */
    uint32 lastChunkSize;  /**< Bytes in the final chunk; 0 when the file is empty
                                or divides exactly, in which case every chunk is
                                a full one.                                      */
} Com_ChunkPlanType;

/** One date in a backfill request, as "YYYYMMDD". */
typedef struct
{
    char date[9]; /**< NUL-terminated. */
} Com_BackfillDateType;

/** Serialisation counters, published as diagnostic data. */
typedef struct
{
    uint32 recordsSerialised; /**< Records successfully written.                   */
    uint32 truncatedRecords;  /**< Records that did not fit their buffer.          */
    uint32 requestsParsed;    /**< Backfill requests accepted.                    */
    uint32 requestsRejected;  /**< Backfill requests with no usable dates.         */
    uint32 datesDropped;      /**< Dates discarded because the array was full.     */
    uint32 longestRecordBytes;/**< High-water mark of a serialised record.         */
} Com_StatisticsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/** Reset the module's counters. */
CHECK_RETURN Std_ReturnType Com_Init(void);

/**
 * @brief Write the CSV header line that every log file starts with.
 *
 * @param[out] buffer   Destination.
 * @param[in]  size     Capacity of @p buffer, including the terminator.
 * @param[out] written  Characters written, excluding the terminator.
 * @return E_OK on success; E_NO_SPACE if @p buffer is too small, in which case nothing is written --
 *         a partial header would make the whole file unparsable by the consumer.
 */
CHECK_RETURN Std_ReturnType Com_FormatCsvHeader(char *buffer, uint16 size, uint16 *written);

/**
 * @brief Serialise @p record as one CSV line.
 *
 * Fields are emitted in the order the header declares. A value that is not valid this cycle is
 * emitted as an empty field rather than as zero: an empty field means "not measured", whereas a zero
 * means "measured, and it was zero". v1 emitted zeros and commas interchangeably, so a pack that had
 * stopped answering was indistinguishable in the log from one reading 0 V.
 *
 * @param[in]  record   Data to serialise.
 * @param[out] buffer   Destination.
 * @param[in]  size     Capacity of @p buffer, including the terminator.
 * @param[out] written  Characters written, excluding the terminator.
 * @return E_OK on success; E_NO_SPACE if the record does not fit, in which case @p buffer holds a
 *         valid NUL-terminated prefix and @p written reports its length, so the caller can log the
 *         truncation with its actual size rather than guessing.
 */
CHECK_RETURN Std_ReturnType Com_SerialiseCsvRecord(const Com_TelemetryRecordType *record,
                                                   char *buffer, uint16 size, uint16 *written);

/**
 * @brief Work out how a file of @p fileSize bytes divides into @p chunkSize chunks.
 *
 * The replacement for v1's @c numOfPackets - 1. Every quantity a transfer loop needs is returned
 * explicitly, so no call site performs a subtraction that can underflow.
 *
 * @param[in]  fileSize  Total bytes to transfer. 0 is valid and yields a zero-chunk plan.
 * @param[in]  chunkSize Bytes per chunk. Must be non-zero.
 * @param[out] plan      Destination.
 * @return E_OK on success; E_NOT_OK if @p chunkSize is 0 or @p plan is NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Com_ComputeChunkPlan(uint32 fileSize, uint32 chunkSize,
                                                 Com_ChunkPlanType *plan);

/**
 * @brief Byte offset and length of chunk @p index under @p plan.
 *
 * @param[in]  plan      A plan from ::Com_ComputeChunkPlan.
 * @param[in]  index     Zero-based chunk index.
 * @param[in]  chunkSize The same value passed to ::Com_ComputeChunkPlan.
 * @param[out] offset    Byte offset of the chunk.
 * @param[out] length    Byte length of the chunk.
 * @return E_OK on success; E_NOT_FOUND if @p index is past the end of the plan.
 */
CHECK_RETURN Std_ReturnType Com_GetChunkExtent(const Com_ChunkPlanType *plan, uint32 index,
                                               uint32 chunkSize, uint32 *offset, uint32 *length);

/**
 * @brief Parse a comma-separated list of "YYYYMMDD" dates from an inbound request.
 *
 * Fills a caller-supplied fixed array. Anything beyond its capacity is counted and discarded rather
 * than allocated, which is what makes the request path safe against a hostile or simply oversized
 * payload -- v1 called @c new for every date with no limit and no allocation check.
 *
 * @param[in]  payload    Request bytes. Need not be NUL-terminated.
 * @param[in]  payloadLen Length of @p payload.
 * @param[out] dates      Destination array.
 * @param[in]  maxDates   Capacity of @p dates.
 * @param[out] count      Dates written.
 * @param[out] dropped    Valid dates discarded for lack of room. May be NULL_PTR.
 * @return E_OK if at least one date was accepted; E_NOT_FOUND if the payload held none;
 *         E_NOT_OK on a parameter error.
 */
CHECK_RETURN Std_ReturnType Com_ParseBackfillRequest(const uint8 *payload, uint16 payloadLen,
                                                     Com_BackfillDateType *dates, uint8 maxDates,
                                                     uint8 *count, uint8 *dropped);

/**
 * @brief Whether @p date is a well-formed and real calendar date in "YYYYMMDD" form.
 *
 * Validates the day against the month and the year's leap status. v1 accepted any day from 1 to 31
 * in any month, so a request for 20240230 produced a file name that could never exist and a transfer
 * that reported "Not Found" -- indistinguishable from a genuinely missing log.
 */
boolean Com_IsValidDate(const char *date);

/**
 * @brief Format the log file name for @p date, as "/YYYYMMDD.csv".
 *
 * @param[out] buffer Destination, at least ::COM_FILENAME_SIZE bytes.
 * @param[in]  size   Capacity of @p buffer.
 * @param[in]  date   Eight-character date, NUL-terminated.
 * @return E_OK on success; E_NOT_OK if the buffer is too small or the date is invalid.
 */
CHECK_RETURN Std_ReturnType Com_FormatLogFileName(char *buffer, uint16 size, const char *date);

/**
 * @brief Read the serialisation counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Com_GetStatistics(Com_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Com_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* COM_H */

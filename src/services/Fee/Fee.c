/**
 * @file    Fee.c
 * @brief   Flash EEPROM emulation implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Fee/Fee.h"

#include <string.h>

#include "services/Crc/Crc.h"
#include "services/Det/Det.h"
#include "mcal/Fls/Fls.h"

/*==================================================================================================
 *  On-media layout
 *
 *  Both headers are serialised field by field rather than by casting a struct over the buffer.
 *  A struct overlay would make the layout depend on the compiler's padding and endianness, and
 *  this layout has to stay byte-identical across toolchains for a field unit's data to remain
 *  readable after a firmware update built elsewhere.
 *
 *  Sector header, 16 bytes:
 *      0  uint32  magic            FEE_SECTOR_MAGIC
 *      4  uint16  formatVersion    FEE_FORMAT_VERSION
 *      6  uint16  sequence         higher value wins
 *      8  uint16  headerCrc        CRC-16 over bytes 0..7
 *     10  uint8   reserved[6]      erased
 *
 *  Record header, 16 bytes:
 *      0  uint16  blockId
 *      2  uint16  payloadLength
 *      4  uint16  writeCounter     per-block, wraps; compared modulo 2^16
 *      6  uint16  headerCrc        CRC-16 over bytes 0..5
 *      8  uint32  payloadCrc       CRC-32 over the payload
 *     12  uint8   state            0xFF erased, 0xFC valid, 0xF0 invalidated
 *     13  uint8   reserved[3]      erased
 *================================================================================================*/

#define FEE_SH_OFF_MAGIC 0u
#define FEE_SH_OFF_VERSION 4u
#define FEE_SH_OFF_SEQUENCE 6u
#define FEE_SH_OFF_CRC 8u
#define FEE_SH_CRC_COVERAGE 8u

#define FEE_RH_OFF_BLOCK_ID 0u
#define FEE_RH_OFF_LENGTH 2u
#define FEE_RH_OFF_COUNTER 4u
#define FEE_RH_OFF_HEADER_CRC 6u
#define FEE_RH_OFF_PAYLOAD_CRC 8u
#define FEE_RH_OFF_STATE 12u
#define FEE_RH_CRC_COVERAGE 6u

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((FEE_SECTOR_HEADER_SIZE % FEE_ALIGNMENT) == 0u,
               "sector header must be write-aligned");
_Static_assert((FEE_RECORD_HEADER_SIZE % FEE_ALIGNMENT) == 0u,
               "record header must be write-aligned");
_Static_assert(FEE_RH_OFF_STATE < FEE_RECORD_HEADER_SIZE, "state field must lie in the header");
/* The commit byte must sit in its own aligned word, or writing it would rewrite neighbouring
 * header bytes -- which on NOR flash can only clear bits and would corrupt the CRC. */
_Static_assert((FEE_RH_OFF_STATE % FEE_ALIGNMENT) == 0u,
               "the state byte must start an aligned word so committing touches nothing else");
_Static_assert(FEE_SECTOR_COUNT == 2u, "the garbage collector assumes exactly two sectors");
#endif

/*==================================================================================================
 *  Block configuration table
 *================================================================================================*/

typedef struct
{
    Fee_BlockIdType id;
    uint16 length;
} Fee_BlockConfigType;

STATIC const Fee_BlockConfigType Fee_BlockConfig[FEE_BLOCK_COUNT] = {
    {FEE_BLOCK_ODOMETER, FEE_LENGTH_ODOMETER},
    {FEE_BLOCK_DEVICE_CONFIG, FEE_LENGTH_DEVICE_CONFIG},
    {FEE_BLOCK_CALIBRATION, FEE_LENGTH_CALIBRATION},
    {FEE_BLOCK_RESTART_INFO, FEE_LENGTH_RESTART_INFO},
    {FEE_BLOCK_DTC_STORE, FEE_LENGTH_DTC_STORE},
    {FEE_BLOCK_TELEMETRY_CURSOR, FEE_LENGTH_TELEMETRY_CURSOR},
    {FEE_BLOCK_ENERGY_COUNTERS, FEE_LENGTH_ENERGY_COUNTERS},
};

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean Fee_Initialised = FALSE;
STATIC uint8 Fee_ActiveSector;     /* 0 or 1                                    */
STATIC uint16 Fee_ActiveSequence;  /* sequence number of the active sector       */
STATIC uint32 Fee_WriteCursor;     /* offset within the active sector, aligned   */
STATIC Fee_StatusType Fee_Status;

/**
 * @brief Set when a failed append may have left bits programmed at the write cursor.
 *
 * A failing Fls_Write does not mean nothing reached the media. A supply collapse part way through a
 * programming pulse leaves some cells cleared, and flash cells cannot be un-cleared without an erase --
 * so the slot at the cursor can never be trusted or reused again.
 *
 * Two consequences follow, and both are why this flag exists rather than simply retrying:
 *
 *  1. The cursor is deliberately not advanced on failure, so a retry would program the *same* slot.
 *     Writing a second header over a partially programmed one ANDs the two together, producing a
 *     header whose CRC cannot match, so the retry can never succeed however many times it is made.
 *  2. Every scan -- Fee_FindNewestRecord, Fee_HighestCounter -- stops at an unparseable header,
 *     because the length needed to step over it is exactly what was lost. So the damaged slot hides
 *     every record written after it, and a retry that did somehow land beyond it would be invisible.
 *
 * The recovery is a garbage collection, which is safe at this moment for a specific reason: the
 * damaged slot sits at the cursor, past every valid record, so a scan from the start of the sector
 * reaches all of them before it stops. Collecting copies those into the other sector and leaves the
 * damage behind. ::Fee_WriteBlock acts on the flag before its next append rather than collecting from
 * inside ::Fee_AppendRecord, because collection itself appends and the recursion would be a trap.
 */
STATIC boolean Fee_SlotPoisoned = FALSE;

/**
 * @brief Staging buffer for a record's payload during garbage collection.
 *
 * One buffer at the longest block length. GC copies one record at a time, and the module is
 * driven from a single task, so there is never more than one copy in flight.
 */
STATIC uint8 Fee_Staging[FEE_MAX_BLOCK_LENGTH];

/*==================================================================================================
 *  Serialisation helpers
 *================================================================================================*/

STATIC void Fee_PutU16(uint8 *p, uint16 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)(v >> 8u);
}

STATIC uint16 Fee_GetU16(const uint8 *p)
{
    return (uint16)((uint16)p[0] | ((uint16)p[1] << 8u));
}

STATIC void Fee_PutU32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFuL);
    p[1] = (uint8)((v >> 8u) & 0xFFuL);
    p[2] = (uint8)((v >> 16u) & 0xFFuL);
    p[3] = (uint8)((v >> 24u) & 0xFFuL);
}

STATIC uint32 Fee_GetU32(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8u) | ((uint32)p[2] << 16u) | ((uint32)p[3] << 24u);
}

/** Round @p value up to the next ::FEE_ALIGNMENT boundary. */
STATIC uint32 Fee_AlignUp(uint32 value)
{
    const uint32 remainder = value % FEE_ALIGNMENT;
    return (remainder == 0u) ? value : (value + (FEE_ALIGNMENT - remainder));
}

/** Base address of sector @p index within the Fls partition. */
STATIC uint32 Fee_SectorBase(uint8 index)
{
    return (uint32)index * (uint32)FEE_SECTOR_SIZE;
}

/**
 * @brief Look up @p blockId's configured length.
 * @return TRUE if the block is configured, with @p length set.
 */
STATIC boolean Fee_LookupBlock(Fee_BlockIdType blockId, uint16 *length)
{
    uint8 i;

    for (i = 0u; i < (uint8)FEE_BLOCK_COUNT; i++)
    {
        if (Fee_BlockConfig[i].id == blockId)
        {
            *length = Fee_BlockConfig[i].length;
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Whether @p a is newer than @p b, comparing write counters modulo 2^16.
 *
 * Modular comparison rather than plain @c > so the comparison stays correct when the counter
 * wraps. A block written once per minute for a decade reaches about five million writes, so
 * the 16-bit counter wraps roughly every 45 days -- frequently enough that a naive comparison
 * would eventually pick the older record and silently roll the odometer backwards.
 */
STATIC boolean Fee_CounterIsNewer(uint16 a, uint16 b)
{
    return (((uint16)(a - b) != 0u) && ((uint16)(a - b) < 0x8000u)) ? TRUE : FALSE;
}

/*==================================================================================================
 *  Sector headers
 *================================================================================================*/

/**
 * @brief Read and validate sector @p index's header.
 * @param[in]  index    Sector index, 0 or 1.
 * @param[out] sequence Sequence number, valid only when E_OK is returned.
 * @return E_OK if the header is present, the magic and version match and the CRC verifies.
 */
STATIC Std_ReturnType Fee_ReadSectorHeader(uint8 index, uint16 *sequence)
{
    uint8 header[FEE_SECTOR_HEADER_SIZE];
    uint16 storedCrc;
    uint16 computedCrc;

    if (Fls_Read(Fee_SectorBase(index), header, (Fls_LengthType)sizeof(header)) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        return E_NOT_OK;
    }

    if (Fee_GetU32(&header[FEE_SH_OFF_MAGIC]) != FEE_SECTOR_MAGIC)
    {
        return E_NOT_OK;
    }

    /* An unrecognised layout version is treated as unformatted. Parsing it with today's
     * offsets would decode old records into plausible-looking wrong values, which is far
     * worse than discarding them. */
    if (Fee_GetU16(&header[FEE_SH_OFF_VERSION]) != (uint16)FEE_FORMAT_VERSION)
    {
        return E_NOT_OK;
    }

    storedCrc = Fee_GetU16(&header[FEE_SH_OFF_CRC]);
    computedCrc = Crc_CalculateCRC16(header, FEE_SH_CRC_COVERAGE, 0u, TRUE);
    if (storedCrc != computedCrc)
    {
        return E_NOT_OK;
    }

    *sequence = Fee_GetU16(&header[FEE_SH_OFF_SEQUENCE]);
    return E_OK;
}

/** Write sector @p index's header with sequence @p sequence. The GC commit point. */
STATIC Std_ReturnType Fee_WriteSectorHeader(uint8 index, uint16 sequence)
{
    uint8 header[FEE_SECTOR_HEADER_SIZE];
    uint8 i;

    for (i = 0u; i < (uint8)sizeof(header); i++)
    {
        header[i] = (uint8)FLS_ERASED_VALUE;
    }

    Fee_PutU32(&header[FEE_SH_OFF_MAGIC], FEE_SECTOR_MAGIC);
    Fee_PutU16(&header[FEE_SH_OFF_VERSION], (uint16)FEE_FORMAT_VERSION);
    Fee_PutU16(&header[FEE_SH_OFF_SEQUENCE], sequence);
    Fee_PutU16(&header[FEE_SH_OFF_CRC],
               Crc_CalculateCRC16(header, FEE_SH_CRC_COVERAGE, 0u, TRUE));

    if (Fls_Write(Fee_SectorBase(index), header, (Fls_LengthType)sizeof(header)) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        return E_NOT_OK;
    }
    return E_OK;
}

/*==================================================================================================
 *  Record scanning
 *================================================================================================*/

/** One parsed record header plus where it sits. */
typedef struct
{
    uint32 offset;        /**< Offset of the record header within the sector.  */
    Fee_BlockIdType id;   /**< Block identifier.                              */
    uint16 length;        /**< Payload length.                                */
    uint16 counter;       /**< Write counter.                                 */
    uint32 payloadCrc;    /**< Stored payload CRC.                            */
    uint8 state;          /**< Record state byte.                             */
} Fee_RecordType;

/**
 * @brief Parse the record header at @p offset in the active sector.
 *
 * @return E_OK if a well-formed header was parsed; E_NOT_FOUND if the slot is erased (which
 *         means the end of the written area); E_NOT_OK if the header CRC fails, which makes
 *         everything beyond it unparseable because the length needed to skip it is unknown.
 */
STATIC Std_ReturnType Fee_ParseRecordHeader(uint32 offset, Fee_RecordType *record)
{
    uint8 header[FEE_RECORD_HEADER_SIZE];
    uint16 storedCrc;
    uint8 i;
    boolean allErased = TRUE;

    if (Fls_Read(Fee_SectorBase(Fee_ActiveSector) + offset, header,
                 (Fls_LengthType)sizeof(header)) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        return E_NOT_OK;
    }

    for (i = 0u; i < FEE_RH_CRC_COVERAGE; i++)
    {
        if (header[i] != (uint8)FLS_ERASED_VALUE)
        {
            allErased = FALSE;
            break;
        }
    }
    if (allErased != FALSE)
    {
        return E_NOT_FOUND;
    }

    storedCrc = Fee_GetU16(&header[FEE_RH_OFF_HEADER_CRC]);
    if (storedCrc != Crc_CalculateCRC16(header, FEE_RH_CRC_COVERAGE, 0u, TRUE))
    {
        return E_NOT_OK;
    }

    record->offset = offset;
    record->id = (Fee_BlockIdType)Fee_GetU16(&header[FEE_RH_OFF_BLOCK_ID]);
    record->length = Fee_GetU16(&header[FEE_RH_OFF_LENGTH]);
    record->counter = Fee_GetU16(&header[FEE_RH_OFF_COUNTER]);
    record->payloadCrc = Fee_GetU32(&header[FEE_RH_OFF_PAYLOAD_CRC]);
    record->state = header[FEE_RH_OFF_STATE];

    /* A header that passed its CRC but declares a length the configuration does not allow
     * indicates media damage that happened to leave a valid CRC, or a layout mismatch the
     * version check missed. Either way the record cannot be trusted. */
    if ((record->length == 0u) || (record->length > (uint16)FEE_MAX_BLOCK_LENGTH))
    {
        return E_NOT_OK;
    }

    return E_OK;
}

/** Total bytes a record with @p payloadLength occupies, header included and aligned. */
STATIC uint32 Fee_RecordSpan(uint16 payloadLength)
{
    return Fee_AlignUp((uint32)FEE_RECORD_HEADER_SIZE + (uint32)payloadLength);
}

/**
 * @brief Walk the active sector and report the first free offset.
 *
 * Also counts the incomplete records found, which is the direct measure of how many writes
 * this unit has had interrupted by a supply loss -- a number worth having in the field.
 */
STATIC uint32 Fee_FindWriteCursor(void)
{
    uint32 offset = FEE_SECTOR_HEADER_SIZE;

    while ((offset + FEE_RECORD_HEADER_SIZE) <= (uint32)FEE_SECTOR_SIZE)
    {
        Fee_RecordType record;
        const Std_ReturnType status = Fee_ParseRecordHeader(offset, &record);

        if (status == E_NOT_FOUND)
        {
            break; /* erased slot: this is the end of the written area */
        }
        if (status != E_OK)
        {
            /* Unparseable header. The length needed to step over it is unknown, so nothing
             * beyond this point can be read. Treating the remainder as full forces a garbage
             * collection, which rewrites the surviving records into a clean sector and
             * reclaims the damaged space. */
            Fee_Status.crcFailureCount++;
            offset = (uint32)FEE_SECTOR_SIZE;
            break;
        }

        if (record.state == (uint8)FEE_RECORD_STATE_ERASED)
        {
            Fee_Status.incompleteRecordCount++;
        }

        offset += Fee_RecordSpan(record.length);
    }

    return offset;
}

/**
 * @brief Find the newest committed, CRC-verified record for @p blockId.
 *
 * @param[in]  blockId Block to search for.
 * @param[out] found   Populated on success.
 * @return E_OK if such a record exists; E_NOT_FOUND if the block has never been written;
 *         E_CRC_FAIL if records exist but none verifies.
 */
STATIC Std_ReturnType Fee_FindNewestRecord(Fee_BlockIdType blockId, Fee_RecordType *found)
{
    uint32 offset = FEE_SECTOR_HEADER_SIZE;
    boolean haveCandidate = FALSE;
    boolean sawBlock = FALSE;
    boolean sawInvalidation = FALSE;
    uint16 invalidationCounter = 0u;

    while ((offset + FEE_RECORD_HEADER_SIZE) <= (uint32)FEE_SECTOR_SIZE)
    {
        Fee_RecordType record;

        if (Fee_ParseRecordHeader(offset, &record) != E_OK)
        {
            break;
        }

        if (record.id == blockId)
        {
            /* Only a *committed* record counts as evidence that the block exists. A record
             * still in the erased state is a write that was interrupted before its commit
             * byte, which means it never happened -- and the distinction matters at the call
             * site: "damaged" (E_CRC_FAIL) warrants a diagnostic trouble code, whereas "never
             * written" (E_NOT_FOUND) just means NvM should apply its default. Counting an
             * uncommitted record here would report every interrupted first write as data
             * corruption. */
            if (record.state == (uint8)FEE_RECORD_STATE_INVALID)
            {
                sawBlock = TRUE;

                /* Invalidations compete on the same counter axis as writes, so a write that
                 * follows an invalidation revives the block and an invalidation that follows
                 * a write retires it. */
                if ((sawInvalidation == FALSE) ||
                    (Fee_CounterIsNewer(record.counter, invalidationCounter) != FALSE))
                {
                    sawInvalidation = TRUE;
                    invalidationCounter = record.counter;
                }
            }
            else if (record.state == (uint8)FEE_RECORD_STATE_VALID)
            {
                sawBlock = TRUE;

                if ((haveCandidate == FALSE) ||
                    (Fee_CounterIsNewer(record.counter, found->counter) != FALSE))
                {
                    /* Verify the payload before accepting the record as the candidate. A
                     * newer record that fails its CRC must lose to an older one that passes
                     * -- that fallback is what makes a single corrupted write survivable. */
                    uint8 payload[FEE_MAX_BLOCK_LENGTH];

                    if (Fls_Read(Fee_SectorBase(Fee_ActiveSector) + record.offset +
                                     FEE_RECORD_HEADER_SIZE,
                                 payload, (Fls_LengthType)record.length) == E_OK)
                    {
                        const uint32 crc =
                            Crc_CalculateCRC32(payload, (uint32)record.length, 0u, TRUE);

                        if (crc == record.payloadCrc)
                        {
                            *found = record;
                            haveCandidate = TRUE;
                        }
                        else
                        {
                            Fee_Status.crcFailureCount++;
                        }
                    }
                    else
                    {
                        Fee_Status.mediaErrorCount++;
                    }
                }
            }
            else
            {
                /* State is erased: the write was interrupted before its commit byte. */
            }
        }

        offset += Fee_RecordSpan(record.length);
    }

    if ((sawInvalidation != FALSE) &&
        ((haveCandidate == FALSE) ||
         (Fee_CounterIsNewer(invalidationCounter, found->counter) != FALSE)))
    {
        return E_NOT_FOUND;
    }
    if (haveCandidate != FALSE)
    {
        return E_OK;
    }
    return (sawBlock != FALSE) ? E_CRC_FAIL : E_NOT_FOUND;
}

/** Highest write counter seen for @p blockId, or 0 if none. */
STATIC uint16 Fee_HighestCounter(Fee_BlockIdType blockId)
{
    uint32 offset = FEE_SECTOR_HEADER_SIZE;
    uint16 highest = 0u;
    boolean any = FALSE;

    while ((offset + FEE_RECORD_HEADER_SIZE) <= (uint32)FEE_SECTOR_SIZE)
    {
        Fee_RecordType record;

        if (Fee_ParseRecordHeader(offset, &record) != E_OK)
        {
            break;
        }
        if (record.id == blockId)
        {
            if ((any == FALSE) || (Fee_CounterIsNewer(record.counter, highest) != FALSE))
            {
                highest = record.counter;
                any = TRUE;
            }
        }
        offset += Fee_RecordSpan(record.length);
    }

    return highest;
}

/*==================================================================================================
 *  Record writing
 *================================================================================================*/

/**
 * @brief Append a record at the write cursor and commit it.
 *
 * The three-step sequence documented in Fee.h. Every step's failure leaves the previous value
 * intact, because nothing about the new record is visible to a reader until the commit byte
 * lands.
 */
STATIC Std_ReturnType Fee_AppendRecord(Fee_BlockIdType blockId, const uint8 *payload,
                                       uint16 length, uint16 counter, uint8 finalState)
{
    uint8 header[FEE_RECORD_HEADER_SIZE];
    uint8 commit[FEE_ALIGNMENT];
    const uint32 base = Fee_SectorBase(Fee_ActiveSector) + Fee_WriteCursor;
    const uint32 span = Fee_RecordSpan(length);
    uint8 i;

    if ((Fee_WriteCursor + span) > (uint32)FEE_SECTOR_SIZE)
    {
        return E_NO_SPACE;
    }

    /* Step 1: header, with the state byte left at the erased value. Writing 0xFF over erased
     * flash clears no bits, so the slot stays uncommitted. */
    for (i = 0u; i < (uint8)sizeof(header); i++)
    {
        header[i] = (uint8)FLS_ERASED_VALUE;
    }
    Fee_PutU16(&header[FEE_RH_OFF_BLOCK_ID], (uint16)blockId);
    Fee_PutU16(&header[FEE_RH_OFF_LENGTH], length);
    Fee_PutU16(&header[FEE_RH_OFF_COUNTER], counter);
    Fee_PutU16(&header[FEE_RH_OFF_HEADER_CRC],
               Crc_CalculateCRC16(header, FEE_RH_CRC_COVERAGE, 0u, TRUE));
    Fee_PutU32(&header[FEE_RH_OFF_PAYLOAD_CRC],
               (payload != NULL_PTR) ? Crc_CalculateCRC32(payload, (uint32)length, 0u, TRUE)
                                     : 0uL);

    if (Fls_Write(base, header, (Fls_LengthType)sizeof(header)) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        Fee_SlotPoisoned = TRUE;
        return E_NOT_OK;
    }

    /* Step 2: payload. An invalidation record carries none. */
    if (payload != NULL_PTR)
    {
        uint8 padded[FEE_MAX_BLOCK_LENGTH + FEE_ALIGNMENT];
        const uint32 paddedLength = Fee_AlignUp((uint32)length);

        /* Checked here rather than relied upon. The configured block lengths do fit -- Fee_Cfg.h asserts
         * it -- but that invariant lives two call levels away, and a bound this function cannot see is one
         * a later change can break without anything pointing back here. */
        if (paddedLength > (uint32)sizeof(padded))
        {
            (void)Det_ReportError(MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_WRITE,
                                  FEE_E_INVALID_BLOCK_NO);
            return E_NOT_OK;
        }

        /* The whole buffer is set to the erased value first, then the payload copied over it. The padding
         * beyond the payload is never read -- only paddedLength bytes are written -- but initialising it
         * costs a memset of at most 264 bytes and removes any question of what reaches the media. */
        (void)memset(padded, (int)FLS_ERASED_VALUE, sizeof(padded));
        (void)memcpy(padded, payload, (size_t)length);

        if (Fls_Write(base + FEE_RECORD_HEADER_SIZE, padded, (Fls_LengthType)paddedLength) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            Fee_SlotPoisoned = TRUE;
            return E_NOT_OK;
        }
    }

    /* Step 3: the commit. One aligned word whose first byte is the state; the rest stay
     * erased so no other header field is disturbed. */
    for (i = 0u; i < (uint8)FEE_ALIGNMENT; i++)
    {
        commit[i] = (uint8)FLS_ERASED_VALUE;
    }
    commit[0] = finalState;

    if (Fls_Write(base + FEE_RH_OFF_STATE, commit, (Fls_LengthType)FEE_ALIGNMENT) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        Fee_SlotPoisoned = TRUE;
        return E_NOT_OK;
    }

    Fee_WriteCursor += span;
    Fee_Status.writeCount++;
    return E_OK;
}

/*==================================================================================================
 *  Garbage collection
 *================================================================================================*/

Std_ReturnType Fee_GarbageCollect(void)
{
    const uint8 source = Fee_ActiveSector;
    const uint8 target = (uint8)((Fee_ActiveSector + 1u) % FEE_SECTOR_COUNT);
    const uint16 newSequence = (uint16)(Fee_ActiveSequence + 1u);
    uint32 targetCursor = FEE_SECTOR_HEADER_SIZE;
    uint8 blockIndex;

    DET_CHECK_RETURN(Fee_Initialised != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_GC,
                     FEE_E_UNINIT, E_NOT_OK);

    /* Step 1: a clean target. */
    if (Fls_Erase(Fee_SectorBase(target), (Fls_LengthType)FEE_SECTOR_SIZE) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        return E_NOT_OK;
    }

    /* Step 2: copy the newest valid record of every configured block. Records are written
     * directly rather than through Fee_AppendRecord, because the cursor being advanced belongs
     * to the target sector, which is not yet active. */
    for (blockIndex = 0u; blockIndex < (uint8)FEE_BLOCK_COUNT; blockIndex++)
    {
        const Fee_BlockIdType blockId = Fee_BlockConfig[blockIndex].id;
        const uint16 length = Fee_BlockConfig[blockIndex].length;
        Fee_RecordType record;
        uint8 header[FEE_RECORD_HEADER_SIZE];
        uint8 commit[FEE_ALIGNMENT];
        uint32 span;
        uint32 i;

        if (Fee_FindNewestRecord(blockId, &record) != E_OK)
        {
            continue; /* never written, or every copy corrupt: nothing worth carrying over */
        }

        if (Fls_Read(Fee_SectorBase(source) + record.offset + FEE_RECORD_HEADER_SIZE, Fee_Staging,
                     (Fls_LengthType)record.length) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            continue;
        }

        span = Fee_RecordSpan(record.length);
        if ((targetCursor + span) > (uint32)FEE_SECTOR_SIZE)
        {
            /* Every block's newest record must fit in one empty sector, or the configuration
             * is over-committed. Checked at build time by the assertion below, so reaching
             * here means media damage rather than a sizing error. */
            (void)Det_ReportError(MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_GC,
                                  FEE_E_NO_SPACE);
            break;
        }

        for (i = 0u; i < (uint32)sizeof(header); i++)
        {
            header[i] = (uint8)FLS_ERASED_VALUE;
        }
        Fee_PutU16(&header[FEE_RH_OFF_BLOCK_ID], (uint16)blockId);
        Fee_PutU16(&header[FEE_RH_OFF_LENGTH], record.length);
        Fee_PutU16(&header[FEE_RH_OFF_COUNTER], record.counter);
        Fee_PutU16(&header[FEE_RH_OFF_HEADER_CRC],
                   Crc_CalculateCRC16(header, FEE_RH_CRC_COVERAGE, 0u, TRUE));
        Fee_PutU32(&header[FEE_RH_OFF_PAYLOAD_CRC], record.payloadCrc);

        if (Fls_Write(Fee_SectorBase(target) + targetCursor, header,
                      (Fls_LengthType)sizeof(header)) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            return E_NOT_OK;
        }

        {
            uint8 padded[FEE_MAX_BLOCK_LENGTH + FEE_ALIGNMENT];
            const uint32 paddedLength = Fee_AlignUp((uint32)record.length);

            /* Fee_ParseRecordHeader already rejects a length above FEE_MAX_BLOCK_LENGTH, so this cannot
             * fire -- but a garbage collection that overran a stack buffer would corrupt the one operation
             * that temporarily holds the only copy of every block, so it is checked anyway. */
            if (paddedLength > (uint32)sizeof(padded))
            {
                (void)Det_ReportError(MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_GC,
                                      FEE_E_INVALID_BLOCK_NO);
                return E_NOT_OK;
            }

            (void)memset(padded, (int)FLS_ERASED_VALUE, sizeof(padded));
            (void)memcpy(padded, Fee_Staging, (size_t)record.length);
            if (Fls_Write(Fee_SectorBase(target) + targetCursor + FEE_RECORD_HEADER_SIZE, padded,
                          (Fls_LengthType)paddedLength) != E_OK)
            {
                Fee_Status.mediaErrorCount++;
                return E_NOT_OK;
            }
        }

        for (i = 0u; i < (uint32)FEE_ALIGNMENT; i++)
        {
            commit[i] = (uint8)FLS_ERASED_VALUE;
        }
        commit[0] = (uint8)FEE_RECORD_STATE_VALID;
        if (Fls_Write(Fee_SectorBase(target) + targetCursor + FEE_RH_OFF_STATE, commit,
                      (Fls_LengthType)FEE_ALIGNMENT) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            return E_NOT_OK;
        }

        targetCursor += span;
        COMPILER_UNUSED(length);
    }

    /* Step 3: the commit. Until this header exists the target is not a Fee sector at all and
     * the source remains authoritative, so a supply loss anywhere above loses nothing. */
    if (Fee_WriteSectorHeader(target, newSequence) != E_OK)
    {
        return E_NOT_OK;
    }

    Fee_ActiveSector = target;
    Fee_ActiveSequence = newSequence;
    Fee_WriteCursor = targetCursor;
    Fee_Status.gcCount++;
    Fee_Status.activeSector = target;
    Fee_Status.activeSequence = newSequence;

    /* Step 4: reclaim the old sector. A failure here is not fatal -- the next Init sees two
     * valid headers and erases the loser. */
    if (Fls_Erase(Fee_SectorBase(source), (Fls_LengthType)FEE_SECTOR_SIZE) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
    }

    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType Fee_Format(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)FEE_SECTOR_COUNT; i++)
    {
        if (Fls_Erase(Fee_SectorBase(i), (Fls_LengthType)FEE_SECTOR_SIZE) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            return E_NOT_OK;
        }
    }

    if (Fee_WriteSectorHeader(0u, 1u) != E_OK)
    {
        return E_NOT_OK;
    }

    Fee_ActiveSector = 0u;
    Fee_ActiveSequence = 1u;
    Fee_WriteCursor = FEE_SECTOR_HEADER_SIZE;
    Fee_Status.activeSector = 0u;
    Fee_Status.activeSequence = 1u;
    /* A fresh scan has just established the cursor from the media, so any poisoning from a previous
     * session has already been accounted for by Fee_FindWriteCursor. */
    Fee_SlotPoisoned = FALSE;
    Fee_Initialised = TRUE;

    return E_OK;
}

Std_ReturnType Fee_Init(void)
{
    uint16 sequence[FEE_SECTOR_COUNT];
    boolean valid[FEE_SECTOR_COUNT];
    uint8 i;

    Fee_Status.writeCount = 0u;
    Fee_Status.gcCount = 0u;
    Fee_Status.crcFailureCount = 0u;
    Fee_Status.incompleteRecordCount = 0u;
    Fee_Status.mediaErrorCount = 0u;
    Fee_Status.layoutRecovered = FALSE;
    Fee_Initialised = FALSE;

    for (i = 0u; i < (uint8)FEE_SECTOR_COUNT; i++)
    {
        sequence[i] = 0u;
        valid[i] = (Fee_ReadSectorHeader(i, &sequence[i]) == E_OK) ? TRUE : FALSE;
    }

    if ((valid[0] == FALSE) && (valid[1] == FALSE))
    {
        /* Virgin part, or a layout from an incompatible firmware version. Formatting is the
         * right response to both; NvM will then supply defaults for every block. */
        return Fee_Format();
    }

    if ((valid[0] != FALSE) && (valid[1] != FALSE))
    {
        /* Two valid sectors is the fingerprint of a garbage collection interrupted between
         * its commit and its cleanup. The higher sequence number is the finished copy. */
        const uint8 winner = (Fee_CounterIsNewer(sequence[1], sequence[0]) != FALSE) ? 1u : 0u;
        const uint8 loser = (uint8)((winner + 1u) % FEE_SECTOR_COUNT);

        Fee_ActiveSector = winner;
        Fee_ActiveSequence = sequence[winner];
        Fee_Status.layoutRecovered = TRUE;

        if (Fls_Erase(Fee_SectorBase(loser), (Fls_LengthType)FEE_SECTOR_SIZE) != E_OK)
        {
            Fee_Status.mediaErrorCount++;
            /* Not fatal: the winner is intact and usable. The stale sector is retried on the
             * next start, and garbage collection will erase it before reuse anyway. */
        }
    }
    else
    {
        Fee_ActiveSector = (valid[0] != FALSE) ? (uint8)0u : (uint8)1u;
        Fee_ActiveSequence = sequence[Fee_ActiveSector];
    }

    /* A fresh scan has just established the cursor from the media, so any poisoning from a previous
     * session has already been accounted for by Fee_FindWriteCursor. */
    Fee_SlotPoisoned = FALSE;
    Fee_Initialised = TRUE;
    Fee_WriteCursor = Fee_FindWriteCursor();
    Fee_Status.activeSector = Fee_ActiveSector;
    Fee_Status.activeSequence = Fee_ActiveSequence;

    return E_OK;
}

Std_ReturnType Fee_ReadBlock(Fee_BlockIdType blockId, uint8 *buffer, uint16 offset, uint16 length)
{
    uint16 configuredLength = 0u;
    Fee_RecordType record;
    Std_ReturnType status;

    DET_CHECK_RETURN(Fee_Initialised != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_READ,
                     FEE_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(buffer != NULL_PTR, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_READ,
                     FEE_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(Fee_LookupBlock(blockId, &configuredLength) != FALSE, MODULE_ID_FEE,
                     INSTANCE_ID_SINGLE, FEE_API_ID_READ, FEE_E_INVALID_BLOCK_NO, E_NOT_OK);
    DET_CHECK_RETURN(((uint32)offset + (uint32)length) <= (uint32)configuredLength, MODULE_ID_FEE,
                     INSTANCE_ID_SINGLE, FEE_API_ID_READ, FEE_E_INVALID_LENGTH, E_NOT_OK);

    status = Fee_FindNewestRecord(blockId, &record);
    if (status != E_OK)
    {
        return status;
    }

    if (((uint32)offset + (uint32)length) > (uint32)record.length)
    {
        /* The stored record is shorter than the read asks for. This happens when a block's
         * configured length grows across a firmware update; reporting it rather than padding
         * lets NvM apply its defaults for the whole block instead of returning a structure
         * that is half old data and half erased flash. */
        return E_NOT_OK;
    }

    if (Fls_Read(Fee_SectorBase(Fee_ActiveSector) + record.offset + FEE_RECORD_HEADER_SIZE +
                     (uint32)offset,
                 buffer, (Fls_LengthType)length) != E_OK)
    {
        Fee_Status.mediaErrorCount++;
        return E_NOT_OK;
    }

    return E_OK;
}

Std_ReturnType Fee_WriteBlock(Fee_BlockIdType blockId, const uint8 *buffer)
{
    uint16 length = 0u;
    uint16 counter;
    Std_ReturnType status;

    DET_CHECK_RETURN(Fee_Initialised != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_WRITE,
                     FEE_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(buffer != NULL_PTR, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_WRITE,
                     FEE_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(Fee_LookupBlock(blockId, &length) != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE,
                     FEE_API_ID_WRITE, FEE_E_INVALID_BLOCK_NO, E_NOT_OK);

    /* A previous append left the cursor pointing at media that may be partially programmed. Nothing
     * can be written or read past it, so it is reclaimed before anything else is attempted. See
     * ::Fee_SlotPoisoned. */
    if (Fee_SlotPoisoned != FALSE)
    {
        if (Fee_GarbageCollect() != E_OK)
        {
            return E_NOT_OK;
        }
        Fee_SlotPoisoned = FALSE;
    }

    /* Collect early rather than at the moment the sector actually fills, so that a write of
     * the largest block never discovers there is nowhere to put it. */
    if ((Fee_WriteCursor + Fee_RecordSpan(length) + FEE_GC_THRESHOLD_BYTES) >
        (uint32)FEE_SECTOR_SIZE)
    {
        if (Fee_GarbageCollect() != E_OK)
        {
            return E_NOT_OK;
        }
    }

    counter = (uint16)(Fee_HighestCounter(blockId) + 1u);

    status = Fee_AppendRecord(blockId, buffer, length, counter, (uint8)FEE_RECORD_STATE_VALID);

    if (status == E_NO_SPACE)
    {
        /* Unexpected after the pre-emptive collection above, so the sector must have been
         * fuller than the cursor suggested. One forced collection, then one retry. */
        if (Fee_GarbageCollect() != E_OK)
        {
            return E_NOT_OK;
        }
        status = Fee_AppendRecord(blockId, buffer, length, counter, (uint8)FEE_RECORD_STATE_VALID);
    }

    Fee_Status.bytesUsed = Fee_WriteCursor;
    Fee_Status.bytesFree = (uint32)FEE_SECTOR_SIZE - Fee_WriteCursor;

    return status;
}

Std_ReturnType Fee_InvalidateBlock(Fee_BlockIdType blockId)
{
    uint16 length = 0u;
    uint16 counter;

    DET_CHECK_RETURN(Fee_Initialised != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE,
                     FEE_API_ID_INVALIDATE, FEE_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(Fee_LookupBlock(blockId, &length) != FALSE, MODULE_ID_FEE, INSTANCE_ID_SINGLE,
                     FEE_API_ID_INVALIDATE, FEE_E_INVALID_BLOCK_NO, E_NOT_OK);

    if ((Fee_WriteCursor + Fee_RecordSpan(length) + FEE_GC_THRESHOLD_BYTES) >
        (uint32)FEE_SECTOR_SIZE)
    {
        if (Fee_GarbageCollect() != E_OK)
        {
            return E_NOT_OK;
        }
    }

    counter = (uint16)(Fee_HighestCounter(blockId) + 1u);

    /* An invalidation is a record like any other -- header, no payload, then the commit byte
     * -- so retiring a block is as crash safe as writing one. The length is still declared so
     * the scanner can step over it. */
    {
        uint8 empty[FEE_ALIGNMENT];
        uint8 i;

        for (i = 0u; i < (uint8)FEE_ALIGNMENT; i++)
        {
            empty[i] = (uint8)FLS_ERASED_VALUE;
        }
        return Fee_AppendRecord(blockId, empty, (uint16)FEE_ALIGNMENT, counter,
                                (uint8)FEE_RECORD_STATE_INVALID);
    }
}

Std_ReturnType Fee_GetStatus(Fee_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_FEE, INSTANCE_ID_SINGLE, FEE_API_ID_GET_STATUS,
                     FEE_E_PARAM_POINTER, E_NOT_OK);

    Fee_Status.activeSector = Fee_ActiveSector;
    Fee_Status.activeSequence = Fee_ActiveSequence;
    Fee_Status.bytesUsed = Fee_WriteCursor;
    Fee_Status.bytesFree = (Fee_WriteCursor <= (uint32)FEE_SECTOR_SIZE)
                               ? ((uint32)FEE_SECTOR_SIZE - Fee_WriteCursor)
                               : 0u;

    *status = Fee_Status;
    return E_OK;
}

void Fee_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = FEE_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_FEE;
        versioninfo->sw_major_version = FEE_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = FEE_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = FEE_SW_PATCH_VERSION;
    }
}

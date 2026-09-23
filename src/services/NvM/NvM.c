/**
 * @file    NvM.c
 * @brief   NVRAM manager implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "NvM.h"

#include <string.h>

#include "Crc.h"
#include "Det.h"
#include "Fee.h"

/*==================================================================================================
 *  Block descriptor table
 *================================================================================================*/

typedef struct
{
    Fee_BlockIdType feeBlock;  /**< Where Fee stores this block.                      */
    uint16 length;             /**< Payload length, excluding NvM's CRC.              */
    void *mirror;              /**< RAM mirror.                                       */
    const void *defaults;      /**< Compiled-in default image.                        */
    boolean immediate;         /**< TRUE to write through on every change.            */
} NvM_BlockDescriptorType;

/*------------------------------- RAM mirrors --------------------------------*/

STATIC NvM_OdometerType NvM_MirrorOdometer;
STATIC NvM_DeviceConfigType NvM_MirrorDeviceConfig;
STATIC NvM_CalibrationType NvM_MirrorCalibration;
STATIC NvM_RestartInfoType NvM_MirrorRestartInfo;
STATIC NvM_EnergyCountersType NvM_MirrorEnergyCounters;

/*------------------------------- Defaults -----------------------------------*/

STATIC const NvM_OdometerType NvM_DefaultOdometer = {
    NVM_STRUCT_VERSION, 0u, 0uLL, 0uLL, 0uL, 0uL,
};

STATIC const NvM_DeviceConfigType NvM_DefaultDeviceConfig = {
    NVM_STRUCT_VERSION, NVM_DEFAULT_BROKER_PORT, {0}, {0},
};

STATIC const NvM_CalibrationType NvM_DefaultCalibration = {
    NVM_STRUCT_VERSION,
    NVM_DEFAULT_TYRE_DIAMETER_MILLI_INCH,
    NVM_DEFAULT_GEAR_RATIO_MILLI,
    NVM_DEFAULT_VBATT_DIVIDER_MILLI,
    NVM_DEFAULT_VBATT_OFFSET_MV,
    NVM_DEFAULT_MAX_PLAUSIBLE_RPM,
    0uL,
};

STATIC const NvM_RestartInfoType NvM_DefaultRestartInfo = {
    NVM_STRUCT_VERSION, 0u, 0uL, 0uL, 0uL,
};

STATIC const NvM_EnergyCountersType NvM_DefaultEnergyCounters = {
    NVM_STRUCT_VERSION, 0u, 0uLL, 0uLL, 0uL,
};

/*------------------------------ Descriptors ---------------------------------*/

STATIC const NvM_BlockDescriptorType NvM_Blocks[NVM_BLOCK_COUNT] = {
    /* The odometer writes through immediately. A deferred write is precisely the window in
     * which the power cut that loses the value happens, and the whole product is that value. */
    {FEE_BLOCK_ODOMETER, NVM_LENGTH_ODOMETER, &NvM_MirrorOdometer, &NvM_DefaultOdometer, TRUE},
    {FEE_BLOCK_DEVICE_CONFIG, NVM_LENGTH_DEVICE_CONFIG, &NvM_MirrorDeviceConfig,
     &NvM_DefaultDeviceConfig, FALSE},
    {FEE_BLOCK_CALIBRATION, NVM_LENGTH_CALIBRATION, &NvM_MirrorCalibration,
     &NvM_DefaultCalibration, FALSE},
    /* Restart info must also be durable at once: its entire purpose is to be readable after
     * the reset that incremented it. */
    {FEE_BLOCK_RESTART_INFO, NVM_LENGTH_RESTART_INFO, &NvM_MirrorRestartInfo,
     &NvM_DefaultRestartInfo, TRUE},
    {FEE_BLOCK_ENERGY_COUNTERS, NVM_LENGTH_ENERGY_COUNTERS, &NvM_MirrorEnergyCounters,
     &NvM_DefaultEnergyCounters, FALSE},
};

/*==================================================================================================
 *  Build-time guarantees
 *
 *  Each structure plus NvM's CRC must fit the Fee block it is stored in. Checking here turns an
 *  over-sized structure into a build failure instead of a truncation nobody notices until a
 *  field unit reads back half a record.
 *================================================================================================*/

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((sizeof(NvM_OdometerType) + NVM_CRC_SIZE) <= FEE_LENGTH_ODOMETER,
               "NvM_OdometerType does not fit its Fee block");
_Static_assert((sizeof(NvM_DeviceConfigType) + NVM_CRC_SIZE) <= FEE_LENGTH_DEVICE_CONFIG,
               "NvM_DeviceConfigType does not fit its Fee block");
_Static_assert((sizeof(NvM_CalibrationType) + NVM_CRC_SIZE) <= FEE_LENGTH_CALIBRATION,
               "NvM_CalibrationType does not fit its Fee block");
_Static_assert((sizeof(NvM_RestartInfoType) + NVM_CRC_SIZE) <= FEE_LENGTH_RESTART_INFO,
               "NvM_RestartInfoType does not fit its Fee block");
_Static_assert((sizeof(NvM_EnergyCountersType) + NVM_CRC_SIZE) <= FEE_LENGTH_ENERGY_COUNTERS,
               "NvM_EnergyCountersType does not fit its Fee block");
_Static_assert(NVM_MAX_BLOCK_LENGTH >= sizeof(NvM_DeviceConfigType),
               "NVM_MAX_BLOCK_LENGTH is smaller than the largest block");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean NvM_Initialised = FALSE;
STATIC boolean NvM_Dirty[NVM_BLOCK_COUNT];
STATIC NvM_RequestResultType NvM_Result[NVM_BLOCK_COUNT];
STATIC NvM_StatisticsType NvM_Stats;

/** Last image committed for each block, so write-on-change can compare without a flash read. */
STATIC uint8 NvM_LastWritten[NVM_BLOCK_COUNT][NVM_MAX_BLOCK_LENGTH];
STATIC boolean NvM_LastWrittenValid[NVM_BLOCK_COUNT];

/** Staging buffer: payload followed by NvM's CRC. */
STATIC uint8 NvM_Staging[NVM_MAX_BLOCK_LENGTH + NVM_CRC_SIZE];

/** Round-robin cursor for ::NvM_MainFunction, so no block can be starved. */
STATIC uint8 NvM_MainCursor;

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

STATIC boolean NvM_BlockValid(NvM_BlockIdType blockId)
{
    return (blockId < (NvM_BlockIdType)NVM_BLOCK_COUNT) ? TRUE : FALSE;
}

STATIC void NvM_PutU32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFuL);
    p[1] = (uint8)((v >> 8u) & 0xFFuL);
    p[2] = (uint8)((v >> 16u) & 0xFFuL);
    p[3] = (uint8)((v >> 24u) & 0xFFuL);
}

STATIC uint32 NvM_GetU32(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8u) | ((uint32)p[2] << 16u) | ((uint32)p[3] << 24u);
}

/** Copy a block's default image into its RAM mirror and record that it happened. */
STATIC void NvM_ApplyDefaults(uint8 index)
{
    (void)memcpy(NvM_Blocks[index].mirror, NvM_Blocks[index].defaults, NvM_Blocks[index].length);
    NvM_Result[index] = NVM_REQ_RESTORED_DEFAULTS;
    NvM_Stats.defaultsApplied++;

    /* The defaults are not on the media, so the write-on-change comparison must not believe
     * they are -- otherwise the first real write would be suppressed as "unchanged" and the
     * block would stay absent forever. */
    NvM_LastWrittenValid[index] = FALSE;
}

/** Commit block @p index to Fee, appending NvM's own CRC. */
STATIC Std_ReturnType NvM_Commit(uint8 index)
{
    const uint16 length = NvM_Blocks[index].length;
    uint32 crc;

    (void)memcpy(NvM_Staging, NvM_Blocks[index].mirror, length);
    crc = Crc_CalculateCRC32(NvM_Staging, (uint32)length, 0u, TRUE);
    NvM_PutU32(&NvM_Staging[length], crc);

    /* Fee's block is sized for payload plus CRC; anything beyond that is padding it does not
     * interpret, so only the meaningful prefix needs filling. */
    if (Fee_WriteBlock(NvM_Blocks[index].feeBlock, NvM_Staging) != E_OK)
    {
        NvM_Result[index] = NVM_REQ_NOT_OK;
        NvM_Stats.writeFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_NVM, index, NVM_API_ID_WRITE_BLOCK,
                                     NVM_E_WRITE_FAILED);
        return E_NOT_OK;
    }

    (void)memcpy(NvM_LastWritten[index], NvM_Blocks[index].mirror, length);
    NvM_LastWrittenValid[index] = TRUE;
    NvM_Dirty[index] = FALSE;
    NvM_Result[index] = NVM_REQ_OK;
    NvM_Stats.writeCount++;

    return E_OK;
}

/** Load block @p index from Fee into its RAM mirror, falling back to defaults. */
STATIC void NvM_Load(uint8 index)
{
    const uint16 length = NvM_Blocks[index].length;
    const uint16 stored = (uint16)(length + NVM_CRC_SIZE);
    Std_ReturnType status;

    status = Fee_ReadBlock(NvM_Blocks[index].feeBlock, NvM_Staging, 0u, stored);

    if (status != E_OK)
    {
        if (status == E_CRC_FAIL)
        {
            NvM_Stats.integrityFailures++;
            (void)Det_ReportRuntimeError(MODULE_ID_NVM, index, NVM_API_ID_READ_BLOCK,
                                         NVM_E_INTEGRITY_FAILED);
        }
        NvM_ApplyDefaults(index);
        return;
    }

    /* NvM's own CRC is checked even though Fee already verified its record. Fee's check proves
     * the media is intact; this one covers the whole path, including a RAM bit flip in the
     * staging buffer and any mismatch between what was intended and what was stored. */
    if (NvM_GetU32(&NvM_Staging[length]) !=
        Crc_CalculateCRC32(NvM_Staging, (uint32)length, 0u, TRUE))
    {
        NvM_Stats.integrityFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_NVM, index, NVM_API_ID_READ_BLOCK,
                                     NVM_E_INTEGRITY_FAILED);
        NvM_ApplyDefaults(index);
        return;
    }

    (void)memcpy(NvM_Blocks[index].mirror, NvM_Staging, length);
    (void)memcpy(NvM_LastWritten[index], NvM_Staging, length);
    NvM_LastWrittenValid[index] = TRUE;
    NvM_Result[index] = NVM_REQ_OK;
    NvM_Stats.readCount++;
}

/** Count the blocks currently awaiting a deferred write. */
STATIC uint8 NvM_CountDirty(void)
{
    uint8 i;
    uint8 count = 0u;

    for (i = 0u; i < (uint8)NVM_BLOCK_COUNT; i++)
    {
        if (NvM_Dirty[i] != FALSE)
        {
            count++;
        }
    }
    return count;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType NvM_Init(void)
{
    uint8 i;

    NvM_Stats.writeCount = 0u;
    NvM_Stats.skippedWriteCount = 0u;
    NvM_Stats.readCount = 0u;
    NvM_Stats.integrityFailures = 0u;
    NvM_Stats.defaultsApplied = 0u;
    NvM_Stats.writeFailures = 0u;
    NvM_Stats.dirtyBlockCount = 0u;
    NvM_MainCursor = 0u;

    for (i = 0u; i < (uint8)NVM_BLOCK_COUNT; i++)
    {
        NvM_Dirty[i] = FALSE;
        NvM_LastWrittenValid[i] = FALSE;
        NvM_Result[i] = NVM_REQ_NOT_OK;
    }

    NvM_Initialised = TRUE;

    for (i = 0u; i < (uint8)NVM_BLOCK_COUNT; i++)
    {
        NvM_Load(i);
    }

    return E_OK;
}

Std_ReturnType NvM_ReadBlock(NvM_BlockIdType blockId, void *destination)
{
    DET_CHECK_RETURN(NvM_Initialised != FALSE, MODULE_ID_NVM, blockId, NVM_API_ID_READ_BLOCK,
                     NVM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(NvM_BlockValid(blockId) != FALSE, MODULE_ID_NVM, blockId,
                     NVM_API_ID_READ_BLOCK, NVM_E_PARAM_BLOCK_ID, E_NOT_OK);
    DET_CHECK_RETURN(destination != NULL_PTR, MODULE_ID_NVM, blockId, NVM_API_ID_READ_BLOCK,
                     NVM_E_PARAM_POINTER, E_NOT_OK);

    (void)memcpy(destination, NvM_Blocks[blockId].mirror, NvM_Blocks[blockId].length);
    return E_OK;
}

Std_ReturnType NvM_WriteBlock(NvM_BlockIdType blockId, const void *source)
{
    const uint8 index = (uint8)blockId;
    uint16 length;

    DET_CHECK_RETURN(NvM_Initialised != FALSE, MODULE_ID_NVM, blockId, NVM_API_ID_WRITE_BLOCK,
                     NVM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(NvM_BlockValid(blockId) != FALSE, MODULE_ID_NVM, blockId,
                     NVM_API_ID_WRITE_BLOCK, NVM_E_PARAM_BLOCK_ID, E_NOT_OK);
    DET_CHECK_RETURN(source != NULL_PTR, MODULE_ID_NVM, blockId, NVM_API_ID_WRITE_BLOCK,
                     NVM_E_PARAM_POINTER, E_NOT_OK);

    length = NvM_Blocks[index].length;
    (void)memcpy(NvM_Blocks[index].mirror, source, length);

    /* Write-on-change. The odometer runnable calls this every cycle; only the cycles where the
     * vehicle actually moved should reach the flash, or the endurance budget is spent on a
     * parked vehicle. */
    if ((NvM_LastWrittenValid[index] != FALSE) &&
        (memcmp(NvM_LastWritten[index], NvM_Blocks[index].mirror, length) == 0))
    {
        NvM_Dirty[index] = FALSE;
        NvM_Result[index] = NVM_REQ_BLOCK_SKIPPED;
        NvM_Stats.skippedWriteCount++;
        return E_OK;
    }

    if (NvM_Blocks[index].immediate != FALSE)
    {
        return NvM_Commit(index);
    }

    NvM_Dirty[index] = TRUE;
    NvM_Result[index] = NVM_REQ_PENDING;
    NvM_Stats.dirtyBlockCount = NvM_CountDirty();
    return E_OK;
}

Std_ReturnType NvM_WriteImmediate(NvM_BlockIdType blockId)
{
    DET_CHECK_RETURN(NvM_Initialised != FALSE, MODULE_ID_NVM, blockId, NVM_API_ID_WRITE_BLOCK,
                     NVM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(NvM_BlockValid(blockId) != FALSE, MODULE_ID_NVM, blockId,
                     NVM_API_ID_WRITE_BLOCK, NVM_E_PARAM_BLOCK_ID, E_NOT_OK);

    if (NvM_Dirty[blockId] == FALSE)
    {
        return E_OK;
    }
    return NvM_Commit((uint8)blockId);
}

Std_ReturnType NvM_WriteAll(void)
{
    uint8 i;
    boolean allOk = TRUE;

    DET_CHECK_RETURN(NvM_Initialised != FALSE, MODULE_ID_NVM, INSTANCE_ID_SINGLE,
                     NVM_API_ID_WRITE_ALL, NVM_E_UNINIT, E_NOT_OK);

    for (i = 0u; i < (uint8)NVM_BLOCK_COUNT; i++)
    {
        if (NvM_Dirty[i] != FALSE)
        {
            /* Every block is attempted even after one fails. Losing a single block is far
             * better than abandoning the remaining ones on the shutdown path. */
            if (NvM_Commit(i) != E_OK)
            {
                allOk = FALSE;
            }
        }
    }

    NvM_Stats.dirtyBlockCount = NvM_CountDirty();
    return (allOk != FALSE) ? E_OK : E_NOT_OK;
}

Std_ReturnType NvM_RestoreBlockDefaults(NvM_BlockIdType blockId)
{
    DET_CHECK_RETURN(NvM_Initialised != FALSE, MODULE_ID_NVM, blockId,
                     NVM_API_ID_RESTORE_DEFAULTS, NVM_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(NvM_BlockValid(blockId) != FALSE, MODULE_ID_NVM, blockId,
                     NVM_API_ID_RESTORE_DEFAULTS, NVM_E_PARAM_BLOCK_ID, E_NOT_OK);

    (void)memcpy(NvM_Blocks[blockId].mirror, NvM_Blocks[blockId].defaults,
                 NvM_Blocks[blockId].length);
    NvM_Dirty[blockId] = TRUE;
    NvM_Result[blockId] = NVM_REQ_PENDING;
    NvM_Stats.dirtyBlockCount = NvM_CountDirty();

    return E_OK;
}

NvM_RequestResultType NvM_GetErrorStatus(NvM_BlockIdType blockId)
{
    if (NvM_BlockValid(blockId) == FALSE)
    {
        return NVM_REQ_NOT_OK;
    }
    return NvM_Result[blockId];
}

void NvM_MainFunction(void)
{
    uint8 attempt;

    if (NvM_Initialised == FALSE)
    {
        return;
    }

    /* At most one block per invocation. A flash write holds the bus long enough to delay the
     * CAN reader, so spreading the work keeps a single scheduler slot's worst case bounded. The
     * round-robin cursor stops a block that is rewritten every cycle from starving the rest. */
    for (attempt = 0u; attempt < (uint8)NVM_BLOCK_COUNT; attempt++)
    {
        const uint8 index = NvM_MainCursor;

        NvM_MainCursor = (uint8)((NvM_MainCursor + 1u) % NVM_BLOCK_COUNT);

        if (NvM_Dirty[index] != FALSE)
        {
            (void)NvM_Commit(index);
            break;
        }
    }

    NvM_Stats.dirtyBlockCount = NvM_CountDirty();
}

Std_ReturnType NvM_GetStatistics(NvM_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_NVM, INSTANCE_ID_SINGLE,
                     NVM_API_ID_MAIN_FUNCTION, NVM_E_PARAM_POINTER, E_NOT_OK);

    NvM_Stats.dirtyBlockCount = NvM_CountDirty();
    *stats = NvM_Stats;
    return E_OK;
}

void NvM_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = NVM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_NVM;
        versioninfo->sw_major_version = NVM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = NVM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = NVM_SW_PATCH_VERSION;
    }
}

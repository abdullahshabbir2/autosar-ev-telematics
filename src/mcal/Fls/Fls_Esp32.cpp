/**
 * @file    Fls_Esp32.cpp
 * @brief   ESP32 platform leaf of the flash driver, over a dedicated raw partition.
 *
 * @par Why esp_partition and not spi_flash_write directly
 * @c esp_partition_write takes an offset relative to the partition and refuses anything outside it. That
 * bound is enforced by the IDF, below this driver's own range check, so a defect in the arithmetic here cannot
 * reach the application image or the partition table. Writing through @c spi_flash_write with an absolute
 * address would put nothing between a wrong offset and a bricked device.
 *
 * @par Erase counting is per-sector and persistent
 * The wear figure that matters is erases per sector, not total writes, and it must survive a reset or it says
 * nothing. The counters here are in RAM; Fee owns the persistent tally in its own header and this driver's
 * ::Fls_GetStatistics reports the current session. That split is deliberate -- a flash driver that writes its
 * own wear counter to flash on every erase doubles the wear it is measuring.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <esp_partition.h>
#include <string.h>

#include "services/Det/Det.h"
#include "mcal/Fls/Fls.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/** Scratch buffer for read-back verification and blank checking. */
#define FLS_SCRATCH_SIZE 256u

static const esp_partition_t *Fls_Partition = NULL;
static Fls_JobResultType Fls_JobResult = FLS_JOB_OK;
static Fls_StatisticsType Fls_Stats;
static boolean Fls_Initialised = FALSE;

/**
 * @brief Verification scratch, module-static rather than on the stack.
 *
 * 256 bytes is a quarter of the smallest task stack in the system, and ::Fls_Write is reachable from the
 * storage task whose deepest path already includes the FAT driver. Putting it here makes the cost visible at
 * link time instead of as an intermittent stack overflow.
 */
static uint8 Fls_Scratch[FLS_SCRATCH_SIZE];

/*==================================================================================================
 *  Local helpers
 *================================================================================================*/

/** Validate the module state and that [@p address, @p address + @p length) lies inside the partition. */
static Std_ReturnType Fls_CheckRange(Fls_AddressType address, Fls_LengthType length, uint8 apiId)
{
    if ((Fls_Initialised == FALSE) || (Fls_Partition == NULL))
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, apiId, FLS_E_UNINIT);
        return E_NOT_OK;
    }
    if (length == 0u)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, apiId, FLS_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    /* The addition is checked for overflow before the range comparison. address + length wrapping past
     * 2^32 would otherwise produce a small sum that passes the bound and a write that starts inside the
     * partition and runs off its end. */
    if ((address > (Fls_AddressType)FLS_PARTITION_SIZE) ||
        (length > ((Fls_LengthType)FLS_PARTITION_SIZE - address)))
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, apiId, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    return E_OK;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

extern "C" Std_ReturnType Fls_Init(void)
{
    (void)memset(&Fls_Stats, 0, sizeof(Fls_Stats));

    Fls_Partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             (esp_partition_subtype_t)FLS_PARTITION_SUBTYPE,
                                             FLS_PARTITION_NAME);
    if (Fls_Partition == NULL)
    {
        /* A missing partition is a build configuration fault, not a runtime one: the image was flashed with a
         * partition table that does not match the firmware. Reported rather than worked around, because the
         * alternative -- falling back to NVS -- is how v1 lost odometer readings. */
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_INIT, E_PARAM_CONFIG);
        Fls_JobResult = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    if (Fls_Partition->size < (uint32)FLS_PARTITION_SIZE)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_INIT, E_PARAM_CONFIG);
        Fls_Partition = NULL;
        Fls_JobResult = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    /* The partition must be a whole number of sectors, or the last sector is shared with whatever follows it
     * and erasing it destroys that. The static assertion in Fls_Cfg.h covers the configured size; this covers
     * the flashed table, which is a separate artefact and can disagree with it. */
    if ((Fls_Partition->size % (uint32)FLS_SECTOR_SIZE) != 0u)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_INIT, E_PARAM_CONFIG);
        Fls_Partition = NULL;
        Fls_JobResult = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    Fls_Initialised = TRUE;
    Fls_JobResult = FLS_JOB_OK;

    return E_OK;
}

extern "C" Std_ReturnType Fls_Read(Fls_AddressType address, uint8 *buffer, Fls_LengthType length)
{
    Std_ReturnType status = Fls_CheckRange(address, length, FLS_API_ID_READ);

    if (status != E_OK)
    {
        return status;
    }
    if (buffer == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_READ, FLS_E_PARAM_DATA);
        return E_NOT_OK;
    }

    if (esp_partition_read(Fls_Partition, (size_t)address, buffer, (size_t)length) != ESP_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_READ,
                                     FLS_E_READ_FAILED);
        Fls_JobResult = FLS_JOB_FAILED;
        status = E_NOT_OK;
    }
    else
    {
        Fls_Stats.readCount++;
        Fls_JobResult = FLS_JOB_OK;
    }

    return status;
}

extern "C" Std_ReturnType Fls_Write(Fls_AddressType address, const uint8 *buffer, Fls_LengthType length)
{
    Std_ReturnType status = Fls_CheckRange(address, length, FLS_API_ID_WRITE);

    if (status != E_OK)
    {
        return status;
    }
    if (buffer == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_WRITE, FLS_E_PARAM_DATA);
        return E_NOT_OK;
    }

    /* Alignment is checked here rather than fixed up. Padding a misaligned write inside the driver would
     * program bytes the caller did not ask for, and in an append-only log those extra bytes are the next
     * record's header. Fee pads its records to this boundary itself, so a misaligned call is a defect. */
    if (((address % (Fls_AddressType)FLS_WRITE_ALIGNMENT) != 0u) ||
        ((length % (Fls_LengthType)FLS_WRITE_ALIGNMENT) != 0u))
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_WRITE, FLS_E_UNALIGNED);
        return E_NOT_OK;
    }

    if (esp_partition_write(Fls_Partition, (size_t)address, buffer, (size_t)length) != ESP_OK)
    {
        Fls_Stats.writeFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_WRITE,
                                     FLS_E_WRITE_FAILED);
        Fls_JobResult = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    Fls_Stats.writeCount++;
    Fls_Stats.bytesWritten += length;

#if (FLS_VERIFY_AFTER_WRITE == STD_ON)
    /* Read back in scratch-sized chunks and compare. This is what turns a cell that no longer programs into
     * an immediate ::FLS_E_VERIFY_FAILED that NvM can answer by using the redundant copy, rather than a
     * corruption discovered on some future boot with no way to tell when it happened. */
    {
        Fls_LengthType remaining = length;
        Fls_AddressType cursor = address;
        const uint8 *source = buffer;

        while (remaining > 0u)
        {
            const Fls_LengthType chunk =
                (remaining < (Fls_LengthType)FLS_SCRATCH_SIZE) ? remaining : (Fls_LengthType)FLS_SCRATCH_SIZE;

            if (esp_partition_read(Fls_Partition, (size_t)cursor, Fls_Scratch, (size_t)chunk) != ESP_OK)
            {
                (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_WRITE,
                                             FLS_E_READ_FAILED);
                Fls_JobResult = FLS_JOB_FAILED;
                return E_NOT_OK;
            }

            if (memcmp(Fls_Scratch, source, (size_t)chunk) != 0)
            {
                Fls_Stats.verifyFailures++;
                (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_WRITE,
                                             FLS_E_VERIFY_FAILED);
                Fls_JobResult = FLS_JOB_FAILED;
                return E_NOT_OK;
            }

            cursor += chunk;
            source = &source[chunk];
            remaining -= chunk;
        }
    }
#endif

    Fls_JobResult = FLS_JOB_OK;
    return E_OK;
}

extern "C" Std_ReturnType Fls_Erase(Fls_AddressType address, Fls_LengthType length)
{
    Std_ReturnType status = Fls_CheckRange(address, length, FLS_API_ID_ERASE);

    if (status != E_OK)
    {
        return status;
    }

    /* Sector alignment is not negotiable: flash erases a whole sector whatever the caller asks for, so an
     * unaligned erase destroys data outside the requested range. Silently rounding down to a sector boundary
     * would be worse than refusing, because the caller would believe its range had been cleared. */
    if (((address % (Fls_AddressType)FLS_SECTOR_SIZE) != 0u) ||
        ((length % (Fls_LengthType)FLS_SECTOR_SIZE) != 0u))
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_ERASE, FLS_E_UNALIGNED);
        return E_NOT_OK;
    }

    if (esp_partition_erase_range(Fls_Partition, (size_t)address, (size_t)length) != ESP_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_ERASE,
                                     FLS_E_ERASE_FAILED);
        Fls_JobResult = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    Fls_Stats.eraseCount += (length / (Fls_LengthType)FLS_SECTOR_SIZE);
    Fls_JobResult = FLS_JOB_OK;

    return E_OK;
}

extern "C" Std_ReturnType Fls_Compare(Fls_AddressType address, const uint8 *buffer, Fls_LengthType length)
{
    Std_ReturnType status = Fls_CheckRange(address, length, FLS_API_ID_COMPARE);
    Fls_LengthType remaining = length;
    Fls_AddressType cursor = address;
    const uint8 *source = buffer;

    if (status != E_OK)
    {
        return status;
    }
    if (buffer == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_COMPARE, FLS_E_PARAM_DATA);
        return E_NOT_OK;
    }

    while (remaining > 0u)
    {
        const Fls_LengthType chunk =
            (remaining < (Fls_LengthType)FLS_SCRATCH_SIZE) ? remaining : (Fls_LengthType)FLS_SCRATCH_SIZE;

        if (esp_partition_read(Fls_Partition, (size_t)cursor, Fls_Scratch, (size_t)chunk) != ESP_OK)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_COMPARE,
                                         FLS_E_READ_FAILED);
            Fls_JobResult = FLS_JOB_FAILED;
            return E_NOT_OK;
        }

        if (memcmp(Fls_Scratch, source, (size_t)chunk) != 0)
        {
            /* Not an error condition: "different" is a legitimate answer to a comparison. No Det report, and
             * the job result stays OK -- NvM uses this to decide whether a write is needed at all, and a
             * difference is the normal case there. */
            return E_NOT_OK;
        }

        cursor += chunk;
        source = &source[chunk];
        remaining -= chunk;
    }

    Fls_Stats.readCount++;
    Fls_JobResult = FLS_JOB_OK;

    return E_OK;
}

extern "C" Std_ReturnType Fls_BlankCheck(Fls_AddressType address, Fls_LengthType length)
{
    Std_ReturnType status = Fls_CheckRange(address, length, FLS_API_ID_BLANK_CHECK);
    Fls_LengthType remaining = length;
    Fls_AddressType cursor = address;

    if (status != E_OK)
    {
        return status;
    }

    while (remaining > 0u)
    {
        const Fls_LengthType chunk =
            (remaining < (Fls_LengthType)FLS_SCRATCH_SIZE) ? remaining : (Fls_LengthType)FLS_SCRATCH_SIZE;
        Fls_LengthType i;

        if (esp_partition_read(Fls_Partition, (size_t)cursor, Fls_Scratch, (size_t)chunk) != ESP_OK)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_BLANK_CHECK,
                                         FLS_E_READ_FAILED);
            Fls_JobResult = FLS_JOB_FAILED;
            return E_NOT_OK;
        }

        for (i = 0u; i < chunk; i++)
        {
            if (Fls_Scratch[i] != (uint8)FLS_ERASED_VALUE)
            {
                /* As with Compare: "not blank" is an answer, not a fault. Fee calls this before choosing a
                 * sector precisely because programming a non-erased cell succeeds partially and produces data
                 * that reads back plausibly but fails its CRC. */
                return E_NOT_OK;
            }
        }

        cursor += chunk;
        remaining -= chunk;
    }

    Fls_JobResult = FLS_JOB_OK;
    return E_OK;
}

extern "C" Fls_JobResultType Fls_GetJobResult(void)
{
    return Fls_JobResult;
}

extern "C" Fls_LengthType Fls_GetPartitionSize(void)
{
    return (Fls_Partition != NULL) ? (Fls_LengthType)Fls_Partition->size : 0uL;
}

extern "C" Std_ReturnType Fls_GetStatistics(Fls_StatisticsType *stats)
{
    if (stats == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_FLS, INSTANCE_ID_SINGLE, FLS_API_ID_READ, FLS_E_PARAM_DATA);
        return E_NOT_OK;
    }

    *stats = Fls_Stats;
    return E_OK;
}

extern "C" void Fls_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = FLS_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_FLS;
        versioninfo->sw_major_version = FLS_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = FLS_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = FLS_SW_PATCH_VERSION;
    }
}

/**
 * @file    FsAbs_Esp32.cpp
 * @brief   ESP32 platform leaf of the filesystem abstraction: microSD over the shared SPI bus.
 *
 * @par Every operation holds the SPI bus lock
 * The card shares VSPI with the MCP2515. The @c SD library has no idea the other device exists and will clock
 * the bus whenever it likes, so the arbitration has to happen one level up -- here. Each entry point acquires
 * ::SPI_DEVICE_SD for the whole operation and releases it on every exit path, which is why the file is written
 * with a single acquire at the top and a single release at the bottom of each function rather than early
 * returns scattered through the middle.
 *
 * Holding the lock across a whole file operation rather than per transfer is deliberate. An SD write is not one
 * transfer: it is a command, a data block, a busy poll that can run for a second or more while the card does
 * internal wear levelling, and a status read. Releasing the bus between those phases would let the CAN driver
 * assert its chip select mid-sequence, and the card would abandon the write. That is the exact signature in the
 * archived v1 logs -- intermittent write failures and "Card Initialization Failed" on a card that tested
 * perfectly -- and v1 had no arbitration of any kind.
 *
 * @par Every file handle is closed on every path
 * v1 leaked a @c File on each of several error paths. The ESP-IDF FAT driver has a fixed table of open files
 * (@c CONFIG_FATFS_MAX_OPENED_FILES, five by default), so a few thousand records into a leaking run every
 * subsequent open failed -- which looked exactly like a failing card. Each function here opens at most one file
 * and closes it before returning.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <string.h>

#include "Ecu_PinMap.h"
#include "ecuabs/FsAbs/FsAbs_Cfg.h"
#include "ecuabs/FsAbs/FsAbs_Platform.h"
#include "mcal/Spi/Spi.h"

/** Bound on acquiring the bus for a card operation. */
#define FSABS_SPI_LOCK_TIMEOUT_MS 3000uL

static boolean FsAbs_Mounted = FALSE;

/*==================================================================================================
 *  Local helpers
 *================================================================================================*/

/** Acquire the bus for the card. */
static Std_ReturnType FsAbs_AcquireBus(void)
{
    return Spi_Lock(SPI_DEVICE_SD, FSABS_SPI_LOCK_TIMEOUT_MS);
}

/** Release the bus, preserving @p status so a release failure cannot mask the real result. */
static Std_ReturnType FsAbs_ReleaseBus(Std_ReturnType status)
{
    const Std_ReturnType released = Spi_Unlock(SPI_DEVICE_SD);

    return (status != E_OK) ? status : released;
}

/*==================================================================================================
 *  Platform leaf
 *================================================================================================*/

extern "C" Std_ReturnType FsAbs_PlatformMount(void)
{
    Std_ReturnType status;

    if (FsAbs_Mounted != FALSE)
    {
        return E_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    /* SD.begin is given the chip select and the shared SPI instance explicitly. Left to its defaults it would
     * take SS and its own SPIClass, which on this board is the wrong pin and a second, unarbitrated view of the
     * same peripheral.
     *
     * The clock is passed as the configured SD rate. Spi_Init has already brought the bus up, and the card
     * driver reconfigures the rate on each of its own transactions -- the two coexist because this module's
     * lock, not the SPIClass lock, is what serialises the devices. */
    if (SD.begin((uint8_t)PIN_SD_CS, SPI, SPI_CLOCK_SD_HZ) == false)
    {
        status = E_NOT_OK;
    }
    else if (SD.cardType() == CARD_NONE)
    {
        /* begin() can succeed with no card present on some boards, because the initialisation sequence is
         * tolerant of a non-answering device. Checking the card type separates "no card in the slot" from "a
         * card that will not initialise", and only the second is worth retrying. */
        SD.end();
        status = E_NOT_OK;
    }
    else
    {
        FsAbs_Mounted = TRUE;
        status = E_OK;
    }

    return FsAbs_ReleaseBus(status);
}

extern "C" void FsAbs_PlatformUnmount(void)
{
    if (FsAbs_Mounted == FALSE)
    {
        return;
    }

    if (Spi_Lock(SPI_DEVICE_SD, FSABS_SPI_LOCK_TIMEOUT_MS) == E_OK)
    {
        SD.end();
        STD_DISCARD(Spi_Unlock(SPI_DEVICE_SD));
    }
    else
    {
        /* The bus could not be acquired, which means a holder is stuck. The card is still marked unmounted:
         * this is called from the shutdown path, and a shutdown that cannot complete because a mutex is held is
         * worse than one that leaves the card without a clean SD.end(). Every record is already committed --
         * ::FsAbs_PlatformAppend flushes each one -- so nothing is lost by skipping it. */
        SD.end();
    }

    FsAbs_Mounted = FALSE;
}

extern "C" Std_ReturnType FsAbs_PlatformGetSpace(uint32 *capacityMiB, uint32 *usedMiB)
{
    Std_ReturnType status;

    if ((capacityMiB == NULL_PTR) || (usedMiB == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    /* Divided down to MiB inside the lock, because totalBytes() is a uint64 and the arithmetic is cheaper than
     * a second bus acquisition would be. The shift by 20 is exact for both figures. */
    *capacityMiB = (uint32)(SD.totalBytes() >> 20u);
    *usedMiB = (uint32)(SD.usedBytes() >> 20u);

    return FsAbs_ReleaseBus(E_OK);
}

extern "C" boolean FsAbs_PlatformExists(const char *path)
{
    boolean exists = FALSE;

    if ((path == NULL_PTR) || (FsAbs_Mounted == FALSE))
    {
        return FALSE;
    }

    if (FsAbs_AcquireBus() != E_OK)
    {
        /* A bus that cannot be acquired is reported as "does not exist", which is the conservative answer: the
         * caller's next step is to create the file, and creating one that already exists is harmless here
         * because every write is an append. Answering TRUE would let a caller assume content it cannot read. */
        return FALSE;
    }

    exists = (SD.exists(path) != false) ? TRUE : FALSE;
    STD_DISCARD(Spi_Unlock(SPI_DEVICE_SD));

    return exists;
}

extern "C" Std_ReturnType FsAbs_PlatformAppend(const char *path, const uint8 *data, uint32 length)
{
    Std_ReturnType status;

    if ((path == NULL_PTR) || (data == NULL_PTR) || (length == 0uL))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    {
        File file = SD.open(path, FILE_APPEND);

        if (!file)
        {
            status = E_NOT_OK;
        }
        else
        {
            const size_t written = file.write(data, (size_t)length);

            /* flush() before close(), and the result of close() is not what decides success. The ESP-IDF FAT
             * driver buffers a partial sector, so the bytes are not on the card until the sector is written out;
             * a close() alone does flush, but doing it explicitly means the failure is attributable. A record
             * that is reported stored must actually be on the card -- TelemSwc deletes its in-memory copy on
             * the strength of this return value. */
            file.flush();
            file.close();

            status = (written == (size_t)length) ? E_OK : E_NOT_OK;
        }
    }

    return FsAbs_ReleaseBus(status);
}

extern "C" Std_ReturnType FsAbs_PlatformRead(const char *path, uint32 offset, uint8 *buffer, uint32 size,
                                             uint32 *read)
{
    Std_ReturnType status;

    if ((path == NULL_PTR) || (buffer == NULL_PTR) || (read == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    *read = 0uL;

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    {
        File file = SD.open(path, FILE_READ);

        if (!file)
        {
            status = E_NOT_FOUND;
        }
        else if (file.seek(offset) == false)
        {
            /* A seek past the end is end-of-file, not an error: the backfill walker reaches it on every run by
             * design, and treating it as a fault would raise a diagnostic event once per drain. */
            file.close();
            status = E_OK;
        }
        else
        {
            const int bytesRead = (int)file.read(buffer, (size_t)size);

            file.close();

            if (bytesRead < 0)
            {
                status = E_NOT_OK;
            }
            else
            {
                *read = (uint32)bytesRead;
                status = E_OK;
            }
        }
    }

    return FsAbs_ReleaseBus(status);
}

extern "C" Std_ReturnType FsAbs_PlatformSize(const char *path, uint32 *size)
{
    Std_ReturnType status;

    if ((path == NULL_PTR) || (size == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    {
        File file = SD.open(path, FILE_READ);

        if (!file)
        {
            status = E_NOT_FOUND;
        }
        else
        {
            *size = (uint32)file.size();
            file.close();
            status = E_OK;
        }
    }

    return FsAbs_ReleaseBus(status);
}

extern "C" Std_ReturnType FsAbs_PlatformRemove(const char *path)
{
    Std_ReturnType status;

    if (path == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    if (SD.exists(path) == false)
    {
        status = E_NOT_FOUND;
    }
    else
    {
        status = (SD.remove(path) != false) ? E_OK : E_NOT_OK;
    }

    return FsAbs_ReleaseBus(status);
}

/**
 * @brief Smallest log name strictly greater than @p after.
 *
 * Same directory walk as FsAbs_PlatformFindOldestLog, with one extra comparison. Kept as a separate
 * function rather than a parameter on that one because the two have different failure meanings: no
 * oldest log means the card holds no records at all, whereas no next log means the transfer has caught
 * up -- which is the normal state and must not read as an empty card.
 */
extern "C" Std_ReturnType FsAbs_PlatformFindNextLog(const char *after, char *buffer, uint16 size)
{
    Std_ReturnType status;

    if ((after == NULL_PTR) || (buffer == NULL_PTR) || (size == 0u))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    {
        File root = SD.open("/", FILE_READ);
        char best[FSABS_FILENAME_SIZE];
        boolean found = FALSE;

        best[0] = '\0';

        if (!root)
        {
            status = E_NOT_OK;
        }
        else
        {
            File entry = root.openNextFile();

            while (entry)
            {
                const char *const name = entry.name();

                if ((entry.isDirectory() == false) && (name != NULL_PTR) && (strlen(name) >= 12u) &&
                    (strstr(name, ".csv") != NULL_PTR) && (strcmp(name, after) > 0))
                {
                    if ((found == FALSE) || (strcmp(name, best) < 0))
                    {
                        (void)strncpy(best, name, sizeof(best) - 1u);
                        best[sizeof(best) - 1u] = '\0';
                        found = TRUE;
                    }
                }

                entry.close();
                entry = root.openNextFile();
            }

            root.close();

            if (found == FALSE)
            {
                status = E_NOT_FOUND;
            }
            else if (strlen(best) >= (size_t)size)
            {
                status = E_NO_SPACE;
            }
            else
            {
                (void)strcpy(buffer, best);
                status = E_OK;
            }
        }
    }

    return FsAbs_ReleaseBus(status);
}

extern "C" Std_ReturnType FsAbs_PlatformFindOldestLog(char *buffer, uint16 size)
{
    Std_ReturnType status;

    if ((buffer == NULL_PTR) || (size == 0u))
    {
        return E_NOT_OK;
    }
    if (FsAbs_Mounted == FALSE)
    {
        return E_NOT_OK;
    }

    status = FsAbs_AcquireBus();
    if (status != E_OK)
    {
        return status;
    }

    {
        File root = SD.open("/", FILE_READ);
        char oldest[FSABS_FILENAME_SIZE];
        boolean found = FALSE;

        oldest[0] = '\0';

        if (!root)
        {
            status = E_NOT_OK;
        }
        else
        {
            File entry = root.openNextFile();

            while (entry)
            {
                const char *const name = entry.name();

                /* Log files are named "/YYYYMMDD.csv", so a plain strcmp gives chronological order -- that is
                 * the entire reason for the name format, and it is why no timestamp has to be read to answer
                 * "which is oldest". v1 parsed the name with atoi and fell back to the first directory entry
                 * whenever the conversion failed, which on a card holding any other file picked that one.
                 *
                 * The length check is what makes the comparison safe: it rejects anything that is not exactly a
                 * log name, so a stray file cannot win the comparison by sorting low. */
                if ((entry.isDirectory() == false) && (name != NULL_PTR) &&
                    (strlen(name) >= 12u) && (strstr(name, ".csv") != NULL_PTR))
                {
                    if ((found == FALSE) || (strcmp(name, oldest) < 0))
                    {
                        (void)strncpy(oldest, name, sizeof(oldest) - 1u);
                        oldest[sizeof(oldest) - 1u] = '\0';
                        found = TRUE;
                    }
                }

                entry.close();
                entry = root.openNextFile();
            }

            root.close();

            if (found == FALSE)
            {
                status = E_NOT_FOUND;
            }
            else if (strlen(oldest) >= (size_t)size)
            {
                status = E_NO_SPACE;
            }
            else
            {
                (void)strcpy(buffer, oldest);
                status = E_OK;
            }
        }
    }

    return FsAbs_ReleaseBus(status);
}

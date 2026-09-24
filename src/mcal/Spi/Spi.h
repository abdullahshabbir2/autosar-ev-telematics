/**
 * @file    Spi.h
 * @brief   AUTOSAR SPI Handler/Driver (SWS_SPIDriver) -- ESP32 VSPI adaptation.
 *
 * One SPI bus is shared by the MCP2515 CAN controller and the microSD card. Sharing a
 * bus between two devices with different clock rates and different transaction lengths
 * is the whole difficulty of this module, and it is handled by making the bus an
 * explicitly owned resource.
 *
 * @par Why locking is mandatory here
 * The MCP2515 and the SD card disagree about clock speed (10 MHz against 20 MHz) and
 * about how long a transaction runs (tens of microseconds against tens of
 * milliseconds while the card does internal wear levelling). If the CAN task asserts
 * its chip select while the SD driver is mid-block, the card sees a truncated command
 * and returns to idle; the SD driver then reports a write failure that looks like a
 * failing card. The archived v1 logs contain exactly that signature -- intermittent
 * "Card Initialization Failed" and write failures on a card that tested fine -- and v1
 * had no bus arbitration of any kind: the CAN code drove chip select directly while the
 * Arduino @c SD library drove its own.
 *
 * ::Spi_Lock acquires the bus for one device, reconfigures the clock and mode for it,
 * and blocks any other device until ::Spi_Unlock. Every transaction must sit inside a
 * lock, and ::Spi_Transfer reports ::SPI_E_SEQ_IN_PROGRESS if it does not.
 *
 * @req SWREQ-COM-0020, SWREQ-COM-0021, SWREQ-SAF-0012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef SPI_H
#define SPI_H

#include "base/Autosar_ModuleIds.h"
#include "mcal/Spi/Spi_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_VENDOR_ID 0xFFFEu
#define SPI_AR_RELEASE_MAJOR_VERSION 4u
#define SPI_AR_RELEASE_MINOR_VERSION 4u
#define SPI_SW_MAJOR_VERSION 2u
#define SPI_SW_MINOR_VERSION 0u
#define SPI_SW_PATCH_VERSION 0u

#define SPI_API_ID_INIT 0x00u
#define SPI_API_ID_LOCK 0x0Au
#define SPI_API_ID_UNLOCK 0x0Bu
#define SPI_API_ID_TRANSFER 0x20u

#define SPI_E_UNINIT E_UNINIT
#define SPI_E_PARAM_POINTER E_PARAM_POINTER
#define SPI_E_PARAM_DEVICE 0x20u
#define SPI_E_SEQ_IN_PROGRESS 0x21u
#define SPI_E_NOT_LOCKED 0x22u
#define SPI_E_LOCK_TIMEOUT 0x23u
#define SPI_E_WRONG_OWNER 0x24u

/** Logical device on the shared bus; values are defined in Spi_Cfg.h. */
typedef uint8 Spi_DeviceType;

/** Clock polarity and phase, in the conventional numbering. */
typedef enum
{
    SPI_MODE_0 = 0, /**< CPOL = 0, CPHA = 0 -- MCP2515 and SD card. */
    SPI_MODE_1 = 1, /**< CPOL = 0, CPHA = 1.                        */
    SPI_MODE_2 = 2, /**< CPOL = 1, CPHA = 0.                        */
    SPI_MODE_3 = 3  /**< CPOL = 1, CPHA = 1.                        */
} Spi_ModeType;

/** Bus usage counters, published as diagnostic data. */
typedef struct
{
    uint32 transferCount;      /**< Completed ::Spi_Transfer calls.                */
    uint32 bytesTransferred;   /**< Total bytes clocked in either direction.       */
    uint32 lockContentionCount;/**< Times a lock had to wait for the other device. */
    uint32 lockTimeoutCount;   /**< Times a lock gave up -- always a defect.       */
} Spi_StatisticsType;

/**
 * @brief Initialise the bus and put every chip select in its inactive (high) state.
 *
 * Chip selects are driven high *before* the peripheral is configured. A chip select
 * that floats low while the bus is brought up makes the attached device interpret
 * initialisation clocking as a command.
 */
CHECK_RETURN Std_ReturnType Spi_Init(void);

/**
 * @brief Acquire the bus for @p device and apply its clock rate and mode.
 *
 * @param device    Device requesting the bus.
 * @param timeoutMs How long to wait for the other device to finish.
 * @return E_OK on acquisition; E_TIMEOUT if the bus stayed busy, which is reported to
 *         Det because it means a holder failed to unlock.
 *
 * @note Does *not* assert chip select -- ::Spi_Transfer does that per transaction, so
 *       that a lock can span several transactions with gaps between them, which is what
 *       the MCP2515 read-modify-write sequences need.
 */
CHECK_RETURN Std_ReturnType Spi_Lock(Spi_DeviceType device, uint32 timeoutMs);

/**
 * @brief Release the bus.
 * @return E_OK on success; E_NOT_OK if @p device is not the current owner, which is a
 *         caller defect and is reported as ::SPI_E_WRONG_OWNER.
 */
CHECK_RETURN Std_ReturnType Spi_Unlock(Spi_DeviceType device);

/** Device currently holding the bus, or ::SPI_DEVICE_NONE. */
Spi_DeviceType Spi_GetOwner(void);

/**
 * @brief Clock @p length bytes to and from the locked device.
 *
 * Full duplex. @p txData may be NULL_PTR to clock out ::SPI_DUMMY_BYTE while reading;
 * @p rxData may be NULL_PTR to discard what is read. Both NULL_PTR is a caller defect.
 * Chip select is asserted for the duration of the call and released on return.
 *
 * @return E_OK on success; ::SPI_E_NOT_LOCKED if the caller does not hold the bus.
 */
CHECK_RETURN Std_ReturnType Spi_Transfer(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                         uint16 length);

/**
 * @brief Clock bytes with chip select held across several calls.
 *
 * For sequences where the device requires chip select to stay low between phases --
 * the MCP2515 SPI command set does. The caller is responsible for the matching
 * ::Spi_ChipSelectAssert and ::Spi_ChipSelectDeassert.
 */
CHECK_RETURN Std_ReturnType Spi_TransferContinuous(Spi_DeviceType device, const uint8 *txData,
                                                   uint8 *rxData, uint16 length);

/** Assert (drive low) @p device's chip select. Requires the bus lock. */
CHECK_RETURN Std_ReturnType Spi_ChipSelectAssert(Spi_DeviceType device);

/** Deassert (drive high) @p device's chip select. Requires the bus lock. */
CHECK_RETURN Std_ReturnType Spi_ChipSelectDeassert(Spi_DeviceType device);

/**
 * @brief Read the bus counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Spi_GetStatistics(Spi_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Spi_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* SPI_H */

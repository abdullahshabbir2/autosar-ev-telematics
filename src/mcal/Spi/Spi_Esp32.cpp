/**
 * @file    Spi_Esp32.cpp
 * @brief   ESP32 platform leaf of the SPI handler/driver: VSPI with explicit bus ownership.
 *
 * @par What owns what
 * Two mutual-exclusion mechanisms are in play and they must not be confused:
 *
 *  - **This module's mutex** decides which logical device (::SPI_DEVICE_CAN or ::SPI_DEVICE_SD) may use the
 *    bus at all. It is held across a whole logical operation -- an MCP2515 read-modify-write, or an SD block
 *    write -- which may be thousands of times longer than a single transfer.
 *  - **The Arduino SPIClass internal lock**, taken by @c beginTransaction, protects one transfer's clock and
 *    mode settings.
 *
 * ::Spi_Lock deliberately takes only the first. If it also opened an @c SPIClass transaction, the ESP32 @c SD
 * library -- which calls @c beginTransaction itself on every block -- would deadlock against a caller holding
 * the bus on its behalf. So the settings are applied per transfer instead, and FsAbs can hold this module's
 * lock across an SD operation while the card driver continues to manage its own transactions underneath.
 * That layering is what makes it possible to arbitrate a bus shared with a third-party driver at all.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "services/Det/Det.h"
#include "Ecu_PinMap.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Spi/Spi.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/** Per-device bus parameters, indexed by ::Spi_DeviceType. */
typedef struct
{
    uint8 csPin;       /**< Chip select, active low.    */
    uint32 clockHz;    /**< Clock rate for this device. */
    Spi_ModeType mode; /**< Clock polarity and phase.   */
} Spi_DeviceConfigType;

static const Spi_DeviceConfigType Spi_DeviceConfig[SPI_DEVICE_COUNT] = {
    /* SPI_DEVICE_CAN */ {SPI_CS_PIN_CAN, SPI_CLOCK_CAN_HZ, SPI_MODE_0},
    /* SPI_DEVICE_SD  */ {SPI_CS_PIN_SD, SPI_CLOCK_SD_HZ, SPI_MODE_0},
};

static SemaphoreHandle_t Spi_BusMutex = NULL;
static volatile Spi_DeviceType Spi_Owner = SPI_DEVICE_NONE;
static Spi_StatisticsType Spi_Stats;
static boolean Spi_Initialised = FALSE;

/*==================================================================================================
 *  Local helpers
 *================================================================================================*/

/** TRUE if @p device names a configured device. */
static boolean Spi_DeviceIsValid(Spi_DeviceType device)
{
    return (device < (Spi_DeviceType)SPI_DEVICE_COUNT) ? TRUE : FALSE;
}

/**
 * @brief Check that @p device is valid, the module is initialised, and @p device holds the bus.
 *
 * Every transfer entry point starts here. Folding the three checks into one place is what stops the
 * "transaction outside a lock" case from being possible to omit in one of them.
 */
static Std_ReturnType Spi_CheckOwner(Spi_DeviceType device, uint8 apiId)
{
    if (Spi_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, apiId, SPI_E_UNINIT);
        return E_NOT_OK;
    }
    if (Spi_DeviceIsValid(device) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, apiId, SPI_E_PARAM_DEVICE);
        return E_NOT_OK;
    }
    if (Spi_Owner != device)
    {
        /* Either nothing holds the bus, or the other device does. Both are caller defects, and both would
         * otherwise corrupt whatever the real owner was doing -- which is exactly the v1 failure this module
         * exists to make impossible. */
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, apiId,
                              (Spi_Owner == SPI_DEVICE_NONE) ? SPI_E_NOT_LOCKED : SPI_E_WRONG_OWNER);
        return E_NOT_OK;
    }

    return E_OK;
}

/** Apply @p device's clock rate and mode for the duration of one transfer. */
static void Spi_BeginTransaction(Spi_DeviceType device)
{
    const Spi_DeviceConfigType *const config = &Spi_DeviceConfig[device];
    uint8_t dataMode;

    switch (config->mode)
    {
    case SPI_MODE_1:
        dataMode = SPI_MODE1;
        break;
    case SPI_MODE_2:
        dataMode = SPI_MODE2;
        break;
    case SPI_MODE_3:
        dataMode = SPI_MODE3;
        break;
    case SPI_MODE_0:
    default:
        dataMode = SPI_MODE0;
        break;
    }

#if (SPI_BIT_ORDER_MSB_FIRST == STD_ON)
    SPI.beginTransaction(SPISettings(config->clockHz, MSBFIRST, dataMode));
#else
    SPI.beginTransaction(SPISettings(config->clockHz, LSBFIRST, dataMode));
#endif
}

/*==================================================================================================
 *  API
 *================================================================================================*/

extern "C" Std_ReturnType Spi_Init(void)
{
    uint8 i;

    if (Spi_BusMutex == NULL)
    {
        Spi_BusMutex = xSemaphoreCreateMutex();
        if (Spi_BusMutex == NULL)
        {
            /* Out of heap this early means the ECU cannot arbitrate the bus at all. Refusing is correct:
             * running on without arbitration is precisely the v1 behaviour, and it damages card data. */
            (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_INIT, E_NO_SPACE);
            return E_NOT_OK;
        }
    }

    /* Chip selects inactive before the peripheral is brought up. Port_Init has already done this; repeating
     * it costs two register writes and removes a dependency on initialisation order that this module has no
     * way to verify. */
    for (i = 0u; i < (uint8)SPI_DEVICE_COUNT; i++)
    {
        digitalWrite(Spi_DeviceConfig[i].csPin, HIGH);
        pinMode(Spi_DeviceConfig[i].csPin, OUTPUT);
        digitalWrite(Spi_DeviceConfig[i].csPin, HIGH);
    }

    /* -1 for the chip select, because this module drives every one of them itself: SPIClass can only track
     * one, and this bus has two. */
    SPI.begin((int8_t)PIN_SPI_SCK, (int8_t)PIN_SPI_MISO, (int8_t)PIN_SPI_MOSI, -1);

    Spi_Owner = SPI_DEVICE_NONE;
    (void)memset(&Spi_Stats, 0, sizeof(Spi_Stats));
    Spi_Initialised = TRUE;

    return E_OK;
}

extern "C" Std_ReturnType Spi_Lock(Spi_DeviceType device, uint32 timeoutMs)
{
    if (Spi_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_LOCK, SPI_E_UNINIT);
        return E_NOT_OK;
    }
    if (Spi_DeviceIsValid(device) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_LOCK, SPI_E_PARAM_DEVICE);
        return E_NOT_OK;
    }

    /* Contention is counted before the blocking take, not from how long the take waited, so the counter
     * reflects how often the two devices actually competed. A bus with a high contention count and no
     * timeouts is healthy; one with timeouts has a holder that failed to unlock. */
    if (Spi_Owner != SPI_DEVICE_NONE)
    {
        Spi_Stats.lockContentionCount++;
    }

    if (xSemaphoreTake(Spi_BusMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE)
    {
        Spi_Stats.lockTimeoutCount++;
        /* A runtime error, not a development one: the code is correct but a holder is stuck. Dem raises the
         * corresponding event, because a bus that cannot be acquired costs both CAN and storage. */
        (void)Det_ReportRuntimeError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_LOCK, SPI_E_LOCK_TIMEOUT);
        return E_TIMEOUT;
    }

    Spi_Owner = device;
    return E_OK;
}

extern "C" Std_ReturnType Spi_Unlock(Spi_DeviceType device)
{
    const Std_ReturnType check = Spi_CheckOwner(device, SPI_API_ID_UNLOCK);

    if (check != E_OK)
    {
        return check;
    }

    /* Chip select is forced inactive on release even if the caller left it asserted. Leaving it low would
     * route the next owner's traffic into this device, and the cost of one extra write is nothing next to
     * diagnosing that. */
    digitalWrite(Spi_DeviceConfig[device].csPin, HIGH);

    Spi_Owner = SPI_DEVICE_NONE;
    (void)xSemaphoreGive(Spi_BusMutex);

    return E_OK;
}

extern "C" Spi_DeviceType Spi_GetOwner(void)
{
    return Spi_Owner;
}

extern "C" Std_ReturnType Spi_ChipSelectAssert(Spi_DeviceType device)
{
    const Std_ReturnType check = Spi_CheckOwner(device, SPI_API_ID_TRANSFER);

    if (check != E_OK)
    {
        return check;
    }

    digitalWrite(Spi_DeviceConfig[device].csPin, LOW);
    Gpt_DelayUs(SPI_CS_SETUP_US);

    return E_OK;
}

extern "C" Std_ReturnType Spi_ChipSelectDeassert(Spi_DeviceType device)
{
    const Std_ReturnType check = Spi_CheckOwner(device, SPI_API_ID_TRANSFER);

    if (check != E_OK)
    {
        return check;
    }

    Gpt_DelayUs(SPI_CS_SETUP_US);
    digitalWrite(Spi_DeviceConfig[device].csPin, HIGH);

    return E_OK;
}

/** Shared body of ::Spi_Transfer and ::Spi_TransferContinuous. */
static Std_ReturnType Spi_TransferBody(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                       uint16 length)
{
    uint16 i;

    Spi_BeginTransaction(device);

    for (i = 0u; i < length; i++)
    {
        const uint8 out = (txData != NULL_PTR) ? txData[i] : (uint8)SPI_DUMMY_BYTE;
        const uint8 in = (uint8)SPI.transfer(out);

        if (rxData != NULL_PTR)
        {
            rxData[i] = in;
        }
    }

    SPI.endTransaction();

    Spi_Stats.transferCount++;
    Spi_Stats.bytesTransferred += (uint32)length;

    return E_OK;
}

extern "C" Std_ReturnType Spi_Transfer(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                       uint16 length)
{
    Std_ReturnType status = Spi_CheckOwner(device, SPI_API_ID_TRANSFER);

    if (status != E_OK)
    {
        return status;
    }
    if ((txData == NULL_PTR) && (rxData == NULL_PTR))
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_TRANSFER, SPI_E_PARAM_POINTER);
        return E_NOT_OK;
    }
    if (length == 0u)
    {
        return E_OK;
    }

    status = Spi_ChipSelectAssert(device);
    if (status != E_OK)
    {
        return status;
    }

    status = Spi_TransferBody(device, txData, rxData, length);

    /* The deassert result is kept only if the transfer itself succeeded, so that a genuine transfer error is
     * never masked by a chip-select complaint that can only be a consequence of it. */
    {
        const Std_ReturnType release = Spi_ChipSelectDeassert(device);

        if (status == E_OK)
        {
            status = release;
        }
    }

    return status;
}

extern "C" Std_ReturnType Spi_TransferContinuous(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                                 uint16 length)
{
    const Std_ReturnType status = Spi_CheckOwner(device, SPI_API_ID_TRANSFER);

    if (status != E_OK)
    {
        return status;
    }
    if ((txData == NULL_PTR) && (rxData == NULL_PTR))
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_TRANSFER, SPI_E_PARAM_POINTER);
        return E_NOT_OK;
    }
    if (length == 0u)
    {
        return E_OK;
    }

    /* Chip select is the caller's responsibility here -- that is the whole difference from ::Spi_Transfer.
     * The MCP2515 command set requires it to stay low across a command byte, an address byte and a data
     * phase that the driver issues as separate calls. */
    return Spi_TransferBody(device, txData, rxData, length);
}

extern "C" Std_ReturnType Spi_GetStatistics(Spi_StatisticsType *stats)
{
    if (stats == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_SPI, INSTANCE_ID_SINGLE, SPI_API_ID_TRANSFER, SPI_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    *stats = Spi_Stats;
    return E_OK;
}

extern "C" void Spi_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = SPI_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_SPI;
        versioninfo->sw_major_version = SPI_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = SPI_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = SPI_SW_PATCH_VERSION;
    }
}

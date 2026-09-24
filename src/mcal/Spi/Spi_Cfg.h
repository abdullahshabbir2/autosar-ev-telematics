/**
 * @file    Spi_Cfg.h
 * @brief   SPI device configuration for the shared VSPI bus.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef SPI_CFG_H
#define SPI_CFG_H

#include "Ecu_PinMap.h"
#include "base/Std_Types.h"

#define SPI_DEV_ERROR_DETECT STD_ON

/*------------------------------- Devices -------------------------------------*/

#define SPI_DEVICE_NONE ((Spi_DeviceType)0xFFu) /**< Bus is free. */
#define SPI_DEVICE_CAN ((Spi_DeviceType)0u)     /**< MCP2515.     */
#define SPI_DEVICE_SD ((Spi_DeviceType)1u)      /**< microSD card. */

#define SPI_DEVICE_COUNT 2u

/*------------------------------ Clock rates ----------------------------------*/

/**
 * @brief MCP2515 clock rate, in Hz.
 *
 * 10 MHz, the datasheet maximum. The device sits on a short trace next to the MCU on
 * this board; where it is on a flying lead from a breakout module, drop this to 4 MHz --
 * the failure mode is corrupted register reads, which surface as a CAN init failure
 * rather than as anything subtle.
 */
#define SPI_CLOCK_CAN_HZ 10000000uL

/**
 * @brief microSD clock rate, in Hz.
 *
 * 20 MHz rather than the 25 MHz the SD specification allows in SPI mode. The bus is
 * shared with the MCP2515 and the wiring is sized for the slower device, so the extra
 * 5 MHz would buy about 8 % on a transfer that is dominated by the card's internal
 * programming time anyway.
 */
#define SPI_CLOCK_SD_HZ 20000000uL

/*------------------------------ Bus locking ----------------------------------*/

/**
 * @brief Default bound on ::Spi_Lock, in milliseconds.
 *
 * 2500 ms, set by the worst case it must tolerate: an SD card doing internal wear
 * levelling can hold the bus for up to about 2 s, and a lock timeout shorter than that
 * would turn a normal slow write into a spurious CAN fault.
 */
#define SPI_LOCK_TIMEOUT_MS 2500u

/** Byte clocked out when a caller only wants to read. */
#define SPI_DUMMY_BYTE 0xFFu

/** Bit order. Both devices on this bus are MSB-first. */
#define SPI_BIT_ORDER_MSB_FIRST STD_ON

/*--------------------------- Chip select pins --------------------------------*/

#define SPI_CS_PIN_CAN PIN_CAN_CS
#define SPI_CS_PIN_SD PIN_SD_CS

/**
 * @brief Setup and hold time around chip select, in microseconds.
 *
 * 1 us. The MCP2515 needs 50 ns of chip-select setup before the first clock edge; 1 us
 * is the smallest interval the ESP32 can produce reliably from software and costs
 * nothing at these transaction rates.
 */
#define SPI_CS_SETUP_US 1u

#endif /* SPI_CFG_H */

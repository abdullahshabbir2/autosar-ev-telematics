/**
 * @file    Uart_Cfg.h
 * @brief   UART instance configuration.
 *
 * The ESP32 has three UART peripherals and this design needs three serial links plus a
 * console -- one more than the hardware provides. Ecu_PinMap.h records the resolution
 * in full; the allocation it implies is:
 *
 *   | Instance          | Peripheral        | Link                          |
 *   |-------------------|-------------------|-------------------------------|
 *   | (console)         | UART0             | Debug console and flashing     |
 *   | UART_INSTANCE_RS485 | UART1           | Battery bus, 4800 8E1          |
 *   | UART_INSTANCE_GSM | UART2             | SIM800L modem, 9600 8N1        |
 *   | UART_INSTANCE_GNSS| software UART, RX only | NEO-6M NMEA, 9600 8N1     |
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef UART_CFG_H
#define UART_CFG_H

#include "Ecu_PinMap.h"
#include "Std_Types.h"

#define UART_DEV_ERROR_DETECT STD_ON

/*------------------------------- Instances -----------------------------------*/

#define UART_INSTANCE_RS485 ((Uart_InstanceType)0u)
#define UART_INSTANCE_GNSS ((Uart_InstanceType)1u)
#define UART_INSTANCE_GSM ((Uart_InstanceType)2u)

/** Number of instances the driver manages. Must match the stub's model. */
#define UART_INSTANCE_COUNT 3u

/*------------------------------- Baud rates ----------------------------------*/

/**
 * @brief Battery bus baud rate.
 *
 * 4800 8E1, fixed by the pack's BMS. A 15-byte request therefore occupies 15 * 10 /
 * 4800 = 31 ms on the wire, and an 81-byte cell-parameter response 169 ms. Those two
 * numbers set the RS485 timeouts; see ::RS485IF_RESPONSE_TIMEOUT_MS.
 */
#define UART_BAUD_RS485 4800uL

/** NEO-6M default NMEA baud rate. */
#define UART_BAUD_GNSS 9600uL

/**
 * @brief SIM800L baud rate.
 *
 * Fixed at 9600 rather than left on the modem's autobaud. Autobaud resynchronises on
 * every "AT" and fails unpredictably once the modem is busy with a data call, which
 * makes a stalled modem indistinguishable from a wiring fault.
 */
#define UART_BAUD_GSM 9600uL

/*----------------------------- Buffer sizes ----------------------------------*/

/**
 * @brief Receive buffer for the battery bus, in bytes.
 *
 * 256, which is three times the longest response (81 bytes). Sized so that a response
 * arriving while the previous one is still being parsed cannot overrun, without
 * reserving memory for traffic the half-duplex protocol makes impossible.
 */
#define UART_RX_BUFFER_RS485 256u

/**
 * @brief Receive buffer for GNSS, in bytes.
 *
 * 512. A full NMEA burst at 1 Hz is about 500 bytes across six sentences, and the
 * consumer only runs every 3 s, so the buffer has to hold roughly one burst while
 * older ones are discarded.
 */
#define UART_RX_BUFFER_GNSS 512u

/**
 * @brief Receive buffer for the modem, in bytes.
 *
 * 1024, because an unsolicited result code can arrive interleaved with the body of an
 * HTTP response and the AT parser needs both intact to tell them apart.
 */
#define UART_RX_BUFFER_GSM 1024u

/*----------------------------- Timeouts --------------------------------------*/

/**
 * @brief Default bound on ::Uart_DrainTx, in milliseconds.
 *
 * 100 ms is more than 3x the 31 ms a maximum-length request takes at 4800 baud, so it
 * only ever expires on a genuinely stuck peripheral.
 */
#define UART_DRAIN_TIMEOUT_MS 100u

#endif /* UART_CFG_H */

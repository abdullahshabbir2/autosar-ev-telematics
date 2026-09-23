/**
 * @file    Uart.h
 * @brief   AUTOSAR-style asynchronous serial driver -- ESP32 UART adaptation.
 *
 * AUTOSAR has no generic UART driver (asynchronous serial reaches the stack through
 * LinIf or a complex device driver), so this module follows the house style of the
 * MCAL rather than a specific SWS: instance-indexed, ::Std_ReturnType throughout,
 * pre-compile configured, no dynamic allocation, and no Arduino types in the
 * interface.
 *
 * @par The flush trap this interface closes
 * Arduino's @c Serial.flush() waits for the *transmit* buffer to drain. It does not
 * discard received bytes. v1 called it in three places intending to purge stale
 * receive data before a request -- so every RS485 exchange began by parsing whatever
 * the previous exchange had left in the receive FIFO, and a late reply from the
 * previous pack could be decoded as the current pack's reply. The two operations are
 * separated here and neither name can be mistaken for the other:
 *
 *  - ::Uart_DiscardRx  -- throw away buffered received bytes.
 *  - ::Uart_DrainTx    -- block until the last bit has left the shift register.
 *
 * ::Uart_DrainTx is also the reason the RS485 transceiver turnaround is correct: the
 * driver-enable line may only drop once the shift register is empty, and Arduino's
 * @c write() returns as soon as the bytes are buffered, not sent. v1 dropped DE
 * immediately after @c write(), truncating the last character of every request on the
 * wire.
 *
 * @req SWREQ-COM-0001, SWREQ-COM-0002, SWREQ-COM-0003
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef UART_H
#define UART_H

#include "Autosar_ModuleIds.h"
#include "Std_Types.h"
#include "Uart_Cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_VENDOR_ID 0xFFFEu
#define UART_AR_RELEASE_MAJOR_VERSION 4u
#define UART_AR_RELEASE_MINOR_VERSION 4u
#define UART_SW_MAJOR_VERSION 2u
#define UART_SW_MINOR_VERSION 0u
#define UART_SW_PATCH_VERSION 0u

#define UART_API_ID_INIT 0x00u
#define UART_API_ID_OPEN 0x01u
#define UART_API_ID_CLOSE 0x02u
#define UART_API_ID_WRITE 0x03u
#define UART_API_ID_READ 0x04u
#define UART_API_ID_DISCARD_RX 0x05u
#define UART_API_ID_DRAIN_TX 0x06u
#define UART_API_ID_BYTES_AVAILABLE 0x07u

#define UART_E_UNINIT E_UNINIT
#define UART_E_PARAM_INSTANCE 0x20u
#define UART_E_PARAM_POINTER E_PARAM_POINTER
#define UART_E_PARAM_CONFIG E_PARAM_CONFIG
#define UART_E_NOT_OPEN 0x21u
#define UART_E_ALREADY_OPEN 0x22u
#define UART_E_TX_OVERFLOW 0x23u
#define UART_E_RX_OVERRUN 0x24u

/** Logical UART instance; values are defined in Uart_Cfg.h. */
typedef uint8 Uart_InstanceType;

/** Character framing: data bits, parity, stop bits. */
typedef enum
{
    UART_FRAME_8N1 = 0, /**< 8 data, no parity, 1 stop -- GNSS, GSM.      */
    UART_FRAME_8E1 = 1, /**< 8 data, even parity, 1 stop -- battery bus.  */
    UART_FRAME_8O1 = 2, /**< 8 data, odd parity, 1 stop.                  */
    UART_FRAME_8N2 = 3  /**< 8 data, no parity, 2 stop.                   */
} Uart_FrameFormatType;

/** Per-instance open parameters. */
typedef struct
{
    uint32 baudRate;                /**< Bits per second.                            */
    Uart_FrameFormatType frame;     /**< Character framing.                          */
    uint8 rxPin;                    /**< GPIO for RX, or ::PIN_NOT_CONNECTED.        */
    uint8 txPin;                    /**< GPIO for TX, or ::PIN_NOT_CONNECTED.        */
    uint16 rxBufferSize;            /**< Driver receive buffer, bytes.               */
    boolean invertRx;               /**< TRUE if the line is logically inverted.     */
} Uart_ConfigType;

/** Per-instance error and throughput counters, published as diagnostic data. */
typedef struct
{
    uint32 bytesTransmitted; /**< Total bytes handed to the hardware.        */
    uint32 bytesReceived;    /**< Total bytes delivered to callers.          */
    uint32 rxOverrunCount;   /**< Times the receive buffer overflowed.       */
    uint32 frameErrorCount;  /**< Framing or parity errors detected.         */
    uint32 rxDiscardedBytes; /**< Bytes thrown away by ::Uart_DiscardRx.     */
} Uart_StatisticsType;

/**
 * @brief Prepare the module. Does not open any instance.
 * @return E_OK always.
 */
CHECK_RETURN Std_ReturnType Uart_Init(void);

/**
 * @brief Open @p instance with @p config.
 *
 * @return E_OK on success; E_NOT_OK for an unknown instance, a NULL @p config, an
 *         unsupported baud rate, or an instance that is already open.
 */
CHECK_RETURN Std_ReturnType Uart_Open(Uart_InstanceType instance, const Uart_ConfigType *config);

/** Close @p instance and release its pins. */
CHECK_RETURN Std_ReturnType Uart_Close(Uart_InstanceType instance);

/** TRUE if @p instance is currently open. */
boolean Uart_IsOpen(Uart_InstanceType instance);

/**
 * @brief Queue @p length bytes for transmission on @p instance.
 *
 * Returns as soon as the bytes are accepted by the driver, *not* when they have been
 * sent. Callers that must know the line is idle -- anything driving a half-duplex
 * transceiver -- must follow this with ::Uart_DrainTx.
 *
 * @return E_OK if all bytes were accepted; E_NO_SPACE if the transmit buffer could
 *         not take them all (nothing is queued in that case, so a partial frame is
 *         never emitted); E_NOT_OK on a parameter or state error.
 */
CHECK_RETURN Std_ReturnType Uart_Write(Uart_InstanceType instance, const uint8 *data,
                                       uint16 length);

/**
 * @brief Read up to @p maxLength bytes from @p instance without blocking.
 *
 * @param[in]  instance   Instance to read.
 * @param[out] buffer     Destination.
 * @param[in]  maxLength  Capacity of @p buffer.
 * @param[out] actualLength Bytes actually written, possibly 0.
 * @return E_OK even when zero bytes were available -- "nothing yet" is a normal
 *         outcome for a polled bus, not an error. E_NOT_OK only for a parameter or
 *         state error.
 */
CHECK_RETURN Std_ReturnType Uart_Read(Uart_InstanceType instance, uint8 *buffer, uint16 maxLength,
                                      uint16 *actualLength);

/**
 * @brief Read exactly @p length bytes, or give up after @p timeoutMs.
 *
 * The form every request/response protocol on this ECU wants. Returns as soon as the
 * last byte arrives rather than always waiting out the timeout -- v1's equivalent loop
 * had no early exit and burned a full second on every battery frame, three times per
 * pack, four packs, which is 12 s of the 3 s acquisition budget spent waiting for data
 * that had already arrived.
 *
 * @param[out] actualLength Bytes received, valid on both E_OK and E_TIMEOUT so that a
 *                          caller can log how much of a truncated frame arrived.
 * @return E_OK if @p length bytes arrived; E_TIMEOUT if the deadline passed first;
 *         E_NOT_OK on a parameter or state error.
 */
CHECK_RETURN Std_ReturnType Uart_ReadExact(Uart_InstanceType instance, uint8 *buffer, uint16 length,
                                           uint32 timeoutMs, uint16 *actualLength);

/** Bytes currently buffered and readable on @p instance. */
uint16 Uart_BytesAvailable(Uart_InstanceType instance);

/**
 * @brief Discard every buffered received byte on @p instance.
 *
 * This is the operation v1 intended when it called @c flush(). Use it before issuing a
 * request so that the response parser cannot consume a stale reply.
 *
 * @return Number of bytes discarded, which is worth logging: a non-zero count before
 *         a request means the previous exchange left the bus out of step.
 */
uint16 Uart_DiscardRx(Uart_InstanceType instance);

/**
 * @brief Block until every queued byte has physically left @p instance.
 *
 * Bounded by @p timeoutMs so a stuck peripheral cannot hang the caller forever.
 *
 * @return E_OK once the shift register is empty; E_TIMEOUT otherwise.
 */
CHECK_RETURN Std_ReturnType Uart_DrainTx(Uart_InstanceType instance, uint32 timeoutMs);

/**
 * @brief Read the per-instance counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR or the instance is unknown.
 */
CHECK_RETURN Std_ReturnType Uart_GetStatistics(Uart_InstanceType instance,
                                               Uart_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Uart_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* UART_H */

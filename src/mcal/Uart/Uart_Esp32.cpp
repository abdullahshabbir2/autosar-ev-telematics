/**
 * @file    Uart_Esp32.cpp
 * @brief   ESP32 platform leaf of the serial driver: three links on two-and-a-bit peripherals.
 *
 * @par Three links, three peripherals, and a console
 * The ESP32 has three UARTs and this design needs four serial endpoints. UART0 is not negotiable -- it is the
 * flashing interface and the debug console -- so one of the three links has to be synthesised. GNSS is the
 * obvious candidate and the only one that works: it is the only link that never transmits, so a receive-only
 * software UART costs nothing in capability. At 9600 baud a bit is 104 us, which the interrupt-driven soft
 * UART handles comfortably; the same trick at the modem's traffic volumes would not be safe.
 *
 * v1 attempted the same allocation and got it wrong in two ways at once: it constructed the GNSS port as
 * @c HardwareSerial(1) without pin arguments -- which lands on GPIO9/GPIO10, wired to the internal SPI flash
 * on every WROOM-32 module -- and then also handed UART2 to both the RS485 bus and the modem. Ecu_PinMap.h
 * records both conflicts and their resolution.
 *
 * @par Frame format and the ESP32 SERIAL_ macros
 * The 8E1 framing the battery bus needs is expressed through @c SERIAL_8E1. It is worth being explicit that
 * this is real hardware parity, not a software approximation: a parity error sets a status bit that the
 * peripheral reports, and Rs485If's CRC check is therefore a second independent line of defence rather than
 * the only one.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include <string.h>

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Uart/Uart.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/**
 * @brief The software serial port used for GNSS.
 *
 * File-scope rather than heap allocated, so the memory is accounted for at link time and a late open cannot
 * fail for want of heap. Receive-only: the transmit pin is passed as -1.
 */
static SoftwareSerial Uart_GnssPort;

/** Per-instance runtime state. */
typedef struct
{
    Stream *stream;            /**< The port, once open; NULL otherwise.      */
    boolean isOpen;            /**< Whether ::Uart_Open has succeeded.        */
    boolean isSoftware;        /**< TRUE for the software port.               */
    uint8 hardwarePortNumber;  /**< UART peripheral index, hardware only.     */
    Uart_StatisticsType stats; /**< Counters for this instance.               */
} Uart_InstanceStateType;

static Uart_InstanceStateType Uart_State[UART_INSTANCE_COUNT];
static boolean Uart_Initialised = FALSE;

/*==================================================================================================
 *  Local helpers
 *================================================================================================*/

/** TRUE if @p instance is a configured instance index. */
static boolean Uart_InstanceIsValid(Uart_InstanceType instance)
{
    return (instance < (Uart_InstanceType)UART_INSTANCE_COUNT) ? TRUE : FALSE;
}

/**
 * @brief Validate @p instance and require it to be open.
 * @return Pointer to the state, or NULL_PTR after reporting the reason to Det.
 */
static Uart_InstanceStateType *Uart_GetOpenState(Uart_InstanceType instance, uint8 apiId)
{
    if (Uart_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, INSTANCE_ID_SINGLE, apiId, UART_E_UNINIT);
        return NULL_PTR;
    }
    if (Uart_InstanceIsValid(instance) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, apiId, UART_E_PARAM_INSTANCE);
        return NULL_PTR;
    }
    if (Uart_State[instance].isOpen == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, apiId, UART_E_NOT_OPEN);
        return NULL_PTR;
    }

    return &Uart_State[instance];
}

/** Translate ::Uart_FrameFormatType to the ESP32 serial configuration word. */
static uint32 Uart_FrameToConfig(Uart_FrameFormatType frame, boolean *supported)
{
    uint32 config;

    *supported = TRUE;

    switch (frame)
    {
    case UART_FRAME_8N1:
        config = SERIAL_8N1;
        break;
    case UART_FRAME_8E1:
        config = SERIAL_8E1;
        break;
    case UART_FRAME_8O1:
        config = SERIAL_8O1;
        break;
    case UART_FRAME_8N2:
        config = SERIAL_8N2;
        break;
    default:
        config = SERIAL_8N1;
        *supported = FALSE;
        break;
    }

    return config;
}

/**
 * @brief The hardware peripheral each instance uses.
 *
 * A function rather than a table because ::UART_INSTANCE_GNSS has no peripheral, and a table with a hole in
 * it invites an index that reads the hole.
 */
static HardwareSerial *Uart_HardwarePort(Uart_InstanceType instance)
{
    HardwareSerial *port;

    if (instance == UART_INSTANCE_RS485)
    {
        port = &Serial1;
    }
    else if (instance == UART_INSTANCE_GSM)
    {
        port = &Serial2;
    }
    else
    {
        port = NULL_PTR;
    }

    return port;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

extern "C" Std_ReturnType Uart_Init(void)
{
    (void)memset(Uart_State, 0, sizeof(Uart_State));

    Uart_State[UART_INSTANCE_RS485].hardwarePortNumber = 1u;
    Uart_State[UART_INSTANCE_GSM].hardwarePortNumber = 2u;
    Uart_State[UART_INSTANCE_GNSS].isSoftware = TRUE;

    Uart_Initialised = TRUE;
    return E_OK;
}

extern "C" Std_ReturnType Uart_Open(Uart_InstanceType instance, const Uart_ConfigType *config)
{
    Uart_InstanceStateType *state;
    boolean frameSupported = FALSE;
    uint32 serialConfig;

    if (Uart_Initialised == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, INSTANCE_ID_SINGLE, UART_API_ID_OPEN, UART_E_UNINIT);
        return E_NOT_OK;
    }
    if (Uart_InstanceIsValid(instance) == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_INSTANCE);
        return E_NOT_OK;
    }
    if (config == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    state = &Uart_State[instance];

    if (state->isOpen != FALSE)
    {
        /* Reopening would silently reconfigure a port another module is mid-exchange on. Reported rather than
         * tolerated, because the second caller has misunderstood who owns the link. */
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_ALREADY_OPEN);
        return E_NOT_OK;
    }

    serialConfig = Uart_FrameToConfig(config->frame, &frameSupported);
    if (frameSupported == FALSE)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_CONFIG);
        return E_NOT_OK;
    }
    if ((config->baudRate == 0uL) || (config->baudRate > 921600uL))
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_CONFIG);
        return E_NOT_OK;
    }

    if (state->isSoftware != FALSE)
    {
        /* Receive-only: -1 for the transmit pin. The soft UART allocates its own receive buffer, so
         * rxBufferSize is honoured here rather than through a separate call.
         *
         * A software UART that is asked to transmit would have to disable interrupts for the whole character,
         * which on this part means dropping RS485 bytes. Refusing the configuration outright is the only
         * honest answer -- see the module comment on why GNSS is the one link that can tolerate this. */
        if (config->txPin != PIN_NOT_CONNECTED)
        {
            (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_CONFIG);
            return E_NOT_OK;
        }

        Uart_GnssPort.begin(config->baudRate, SWSERIAL_8N1, (int8_t)config->rxPin, -1,
                            (config->invertRx != FALSE));
        if (!Uart_GnssPort)
        {
            (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_CONFIG);
            return E_NOT_OK;
        }

        Uart_GnssPort.enableIntTx(false);
        state->stream = &Uart_GnssPort;
    }
    else
    {
        HardwareSerial *const port = Uart_HardwarePort(instance);

        if (port == NULL_PTR)
        {
            (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_OPEN, UART_E_PARAM_INSTANCE);
            return E_NOT_OK;
        }

        /* setRxBufferSize must precede begin(): the driver allocates the ring buffer during begin() and
         * ignores a later change. v1 called it afterwards on the modem port, so the modem ran on the 256-byte
         * default and lost the tail of every HTTP response longer than that. */
        (void)port->setRxBufferSize(config->rxBufferSize);
        port->begin(config->baudRate, serialConfig, (int8_t)config->rxPin, (int8_t)config->txPin,
                    (config->invertRx != FALSE));
        state->stream = port;
    }

    state->isOpen = TRUE;
    return E_OK;
}

extern "C" Std_ReturnType Uart_Close(Uart_InstanceType instance)
{
    Uart_InstanceStateType *const state = Uart_GetOpenState(instance, UART_API_ID_CLOSE);

    if (state == NULL_PTR)
    {
        return E_NOT_OK;
    }

    /* Drain before closing. Closing a port with bytes still in the transmit FIFO truncates them on the wire,
     * and for the modem that means a half-written AT command it will answer at an arbitrary later moment.
     * Bounded, because a stuck peripheral must not be able to prevent shutdown -- which is why the result is
     * deliberately discarded here rather than propagated. */
    STD_DISCARD(Uart_DrainTx(instance, UART_DRAIN_TIMEOUT_MS));

    if (state->isSoftware != FALSE)
    {
        Uart_GnssPort.end();
    }
    else
    {
        HardwareSerial *const port = Uart_HardwarePort(instance);

        if (port != NULL_PTR)
        {
            port->end();
        }
    }

    state->stream = NULL_PTR;
    state->isOpen = FALSE;

    return E_OK;
}

extern "C" boolean Uart_IsOpen(Uart_InstanceType instance)
{
    if ((Uart_Initialised == FALSE) || (Uart_InstanceIsValid(instance) == FALSE))
    {
        return FALSE;
    }

    return Uart_State[instance].isOpen;
}

extern "C" Std_ReturnType Uart_Write(Uart_InstanceType instance, const uint8 *data, uint16 length)
{
    Uart_InstanceStateType *const state = Uart_GetOpenState(instance, UART_API_ID_WRITE);
    size_t written;

    if (state == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if ((data == NULL_PTR) && (length != 0u))
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_WRITE, UART_E_PARAM_POINTER);
        return E_NOT_OK;
    }
    if (length == 0u)
    {
        return E_OK;
    }

    /* availableForWrite() is checked before writing a single byte, because the contract is all-or-nothing:
     * a partially written request is a frame the peer will reject after waiting out its own timeout, whereas
     * a refused write is something the caller can retry cleanly. Arduino's write() would otherwise block
     * until space appeared, turning a full buffer into an unbounded stall on the acquisition task. */
    if ((uint16)state->stream->availableForWrite() < length)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_UART, instance, UART_API_ID_WRITE, UART_E_TX_OVERFLOW);
        return E_NO_SPACE;
    }

    written = state->stream->write(data, (size_t)length);
    state->stats.bytesTransmitted += (uint32)written;

    if (written != (size_t)length)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_UART, instance, UART_API_ID_WRITE, UART_E_TX_OVERFLOW);
        return E_NO_SPACE;
    }

    return E_OK;
}

extern "C" Std_ReturnType Uart_Read(Uart_InstanceType instance, uint8 *buffer, uint16 maxLength,
                                    uint16 *actualLength)
{
    Uart_InstanceStateType *const state = Uart_GetOpenState(instance, UART_API_ID_READ);
    uint16 count = 0u;

    if (state == NULL_PTR)
    {
        return E_NOT_OK;
    }
    if ((buffer == NULL_PTR) || (actualLength == NULL_PTR))
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_READ, UART_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    while (count < maxLength)
    {
        const int byteRead = state->stream->read();

        if (byteRead < 0)
        {
            break;
        }

        buffer[count] = (uint8)byteRead;
        count++;
    }

    state->stats.bytesReceived += (uint32)count;
    *actualLength = count;

    /* Zero bytes is E_OK, not an error. On a polled half-duplex bus "nothing has arrived yet" is the normal
     * state for most of every exchange, and returning an error for it would force every caller to distinguish
     * a real fault from an empty poll -- which is where v1's response parser went wrong. */
    return E_OK;
}

extern "C" uint16 Uart_BytesAvailable(Uart_InstanceType instance)
{
    if ((Uart_Initialised == FALSE) || (Uart_InstanceIsValid(instance) == FALSE)
        || (Uart_State[instance].isOpen == FALSE))
    {
        return 0u;
    }

    const int available = Uart_State[instance].stream->available();

    return (available > 0) ? (uint16)available : 0u;
}

extern "C" uint16 Uart_DiscardRx(Uart_InstanceType instance)
{
    Uart_InstanceStateType *const state = Uart_GetOpenState(instance, UART_API_ID_DISCARD_RX);
    uint16 discarded = 0u;

    if (state == NULL_PTR)
    {
        return 0u;
    }

    /* This is the operation v1 meant when it called flush(). The bound is the count buffered at entry, not
     * "until available() reads zero": on a live GNSS link bytes keep arriving at 9600 baud, so draining to
     * empty would never terminate. Discarding exactly what was already stale is also the correct semantics --
     * a byte that arrives during the drain belongs to the exchange about to begin. */
    {
        const uint16 pending = Uart_BytesAvailable(instance);

        while ((discarded < pending) && (state->stream->read() >= 0))
        {
            discarded++;
        }
    }

    state->stats.rxDiscardedBytes += (uint32)discarded;

    return discarded;
}

extern "C" Std_ReturnType Uart_DrainTx(Uart_InstanceType instance, uint32 timeoutMs)
{
    Uart_InstanceStateType *const state = Uart_GetOpenState(instance, UART_API_ID_DRAIN_TX);

    if (state == NULL_PTR)
    {
        return E_NOT_OK;
    }

    /* The software port never transmits, so there is nothing to drain and reporting success is correct rather
     * than a shortcut. */
    if (state->isSoftware != FALSE)
    {
        return E_OK;
    }

    {
        HardwareSerial *const port = Uart_HardwarePort(instance);
        const Gpt_TimestampType start = Gpt_GetMonotonicMs();

        if (port == NULL_PTR)
        {
            return E_NOT_OK;
        }

        /* Two conditions, both required. availableForWrite() reaching capacity says the ring buffer is empty;
         * it does not say the shift register is. flush() waits for the shift register. Only after both is the
         * line genuinely idle, which is the precondition for dropping the RS485 driver enable -- v1 dropped DE
         * straight after write() and truncated the last character of every request it ever sent. */
        while ((port->availableForWrite() <= 0) && (Gpt_HasElapsed(start, timeoutMs) == FALSE))
        {
            Gpt_DelayMs(1u);
        }

        if (Gpt_HasElapsed(start, timeoutMs) != FALSE)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_UART, instance, UART_API_ID_DRAIN_TX, E_TIMEOUT);
            return E_TIMEOUT;
        }

        port->flush();
    }

    return E_OK;
}

extern "C" Std_ReturnType Uart_GetStatistics(Uart_InstanceType instance, Uart_StatisticsType *stats)
{
    if ((Uart_Initialised == FALSE) || (Uart_InstanceIsValid(instance) == FALSE))
    {
        return E_NOT_OK;
    }
    if (stats == NULL_PTR)
    {
        (void)Det_ReportError(MODULE_ID_UART, instance, UART_API_ID_BYTES_AVAILABLE, UART_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    *stats = Uart_State[instance].stats;
    return E_OK;
}

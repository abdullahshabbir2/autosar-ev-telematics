/**
 * @file    Rs485If.c
 * @brief   RS485 battery bus transport and protocol handler.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "ecuabs/Rs485If/Rs485If.h"

#include "services/Crc/Crc.h"
#include "services/Det/Det.h"
#include "mcal/Dio/Dio.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Uart/Uart.h"

/*==================================================================================================
 *  Frame field offsets
 *================================================================================================*/

#define RS485IF_OFF_START 0u    /* 0xFF                       */
#define RS485IF_OFF_DIR 1u      /* 0x00 request, 0x55 reply   */
#define RS485IF_OFF_LEN_HI 2u   /* declared total length, MSB */
#define RS485IF_OFF_LEN_LO 3u
#define RS485IF_OFF_TYPE 4u     /* 0x01 addressed, 0x03 broadcast */
#define RS485IF_OFF_SERIAL 5u   /* 4 bytes, most significant first */
#define RS485IF_OFF_CMD 9u      /* command, then 3 reserved bytes  */
#define RS485IF_OFF_PAYLOAD_LEN_HI 11u /* only in extended requests */
#define RS485IF_OFF_PAYLOAD_LEN_LO 12u
#define RS485IF_OFF_PAYLOAD 13u

/*==================================================================================================
 *  Compile-time guarantees on frame sizes
 *
 *  These are the assertions whose absence let the v1 frame builders write past the end of
 *  their arrays. A frame buffer that cannot hold its own CRC is now a build failure.
 *================================================================================================*/

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(RS485IF_REQUEST_FRAME_SIZE > RS485IF_CRC_SIZE,
               "a request frame must be longer than its CRC");
_Static_assert(RS485IF_REQUEST_FRAME_SIZE_EXT >= (RS485IF_OFF_PAYLOAD + 2u + RS485IF_CRC_SIZE),
               "an extended request must hold header, 2-byte payload and CRC");
_Static_assert(RS485IF_BATTERY_RESPONSE_SIZE ==
                   (RS485IF_PAYLOAD_OFFSET + 38u + RS485IF_CRC_SIZE),
               "BATT0100 payload is 38 bytes; response size disagrees");
_Static_assert(RS485IF_CELL_RESPONSE_SIZE == (RS485IF_PAYLOAD_OFFSET + 66u + RS485IF_CRC_SIZE),
               "BATT0500 payload is 66 bytes; response size disagrees");
_Static_assert(RS485IF_MAX_FRAME_SIZE >= RS485IF_CELL_RESPONSE_SIZE,
               "the shared frame buffer cannot hold the longest response");
#endif

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/** Cached state, indexed 0 .. RS485IF_PACK_COUNT-1 for slots 1 .. RS485IF_PACK_COUNT. */
STATIC Rs485If_PackStateType Rs485If_Packs[RS485IF_PACK_COUNT];
STATIC Rs485If_StatisticsType Rs485If_Stats;
STATIC boolean Rs485If_Initialised = FALSE;

/**
 * @brief Shared frame buffer.
 *
 * One buffer rather than one per call site: at 81 bytes it is too large to sit on a 6 KiB
 * task stack alongside everything else, and the bus is strictly half-duplex and driven
 * from a single task, so there is never more than one frame in flight. The single-caller
 * assumption is enforced by the schedule, not by a lock -- see the task allocation in
 * SchM_Cfg.h.
 */
STATIC uint8 Rs485If_Frame[RS485IF_MAX_FRAME_SIZE];

/*==================================================================================================
 *  Byte-order helpers
 *
 *  The protocol is big-endian on the wire. Fields are assembled as unsigned and
 *  reinterpreted once, at the end, because shifting a promoted uint8 left by 24 is
 *  undefined as soon as its top bit is set -- which is precisely what the v1 decoders did.
 *================================================================================================*/

STATIC uint16 Rs485If_ReadU16(const uint8 *p)
{
    return (uint16)(((uint16)p[0] << 8u) | (uint16)p[1]);
}

STATIC uint32 Rs485If_ReadU32(const uint8 *p)
{
    return ((uint32)p[0] << 24u) | ((uint32)p[1] << 16u) | ((uint32)p[2] << 8u) | (uint32)p[3];
}

/**
 * @brief Reinterpret an unsigned 16-bit field as two's-complement signed.
 *
 * Written as an explicit range test rather than a cast because a cast from an out-of-range
 * unsigned to a signed type is implementation-defined in C99 and only became well-defined
 * in C23. This form produces the same machine code on every compiler tested and is
 * portable by construction.
 */
STATIC sint16 Rs485If_ToS16(uint16 raw)
{
    return (raw <= 0x7FFFu) ? (sint16)raw : (sint16)((sint32)raw - 65536L);
}

/** As ::Rs485If_ToS16, for a 32-bit field: reinterpret the raw value as two's complement. */
STATIC sint32 Rs485If_ToS32(uint32 raw)
{
    return (raw <= 0x7FFFFFFFuL) ? (sint32)raw : (sint32)(-(sint32)(0xFFFFFFFFuL - raw) - 1L);
}

/** Write a 16-bit value big-endian. */
STATIC void Rs485If_WriteU16(uint8 *p, uint16 value)
{
    p[0] = (uint8)(value >> 8u);
    p[1] = (uint8)(value & 0xFFu);
}

/** Write a 32-bit value big-endian. */
STATIC void Rs485If_WriteU32(uint8 *p, uint32 value)
{
    p[0] = (uint8)(value >> 24u);
    p[1] = (uint8)((value >> 16u) & 0xFFu);
    p[2] = (uint8)((value >> 8u) & 0xFFu);
    p[3] = (uint8)(value & 0xFFu);
}

/**
 * @brief Append the frame check over the first @p frameSize - 2 bytes.
 *
 * The caller guarantees @p frameSize is the whole frame including the CRC, so the CRC
 * lands at @p frameSize-2 and @p frameSize-1 and can never overrun -- which is the
 * structural difference from v1's @c appendCRC16(data, length), where @p length meant
 * "bytes to cover" and the two CRC bytes went past whatever the caller had allocated.
 */
STATIC void Rs485If_AppendCrc(uint8 *frame, uint8 frameSize)
{
    const uint16 crc = Crc_CalculateCRC16(frame, (uint32)(frameSize - RS485IF_CRC_SIZE), 0u, TRUE);

    Rs485If_WriteU16(&frame[frameSize - RS485IF_CRC_SIZE], crc);
}

/*==================================================================================================
 *  Frame codec
 *================================================================================================*/

Std_ReturnType Rs485If_BuildRequest(uint8 *frame, uint8 frameSize, uint32 serialNumber,
                                    uint8 command)
{
    uint8 i;

    DET_CHECK_RETURN(frame != NULL_PTR, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_BUILD_FRAME, RS485IF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(frameSize >= RS485IF_REQUEST_FRAME_SIZE, MODULE_ID_RS485IF,
                     INSTANCE_ID_SINGLE, RS485IF_API_ID_BUILD_FRAME, E_PARAM_VALUE, E_NOT_OK);

    for (i = 0u; i < RS485IF_REQUEST_FRAME_SIZE; i++)
    {
        frame[i] = 0x00u;
    }

    frame[RS485IF_OFF_START] = RS485IF_START_BYTE;
    frame[RS485IF_OFF_DIR] = RS485IF_DIR_REQUEST;
    Rs485If_WriteU16(&frame[RS485IF_OFF_LEN_HI], (uint16)RS485IF_REQUEST_FRAME_SIZE);
    frame[RS485IF_OFF_TYPE] = RS485IF_TYPE_ADDRESSED;
    Rs485If_WriteU32(&frame[RS485IF_OFF_SERIAL], serialNumber);
    frame[RS485IF_OFF_CMD] = command;

    Rs485If_AppendCrc(frame, RS485IF_REQUEST_FRAME_SIZE);
    return E_OK;
}

Std_ReturnType Rs485If_BuildRequestExt(uint8 *frame, uint8 frameSize, uint32 serialNumber,
                                       uint8 command, uint8 payloadHigh, uint8 payloadLow)
{
    uint8 i;

    DET_CHECK_RETURN(frame != NULL_PTR, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_BUILD_FRAME, RS485IF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(frameSize >= RS485IF_REQUEST_FRAME_SIZE_EXT, MODULE_ID_RS485IF,
                     INSTANCE_ID_SINGLE, RS485IF_API_ID_BUILD_FRAME, E_PARAM_VALUE, E_NOT_OK);

    for (i = 0u; i < RS485IF_REQUEST_FRAME_SIZE_EXT; i++)
    {
        frame[i] = 0x00u;
    }

    frame[RS485IF_OFF_START] = RS485IF_START_BYTE;
    frame[RS485IF_OFF_DIR] = RS485IF_DIR_REQUEST;
    Rs485If_WriteU16(&frame[RS485IF_OFF_LEN_HI], (uint16)RS485IF_REQUEST_FRAME_SIZE_EXT);
    frame[RS485IF_OFF_TYPE] = RS485IF_TYPE_BROADCAST;
    Rs485If_WriteU32(&frame[RS485IF_OFF_SERIAL], serialNumber);
    frame[RS485IF_OFF_CMD] = command;
    Rs485If_WriteU16(&frame[RS485IF_OFF_PAYLOAD_LEN_HI], 2u);
    frame[RS485IF_OFF_PAYLOAD] = payloadHigh;
    frame[RS485IF_OFF_PAYLOAD + 1u] = payloadLow;

    Rs485If_AppendCrc(frame, RS485IF_REQUEST_FRAME_SIZE_EXT);
    return E_OK;
}

Std_ReturnType Rs485If_ValidateResponse(const uint8 *frame, uint8 frameSize, uint8 expectedSize)
{
    uint16 declaredLength;
    uint16 receivedCrc;
    uint16 computedCrc;

    DET_CHECK_RETURN(frame != NULL_PTR, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_PARSE_FRAME, RS485IF_E_PARAM_POINTER, E_NOT_OK);

    if (frameSize < expectedSize)
    {
        Rs485If_Stats.shortFrames++;
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                     RS485IF_API_ID_PARSE_FRAME, RS485IF_E_SHORT_RESPONSE);
        return E_NOT_OK;
    }

    /* Header before CRC, deliberately. On a bus shared with any other device, a foreign
     * frame should register as a header rejection, not as a CRC failure -- the CRC failure
     * rate is the number used to judge whether the harness is degrading, and polluting it
     * with well-formed frames from a different protocol makes that judgement worthless. */
    if ((frame[RS485IF_OFF_START] != RS485IF_START_BYTE) ||
        (frame[RS485IF_OFF_DIR] != RS485IF_DIR_RESPONSE))
    {
        Rs485If_Stats.headerFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                     RS485IF_API_ID_PARSE_FRAME, RS485IF_E_BAD_HEADER);
        return E_NOT_OK;
    }

    declaredLength = Rs485If_ReadU16(&frame[RS485IF_OFF_LEN_HI]);
    if (declaredLength != (uint16)expectedSize)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                     RS485IF_API_ID_PARSE_FRAME, RS485IF_E_BAD_LENGTH);
        return E_INVALID_PARAM;
    }

    receivedCrc = Rs485If_ReadU16(&frame[expectedSize - RS485IF_CRC_SIZE]);
    computedCrc = Crc_CalculateCRC16(frame, (uint32)(expectedSize - RS485IF_CRC_SIZE), 0u, TRUE);

    if (receivedCrc != computedCrc)
    {
        Rs485If_Stats.crcFailures++;
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                     RS485IF_API_ID_PARSE_FRAME, RS485IF_E_CRC_MISMATCH);
        return E_CRC_FAIL;
    }

    return E_OK;
}

uint32 Rs485If_ParseSerialNumber(const uint8 *frame)
{
    if (frame == NULL_PTR)
    {
        return 0u;
    }
    return Rs485If_ReadU32(&frame[RS485IF_OFF_SERIAL]);
}

Std_ReturnType Rs485If_ParsePackData(const uint8 *frame, Rs485If_PackDataType *data)
{
    const uint8 *p;

    DET_CHECK_RETURN((frame != NULL_PTR) && (data != NULL_PTR), MODULE_ID_RS485IF,
                     INSTANCE_ID_SINGLE, RS485IF_API_ID_PARSE_FRAME, RS485IF_E_PARAM_POINTER,
                     E_NOT_OK);

    p = &frame[RS485IF_PAYLOAD_OFFSET];

    data->voltage = Rs485If_ReadU16(&p[0]);
    data->voltageHighest = Rs485If_ReadU16(&p[2]);
    data->voltageLowest = Rs485If_ReadU16(&p[4]);
    data->current = Rs485If_ToS32(Rs485If_ReadU32(&p[6]));
    data->temperature = Rs485If_ToS16(Rs485If_ReadU16(&p[10]));
    data->temperatureHigh = Rs485If_ToS16(Rs485If_ReadU16(&p[12]));
    data->temperatureLow = Rs485If_ToS16(Rs485If_ReadU16(&p[14]));
    data->stateOfCharge = p[16];
    data->stateOfHealth = p[17];
    data->chargeEnergyWh = Rs485If_ReadU32(&p[18]);
    data->dischargeEnergyWh = Rs485If_ReadU32(&p[22]);
    data->chargeTimeSec = Rs485If_ReadU32(&p[26]);
    data->dischargeTimeSec = Rs485If_ReadU32(&p[30]);
    data->statusFlags = Rs485If_ReadU32(&p[34]);

    return E_OK;
}

Std_ReturnType Rs485If_ParseCellData(const uint8 *frame, Rs485If_CellDataType *data)
{
    const uint8 *p;
    uint8 i;

    DET_CHECK_RETURN((frame != NULL_PTR) && (data != NULL_PTR), MODULE_ID_RS485IF,
                     INSTANCE_ID_SINGLE, RS485IF_API_ID_PARSE_FRAME, RS485IF_E_PARAM_POINTER,
                     E_NOT_OK);

    p = &frame[RS485IF_PAYLOAD_OFFSET];

    for (i = 0u; i < RS485IF_CELLS_PER_PACK; i++)
    {
        data->cellVoltage[i] = Rs485If_ReadU16(&p[(uint16)i * 2u]);
    }
    data->current = Rs485If_ToS32(Rs485If_ReadU32(&p[46]));
    for (i = 0u; i < RS485IF_TEMPS_PER_PACK; i++)
    {
        data->temperature[i] = Rs485If_ToS16(Rs485If_ReadU16(&p[50u + ((uint16)i * 2u)]));
    }
    data->statusFlags1 = Rs485If_ReadU32(&p[58]);
    data->statusFlags2 = Rs485If_ReadU32(&p[62]);

    return E_OK;
}

/*==================================================================================================
 *  Transport
 *================================================================================================*/

/**
 * @brief Send a request and collect a fixed-length reply.
 *
 * The whole half-duplex turnaround lives here so that no caller can get the ordering
 * wrong: purge, assert DE, transmit, wait for the shift register to empty, settle,
 * release DE, then read with an early-exit deadline.
 */
STATIC Std_ReturnType Rs485If_Exchange(const uint8 *request, uint8 requestSize, uint8 *response,
                                       uint8 responseSize)
{
    uint16 stale;
    uint16 received = 0u;
    Std_ReturnType status;

    /* Anything already buffered predates this request and can only mis-attribute a reply.
     * Counting it matters: a non-zero count means the previous exchange left the bus out of
     * step, which is a different fault from a pack that never answers. */
    stale = Uart_DiscardRx(RS485IF_UART_INSTANCE);
    if (stale > 0u)
    {
        Rs485If_Stats.staleRxBytes += stale;
        (void)Det_ReportTransientFault(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                       RS485IF_API_ID_READ_BATTERY, RS485IF_E_STALE_RX);
    }

    Dio_WriteChannel(DIO_CHANNEL_RS485_DE, STD_HIGH);

    status = Uart_Write(RS485IF_UART_INSTANCE, request, requestSize);
    if (status == E_OK)
    {
        /* write() returns when the bytes are buffered, not when they are on the wire.
         * Releasing DE here would truncate the tail of the frame. */
        status = Uart_DrainTx(RS485IF_UART_INSTANCE, UART_DRAIN_TIMEOUT_MS);
    }

    Gpt_DelayUs(RS485IF_TURNAROUND_US);
    Dio_WriteChannel(DIO_CHANNEL_RS485_DE, STD_LOW);

    if (status != E_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_TX_FAILED);
        return E_NOT_OK;
    }

    Rs485If_Stats.framesSent++;

    status = Uart_ReadExact(RS485IF_UART_INSTANCE, response, responseSize,
                            RS485IF_RESPONSE_TIMEOUT_MS(responseSize), &received);

    if (status == E_TIMEOUT)
    {
        /* Nothing at all means an absent or unpowered pack; a partial frame means the pack
         * is alive but the link is corrupting traffic. Distinguishing them is the
         * difference between "check the connector" and "check the termination". */
        if (received == 0u)
        {
            Rs485If_Stats.timeouts++;
            (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                         RS485IF_API_ID_READ_BATTERY, RS485IF_E_NO_RESPONSE);
        }
        else
        {
            Rs485If_Stats.shortFrames++;
            (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                                         RS485IF_API_ID_READ_BATTERY, RS485IF_E_SHORT_RESPONSE);
        }
        return E_TIMEOUT;
    }
    if (status != E_OK)
    {
        return E_NOT_OK;
    }

    Rs485If_Stats.framesReceived++;
    return E_OK;
}

/**
 * @brief Issue one addressed request and validate the reply, with retries.
 *
 * @param slot          Pack being addressed, for error attribution. 0 for a broadcast.
 * @param serialNumber  Serial number to address, or 0 to broadcast.
 * @param command       RS485IF_CMD_* value.
 * @param responseSize  Exact length of the expected reply.
 * @param checkSerial   TRUE to require the reply to echo @p serialNumber.
 */
STATIC Std_ReturnType Rs485If_Transact(Rs485If_SlotType slot, uint32 serialNumber, uint8 command,
                                       uint8 responseSize, boolean checkSerial)
{
    uint8 request[RS485IF_REQUEST_FRAME_SIZE];
    uint8 attempt;
    Std_ReturnType last = E_NOT_OK;

    if (Rs485If_BuildRequest(request, (uint8)sizeof(request), serialNumber, command) != E_OK)
    {
        return E_NOT_OK;
    }

    for (attempt = 0u; attempt <= (uint8)RS485IF_REQUEST_RETRIES; attempt++)
    {
        if (attempt > 0u)
        {
            Gpt_DelayMs(RS485IF_INTER_REQUEST_GAP_MS);
        }

        last = Rs485If_Exchange(request, (uint8)sizeof(request), Rs485If_Frame, responseSize);
        if (last != E_OK)
        {
            continue;
        }

        last = Rs485If_ValidateResponse(Rs485If_Frame, responseSize, responseSize);
        if (last != E_OK)
        {
            continue;
        }

        /* The protocol echoes the addressed serial number. Checking it is what makes a
         * stale or cross-talked reply detectable -- v1 validated only the CRC, so a
         * perfectly formed reply from the wrong pack was accepted and its measurements
         * were logged against another pack's slot. */
        if ((checkSerial != FALSE) &&
            (Rs485If_ParseSerialNumber(Rs485If_Frame) != serialNumber))
        {
            Rs485If_Stats.serialMismatches++;
            (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, slot, RS485IF_API_ID_PARSE_FRAME,
                                         RS485IF_E_SERIAL_MISMATCH);
            last = E_NOT_OK;
            continue;
        }

        return E_OK;
    }

    return last;
}

/** Convert a 1-based slot to a 0-based index, or return FALSE if out of range. */
STATIC boolean Rs485If_SlotToIndex(Rs485If_SlotType slot, uint8 *index)
{
    if ((slot < 1u) || (slot > (Rs485If_SlotType)RS485IF_PACK_COUNT))
    {
        return FALSE;
    }
    *index = (uint8)(slot - 1u);
    return TRUE;
}

/** Record the outcome of a read against a pack's failure counters. */
STATIC void Rs485If_RecordOutcome(uint8 index, boolean success)
{
    Rs485If_Packs[index].totalRequests++;

    if (success != FALSE)
    {
        Rs485If_Packs[index].consecutiveFailures = 0u;
    }
    else
    {
        Rs485If_Packs[index].totalFailures++;
        if (Rs485If_Packs[index].consecutiveFailures < 0xFFFFu)
        {
            Rs485If_Packs[index].consecutiveFailures++;
        }
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Return every piece of session state to its power-on value.
 *
 * Shared by ::Rs485If_Init and ::Rs485If_DeInit so that both endpoints of a bus session
 * leave identical state behind. That makes every counter and every cached reading
 * unambiguously "since the current session began": there is no way to observe a pack marked
 * present, or a failure counted, from a session that has already ended. Anything needed
 * across a restart must be copied out before shutdown -- EcuM does that, publishing the
 * figures in its final health record.
 */
STATIC void Rs485If_ResetState(void)
{
    uint8 i;

    for (i = 0u; i < (uint8)RS485IF_PACK_COUNT; i++)
    {
        Rs485If_Packs[i].serialNumber = 0u;
        Rs485If_Packs[i].present = FALSE;
        Rs485If_Packs[i].packDataValid = FALSE;
        Rs485If_Packs[i].cellDataValid = FALSE;
        Rs485If_Packs[i].consecutiveFailures = 0u;
        Rs485If_Packs[i].totalRequests = 0u;
        Rs485If_Packs[i].totalFailures = 0u;
    }

    Rs485If_Stats.framesSent = 0u;
    Rs485If_Stats.framesReceived = 0u;
    Rs485If_Stats.crcFailures = 0u;
    Rs485If_Stats.timeouts = 0u;
    Rs485If_Stats.shortFrames = 0u;
    Rs485If_Stats.headerFailures = 0u;
    Rs485If_Stats.serialMismatches = 0u;
    Rs485If_Stats.staleRxBytes = 0u;
}

Std_ReturnType Rs485If_Init(void)
{
    Uart_ConfigType config;

    Rs485If_ResetState();

    /* Receive is the resting state: a half-duplex bus with the driver enabled blocks every
     * other device, so DE goes low before the port is even opened. */
    Dio_WriteChannel(DIO_CHANNEL_RS485_DE, STD_LOW);

    config.baudRate = UART_BAUD_RS485;
    config.frame = UART_FRAME_8E1;
    config.rxPin = PIN_RS485_RX;
    config.txPin = PIN_RS485_TX;
    config.rxBufferSize = UART_RX_BUFFER_RS485;
    config.invertRx = FALSE;

    if (Uart_Open(RS485IF_UART_INSTANCE, &config) != E_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_RS485IF, INSTANCE_ID_SINGLE, RS485IF_API_ID_INIT,
                                     E_PARAM_CONFIG);
        return E_NOT_OK;
    }

    Rs485If_Initialised = TRUE;
    return E_OK;
}

void Rs485If_DeInit(void)
{
    if (Rs485If_Initialised != FALSE)
    {
        STD_DISCARD(Uart_Close(RS485IF_UART_INSTANCE));
    }
    Dio_WriteChannel(DIO_CHANNEL_RS485_DE, STD_LOW);
    Rs485If_Initialised = FALSE;
    Rs485If_ResetState();
}

Std_ReturnType Rs485If_SwitchPacks(uint8 maskHigh, uint8 maskLow)
{
    uint8 request[RS485IF_REQUEST_FRAME_SIZE_EXT];
    uint8 response[RS485IF_REQUEST_FRAME_SIZE_EXT];
    Std_ReturnType status;

    DET_CHECK_RETURN(Rs485If_Initialised != FALSE, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_SWITCH_PACKS, RS485IF_E_UNINIT, E_NOT_OK);

    /* Broadcast: the switch command addresses the pack controller, not an individual pack,
     * so the serial number field is zero. */
    if (Rs485If_BuildRequestExt(request, (uint8)sizeof(request), 0u, RS485IF_CMD_BATTERY_PARAMS,
                                maskHigh, maskLow) != E_OK)
    {
        return E_NOT_OK;
    }

    status = Rs485If_Exchange(request, (uint8)sizeof(request), response, (uint8)sizeof(response));
    if (status != E_OK)
    {
        return status;
    }

    return Rs485If_ValidateResponse(response, (uint8)sizeof(response),
                                    (uint8)RS485IF_REQUEST_FRAME_SIZE_EXT);
}

Std_ReturnType Rs485If_DiscoverPacks(void)
{
    uint8 index;
    uint8 found = 0u;

    DET_CHECK_RETURN(Rs485If_Initialised != FALSE, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_DISCOVER, RS485IF_E_UNINIT, E_NOT_OK);

    /* All packs off first. An unaddressed serial-number request is answered by every
     * enabled pack simultaneously, and the collision leaves nothing decodable -- so
     * discovery has to enable exactly one at a time. */
    STD_DISCARD(Rs485If_SwitchPacks(0x00u, 0x00u));
    Gpt_DelayMs(RS485IF_SWITCH_SETTLE_MS);

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        const uint8 slotMask = (uint8)(1u << index);

        Rs485If_Packs[index].present = FALSE;
        Rs485If_Packs[index].serialNumber = 0u;

        STD_DISCARD(Rs485If_SwitchPacks(0x00u, slotMask));
        Gpt_DelayMs(RS485IF_SWITCH_SETTLE_MS);

        /* Serial number 0 addresses whichever pack is currently enabled, and the reply's
         * echoed serial cannot be checked against a value we do not yet know -- so
         * checkSerial is FALSE here and only here. */
        if (Rs485If_Transact((Rs485If_SlotType)(index + 1u), 0u, RS485IF_CMD_SERIAL_NUMBER,
                             (uint8)RS485IF_SERIAL_RESPONSE_SIZE, FALSE) == E_OK)
        {
            const uint32 serial = Rs485If_ParseSerialNumber(Rs485If_Frame);

            /* A serial number of zero or all-ones is what an idle or stuck bus reads as,
             * not a real pack identity, so it is rejected rather than cached. */
            if ((serial != 0u) && (serial != 0xFFFFFFFFuL))
            {
                boolean duplicate = FALSE;
                uint8 prior;

                /* Two slots cannot hold the same physical pack. A repeated serial number
                 * means the switch command did not take effect -- a stuck contactor, or a
                 * pack wired to two slots -- and accepting it would double-count that
                 * pack's energy and current in every aggregate the vehicle reports. */
                for (prior = 0u; prior < index; prior++)
                {
                    if ((Rs485If_Packs[prior].present != FALSE) &&
                        (Rs485If_Packs[prior].serialNumber == serial))
                    {
                        duplicate = TRUE;
                        break;
                    }
                }

                if (duplicate != FALSE)
                {
                    (void)Det_ReportRuntimeError(MODULE_ID_RS485IF,
                                                 (uint8)(index + 1u),
                                                 RS485IF_API_ID_DISCOVER,
                                                 RS485IF_E_SERIAL_MISMATCH);
                }
                else
                {
                    Rs485If_Packs[index].serialNumber = serial;
                    Rs485If_Packs[index].present = TRUE;
                    found++;
                }
            }
        }

        STD_DISCARD(Rs485If_SwitchPacks(0x00u, 0x00u));
        Gpt_DelayMs(RS485IF_SWITCH_SETTLE_MS);
    }

    /* Re-enable every slot that answered. Enabling all four unconditionally, as v1 did
     * with a hard-coded 0x0F, energises contactors for packs that are not fitted. */
    {
        uint8 presentMask = 0u;

        for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
        {
            if (Rs485If_Packs[index].present != FALSE)
            {
                presentMask |= (uint8)(1u << index);
            }
        }
        STD_DISCARD(Rs485If_SwitchPacks(0x00u, presentMask));
        Gpt_DelayMs(RS485IF_SWITCH_SETTLE_MS);
    }

    return (found > 0u) ? E_OK : E_NOT_FOUND;
}

Std_ReturnType Rs485If_ReadPackData(Rs485If_SlotType slot)
{
    uint8 index;
    Std_ReturnType status;

    DET_CHECK_RETURN(Rs485If_Initialised != FALSE, MODULE_ID_RS485IF, slot,
                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(Rs485If_SlotToIndex(slot, &index) != FALSE, MODULE_ID_RS485IF, slot,
                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_PARAM_SLOT, E_NOT_OK);

    if (Rs485If_Packs[index].present == FALSE)
    {
        return E_NOT_FOUND;
    }

    status = Rs485If_Transact(slot, Rs485If_Packs[index].serialNumber,
                              RS485IF_CMD_BATTERY_PARAMS,
                              (uint8)RS485IF_BATTERY_RESPONSE_SIZE, TRUE);

    if (status == E_OK)
    {
        status = Rs485If_ParsePackData(Rs485If_Frame, &Rs485If_Packs[index].pack);
    }

    /* The validity flag tracks this read, not the last successful one. A consumer that
     * published a stale reading as current would make a dead pack look healthy -- which is
     * the single most misleading thing a battery monitor can do. */
    Rs485If_Packs[index].packDataValid = (status == E_OK) ? TRUE : FALSE;
    Rs485If_RecordOutcome(index, (status == E_OK) ? TRUE : FALSE);

    return status;
}

Std_ReturnType Rs485If_ReadCellData(Rs485If_SlotType slot)
{
    uint8 index;
    Std_ReturnType status;

    DET_CHECK_RETURN(Rs485If_Initialised != FALSE, MODULE_ID_RS485IF, slot,
                     RS485IF_API_ID_READ_CELLS, RS485IF_E_UNINIT, E_NOT_OK);
    DET_CHECK_RETURN(Rs485If_SlotToIndex(slot, &index) != FALSE, MODULE_ID_RS485IF, slot,
                     RS485IF_API_ID_READ_CELLS, RS485IF_E_PARAM_SLOT, E_NOT_OK);

    if (Rs485If_Packs[index].present == FALSE)
    {
        return E_NOT_FOUND;
    }

    status = Rs485If_Transact(slot, Rs485If_Packs[index].serialNumber, RS485IF_CMD_CELL_PARAMS,
                              (uint8)RS485IF_CELL_RESPONSE_SIZE, TRUE);

    if (status == E_OK)
    {
        status = Rs485If_ParseCellData(Rs485If_Frame, &Rs485If_Packs[index].cells);
    }

    Rs485If_Packs[index].cellDataValid = (status == E_OK) ? TRUE : FALSE;
    Rs485If_RecordOutcome(index, (status == E_OK) ? TRUE : FALSE);

    return status;
}

Std_ReturnType Rs485If_PollAllPacks(void)
{
    uint8 index;
    boolean allOk = TRUE;

    DET_CHECK_RETURN(Rs485If_Initialised != FALSE, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_UNINIT, E_NOT_OK);

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        const Rs485If_SlotType slot = (Rs485If_SlotType)(index + 1u);

        if (Rs485If_Packs[index].present == FALSE)
        {
            continue;
        }

        if (Rs485If_ReadPackData(slot) != E_OK)
        {
            allOk = FALSE;
        }
        Gpt_DelayMs(RS485IF_INTER_REQUEST_GAP_MS);

        if (Rs485If_ReadCellData(slot) != E_OK)
        {
            allOk = FALSE;
        }
        Gpt_DelayMs(RS485IF_INTER_REQUEST_GAP_MS);
    }

    return (allOk != FALSE) ? E_OK : E_NOT_OK;
}

Std_ReturnType Rs485If_GetPackState(Rs485If_SlotType slot, Rs485If_PackStateType *state)
{
    uint8 index;

    DET_CHECK_RETURN(state != NULL_PTR, MODULE_ID_RS485IF, slot, RS485IF_API_ID_READ_BATTERY,
                     RS485IF_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(Rs485If_SlotToIndex(slot, &index) != FALSE, MODULE_ID_RS485IF, slot,
                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_PARAM_SLOT, E_NOT_OK);

    *state = Rs485If_Packs[index];
    return E_OK;
}

uint8 Rs485If_GetPresentPackCount(void)
{
    uint8 index;
    uint8 count = 0u;

    for (index = 0u; index < (uint8)RS485IF_PACK_COUNT; index++)
    {
        if (Rs485If_Packs[index].present != FALSE)
        {
            count++;
        }
    }
    return count;
}

Std_ReturnType Rs485If_GetStatistics(Rs485If_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_RS485IF, INSTANCE_ID_SINGLE,
                     RS485IF_API_ID_READ_BATTERY, RS485IF_E_PARAM_POINTER, E_NOT_OK);

    *stats = Rs485If_Stats;
    return E_OK;
}

void Rs485If_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = RS485IF_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_RS485IF;
        versioninfo->sw_major_version = RS485IF_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = RS485IF_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = RS485IF_SW_PATCH_VERSION;
    }
}

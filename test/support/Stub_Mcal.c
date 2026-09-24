/**
 * @file    Stub_Mcal.c
 * @brief   MCAL platform-leaf test doubles for the host build.
 *
 * Each MCAL module is split into a platform-independent part (compiled from
 * @c src/mcal/<M>/<M>.c on both host and target) and a platform leaf (@c <M>_Esp32.cpp
 * on the target). This file supplies the leaves for the host, so the code under test is
 * the real code everywhere above the hardware boundary.
 *
 * Nothing here blocks, sleeps or depends on wall-clock time: virtual time advances only
 * when a test says so, which is what keeps the suite both fast and repeatable.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "Stub_Mcal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcal/Adc/Adc.h"
#include "mcal/Dio/Dio.h"
#include "mcal/Fls/Fls.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Mcu/Mcu.h"
#include "mcal/Port/Port.h"
#include "mcal/Spi/Spi.h"
#include "mcal/Uart/Uart.h"
#include "mcal/Wdg/Wdg.h"

/*==================================================================================================
 *  SECTION 1 : Gpt -- virtual time
 *================================================================================================*/

static uint64 Stub_TimeUs;
static uint32 Stub_DelayTotalMs;
static uint32 Stub_DelayCalls;
static boolean Stub_DelayAdvances = TRUE;

Std_ReturnType Gpt_Init(void)
{
    return E_OK;
}

Gpt_TimestampType Gpt_GetMonotonicMs(void)
{
    return (Gpt_TimestampType)(Stub_TimeUs / 1000u);
}

uint64 Gpt_GetMonotonicUs(void)
{
    return Stub_TimeUs;
}

void Gpt_DelayMs(uint32 ms)
{
    Stub_DelayTotalMs += ms;
    Stub_DelayCalls++;
    if (Stub_DelayAdvances != FALSE)
    {
        Stub_TimeUs += (uint64)ms * 1000u;
    }
}

void Gpt_DelayUs(uint32 us)
{
    Stub_DelayCalls++;
    if (Stub_DelayAdvances != FALSE)
    {
        Stub_TimeUs += (uint64)us;
    }
}

void Stub_Gpt_AdvanceMs(uint32 ms)
{
    Stub_TimeUs += (uint64)ms * 1000u;
}

void Stub_Gpt_AdvanceUs(uint64 us)
{
    Stub_TimeUs += us;
}

void Stub_Gpt_SetMonotonicMs(uint32 ms)
{
    Stub_TimeUs = (uint64)ms * 1000u;
}

uint32 Stub_Gpt_GetTotalDelayMs(void)
{
    return Stub_DelayTotalMs;
}

uint32 Stub_Gpt_GetDelayCallCount(void)
{
    return Stub_DelayCalls;
}

void Stub_Gpt_SetDelayAdvancesTime(boolean enable)
{
    Stub_DelayAdvances = enable;
}

/*==================================================================================================
 *  SECTION 2 : Mcu -- reset capture
 *================================================================================================*/

static uint8 Stub_ResetReason = (uint8)MCU_RESET_POWER_ON;
static uint32 Stub_ResetCount;
static boolean Stub_ResetArmed;
static jmp_buf Stub_ResetJmp;
static uint8 Stub_DeviceId[MCU_DEVICE_ID_LENGTH] = {0x24u, 0x6Fu, 0x28u, 0xAAu, 0xBBu, 0xCCu};
static uint32 Stub_HeapFree = 180000uL;
static uint32 Stub_HeapMinFree = 150000uL;
static uint32 Stub_HeapLargest = 96000uL;

Std_ReturnType Mcu_Init(void)
{
    return E_OK;
}

Mcu_ResetReasonType Mcu_GetResetReason(void)
{
    return (Mcu_ResetReasonType)Stub_ResetReason;
}

void Mcu_PerformReset(void)
{
    Stub_ResetCount++;

    if (Stub_ResetArmed != FALSE)
    {
        Stub_ResetArmed = FALSE;
        longjmp(Stub_ResetJmp, 1);
    }

    /* An unexpected reset must be loud. Silently returning would let the test continue
     * executing code that on the target would never have run, and the resulting failure
     * would point somewhere unrelated. */
    (void)fprintf(stderr,
                  "\nStub_Mcal: Mcu_PerformReset() called without Stub_Mcu_ArmResetCapture().\n"
                  "           Wrap the call under test in setjmp(*Stub_Mcu_GetResetJmpBuf()).\n");
    abort();
}

Std_ReturnType Mcu_GetDeviceId(uint8 *buffer, uint8 bufferSize)
{
    if ((buffer == NULL_PTR) || (bufferSize < MCU_DEVICE_ID_LENGTH))
    {
        return E_NOT_OK;
    }
    (void)memcpy(buffer, Stub_DeviceId, sizeof(Stub_DeviceId));
    return E_OK;
}

Std_ReturnType Mcu_GetHeapInfo(Mcu_HeapInfoType *info)
{
    if (info == NULL_PTR)
    {
        return E_NOT_OK;
    }
    info->heapFreeBytes = Stub_HeapFree;
    info->heapMinFreeBytes = Stub_HeapMinFree;
    info->heapLargestBlockBytes = Stub_HeapLargest;
    info->internalFreeBytes = Stub_HeapFree;
    return E_OK;
}

uint32 Mcu_GetCpuFrequencyHz(void)
{
    return 240000000uL;
}

jmp_buf *Stub_Mcu_GetResetJmpBuf(void)
{
    return &Stub_ResetJmp;
}

void Stub_Mcu_ArmResetCapture(void)
{
    Stub_ResetArmed = TRUE;
}

boolean Stub_Mcu_WasResetRequested(void)
{
    return (Stub_ResetCount > 0u) ? TRUE : FALSE;
}

uint32 Stub_Mcu_GetResetCount(void)
{
    return Stub_ResetCount;
}

void Stub_Mcu_SetResetReason(uint8 reason)
{
    Stub_ResetReason = reason;
}

void Stub_Mcu_SetDeviceId(const uint8 *id)
{
    if (id != NULL_PTR)
    {
        (void)memcpy(Stub_DeviceId, id, sizeof(Stub_DeviceId));
    }
}

void Stub_Mcu_SetHeapFree(uint32 freeBytes, uint32 minFreeBytes, uint32 largestBlock)
{
    Stub_HeapFree = freeBytes;
    Stub_HeapMinFree = minFreeBytes;
    Stub_HeapLargest = largestBlock;
}

/*==================================================================================================
 *  SECTION 3 : Port
 *================================================================================================*/

Std_ReturnType Port_Init(void)
{
    return E_OK;
}

Std_ReturnType Port_SetPinDirection(uint8 pin, Port_PinDirectionType direction)
{
    COMPILER_UNUSED(pin);
    COMPILER_UNUSED(direction);
    return E_OK;
}

Std_ReturnType Port_SetPinPull(uint8 pin, Port_PullType pull)
{
    COMPILER_UNUSED(pin);
    COMPILER_UNUSED(pull);
    return E_OK;
}

void Port_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = PORT_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_PORT;
        versioninfo->sw_major_version = PORT_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = PORT_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = PORT_SW_PATCH_VERSION;
    }
}

/*==================================================================================================
 *  SECTION 4 : Dio -- level and transition capture
 *================================================================================================*/

static uint8 Stub_DioLevel[STUB_DIO_CHANNEL_COUNT];
static uint8 Stub_DioInput[STUB_DIO_CHANNEL_COUNT];
static uint32 Stub_DioWrites[STUB_DIO_CHANNEL_COUNT];
static uint32 Stub_DioToggles[STUB_DIO_CHANNEL_COUNT];

Dio_LevelType Dio_ReadChannel(Dio_ChannelType channelId)
{
    if (channelId >= STUB_DIO_CHANNEL_COUNT)
    {
        return STD_LOW;
    }
    /* An output channel reads back what was written to it, matching real GPIO
     * behaviour; an input channel reads what the test injected. */
    return (Stub_DioWrites[channelId] > 0u) ? Stub_DioLevel[channelId] : Stub_DioInput[channelId];
}

void Dio_WriteChannel(Dio_ChannelType channelId, Dio_LevelType level)
{
    if (channelId >= STUB_DIO_CHANNEL_COUNT)
    {
        return;
    }
    if ((Stub_DioWrites[channelId] > 0u) && (Stub_DioLevel[channelId] != level))
    {
        Stub_DioToggles[channelId]++;
    }
    Stub_DioLevel[channelId] = level;
    Stub_DioWrites[channelId]++;
}

Dio_LevelType Dio_FlipChannel(Dio_ChannelType channelId)
{
    Dio_LevelType next;

    if (channelId >= STUB_DIO_CHANNEL_COUNT)
    {
        return STD_LOW;
    }
    next = (Dio_LevelType)((Dio_ReadChannel(channelId) == STD_HIGH) ? STD_LOW : STD_HIGH);
    Dio_WriteChannel(channelId, next);
    return next;
}

void Dio_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = DIO_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_DIO;
        versioninfo->sw_major_version = DIO_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = DIO_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = DIO_SW_PATCH_VERSION;
    }
}

uint8 Stub_Dio_GetLevel(uint8 channel)
{
    return (channel < STUB_DIO_CHANNEL_COUNT) ? Stub_DioLevel[channel] : (uint8)STD_LOW;
}

uint32 Stub_Dio_GetWriteCount(uint8 channel)
{
    return (channel < STUB_DIO_CHANNEL_COUNT) ? Stub_DioWrites[channel] : 0u;
}

uint32 Stub_Dio_GetToggleCount(uint8 channel)
{
    return (channel < STUB_DIO_CHANNEL_COUNT) ? Stub_DioToggles[channel] : 0u;
}

void Stub_Dio_SetInputLevel(uint8 channel, uint8 level)
{
    if (channel < STUB_DIO_CHANNEL_COUNT)
    {
        Stub_DioInput[channel] = level;
    }
}

/*==================================================================================================
 *  SECTION 5 : Adc
 *================================================================================================*/

static uint16 Stub_AdcRaw[STUB_DIO_CHANNEL_COUNT];
static uint32 Stub_AdcFailReads;

/* The real leaf refuses a read before Adc_Init and reports ADC_E_UNINIT. Modelled here because
 * the check lives in the platform leaf, so without it the host build could not observe the
 * contract at all -- and a contract the target enforces but the host cannot see is one that a
 * future leaf can quietly drop. */
static boolean Stub_AdcInitialised = FALSE;
static uint32 Stub_AdcReadCount;

/* Scripted per-call samples, so a test can drive the oversampling with a value that differs
 * between samples -- the only way to distinguish averaging from returning the first sample. */
static uint16 Stub_AdcSequence[STUB_ADC_SEQUENCE_MAX];
static uint8 Stub_AdcSequenceLen;
static uint8 Stub_AdcSequenceNext;

Std_ReturnType Adc_Init(void)
{
    Stub_AdcInitialised = TRUE;
    return E_OK;
}

Std_ReturnType Adc_ReadChannelRaw(Adc_ChannelType channel, Adc_ValueType *value)
{
    if ((value == NULL_PTR) || (channel >= STUB_DIO_CHANNEL_COUNT))
    {
        return E_NOT_OK;
    }
    if (Stub_AdcInitialised == FALSE)
    {
        return E_NOT_OK;
    }
    if (Stub_AdcFailReads > 0u)
    {
        Stub_AdcFailReads--;
        return E_NOT_OK;
    }

    Stub_AdcReadCount++;

    /* A scripted sequence wins over the steady value, and the last entry repeats once exhausted so a
     * caller taking more samples than the script provides still gets a defined answer rather than
     * silently reading whatever the steady value happened to be. */
    if (Stub_AdcSequenceLen > 0u)
    {
        const uint8 index = (Stub_AdcSequenceNext < Stub_AdcSequenceLen)
                                ? Stub_AdcSequenceNext
                                : (uint8)(Stub_AdcSequenceLen - 1u);

        *value = Stub_AdcSequence[index];
        if (Stub_AdcSequenceNext < Stub_AdcSequenceLen)
        {
            Stub_AdcSequenceNext++;
        }
        return E_OK;
    }

    *value = Stub_AdcRaw[channel];
    return E_OK;
}

uint32 Stub_Adc_GetReadCount(void)
{
    return Stub_AdcReadCount;
}

void Stub_Adc_SetRawSequence(uint8 channel, const uint16 *samples, uint8 count)
{
    COMPILER_UNUSED(channel);

    if ((samples == NULL_PTR) || (count == 0u) || (count > (uint8)STUB_ADC_SEQUENCE_MAX))
    {
        Stub_AdcSequenceLen = 0u;
        Stub_AdcSequenceNext = 0u;
        return;
    }

    (void)memcpy(Stub_AdcSequence, samples, (size_t)count * sizeof(samples[0]));
    Stub_AdcSequenceLen = count;
    Stub_AdcSequenceNext = 0u;
}

void Stub_Adc_SetRaw(uint8 channel, uint16 raw)
{
    if (channel < STUB_DIO_CHANNEL_COUNT)
    {
        Stub_AdcRaw[channel] = raw;
    }
}

void Stub_Adc_FailNextReads(uint32 count)
{
    Stub_AdcFailReads = count;
}

/*==================================================================================================
 *  SECTION 6 : Uart -- scripted bus
 *================================================================================================*/

typedef struct
{
    uint8 rx[STUB_UART_BUFFER_SIZE];
    uint16 rxLen;
    uint16 rxPos;
    uint8 tx[STUB_UART_BUFFER_SIZE];
    uint16 txLen;
    uint32 baud;
    boolean open;
    uint32 failWrites;
    Uart_StatisticsType stats;
    /* Reply armed by Stub_Uart_SetTxResponse, delivered on each accepted write. */
    uint8 response[STUB_UART_BUFFER_SIZE];
    uint16 responseLen;
    boolean responseArmed;
    boolean responseRepeat;
    uint32 responseCount;
    /* Scripted reply sequence: one entry consumed per accepted write, in order. Takes precedence
     * over the single armed reply above, so a test can script a whole polling round and still fall
     * back to a repeating reply for the requests it does not care about. */
    uint8 script[STUB_UART_SCRIPT_DEPTH][STUB_UART_SCRIPT_FRAME_MAX];
    uint16 scriptLen[STUB_UART_SCRIPT_DEPTH];
    uint8 scriptCount;
    uint8 scriptNext;
} Stub_UartInstanceType;

static Stub_UartInstanceType Stub_Uarts[STUB_UART_INSTANCE_COUNT];

static boolean Stub_UartValid(Uart_InstanceType instance)
{
    return (instance < (Uart_InstanceType)STUB_UART_INSTANCE_COUNT) ? TRUE : FALSE;
}

Std_ReturnType Uart_Init(void)
{
    return E_OK;
}

Std_ReturnType Uart_Open(Uart_InstanceType instance, const Uart_ConfigType *config)
{
    if ((Stub_UartValid(instance) == FALSE) || (config == NULL_PTR))
    {
        return E_NOT_OK;
    }
    /* The same configuration validation the platform leaf performs. A zero baud rate would divide by
     * zero in a real peripheral's divisor calculation. Modelled here so the host build can observe the
     * contract -- one the target enforces and the host cannot see is one a future leaf can quietly
     * drop. */
    if ((config->baudRate == 0uL) || (config->baudRate > 921600uL))
    {
        return E_NOT_OK;
    }
    if (Stub_Uarts[instance].open != FALSE)
    {
        return E_NOT_OK;
    }
    Stub_Uarts[instance].open = TRUE;
    Stub_Uarts[instance].baud = config->baudRate;
    return E_OK;
}

Std_ReturnType Uart_Close(Uart_InstanceType instance)
{
    if (Stub_UartValid(instance) == FALSE)
    {
        return E_NOT_OK;
    }
    Stub_Uarts[instance].open = FALSE;
    return E_OK;
}

boolean Uart_IsOpen(Uart_InstanceType instance)
{
    return (Stub_UartValid(instance) != FALSE) ? Stub_Uarts[instance].open : FALSE;
}

Std_ReturnType Uart_Write(Uart_InstanceType instance, const uint8 *data, uint16 length)
{
    Stub_UartInstanceType *u;

    if ((Stub_UartValid(instance) == FALSE) || (data == NULL_PTR))
    {
        return E_NOT_OK;
    }
    u = &Stub_Uarts[instance];
    if (u->open == FALSE)
    {
        return E_NOT_OK;
    }
    if (u->failWrites > 0u)
    {
        u->failWrites--;
        return E_NOT_OK;
    }
    if (((uint32)u->txLen + (uint32)length) > (uint32)STUB_UART_BUFFER_SIZE)
    {
        return E_NO_SPACE;
    }
    (void)memcpy(&u->tx[u->txLen], data, length);
    u->txLen = (uint16)(u->txLen + length);
    u->stats.bytesTransmitted += length;

    /* Reclaim the already-consumed prefix of the receive buffer before delivering anything.
     *
     * Without this the buffer fills monotonically and the stub starts silently dropping replies part
     * way through a long scripted session -- which presents as the code under test timing out for no
     * visible reason, and sends you looking for a fault in the driver rather than in the harness. A
     * multi-round battery poll crosses 1 KiB of replies well before it has finished. */
    if ((u->rxPos > 0u) && (u->rxPos == u->rxLen))
    {
        u->rxPos = 0u;
        u->rxLen = 0u;
    }
    else if (u->rxPos > 0u)
    {
        const uint16 unread = (uint16)(u->rxLen - u->rxPos);

        (void)memmove(u->rx, &u->rx[u->rxPos], unread);
        u->rxLen = unread;
        u->rxPos = 0u;
    }
    else
    {
        /* Nothing consumed yet; there is nothing to reclaim. */
    }

    /* Deliver the reply as a consequence of the request, so it lands in the receive buffer after any
     * purge the driver performed beforehand.
     *
     * A scripted sequence wins over the single armed reply: a test that scripts a multi-device round
     * needs each request answered differently, and a script entry of zero length means "this device
     * stays silent for this request", which is how an absent pack is modelled without disturbing the
     * ordering of the entries after it. */
    if (u->scriptNext < u->scriptCount)
    {
        const uint16 replyLen = u->scriptLen[u->scriptNext];

        if ((replyLen > 0u) &&
            (((uint32)u->rxLen + (uint32)replyLen) <= (uint32)STUB_UART_BUFFER_SIZE))
        {
            (void)memcpy(&u->rx[u->rxLen], u->script[u->scriptNext], replyLen);
            u->rxLen = (uint16)(u->rxLen + replyLen);
            u->responseCount++;
        }
        u->scriptNext++;
    }
    else if ((u->responseArmed != FALSE) &&
             (((uint32)u->rxLen + (uint32)u->responseLen) <= (uint32)STUB_UART_BUFFER_SIZE))
    {
        (void)memcpy(&u->rx[u->rxLen], u->response, u->responseLen);
        u->rxLen = (uint16)(u->rxLen + u->responseLen);
        u->responseCount++;
        if (u->responseRepeat == FALSE)
        {
            u->responseArmed = FALSE;
        }
    }
    else
    {
        /* Nothing armed: the peer is silent, and the driver's timeout is what must handle it. */
    }

    return E_OK;
}

Std_ReturnType Uart_Read(Uart_InstanceType instance, uint8 *buffer, uint16 maxLength,
                         uint16 *actualLength)
{
    Stub_UartInstanceType *u;
    uint16 available;
    uint16 take;

    if ((Stub_UartValid(instance) == FALSE) || (buffer == NULL_PTR) || (actualLength == NULL_PTR))
    {
        return E_NOT_OK;
    }
    u = &Stub_Uarts[instance];
    if (u->open == FALSE)
    {
        return E_NOT_OK;
    }

    available = (uint16)(u->rxLen - u->rxPos);
    take = (maxLength < available) ? maxLength : available;
    if (take > 0u)
    {
        (void)memcpy(buffer, &u->rx[u->rxPos], take);
        u->rxPos = (uint16)(u->rxPos + take);
        u->stats.bytesReceived += take;
    }
    *actualLength = take;
    return E_OK;
}

uint16 Uart_BytesAvailable(Uart_InstanceType instance)
{
    if (Stub_UartValid(instance) == FALSE)
    {
        return 0u;
    }
    return (uint16)(Stub_Uarts[instance].rxLen - Stub_Uarts[instance].rxPos);
}

uint16 Uart_DiscardRx(Uart_InstanceType instance)
{
    uint16 discarded;

    if (Stub_UartValid(instance) == FALSE)
    {
        return 0u;
    }
    discarded = (uint16)(Stub_Uarts[instance].rxLen - Stub_Uarts[instance].rxPos);
    Stub_Uarts[instance].rxPos = Stub_Uarts[instance].rxLen;
    Stub_Uarts[instance].stats.rxDiscardedBytes += discarded;
    return discarded;
}

Std_ReturnType Uart_DrainTx(Uart_InstanceType instance, uint32 timeoutMs)
{
    COMPILER_UNUSED(timeoutMs);
    /* The stub transmits instantaneously, so the shift register is always already
     * empty. Tests that care about turnaround ordering assert on the DE pin's
     * transition count instead. */
    return (Stub_UartValid(instance) != FALSE) ? E_OK : E_NOT_OK;
}

Std_ReturnType Uart_GetStatistics(Uart_InstanceType instance, Uart_StatisticsType *stats)
{
    if ((Stub_UartValid(instance) == FALSE) || (stats == NULL_PTR))
    {
        return E_NOT_OK;
    }
    *stats = Stub_Uarts[instance].stats;
    return E_OK;
}

void Stub_Uart_QueueRxBytes(uint8 instance, const uint8 *data, uint16 length)
{
    Stub_UartInstanceType *u;

    if ((instance >= STUB_UART_INSTANCE_COUNT) || (data == NULL_PTR))
    {
        return;
    }
    u = &Stub_Uarts[instance];
    if (((uint32)u->rxLen + (uint32)length) > (uint32)STUB_UART_BUFFER_SIZE)
    {
        (void)fprintf(stderr, "\nStub_Uart: RX queue overflow on instance %u\n", instance);
        abort();
    }
    (void)memcpy(&u->rx[u->rxLen], data, length);
    u->rxLen = (uint16)(u->rxLen + length);
}

void Stub_Uart_SetTxResponse(uint8 instance, const uint8 *data, uint16 length, boolean repeat)
{
    Stub_UartInstanceType *u;

    if (instance >= STUB_UART_INSTANCE_COUNT)
    {
        return;
    }
    u = &Stub_Uarts[instance];

    if ((data == NULL_PTR) || (length == 0u) || (length > (uint16)STUB_UART_BUFFER_SIZE))
    {
        u->responseArmed = FALSE;
        u->responseLen = 0u;
        return;
    }

    (void)memcpy(u->response, data, length);
    u->responseLen = length;
    u->responseArmed = TRUE;
    u->responseRepeat = repeat;
}

uint32 Stub_Uart_GetTxResponseCount(uint8 instance)
{
    return (instance < STUB_UART_INSTANCE_COUNT) ? Stub_Uarts[instance].responseCount : 0u;
}

void Stub_Uart_QueueTxResponse(uint8 instance, const uint8 *data, uint16 length)
{
    Stub_UartInstanceType *u;

    if (instance >= STUB_UART_INSTANCE_COUNT)
    {
        return;
    }
    u = &Stub_Uarts[instance];

    if (u->scriptCount >= (uint8)STUB_UART_SCRIPT_DEPTH)
    {
        /* Aborting rather than silently dropping: a script longer than the stub can hold would make
         * the test pass or fail on entries it never delivered, which is worse than not running. */
        abort();
    }
    if (length > (uint16)STUB_UART_SCRIPT_FRAME_MAX)
    {
        abort();
    }

    if ((data != NULL_PTR) && (length > 0u))
    {
        (void)memcpy(u->script[u->scriptCount], data, length);
        u->scriptLen[u->scriptCount] = length;
    }
    else
    {
        /* A silent turn. */
        u->scriptLen[u->scriptCount] = 0u;
    }

    u->scriptCount++;
}

void Stub_Uart_QueueTxSilence(uint8 instance)
{
    Stub_Uart_QueueTxResponse(instance, NULL_PTR, 0u);
}

uint8 Stub_Uart_GetScriptRemaining(uint8 instance)
{
    const Stub_UartInstanceType *u;

    if (instance >= STUB_UART_INSTANCE_COUNT)
    {
        return 0u;
    }
    u = &Stub_Uarts[instance];

    return (uint8)(u->scriptCount - u->scriptNext);
}

void Stub_Uart_QueueRxString(uint8 instance, const char *text)
{
    if (text != NULL_PTR)
    {
        Stub_Uart_QueueRxBytes(instance, (const uint8 *)text, (uint16)strlen(text));
    }
}

uint16 Stub_Uart_GetTxLength(uint8 instance)
{
    return (instance < STUB_UART_INSTANCE_COUNT) ? Stub_Uarts[instance].txLen : 0u;
}

const uint8 *Stub_Uart_GetTxBuffer(uint8 instance)
{
    return (instance < STUB_UART_INSTANCE_COUNT) ? Stub_Uarts[instance].tx : NULL_PTR;
}

void Stub_Uart_ClearTx(uint8 instance)
{
    if (instance < STUB_UART_INSTANCE_COUNT)
    {
        Stub_Uarts[instance].txLen = 0u;
    }
}

uint16 Stub_Uart_GetRxRemaining(uint8 instance)
{
    return (instance < STUB_UART_INSTANCE_COUNT)
               ? (uint16)(Stub_Uarts[instance].rxLen - Stub_Uarts[instance].rxPos)
               : 0u;
}

uint32 Stub_Uart_GetConfiguredBaud(uint8 instance)
{
    return (instance < STUB_UART_INSTANCE_COUNT) ? Stub_Uarts[instance].baud : 0u;
}

void Stub_Uart_FailNextWrites(uint8 instance, uint32 count)
{
    if (instance < STUB_UART_INSTANCE_COUNT)
    {
        Stub_Uarts[instance].failWrites = count;
    }
}

/*==================================================================================================
 *  SECTION 7 : Spi
 *================================================================================================*/

static uint8 Stub_SpiRx[STUB_UART_BUFFER_SIZE];
static uint16 Stub_SpiRxLen;
static uint16 Stub_SpiRxPos;
static uint8 Stub_SpiTx[STUB_UART_BUFFER_SIZE];
static uint16 Stub_SpiTxLen;
static Spi_DeviceType Stub_SpiOwner = SPI_DEVICE_NONE;
static Spi_StatisticsType Stub_SpiStats;

Std_ReturnType Spi_Init(void)
{
    Stub_SpiOwner = SPI_DEVICE_NONE;
    return E_OK;
}

Std_ReturnType Spi_Lock(Spi_DeviceType device, uint32 timeoutMs)
{
    COMPILER_UNUSED(timeoutMs);
    if (device >= (Spi_DeviceType)SPI_DEVICE_COUNT)
    {
        return E_NOT_OK;
    }
    if (Stub_SpiOwner != SPI_DEVICE_NONE)
    {
        Stub_SpiStats.lockContentionCount++;
        return E_BUSY;
    }
    Stub_SpiOwner = device;
    return E_OK;
}

Std_ReturnType Spi_Unlock(Spi_DeviceType device)
{
    if (Stub_SpiOwner != device)
    {
        return E_NOT_OK;
    }
    Stub_SpiOwner = SPI_DEVICE_NONE;
    return E_OK;
}

Spi_DeviceType Spi_GetOwner(void)
{
    return Stub_SpiOwner;
}

static Std_ReturnType Stub_SpiShift(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                    uint16 length)
{
    uint16 i;

    if (Stub_SpiOwner != device)
    {
        return E_NOT_OK;
    }
    if ((txData == NULL_PTR) && (rxData == NULL_PTR))
    {
        return E_NOT_OK;
    }
    if (((uint32)Stub_SpiTxLen + (uint32)length) > (uint32)STUB_UART_BUFFER_SIZE)
    {
        return E_NO_SPACE;
    }

    for (i = 0u; i < length; i++)
    {
        Stub_SpiTx[Stub_SpiTxLen] = (txData != NULL_PTR) ? txData[i] : (uint8)SPI_DUMMY_BYTE;
        Stub_SpiTxLen++;

        if (rxData != NULL_PTR)
        {
            /* Past the end of the scripted response the bus reads as all-ones, which is
             * what a real bus with no driver on MISO produces. */
            rxData[i] = (Stub_SpiRxPos < Stub_SpiRxLen) ? Stub_SpiRx[Stub_SpiRxPos]
                                                       : (uint8)SPI_DUMMY_BYTE;
        }
        if (Stub_SpiRxPos < Stub_SpiRxLen)
        {
            Stub_SpiRxPos++;
        }
    }

    Stub_SpiStats.transferCount++;
    Stub_SpiStats.bytesTransferred += length;
    return E_OK;
}

Std_ReturnType Spi_Transfer(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                            uint16 length)
{
    return Stub_SpiShift(device, txData, rxData, length);
}

Std_ReturnType Spi_TransferContinuous(Spi_DeviceType device, const uint8 *txData, uint8 *rxData,
                                      uint16 length)
{
    return Stub_SpiShift(device, txData, rxData, length);
}

Std_ReturnType Spi_ChipSelectAssert(Spi_DeviceType device)
{
    return (Stub_SpiOwner == device) ? E_OK : E_NOT_OK;
}

Std_ReturnType Spi_ChipSelectDeassert(Spi_DeviceType device)
{
    return (Stub_SpiOwner == device) ? E_OK : E_NOT_OK;
}

Std_ReturnType Spi_GetStatistics(Spi_StatisticsType *stats)
{
    if (stats == NULL_PTR)
    {
        return E_NOT_OK;
    }
    *stats = Stub_SpiStats;
    return E_OK;
}

void Spi_GetVersionInfo(Std_VersionInfoType *versioninfo)
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

void Stub_Spi_QueueRxBytes(const uint8 *data, uint16 length)
{
    if ((data == NULL_PTR) || (((uint32)Stub_SpiRxLen + (uint32)length) >
                               (uint32)STUB_UART_BUFFER_SIZE))
    {
        return;
    }
    (void)memcpy(&Stub_SpiRx[Stub_SpiRxLen], data, length);
    Stub_SpiRxLen = (uint16)(Stub_SpiRxLen + length);
}

uint16 Stub_Spi_GetTxLength(void)
{
    return Stub_SpiTxLen;
}

const uint8 *Stub_Spi_GetTxBuffer(void)
{
    return Stub_SpiTx;
}

/*==================================================================================================
 *  SECTION 8 : Wdg
 *================================================================================================*/

static uint32 Stub_WdgTriggers;
static uint32 Stub_WdgTimeoutMs;
static Wdg_ModeType Stub_WdgMode = WDG_MODE_OFF;

Std_ReturnType Wdg_Init(void)
{
    Stub_WdgMode = WDG_MODE_SLOW;
    Stub_WdgTimeoutMs = WDG_TIMEOUT_SLOW_MS;
    return E_OK;
}

Std_ReturnType Wdg_SetMode(Wdg_ModeType mode)
{
    switch (mode)
    {
    case WDG_MODE_OFF:
#if (WDG_ALLOW_DISABLE == STD_ON)
        Stub_WdgTimeoutMs = 0u;
        break;
#else
        return E_NOT_OK;
#endif
    case WDG_MODE_SLOW:
        Stub_WdgTimeoutMs = WDG_TIMEOUT_SLOW_MS;
        break;
    case WDG_MODE_FAST:
        Stub_WdgTimeoutMs = WDG_TIMEOUT_FAST_MS;
        break;
    default:
        return E_NOT_OK;
    }
    Stub_WdgMode = mode;
    return E_OK;
}

Std_ReturnType Wdg_SubscribeCurrentTask(void)
{
    return E_OK;
}

Std_ReturnType Wdg_UnsubscribeCurrentTask(void)
{
    return E_OK;
}

void Wdg_Trigger(void)
{
    Stub_WdgTriggers++;
}

uint32 Wdg_GetTimeoutMs(void)
{
    return Stub_WdgTimeoutMs;
}

Wdg_ModeType Wdg_GetMode(void)
{
    return Stub_WdgMode;
}

void Wdg_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = WDG_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_WDG;
        versioninfo->sw_major_version = WDG_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = WDG_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = WDG_SW_PATCH_VERSION;
    }
}

uint32 Stub_Wdg_GetTriggerCount(void)
{
    return Stub_WdgTriggers;
}

uint32 Stub_Wdg_GetConfiguredTimeoutMs(void)
{
    return Stub_WdgTimeoutMs;
}

boolean Stub_Wdg_IsDisabled(void)
{
    return (Stub_WdgMode == WDG_MODE_OFF) ? TRUE : FALSE;
}

/*==================================================================================================
 *  SECTION 9 : Fls -- emulated flash with fault injection
 *================================================================================================*/

static uint8 Stub_FlsMedia[STUB_FLS_SIZE];
static uint32 Stub_FlsFailWrites;
static uint32 Stub_FlsFailReads;
static uint32 Stub_FlsTruncateNth;
static uint32 Stub_FlsTruncateBytes;
static uint32 Stub_FlsWriteSeq;
static boolean Stub_FlsAddrFaultArmed;
static uint32 Stub_FlsFaultAddress;
static uint32 Stub_FlsFaultPartial;
static Fls_StatisticsType Stub_FlsStats;
static Fls_JobResultType Stub_FlsJob = FLS_JOB_OK;

Std_ReturnType Fls_Init(void)
{
    Stub_FlsJob = FLS_JOB_OK;
    return E_OK;
}

static boolean Stub_FlsRangeOk(Fls_AddressType address, Fls_LengthType length)
{
    return (((uint64)address + (uint64)length) <= (uint64)STUB_FLS_SIZE) ? TRUE : FALSE;
}

Std_ReturnType Fls_Read(Fls_AddressType address, uint8 *buffer, Fls_LengthType length)
{
    if ((buffer == NULL_PTR) || (Stub_FlsRangeOk(address, length) == FALSE))
    {
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }
    if (Stub_FlsFailReads > 0u)
    {
        Stub_FlsFailReads--;
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }
    (void)memcpy(buffer, &Stub_FlsMedia[address], length);
    Stub_FlsStats.readCount++;
    Stub_FlsJob = FLS_JOB_OK;
    return E_OK;
}

Std_ReturnType Fls_Write(Fls_AddressType address, const uint8 *buffer, Fls_LengthType length)
{
    Fls_LengthType i;
    Fls_LengthType writable = length;
    boolean truncated = FALSE;

    if ((buffer == NULL_PTR) || (Stub_FlsRangeOk(address, length) == FALSE))
    {
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }
    if (((address % FLS_WRITE_ALIGNMENT) != 0u) || ((length % FLS_WRITE_ALIGNMENT) != 0u))
    {
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    Stub_FlsWriteSeq++;

    if ((Stub_FlsTruncateNth != 0u) && (Stub_FlsWriteSeq == Stub_FlsTruncateNth))
    {
        writable = (Stub_FlsTruncateBytes < length) ? Stub_FlsTruncateBytes : length;
        truncated = TRUE;
    }

    /* Address-targeted fault: emulates the supply collapsing while this particular region was
     * being programmed. Matched on the start address rather than the range, because record
     * writes overlap -- a header write spans the commit byte that a later write clears, and a
     * range match would sabotage the wrong one. Disarms itself so a test can continue
     * afterwards and verify recovery. */
    if ((Stub_FlsAddrFaultArmed != FALSE) && (Stub_FlsFaultAddress == address))
    {
        const uint32 limit = Stub_FlsFaultPartial;

        writable = (limit < length) ? limit : length;
        truncated = TRUE;
        Stub_FlsAddrFaultArmed = FALSE;
    }

    /* Real NOR flash can only clear bits: programming over a non-erased cell ANDs the
     * new value into the old one. Modelling that faithfully is what makes the Fee tests
     * meaningful -- a stub that simply assigned would let a missing erase pass. */
    for (i = 0u; i < writable; i++)
    {
        Stub_FlsMedia[address + i] &= buffer[i];
    }
    Stub_FlsStats.bytesWritten += writable;

    if (truncated != FALSE)
    {
        Stub_FlsStats.writeFailures++;
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }
    if (Stub_FlsFailWrites > 0u)
    {
        Stub_FlsFailWrites--;
        Stub_FlsStats.writeFailures++;
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }

    Stub_FlsStats.writeCount++;
    Stub_FlsJob = FLS_JOB_OK;
    return E_OK;
}

Std_ReturnType Fls_Erase(Fls_AddressType address, Fls_LengthType length)
{
    if ((Stub_FlsRangeOk(address, length) == FALSE) ||
        ((address % FLS_SECTOR_SIZE) != 0u) || ((length % FLS_SECTOR_SIZE) != 0u))
    {
        Stub_FlsJob = FLS_JOB_FAILED;
        return E_NOT_OK;
    }
    (void)memset(&Stub_FlsMedia[address], (int)FLS_ERASED_VALUE, length);
    Stub_FlsStats.eraseCount += (length / FLS_SECTOR_SIZE);
    Stub_FlsJob = FLS_JOB_OK;
    return E_OK;
}

Std_ReturnType Fls_Compare(Fls_AddressType address, const uint8 *buffer, Fls_LengthType length)
{
    if ((buffer == NULL_PTR) || (Stub_FlsRangeOk(address, length) == FALSE))
    {
        return E_NOT_OK;
    }
    return (memcmp(&Stub_FlsMedia[address], buffer, length) == 0) ? E_OK : E_NOT_OK;
}

Std_ReturnType Fls_BlankCheck(Fls_AddressType address, Fls_LengthType length)
{
    Fls_LengthType i;

    if (Stub_FlsRangeOk(address, length) == FALSE)
    {
        return E_NOT_OK;
    }
    for (i = 0u; i < length; i++)
    {
        if (Stub_FlsMedia[address + i] != (uint8)FLS_ERASED_VALUE)
        {
            return E_NOT_OK;
        }
    }
    return E_OK;
}

Fls_JobResultType Fls_GetJobResult(void)
{
    return Stub_FlsJob;
}

Fls_LengthType Fls_GetPartitionSize(void)
{
    return (Fls_LengthType)STUB_FLS_SIZE;
}

Std_ReturnType Fls_GetStatistics(Fls_StatisticsType *stats)
{
    if (stats == NULL_PTR)
    {
        return E_NOT_OK;
    }
    *stats = Stub_FlsStats;
    return E_OK;
}

void Fls_GetVersionInfo(Std_VersionInfoType *versioninfo)
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

void Stub_Fls_FailNextWrites(uint32 count)
{
    Stub_FlsFailWrites = count;
}

void Stub_Fls_FailNextReads(uint32 count)
{
    Stub_FlsFailReads = count;
}

void Stub_Fls_TruncateWrite(uint32 n, uint32 bytesWritten)
{
    Stub_FlsTruncateNth = n;
    Stub_FlsTruncateBytes = bytesWritten;
}

void Stub_Fls_FailWriteAtAddress(uint32 address, uint32 partial)
{
    Stub_FlsAddrFaultArmed = TRUE;
    Stub_FlsFaultAddress = address;
    Stub_FlsFaultPartial = partial;
}

void Stub_Fls_ClearFaults(void)
{
    Stub_FlsFailWrites = 0u;
    Stub_FlsFailReads = 0u;
    Stub_FlsTruncateNth = 0u;
    Stub_FlsTruncateBytes = 0u;
    Stub_FlsAddrFaultArmed = FALSE;
    Stub_FlsJob = FLS_JOB_OK;
}

uint32 Stub_Fls_GetWriteSequence(void)
{
    return Stub_FlsWriteSeq;
}

void Stub_Fls_Corrupt(uint32 address, uint8 value)
{
    if (address < (uint32)STUB_FLS_SIZE)
    {
        Stub_FlsMedia[address] = value;
    }
}

uint8 Stub_Fls_Peek(uint32 address)
{
    return (address < (uint32)STUB_FLS_SIZE) ? Stub_FlsMedia[address] : (uint8)FLS_ERASED_VALUE;
}

boolean Stub_Fls_CorruptLastWrittenByte(void)
{
    uint32 index = (uint32)STUB_FLS_SIZE;

    while (index > 0u)
    {
        index--;

        if (Stub_FlsMedia[index] != (uint8)FLS_ERASED_VALUE)
        {
            const uint8 value = Stub_FlsMedia[index];

            if (value == 0u)
            {
                /* No bit left to clear here; keep looking backwards rather than reporting a
                 * corruption that did not happen. */
                continue;
            }

            Stub_FlsMedia[index] = (uint8)(value & (uint8)(value - 1u));
            return TRUE;
        }
    }

    return FALSE;
}

uint32 Stub_Fls_GetTotalBytesWritten(void)
{
    return Stub_FlsStats.bytesWritten;
}

uint32 Stub_Fls_GetEraseCount(void)
{
    return Stub_FlsStats.eraseCount;
}

/*==================================================================================================
 *  SECTION 10 : Global reset
 *================================================================================================*/

void Stub_Mcal_ResetAll(void)
{
    /* Gpt: start at a non-zero time so that a timestamp of 0 is distinguishable from
     * "never set", which several modules rely on. */
    Stub_TimeUs = 1000u;
    Stub_DelayTotalMs = 0u;
    Stub_DelayCalls = 0u;
    Stub_DelayAdvances = TRUE;

    Stub_ResetReason = (uint8)MCU_RESET_POWER_ON;
    Stub_ResetCount = 0u;
    Stub_ResetArmed = FALSE;
    Stub_HeapFree = 180000uL;
    Stub_HeapMinFree = 150000uL;
    Stub_HeapLargest = 96000uL;
    {
        static const uint8 defaultId[MCU_DEVICE_ID_LENGTH] = {0x24u, 0x6Fu, 0x28u,
                                                              0xAAu, 0xBBu, 0xCCu};
        (void)memcpy(Stub_DeviceId, defaultId, sizeof(Stub_DeviceId));
    }

    (void)memset(Stub_DioLevel, 0, sizeof(Stub_DioLevel));
    (void)memset(Stub_DioInput, 0, sizeof(Stub_DioInput));
    (void)memset(Stub_DioWrites, 0, sizeof(Stub_DioWrites));
    (void)memset(Stub_DioToggles, 0, sizeof(Stub_DioToggles));

    (void)memset(Stub_AdcRaw, 0, sizeof(Stub_AdcRaw));
    Stub_AdcInitialised = FALSE;
    Stub_AdcReadCount = 0u;
    Stub_AdcSequenceLen = 0u;
    Stub_AdcSequenceNext = 0u;
    Stub_AdcFailReads = 0u;

    (void)memset(Stub_Uarts, 0, sizeof(Stub_Uarts));

    (void)memset(Stub_SpiRx, 0, sizeof(Stub_SpiRx));
    (void)memset(Stub_SpiTx, 0, sizeof(Stub_SpiTx));
    Stub_SpiRxLen = 0u;
    Stub_SpiRxPos = 0u;
    Stub_SpiTxLen = 0u;
    Stub_SpiOwner = SPI_DEVICE_NONE;
    (void)memset(&Stub_SpiStats, 0, sizeof(Stub_SpiStats));

    Stub_WdgTriggers = 0u;
    Stub_WdgTimeoutMs = 0u;
    Stub_WdgMode = WDG_MODE_OFF;

    /* Flash comes up erased, as a virgin part does. */
    (void)memset(Stub_FlsMedia, (int)FLS_ERASED_VALUE, sizeof(Stub_FlsMedia));
    Stub_FlsFailWrites = 0u;
    Stub_FlsFailReads = 0u;
    Stub_FlsTruncateNth = 0u;
    Stub_FlsTruncateBytes = 0u;
    Stub_FlsWriteSeq = 0u;
    Stub_FlsAddrFaultArmed = FALSE;
    Stub_FlsFaultAddress = 0u;
    Stub_FlsFaultPartial = 0u;
    Stub_FlsJob = FLS_JOB_OK;
    (void)memset(&Stub_FlsStats, 0, sizeof(Stub_FlsStats));
}

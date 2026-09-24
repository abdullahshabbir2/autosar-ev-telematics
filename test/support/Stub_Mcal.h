/**
 * @file    Stub_Mcal.h
 * @brief   Control surface for the MCAL test doubles.
 *
 * The production stack reaches hardware only through the MCAL. Replacing that one
 * layer with controllable doubles makes every module above it testable without a
 * board, and -- more importantly -- testable *deterministically*: time advances
 * only when a test says so, a bus returns exactly the bytes a test scripted, and a
 * flash write fails exactly when a test wants it to.
 *
 * That last capability is the point. Nearly every defect this architecture is meant
 * to prevent lives on an error path -- a brown-out during an NvM write, a truncated
 * RS485 response, an SD card that stops acknowledging. Those paths are unreachable
 * on a bench and are the ones that matter in a vehicle.
 *
 * @par Usage pattern
 * @code
 *   void setUp(void) { Stub_Mcal_ResetAll(); }   // every test starts clean
 *
 *   Stub_Gpt_AdvanceMs(5000u);                   // make a timeout expire
 *   Stub_Uart_QueueRxBytes(0u, frame, sizeof frame);
 *   Stub_Fls_FailNextWrites(1u);                 // inject one write failure
 * @endcode
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef STUB_MCAL_H
#define STUB_MCAL_H

#include <setjmp.h>

#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reset every stub to its power-on state. Call from every suite's setUp(). */
void Stub_Mcal_ResetAll(void);

/*==================================================================================================
 *  Gpt -- virtual time
 *
 *  Time never advances on its own. A test that wants a 30 s timeout to expire says
 *  so in one line and the suite still runs in microseconds; nothing sleeps.
 *================================================================================================*/

/** Move virtual time forward by @p ms milliseconds. */
void Stub_Gpt_AdvanceMs(uint32 ms);

/** Move virtual time forward by @p us microseconds (sub-millisecond resolution). */
void Stub_Gpt_AdvanceUs(uint64 us);

/** Force the millisecond counter to @p ms, to place a test near the 32-bit wrap. */
void Stub_Gpt_SetMonotonicMs(uint32 ms);

/** Total milliseconds passed to Gpt_DelayMs() since the last reset. */
uint32 Stub_Gpt_GetTotalDelayMs(void);

/** Number of Gpt_DelayMs()/Gpt_DelayUs() calls since the last reset. */
uint32 Stub_Gpt_GetDelayCallCount(void);

/**
 * @brief Make Gpt_DelayMs() also advance virtual time (default: it does).
 *
 * Switched off to prove that a state machine makes progress on its own rather than
 * relying on a blocking delay for its timing.
 */
void Stub_Gpt_SetDelayAdvancesTime(boolean enable);

/*==================================================================================================
 *  Mcu -- reset capture
 *
 *  Mcu_PerformReset() is NORETURN, so a test cannot simply call a function that
 *  resets and then assert. Arming a jump target turns the reset into a catchable
 *  event; leaving it unarmed makes an unexpected reset abort the run loudly.
 *================================================================================================*/

/**
 * @brief The jump buffer Mcu_PerformReset() longjmps into when capture is armed.
 *
 * ::setjmp must be called from the test's own stack frame, so the buffer is handed out
 * rather than wrapped. The full pattern is:
 * @code
 *   if (setjmp(*Stub_Mcu_GetResetJmpBuf()) == 0)
 *   {
 *       Stub_Mcu_ArmResetCapture();
 *       Det_Panic(MODULE_ID_DEM, 0u, 0u);   // must not return
 *       TEST_FAIL_MESSAGE("expected a reset");
 *   }
 *   TEST_ASSERT_TRUE(Stub_Mcu_WasResetRequested());
 * @endcode
 */
jmp_buf *Stub_Mcu_GetResetJmpBuf(void);

/** Arm reset capture, so the next Mcu_PerformReset() longjmps instead of aborting. */
void Stub_Mcu_ArmResetCapture(void);

/** TRUE if Mcu_PerformReset() has been called since the last reset of the stubs. */
boolean Stub_Mcu_WasResetRequested(void);

/** Number of Mcu_PerformReset() calls since the last reset of the stubs. */
uint32 Stub_Mcu_GetResetCount(void);

/** Set what Mcu_GetResetReason() reports (default ::MCU_RESET_POWER_ON). */
void Stub_Mcu_SetResetReason(uint8 reason);

/** Set the 6-byte device identity Mcu_GetDeviceId() returns. */
void Stub_Mcu_SetDeviceId(const uint8 *id);

/** Set the heap figures Mcu_GetHeapInfo() reports. */
void Stub_Mcu_SetHeapFree(uint32 freeBytes, uint32 minFreeBytes, uint32 largestBlock);

/*==================================================================================================
 *  Dio -- pin state capture
 *================================================================================================*/

/** Highest channel id the Dio stub tracks. */
#define STUB_DIO_CHANNEL_COUNT 48u

/** Last level written to @p channel, or STD_LOW if never written. */
uint8 Stub_Dio_GetLevel(uint8 channel);

/** Number of Dio_WriteChannel() calls targeting @p channel. */
uint32 Stub_Dio_GetWriteCount(uint8 channel);

/** Number of level *transitions* on @p channel -- what an LED blink test counts. */
uint32 Stub_Dio_GetToggleCount(uint8 channel);

/** Set the level Dio_ReadChannel() reports for @p channel. */
void Stub_Dio_SetInputLevel(uint8 channel, uint8 level);

/*==================================================================================================
 *  Adc -- analogue input
 *================================================================================================*/

/** Set the raw count Adc_ReadChannel() returns for @p channel. */
void Stub_Adc_SetRaw(uint8 channel, uint16 raw);

/** Make the next @p count Adc_ReadChannel() calls fail. */
void Stub_Adc_FailNextReads(uint32 count);

/*==================================================================================================
 *  Uart -- scripted serial bus
 *
 *  Models the two directions independently: a test queues the bytes the peer will
 *  send, runs the code under test, then inspects what the code transmitted. That is
 *  enough to drive the whole RS485 battery protocol, including truncated frames,
 *  CRC corruption and silence.
 *================================================================================================*/

/** Number of UART instances the stub models (RS485, GNSS, GSM). */
#define STUB_UART_INSTANCE_COUNT 3u

/** Capacity of each direction's buffer, per instance. */
#define STUB_UART_BUFFER_SIZE 1024u

/** Append bytes for the code under test to receive from @p instance. */
void Stub_Uart_QueueRxBytes(uint8 instance, const uint8 *data, uint16 length);

/**
 * @brief Arm a reply that is delivered only *after* the next transmit on @p instance.
 *
 * Bytes queued with ::Stub_Uart_QueueRxBytes are already waiting when the code under test
 * runs, so a correctly written request/response driver purges them before issuing its
 * request -- which means pre-queued bytes can never exercise the success path. This models
 * the causality a real peer has: the reply appears in the receive buffer as a consequence
 * of the request, so it survives the purge.
 *
 * @param instance UART instance.
 * @param data     Reply bytes, or NULL_PTR to disarm.
 * @param length   Reply length. Pass fewer bytes than the protocol expects to emulate a
 *                 truncated frame.
 * @param repeat   TRUE to answer every subsequent transmit, FALSE to answer only the next
 *                 one -- which is how a pack that stops responding mid-round is modelled.
 */
void Stub_Uart_SetTxResponse(uint8 instance, const uint8 *data, uint16 length, boolean repeat);

/** Number of times an armed or scripted reply has been delivered on @p instance. */
uint32 Stub_Uart_GetTxResponseCount(uint8 instance);

/** Entries a scripted reply sequence can hold, and the longest frame in one. */
#define STUB_UART_SCRIPT_DEPTH 64u
#define STUB_UART_SCRIPT_FRAME_MAX 96u

/**
 * @brief Append one reply to a scripted sequence, delivered on the next unanswered transmit.
 *
 * ::Stub_Uart_SetTxResponse arms a single reply, which is enough for a one-device exchange. A bus with
 * several devices needs each request answered differently -- four battery packs, three objects each --
 * and that is what this provides: entries are consumed one per accepted write, in the order queued.
 *
 * A script entry takes precedence over the single armed reply, so a test can script the requests it
 * cares about and leave a repeating reply armed for the rest.
 *
 * @param instance UART instance.
 * @param data     Reply bytes, or NULL_PTR for a silent turn.
 * @param length   Reply length, at most ::STUB_UART_SCRIPT_FRAME_MAX. Fewer bytes than the protocol
 *                 expects emulates a truncated frame; zero emulates a device that does not answer.
 */
void Stub_Uart_QueueTxResponse(uint8 instance, const uint8 *data, uint16 length);

/**
 * @brief Append a silent turn to the scripted sequence.
 *
 * Distinct from simply not queueing anything: a silent *entry* keeps the entries after it aligned with
 * the requests that follow, which is what lets one device in the middle of a polling round go quiet
 * without shifting every later reply onto the wrong request.
 */
void Stub_Uart_QueueTxSilence(uint8 instance);

/** Scripted entries not yet delivered on @p instance. Zero means the script was fully consumed. */
uint8 Stub_Uart_GetScriptRemaining(uint8 instance);

/** Append a NUL-terminated string to @p instance's receive queue. */
void Stub_Uart_QueueRxString(uint8 instance, const char *text);

/** Bytes the code under test has transmitted on @p instance. */
uint16 Stub_Uart_GetTxLength(uint8 instance);

/** Pointer to @p instance's transmit capture. Valid until the next stub reset. */
const uint8 *Stub_Uart_GetTxBuffer(uint8 instance);

/** Discard @p instance's transmit capture without disturbing the receive queue. */
void Stub_Uart_ClearTx(uint8 instance);

/** Bytes still waiting in @p instance's receive queue. */
uint16 Stub_Uart_GetRxRemaining(uint8 instance);

/** Baud rate the code under test configured on @p instance, 0 if never opened. */
uint32 Stub_Uart_GetConfiguredBaud(uint8 instance);

/** Make the next @p count Uart_Write() calls report failure. */
void Stub_Uart_FailNextWrites(uint8 instance, uint32 count);

/*==================================================================================================
 *  Can -- scripted frame queue
 *================================================================================================*/

/** Frames the stub can hold in each direction. */
#define STUB_CAN_QUEUE_DEPTH 32u

/** Append a frame for the code under test to receive. */
void Stub_Can_QueueRxFrame(uint32 canId, const uint8 *data, uint8 dlc, boolean isExtended);

/** Number of frames the code under test has transmitted. */
uint16 Stub_Can_GetTxCount(void);

/** Read back transmitted frame @p index. Returns E_NOT_OK if out of range. */
Std_ReturnType Stub_Can_GetTxFrame(uint16 index, uint32 *canId, uint8 *data, uint8 *dlc);

/** Make Can_Init() report failure, to exercise the no-CAN degraded mode. */
void Stub_Can_SetInitFails(boolean fails);

/** Make the controller report bus-off. */
void Stub_Can_SetBusOff(boolean busOff);

/*==================================================================================================
 *  Fls -- emulated flash for Fee/NvM
 *
 *  A byte-addressable RAM array plus two fault injectors. The write-failure and
 *  power-loss injectors are what let the NvM tests assert the property that
 *  actually matters: after an interrupted write, the previous value is still
 *  readable.
 *================================================================================================*/

/** Size of the emulated flash region, matching Fee's configured partition. */
#define STUB_FLS_SIZE 8192u

/** Make the next @p count Fls_Write() calls report failure. */
void Stub_Fls_FailNextWrites(uint32 count);

/** Make the next @p count Fls_Read() calls report failure. */
void Stub_Fls_FailNextReads(uint32 count);

/**
 * @brief Truncate the @p n-th subsequent write after @p bytesWritten bytes.
 *
 * Emulates a brown-out mid-write: the leading bytes land, the rest do not, and the
 * call reports failure. This is the scenario that turns a naive NV layout into an
 * unrecoverable one on the next boot.
 */
void Stub_Fls_TruncateWrite(uint32 n, uint32 bytesWritten);

/**
 * @brief Fail the next write that *starts* at exactly @p address.
 *
 * Addresses a specific place on the media rather than the Nth write, so a power-fail test
 * stays valid when the implementation changes how many writes it issues to get there.
 *
 * Matching is on the start address, not on the range, because record writes overlap: a record
 * header write spans the commit byte that a later, separate write clears. A range match would
 * sabotage the header instead of the commit and silently test something else entirely.
 *
 * @param address  Exact start offset of the write to sabotage.
 * @param partial  Bytes of the write to let through before failing. 0 rejects it outright; a
 *                 non-zero value emulates a brown-out part way through the programming pulse.
 */
void Stub_Fls_FailWriteAtAddress(uint32 address, uint32 partial);

/** Clear every armed write/read/truncate fault without touching the media contents. */
void Stub_Fls_ClearFaults(void);

/** Number of Fls_Write() calls attempted since the last stub reset. */
uint32 Stub_Fls_GetWriteSequence(void);

/** Overwrite a byte behind the driver's back, to emulate bit rot. */
void Stub_Fls_Corrupt(uint32 address, uint8 value);

/** Read a byte directly, bypassing the driver, to assert on-media layout. */
uint8 Stub_Fls_Peek(uint32 address);

/**
 * @brief Clear one bit of the last byte written to the media, behind the driver's back.
 *
 * Emulates bit rot in the most recently committed record without the stub needing to know anything
 * about the layer above's record format -- the last non-erased byte is necessarily inside the newest
 * record's payload or its CRC, and damaging either is what an integrity check exists to catch.
 *
 * The bit is *cleared* rather than flipped, because programmed flash can only lose bits: setting one
 * would emulate a failure mode the media does not have. `value & (value - 1)` clears the lowest set
 * bit, so it always changes something -- unlike masking with a constant, which on an already-even byte
 * changes nothing and leaves the test asserting that uncorrupted data reads back correctly.
 *
 * @return TRUE if a byte was corrupted; FALSE if the media is entirely erased.
 */
boolean Stub_Fls_CorruptLastWrittenByte(void);

/** Total bytes written since the last stub reset -- a wear proxy. */
uint32 Stub_Fls_GetTotalBytesWritten(void);

/** Number of erase operations since the last stub reset. */
uint32 Stub_Fls_GetEraseCount(void);

/*==================================================================================================
 *  Wdg -- watchdog observation
 *================================================================================================*/

/** Number of Wdg_Trigger() calls since the last stub reset. */
uint32 Stub_Wdg_GetTriggerCount(void);

/** Timeout, in ms, that the code under test configured. */
uint32 Stub_Wdg_GetConfiguredTimeoutMs(void);

/** TRUE if the watchdog has been switched to its disabled mode. */
boolean Stub_Wdg_IsDisabled(void);

/*==================================================================================================
 *  Spi
 *================================================================================================*/

/** Queue bytes that the next Spi_Transfer() calls will shift in. */
void Stub_Spi_QueueRxBytes(const uint8 *data, uint16 length);

/** Bytes shifted out by the code under test. */
uint16 Stub_Spi_GetTxLength(void);

/** Pointer to the transmit capture. Valid until the next stub reset. */
const uint8 *Stub_Spi_GetTxBuffer(void);

#ifdef __cplusplus
}
#endif

#endif /* STUB_MCAL_H */

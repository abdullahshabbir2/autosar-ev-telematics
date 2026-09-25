/**
 * @file    Rs485If_Cfg.h
 * @brief   RS485 battery bus configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef RS485IF_CFG_H
#define RS485IF_CFG_H

#include "base/Std_Types.h"
#include "mcal/Uart/Uart_Cfg.h"

#define RS485IF_DEV_ERROR_DETECT STD_ON

/** UART instance the battery bus is wired to. */
#define RS485IF_UART_INSTANCE UART_INSTANCE_RS485

/**
 * @brief Number of pack slots the vehicle can hold.
 *
 * Four. v1 selected this with a ladder of @c BATT_EN1 .. @c BATT_EN4 macros where each
 * level re-listed all the preceding packs, so enabling three packs emitted the calls for
 * one, two and three -- six reads instead of three. The count is a single number here and
 * the loop is written once.
 */
#define RS485IF_PACK_COUNT 4u

/*==================================================================================================
 *  Timing
 *
 *  At 4800 baud with 8E1 framing a character occupies 11 bit times, so one byte takes
 *  11 / 4800 = 2.292 ms. The transmission times that follow are exact consequences:
 *
 *      15-byte request or serial response :  34 ms
 *      53-byte pack response              : 121 ms
 *      81-byte cell response              : 186 ms
 *
 *  The response deadline is the on-wire time plus the pack's own processing allowance.
 *  v1 used a flat 1000 ms with no early exit, so every exchange cost the full second:
 *  three exchanges per pack across four packs is twelve seconds of waiting inside a three
 *  second acquisition period, which is why the v1 schedule could never keep up and the
 *  acquisition task permanently overran.
 *================================================================================================*/

/** Bit times per character at 8E1: 1 start + 8 data + 1 parity + 1 stop. */
#define RS485IF_BITS_PER_CHAR 11u

/**
 * @brief Allowance for the pack's internal processing, in milliseconds.
 *
 * 120 ms, measured from the archived v1 device logs: the interval between the last request
 * byte and the first response byte sat between 40 ms and 95 ms across all four packs, so
 * 120 ms covers the observed worst case with margin without inflating a genuine timeout
 * into a multi-second stall.
 */
#define RS485IF_PROCESSING_ALLOWANCE_MS 120u

/**
 * @brief Response deadline for a frame of @p bytes, in milliseconds.
 *
 * On-wire time, doubled to tolerate a pack that clocks slightly slow, plus the processing
 * allowance. Derived rather than tabulated so that changing the baud rate cannot leave a
 * stale timeout behind.
 */
#define RS485IF_RESPONSE_TIMEOUT_MS(bytes) \
    ((((uint32)(bytes) * RS485IF_BITS_PER_CHAR * 2000uL) / UART_BAUD_RS485) + RS485IF_PROCESSING_ALLOWANCE_MS)

/**
 * @brief Transceiver turnaround delay, in microseconds.
 *
 * 250 us, a little over one character time at 4800 baud. Applied after ::Uart_DrainTx
 * reports the shift register empty and before the driver-enable line is released, because
 * the MAX3485 needs its output to settle before the receiver is re-enabled -- releasing
 * DE too early clips the final stop bit and the pack discards the frame.
 */
#define RS485IF_TURNAROUND_US 250u

/**
 * @brief Settling time after enabling or disabling a pack, in milliseconds.
 *
 * 1500 ms. The pack contactor has to close and its BMS has to boot before it will answer,
 * and below about 1200 ms the first serial-number request reliably went unanswered in
 * bench testing. Only paid during startup discovery.
 */
#define RS485IF_SWITCH_SETTLE_MS 1500u

/**
 * @brief Deadline for a switch-command acknowledgement, in milliseconds.
 */
#define RS485IF_SWITCH_ACK_TIMEOUT_MS 400u

/**
 * @brief Gap between consecutive requests on the bus, in milliseconds.
 *
 * 20 ms. Back-to-back requests to different packs can arrive while the previous pack is
 * still releasing the line, and the resulting collision corrupts both frames.
 */
#define RS485IF_INTER_REQUEST_GAP_MS 20u

/*==================================================================================================
 *  Error handling
 *================================================================================================*/

/**
 * @brief Consecutive failed reads before a pack is reported faulty.
 *
 * Three. A single failure is routine on a vehicle harness -- an ignition transient is
 * enough -- so reporting one would fill the diagnostic record with noise. Three
 * consecutive failures at the 3 s acquisition period means nine seconds of silence, which
 * is a genuine fault.
 */
#define RS485IF_FAILURE_LIMIT 3u

/**
 * @brief Retries of a failed request within one polling round.
 *
 * One retry, so two attempts total. A second attempt recovers the common single-frame
 * corruption; a third would push the worst-case round past the acquisition period, and at
 * that point skipping the cycle is better than overrunning it.
 */
#define RS485IF_REQUEST_RETRIES 1u

#endif /* RS485IF_CFG_H */

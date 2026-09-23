/**
 * @file    Can_Cfg.h
 * @brief   CAN controller configuration -- bit timing, filters, queue sizing.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef CAN_CFG_H
#define CAN_CFG_H

#include "Ecu_PinMap.h"
#include "Std_Types.h"

#define CAN_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Bit timing
 *
 *  Derived rather than copied from a table, so that fitting a different crystal is a
 *  predictable change. For the MCP2515:
 *
 *      T_Q       = 2 * (BRP + 1) / F_OSC
 *      bit time  = (SyncSeg + PropSeg + PS1 + PS2) * T_Q      , SyncSeg is always 1
 *
 *  With F_OSC = 16 MHz, BRP = 0:
 *      T_Q      = 2 / 16e6            = 125 ns
 *      bit time = (1 + 1 + 7 + 7) * 125 ns = 16 * 125 ns = 2.0 us  ->  500 kbit/s
 *
 *  PS1 = PS2 = 7 TQ places the sample point at (1 + 1 + 7) / 16 = 56.25 % of the bit.
 *  That is slightly early against the usual 75 % recommendation, and deliberately so:
 *  this bus is a short in-vehicle stub where propagation delay is negligible, and the
 *  wider PS2 buys more resynchronisation margin against the oscillator tolerance of a
 *  low-cost MCP2515 breakout. SJW = 1 TQ is sufficient for a +/-0.5 % crystal.
 *
 *  Register encoding (MCP2515 datasheet section 5.3):
 *      CNF1 = (SJW - 1) << 6 | BRP                      = 0x00
 *      CNF2 = BTLMODE << 7 | SAM << 6
 *             | (PS1 - 1) << 3 | (PropSeg - 1)          = 0xF0
 *      CNF3 = SOF << 7 | WAKFIL << 6 | (PS2 - 1)        = 0x86
 *
 *  BTLMODE = 1 so PS2 comes from CNF3 rather than being derived. SAM = 1 samples the
 *  bus three times per bit, which is worth the small extra latency on a vehicle
 *  harness. SOF = 1 exposes start-of-frame on the CLKOUT pin, harmless and useful when
 *  a scope is attached.
 *================================================================================================*/

/** Crystal fitted to the MCP2515 module, in Hz. Change this and the CNF values below. */
#define CAN_CRYSTAL_FREQUENCY_HZ 16000000uL

/** Nominal bus bit rate, in bits per second. */
#define CAN_BAUDRATE_BPS 500000uL

#define CAN_CNF1_VALUE 0x00u /**< SJW = 1 TQ, BRP = 0.                        */
#define CAN_CNF2_VALUE 0xF0u /**< BTLMODE = 1, SAM = 1, PS1 = 7 TQ, Prop = 1 TQ. */
#define CAN_CNF3_VALUE 0x86u /**< SOF = 1, PS2 = 7 TQ.                         */

/** Time quanta per bit, for the timing assertion below. */
#define CAN_TQ_PER_BIT 16u

/*==================================================================================================
 *  Acceptance filters
 *
 *  Only the two motor-controller frames are accepted. The MCP2515 has two receive
 *  buffers with independent masks; both are pointed at the same two identifiers, which
 *  turns them into a 2-deep hardware FIFO. That matters because these frames arrive in
 *  a tight pair and a single buffer would drop the second one whenever the software
 *  read is even slightly late.
 *
 *  @par Frames deliberately not accepted
 *  v1 also collected identifiers 0x610, 0x600-0x6FF and 0x190 into @c id_buff and
 *  @c msg_buff. Nothing ever read those buffers -- @c dump_cycle() was declared but
 *  never called -- so the collection was dead code that consumed most of the receive
 *  cycle's time budget. The battery packs are reached over RS485 on this vehicle, not
 *  CAN. They are omitted here; if a CAN-attached BMS variant appears, it gets its own
 *  filter entry and a CanIf PDU, not a raw buffer.
 *================================================================================================*/

/**
 * @brief Motor controller frame: direction, speed mode, RPM, fault code, power mode.
 *
 * 29-bit extended identifier. Layout is documented in the CAN matrix,
 * @ref docs/08-protocols.md, and decoded by ::CanIf_DecodeMcuDriveState.
 */
#define CAN_ID_MCU_DRIVE_STATE 0x10F8109AuL

/** Motor controller frame: DC-link voltage and current. 29-bit extended identifier. */
#define CAN_ID_MCU_CURRENT_VOLTAGE 0x10F8108DuL

/** Number of acceptance filter entries programmed. */
#define CAN_FILTER_COUNT 2u

/** Mask applied to both receive buffers: exact match on all 29 identifier bits. */
#define CAN_FILTER_MASK_EXTENDED 0x1FFFFFFFuL

/*==================================================================================================
 *  Software receive queue
 *================================================================================================*/

/**
 * @brief Depth of the software receive queue, in frames.
 *
 * 16. The motor controller emits its pair of frames at about 20 Hz, so roughly 40
 * frames per second. ::Can_MainFunction_Read runs every 10 ms and therefore sees at
 * most one pair per call; 16 slots absorb an eightfold burst, which covers the worst
 * observed case of the scheduler being held off by an SD write.
 */
#define CAN_RX_QUEUE_DEPTH 16u

/**
 * @brief Frames moved from controller to queue per ::Can_MainFunction_Read call.
 *
 * Bounded so that a babbling node cannot make one scheduler slot run long. Both
 * hardware buffers plus a margin.
 */
#define CAN_MAX_FRAMES_PER_CYCLE 4u

/*==================================================================================================
 *  Error handling
 *================================================================================================*/

/**
 * @brief Delay before attempting bus-off recovery, in milliseconds.
 *
 * 5 s. Bus-off on this vehicle almost always means a disconnected harness, and
 * immediate retry would produce a continuous stream of error frames on a bus that may
 * be shared with something that matters.
 */
#define CAN_BUS_OFF_RECOVERY_MS 5000u

/** Transmit error counter above which the error-warning event is raised. */
#define CAN_ERROR_WARNING_THRESHOLD 96u

/**
 * @brief Attempts made by ::Can_Init before it gives up.
 *
 * Three. The MCP2515 occasionally needs a second reset when its supply rises slowly
 * alongside the ESP32's, which is exactly the condition at power-on in a vehicle.
 */
#define CAN_INIT_RETRY_COUNT 3u

/** Settling delay after a controller reset command, in milliseconds. */
#define CAN_RESET_SETTLE_MS 10u

/*==================================================================================================
 *  Timing sanity check
 *================================================================================================*/

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
/* 2 * (BRP + 1) with BRP = 0 gives the TQ divisor; the product must land on the
 * configured bit rate exactly, or the configuration is internally inconsistent. */
_Static_assert((CAN_CRYSTAL_FREQUENCY_HZ / (2uL * CAN_TQ_PER_BIT)) == CAN_BAUDRATE_BPS,
               "Can_Cfg.h: CNF values, crystal frequency and bit rate disagree");
#endif

#endif /* CAN_CFG_H */

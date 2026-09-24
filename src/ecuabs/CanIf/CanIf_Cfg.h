/**
 * @file    CanIf_Cfg.h
 * @brief   CAN interface configuration: signal layout and ageing limits.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef CANIF_CFG_H
#define CANIF_CFG_H

#include "mcal/Can/Can_Cfg.h"
#include "base/Std_Types.h"

#define CANIF_DEV_ERROR_DETECT STD_ON

/*==================================================================================================
 *  Byte order
 *================================================================================================*/

/**
 * @brief Byte order of multi-byte signals in the motor controller frames.
 *
 * STD_ON for Intel (little endian), which is what the v1 decoders actually implemented despite a
 * comment in the same file claiming the opposite. See the discussion in CanIf.h; this switch exists
 * so that confirming the controller's manual is a one-line change rather than an edit to four
 * expressions, each of which could be got wrong independently.
 */
#define CANIF_MCU_BYTE_ORDER_LITTLE_ENDIAN STD_ON

/*==================================================================================================
 *  Signal layout: CAN_ID_MCU_DRIVE_STATE (0x10F8109A)
 *
 *  Byte 0   bits 1..0  direction        0 invalid, 1 forward, 2 reverse
 *           bit  3     speed mode       0 high, 1 low
 *  Byte 1   RPM, least significant byte
 *  Byte 2   RPM, most significant byte
 *  Byte 3   fault code
 *  Byte 4   low-power mode (0xAA when active)
 *  Byte 5-7 reserved
 *================================================================================================*/

#define CANIF_DS_OFF_FLAGS 0u
#define CANIF_DS_OFF_RPM_LOW 1u
#define CANIF_DS_OFF_RPM_HIGH 2u
#define CANIF_DS_OFF_FAULT 3u
#define CANIF_DS_OFF_LOW_POWER 4u

#define CANIF_DS_MASK_DIRECTION 0x03u
#define CANIF_DS_SHIFT_SPEED_MODE 3u
#define CANIF_DS_MASK_SPEED_MODE 0x01u

/** Bytes the drive-state frame must carry for every signal to be present. */
#define CANIF_DS_MIN_DLC 5u

/*==================================================================================================
 *  Signal layout: CAN_ID_MCU_CURRENT_VOLTAGE (0x10F8108D)
 *
 *  Byte 0   voltage, least significant byte   0.1 V per count
 *  Byte 1   voltage, most significant byte
 *  Byte 2   current, least significant byte   0.1 A per count
 *  Byte 3   current, most significant byte
 *  Byte 4-7 reserved
 *================================================================================================*/

#define CANIF_CV_OFF_VOLTAGE_LOW 0u
#define CANIF_CV_OFF_VOLTAGE_HIGH 1u
#define CANIF_CV_OFF_CURRENT_LOW 2u
#define CANIF_CV_OFF_CURRENT_HIGH 3u

/** Bytes the current/voltage frame must carry. */
#define CANIF_CV_MIN_DLC 4u

/*==================================================================================================
 *  Ageing
 *================================================================================================*/

/**
 * @brief Age beyond which motor controller signals are no longer acted upon, in milliseconds.
 *
 * 500 ms. The controller emits both frames at roughly 20 Hz, so 500 ms is ten missed frames --
 * comfortably past any plausible jitter, and short enough that a controller which stops
 * transmitting is noticed within one acquisition cycle rather than being integrated into the
 * odometer for minutes. v1 had no such limit: it published the last value it ever received,
 * indefinitely.
 */
#define CANIF_MCU_SIGNAL_TIMEOUT_MS 500u

/**
 * @brief Consecutive stale reads before a diagnostic event is raised.
 *
 * 10, which at the 3 s acquisition period is 30 s of silence from the controller.
 */
#define CANIF_STALE_REPORT_THRESHOLD 10u

#endif /* CANIF_CFG_H */

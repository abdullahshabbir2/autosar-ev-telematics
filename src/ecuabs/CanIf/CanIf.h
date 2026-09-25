/**
 * @file    CanIf.h
 * @brief   AUTOSAR CAN Interface (SWS_CANInterface) -- signal extraction from MCU frames.
 *
 * Sits between the CAN driver and the application: pulls frames from Can, decodes the two motor
 * controller messages into named signals, and tracks whether those signals are still fresh.
 *
 * @par Byte order: a contradiction in v1 resolved in favour of the code
 * The v1 source carried a comment stating that "the MCU message frames follow Big Endian Format
 * implying append(data[i], data[i+1])", and then decoded every field the other way:
 *
 *     mcu.rpm     = uint16_t(data[2] << 8 | data[1]);
 *     mcu.voltage = uint16_t(data[1] << 8 | data[0]) * 0.1f;
 *
 * Both read the *lower* index as the least significant byte, which is little endian. The comment
 * and the code cannot both be right. The code is adopted here, for two reasons: it is what was
 * actually running against real hardware, and the CAN matrix it implies -- Intel byte order, which
 * is what the great majority of motor controllers in this class use -- is the more likely of the
 * two. The decision is recorded in [08-protocols.md](docs/08-protocols.md) as an open item to confirm against a
 * bus capture, and ::CANIF_MCU_BYTE_ORDER_LITTLE_ENDIAN makes reversing it a one-line change
 * rather than an edit to four expressions.
 *
 * @par Signal ageing
 * A decoded signal is only useful if it is recent. v1 had no notion of this: it published whatever
 * @c mcudata last contained, so a motor controller that stopped transmitting produced a telemetry
 * record showing its final speed indefinitely -- and the odometer kept integrating it. Every signal
 * here carries the timestamp of the frame it came from, and ::CanIf_IsMcuDataFresh answers the
 * question the consumer actually needs to ask.
 *
 * @req SWREQ-COM-0030 .. SWREQ-COM-0038
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef CANIF_H
#define CANIF_H

#include "base/Autosar_ModuleIds.h"
#include "mcal/Can/Can.h"
#include "ecuabs/CanIf/CanIf_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANIF_VENDOR_ID 0xFFFEu
#define CANIF_SW_MAJOR_VERSION 2u
#define CANIF_SW_MINOR_VERSION 0u
#define CANIF_SW_PATCH_VERSION 0u

#define CANIF_API_ID_INIT 0x01u
#define CANIF_API_ID_RX_INDICATION 0x14u
#define CANIF_API_ID_MAIN_FUNCTION 0x0Eu
#define CANIF_API_ID_GET_MCU_DATA 0x20u
#define CANIF_API_ID_DECODE 0x21u

#define CANIF_E_UNINIT E_UNINIT
#define CANIF_E_PARAM_POINTER E_PARAM_POINTER
#define CANIF_E_INVALID_DLC 0x20u
#define CANIF_E_UNKNOWN_ID 0x21u
#define CANIF_E_SIGNAL_STALE 0x22u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Direction reported by the motor controller. */
typedef enum
{
    CANIF_DIR_INVALID = 0, /**< Neither direction selected.  */
    CANIF_DIR_FORWARD = 1, /**< Forward.                     */
    CANIF_DIR_REVERSE = 2, /**< Reverse.                     */
    CANIF_DIR_RESERVED = 3 /**< Reserved by the protocol.    */
} CanIf_DirectionType;

/** Speed mode reported by the motor controller. */
typedef enum
{
    CANIF_SPEED_MODE_HIGH = 0, /**< High-speed mode. */
    CANIF_SPEED_MODE_LOW = 1   /**< Low-speed mode.  */
} CanIf_SpeedModeType;

/**
 * @brief Fault codes the motor controller reports.
 *
 * Carried over from v1's @c McuFaultCode. The gaps are intentional: the controller's manual
 * defines those values and this vehicle's variant does not use them, so they are left unnamed
 * rather than renumbered -- renumbering would silently change the meaning of a logged value.
 */
typedef enum
{
    CANIF_MCU_FAULT_NONE = 0,           /**< No fault.                        */
    CANIF_MCU_FAULT_OVER_CURRENT = 3,   /**< Phase current limit exceeded.     */
    CANIF_MCU_FAULT_CONTROLLER_HOT = 4, /**< Controller over temperature.      */
    CANIF_MCU_FAULT_UNDER_VOLTAGE = 9,  /**< DC link below its minimum.         */
    CANIF_MCU_FAULT_OVER_VOLTAGE = 10,  /**< DC link above its maximum.         */
    CANIF_MCU_FAULT_MOTOR_HOT = 11,     /**< Motor over temperature.            */
    CANIF_MCU_FAULT_ACCELERATOR = 13    /**< Accelerator input implausible.     */
} CanIf_McuFaultType;

/** Value the controller reports in its low-power-mode byte when that mode is active. */
#define CANIF_MCU_LOW_POWER_ACTIVE 0xAAu

/** Decoded motor controller state, with the freshness of each half. */
typedef struct
{
    /* From CAN_ID_MCU_DRIVE_STATE. */
    CanIf_DirectionType direction; /**< Selected direction.                        */
    CanIf_SpeedModeType speedMode; /**< Selected speed mode.                       */
    uint16 motorRpm;               /**< Motor speed, rpm.                          */
    uint8 faultCode;               /**< ::CanIf_McuFaultType.                      */
    boolean lowPowerMode;          /**< TRUE when the controller is in low power.   */
    uint32 driveStateTimestamp;    /**< When the drive-state frame arrived.         */
    boolean driveStateValid;       /**< TRUE once a drive-state frame has arrived.  */

    /* From CAN_ID_MCU_CURRENT_VOLTAGE. */
    uint16 dcVoltageDeciVolt;       /**< DC link voltage, 0.1 V per count.           */
    uint16 dcCurrentDeciAmp;        /**< DC link current, 0.1 A per count.           */
    uint32 currentVoltageTimestamp; /**< When the current/voltage frame arrived.     */
    boolean currentVoltageValid;    /**< TRUE once such a frame has arrived.         */
} CanIf_McuDataType;

/** Interface counters, published as diagnostic data. */
typedef struct
{
    uint32 framesProcessed;      /**< Frames taken from the driver.                   */
    uint32 driveStateFrames;     /**< Drive-state frames decoded.                     */
    uint32 currentVoltageFrames; /**< Current/voltage frames decoded.              */
    uint32 unknownIdFrames;      /**< Frames whose identifier is not in the matrix.    */
    uint32 badDlcFrames;         /**< Frames too short for the signals they carry.     */
    uint32 staleReads;           /**< Reads that found the data older than its limit.  */
} CanIf_StatisticsType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/** Initialise the interface and clear all decoded signals. */
CHECK_RETURN Std_ReturnType CanIf_Init(void);

/**
 * @brief Drain the driver's receive queue and decode every frame.
 *
 * Driven cyclically by SchM. Bounded by the driver's own queue depth, so its execution time cannot
 * grow with bus load.
 *
 * @return Number of frames decoded.
 */
uint8 CanIf_MainFunction(void);

/**
 * @brief Read the decoded motor controller state.
 * @param[out] data Destination.
 * @return E_OK on success; E_NOT_OK for a NULL pointer or before Init.
 *
 * @note Returns E_OK even when the data is stale. Staleness is a property of the data, not an error
 *       in reading it, and ::CanIf_IsMcuDataFresh is the question a caller should ask.
 */
CHECK_RETURN Std_ReturnType CanIf_GetMcuData(CanIf_McuDataType *data);

/**
 * @brief Whether the motor controller signals are recent enough to act on.
 *
 * @return TRUE if both frames have arrived within ::CANIF_MCU_SIGNAL_TIMEOUT_MS. The odometer uses
 *         this to decide whether to integrate at all, which is what stops a silent controller from
 *         accumulating distance at its last known speed.
 */
boolean CanIf_IsMcuDataFresh(void);

/**
 * @brief Decode a drive-state frame into @p data.
 *
 * Pure function, exposed so the byte layout can be tested against captured frames.
 *
 * @param[in]  pdu  Frame with identifier ::CAN_ID_MCU_DRIVE_STATE.
 * @param[out] data Fields updated in place; the current/voltage half is left untouched.
 * @return E_OK on success; E_NOT_OK if the DLC is too short or a pointer is NULL.
 */
CHECK_RETURN Std_ReturnType CanIf_DecodeMcuDriveState(const Can_PduType *pdu, CanIf_McuDataType *data);

/**
 * @brief Decode a current/voltage frame into @p data.
 * @copydetails CanIf_DecodeMcuDriveState
 */
CHECK_RETURN Std_ReturnType CanIf_DecodeMcuCurrentVoltage(const Can_PduType *pdu, CanIf_McuDataType *data);

/**
 * @brief Read the interface counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType CanIf_GetStatistics(CanIf_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void CanIf_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* CANIF_H */

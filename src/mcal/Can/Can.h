/**
 * @file    Can.h
 * @brief   AUTOSAR CAN driver (SWS_CANDriver) -- MCP2515 over SPI.
 *
 * Presents the MCP2515 as an AUTOSAR CAN controller: initialise, transmit, and hand
 * received frames up to CanIf. Hardware filtering and mailbox management stay here;
 * PDU routing and signal extraction belong to CanIf and Com.
 *
 * @par Bit timing is a configuration input, not a guess
 * The MCP2515 derives its bit rate from the crystal fitted to *that board*. v1 had the
 * whole initialisation commented out with a note that 8 MHz needs 1000 kbps where
 * 16 MHz needs 500 kbps -- which is a symptom of timing having been tuned by trial
 * rather than calculated. ::CAN_CRYSTAL_FREQUENCY_HZ and the segment values derived
 * from it are stated explicitly in Can_Cfg.h with the arithmetic shown, so fitting a
 * different crystal is a configuration change whose effect is predictable.
 *
 * @par Why v1 never actually received CAN data
 * ::Can_Init in v1 returned @c true unconditionally with @c mcp_can.begin() commented
 * out, so the controller was never taken out of configuration mode. @c receive_msg()
 * then polled a controller that could not receive, burning 3 x 1000 ms of its
 * 3000 ms cycle budget in timeout loops before returning false. Every MCU voltage,
 * current, RPM and speed field in the archived logs is therefore empty, and the
 * odometer -- which integrates RPM -- could never advance. Initialisation here is real
 * and its failure is reported rather than masked.
 *
 * @req SWREQ-COM-0010 .. SWREQ-COM-0015
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef CAN_H
#define CAN_H

#include "Autosar_ModuleIds.h"
#include "Can_Cfg.h"
#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_VENDOR_ID 0xFFFEu
#define CAN_AR_RELEASE_MAJOR_VERSION 4u
#define CAN_AR_RELEASE_MINOR_VERSION 4u
#define CAN_SW_MAJOR_VERSION 2u
#define CAN_SW_MINOR_VERSION 0u
#define CAN_SW_PATCH_VERSION 0u

#define CAN_API_ID_INIT 0x00u
#define CAN_API_ID_SET_CONTROLLER_MODE 0x03u
#define CAN_API_ID_WRITE 0x06u
#define CAN_API_ID_MAIN_FUNCTION_READ 0x08u
#define CAN_API_ID_RECEIVE 0x20u

#define CAN_E_UNINIT E_UNINIT
#define CAN_E_PARAM_POINTER E_PARAM_POINTER
#define CAN_E_PARAM_DLC 0x03u
#define CAN_E_PARAM_CONTROLLER 0x04u
#define CAN_E_TRANSITION 0x05u
#define CAN_E_DATALOST 0x06u
#define CAN_E_INIT_FAILED 0x20u
#define CAN_E_BUS_OFF 0x21u
#define CAN_E_SPI_FAILURE 0x22u

/** Maximum payload of a classic CAN frame. */
#define CAN_MAX_DLC 8u

/** Mask selecting the identifier bits of a ::Can_IdType. */
#define CAN_ID_MASK 0x1FFFFFFFuL

/** Set in a ::Can_IdType to mark a 29-bit extended identifier. */
#define CAN_ID_EXTENDED_FLAG 0x80000000uL

/**
 * @brief CAN identifier with the extended-format flag in the top bit.
 *
 * Carrying the format in the identifier rather than a separate field is the AUTOSAR
 * convention and it removes a real hazard: standard ID 0x123 and extended ID 0x123 are
 * different frames, and a filter table that stored only the numeric value would match
 * both.
 */
typedef uint32 Can_IdType;

/** Controller operating mode. */
typedef enum
{
    CAN_CS_UNINIT = 0,  /**< Before ::Can_Init.                      */
    CAN_CS_STOPPED = 1, /**< Initialised, not participating.          */
    CAN_CS_STARTED = 2, /**< Participating in bus traffic.            */
    CAN_CS_SLEEP = 3    /**< Low-power, wakes on bus activity.        */
} Can_ControllerStateType;

/** A received or transmitted frame. */
typedef struct
{
    Can_IdType id;           /**< Identifier, with ::CAN_ID_EXTENDED_FLAG if 29-bit. */
    uint8 dlc;               /**< Payload length, 0 .. ::CAN_MAX_DLC.                */
    uint8 sdu[CAN_MAX_DLC];  /**< Payload.                                           */
    uint32 timestamp;        /**< Gpt_GetMonotonicMs() at reception.                 */
} Can_PduType;

/** Controller error and throughput counters. */
typedef struct
{
    uint32 framesReceived;    /**< Frames accepted by the filters.            */
    uint32 framesTransmitted; /**< Frames successfully transmitted.           */
    uint32 rxOverflowCount;   /**< Frames lost to a full receive queue.       */
    uint32 txFailureCount;    /**< Transmissions the controller rejected.     */
    uint32 busOffCount;       /**< Bus-off transitions since Init.            */
    uint32 errorWarningCount; /**< Error-warning threshold crossings.         */
    uint8 rxErrorCounter;     /**< Controller receive error counter.          */
    uint8 txErrorCounter;     /**< Controller transmit error counter.         */
} Can_StatisticsType;

/**
 * @brief Initialise the controller, load the filter table and enter ::CAN_CS_STOPPED.
 *
 * @return E_OK if the controller acknowledged configuration mode and accepted the bit
 *         timing; E_NOT_OK otherwise. A failure here is reported to Dem as
 *         ::DEM_EVENT_CAN_INIT_FAILED and leaves the ECU running without CAN rather
 *         than resetting: battery, GNSS and voltage data are still worth logging, and
 *         v1's unconditional @c ESP.restart() on CAN failure produced exactly the
 *         reboot loop the crash-loop detector now has to guard against.
 */
CHECK_RETURN Std_ReturnType Can_Init(void);

/**
 * @brief Return the driver to its uninitialised state (SWS_Can_91002).
 *
 * Discards the receive queue and the statistics and marks the controller
 * ::CAN_CS_UNINIT, so a subsequent API call is rejected as uninitialised. Does not touch
 * the hardware: the controller is left wherever it was, and ::Can_Init resets it anyway.
 *
 * Used by EcuM when shutting down, and by the unit tests so that each case starts from a
 * defined state rather than inheriting whatever the previous case left in module-static
 * storage.
 */
void Can_DeInit(void);

/** Move the controller to @p state. */
CHECK_RETURN Std_ReturnType Can_SetControllerMode(Can_ControllerStateType state);

/** Current controller mode. */
Can_ControllerStateType Can_GetControllerMode(void);

/**
 * @brief Transmit @p pdu.
 * @return E_OK if a transmit mailbox accepted it; E_BUSY if all mailboxes are full;
 *         E_NOT_OK on a parameter error or a controller that is not started.
 */
CHECK_RETURN Std_ReturnType Can_Write(const Can_PduType *pdu);

/**
 * @brief Take the oldest frame from the receive queue.
 *
 * @param[out] pdu Destination.
 * @return E_OK if a frame was returned; E_NOT_FOUND if the queue was empty (a normal
 *         outcome, not an error); E_NOT_OK on a parameter or state error.
 */
CHECK_RETURN Std_ReturnType Can_Receive(Can_PduType *pdu);

/**
 * @brief Move frames from the controller into the software receive queue.
 *
 * Called cyclically by SchM. Drains at most ::CAN_MAX_FRAMES_PER_CYCLE frames per
 * call, so a busy bus cannot starve the rest of the schedule -- a bounded worst-case
 * execution time matters more here than emptying the controller in one pass.
 *
 * @return Number of frames moved.
 */
uint8 Can_MainFunction_Read(void);

/** Frames waiting in the software receive queue. */
uint8 Can_GetRxQueueCount(void);

/**
 * @brief Read the controller counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Can_GetStatistics(Can_StatisticsType *stats);

/**
 * @brief TRUE if the controller is bus-off.
 *
 * Bus-off means the transmit error counter passed 255, which on this vehicle means the
 * harness is disconnected or the bus is shorted. Recovery is deliberately not
 * automatic and immediate: ComM schedules it after ::CAN_BUS_OFF_RECOVERY_MS so that a
 * genuinely broken bus is not hammered with recovery attempts.
 */
boolean Can_IsBusOff(void);

/** Attempt bus-off recovery by re-entering and leaving configuration mode. */
CHECK_RETURN Std_ReturnType Can_RecoverBusOff(void);

/*==================================================================================================
 *  Identifier codec
 *
 *  The MCP2515 scatters a 29-bit identifier across four registers with the
 *  extended/standard selector buried in the middle of one of them. Getting that
 *  encoding subtly wrong produces frames that transmit without error and are simply
 *  never acknowledged by the peer, which is close to undiagnosable on a vehicle. The
 *  codec is therefore exposed rather than hidden, so it can be verified exhaustively
 *  off-target: see test_can/test_can.c, which round-trips every representable
 *  identifier.
 *================================================================================================*/

/** Number of registers holding one identifier: SIDH, SIDL, EID8, EID0. */
#define CAN_ID_REGISTER_COUNT 4u

/**
 * @brief Encode @p id into the MCP2515's four identifier registers.
 *
 * @param[in]  id   Identifier, with ::CAN_ID_EXTENDED_FLAG set for 29-bit format.
 * @param[out] regs Destination, exactly ::CAN_ID_REGISTER_COUNT bytes, in register
 *                  order SIDH, SIDL, EID8, EID0.
 */
void Can_EncodeIdentifier(Can_IdType id, uint8 *regs);

/**
 * @brief Decode the MCP2515's four identifier registers back into a ::Can_IdType.
 *
 * @param[in] regs Source, exactly ::CAN_ID_REGISTER_COUNT bytes, in register order.
 * @return The identifier, with ::CAN_ID_EXTENDED_FLAG set if the IDE bit was set.
 *         Returns 0 if @p regs is NULL_PTR.
 */
Can_IdType Can_DecodeIdentifier(const uint8 *regs);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Can_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* CAN_H */

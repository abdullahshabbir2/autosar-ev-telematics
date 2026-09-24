/**
 * @file    DiagSwc.h
 * @brief   Diagnostics component -- DTC persistence and a UDS-style request interface over MQTT.
 *
 * Two jobs. It owns the NvM block that carries the diagnostic record across power cycles, and it serves
 * remote diagnostic requests so a fault can be investigated without physical access to the vehicle.
 *
 * @par Why a reduced UDS rather than the real thing
 * Full ISO 14229 assumes a CAN transport with ISO-TP segmentation, a diagnostic session state machine and
 * security access. None of that applies here: the request arrives as an MQTT message over an IP bearer
 * the ECU already has, and there is no tester on the vehicle. What is kept is the part that carries the
 * meaning -- the service identifiers, the DTC format and the negative-response codes -- so anyone who
 * has worked with UDS reads this without a translation table, and so a later migration to real UDS is a
 * transport change rather than a redesign. The subset is specified in @ref docs/08-protocols.md.
 *
 * @par Why the record must survive a power cycle
 * v1's diagnostic state was a `byte flags[15]` array, overwritten every cycle and gone on reset. A unit
 * returned from the field carried no evidence of what had gone wrong, and these units have no serial
 * connection in service — so the only diagnostic information available was whatever the fleet happened
 * to have received before the fault, which by definition excludes the fault itself.
 *
 * @req SWREQ-DIAG-0040 .. SWREQ-DIAG-0068
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DIAGSWC_H
#define DIAGSWC_H

#include "base/Autosar_ModuleIds.h"
#include "services/Dem/Dem.h"
#include "app/DiagSwc/DiagSwc_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAGSWC_VENDOR_ID 0xFFFEu
#define DIAGSWC_SW_MAJOR_VERSION 2u
#define DIAGSWC_SW_MINOR_VERSION 0u
#define DIAGSWC_SW_PATCH_VERSION 0u

#define DIAGSWC_API_ID_INIT 0x00u
#define DIAGSWC_API_ID_MAIN_FUNCTION 0x0Eu
#define DIAGSWC_API_ID_HANDLE_REQUEST 0x20u
#define DIAGSWC_API_ID_PERSIST 0x21u

#define DIAGSWC_E_UNINIT E_UNINIT
#define DIAGSWC_E_PARAM_POINTER E_PARAM_POINTER
#define DIAGSWC_E_PERSIST_FAILED 0x20u
#define DIAGSWC_E_MALFORMED_REQUEST 0x21u

/*==================================================================================================
 *  UDS service identifiers -- the subset this ECU implements
 *================================================================================================*/

#define DIAGSWC_SID_READ_DTC_INFORMATION 0x19u  /**< List confirmed DTCs.             */
#define DIAGSWC_SID_CLEAR_DIAGNOSTIC_INFO 0x14u /**< Clear the diagnostic record.     */
#define DIAGSWC_SID_READ_DATA_BY_ID 0x22u       /**< Read a data identifier.          */
#define DIAGSWC_SID_WRITE_DATA_BY_ID 0x2Eu      /**< Write a data identifier.         */
#define DIAGSWC_SID_ROUTINE_CONTROL 0x31u       /**< Start a routine.                 */
#define DIAGSWC_SID_ECU_RESET 0x11u             /**< Reset the ECU.                   */

/** Added to a service identifier to form its positive response. */
#define DIAGSWC_POSITIVE_RESPONSE_OFFSET 0x40u

/** Negative response service identifier. */
#define DIAGSWC_NEGATIVE_RESPONSE_SID 0x7Fu

/*==================================================================================================
 *  Negative response codes (ISO 14229-1 table A.1)
 *================================================================================================*/

#define DIAGSWC_NRC_SERVICE_NOT_SUPPORTED 0x11u
#define DIAGSWC_NRC_INCORRECT_LENGTH 0x13u
#define DIAGSWC_NRC_CONDITIONS_NOT_CORRECT 0x22u
#define DIAGSWC_NRC_REQUEST_OUT_OF_RANGE 0x31u
#define DIAGSWC_NRC_GENERAL_PROGRAMMING_FAILURE 0x72u

/*==================================================================================================
 *  Data identifiers
 *
 *  Chosen in the manufacturer-defined 0xF1xx and 0xFD00 ranges. The read-only set covers what a fleet
 *  engineer needs to characterise a unit; the writable set is deliberately tiny, because every writable
 *  identifier is a way to misconfigure a vehicle remotely.
 *================================================================================================*/

#define DIAGSWC_DID_FIRMWARE_VERSION 0xF189u   /**< Read-only: firmware version string. */
#define DIAGSWC_DID_DEVICE_ID 0xF18Au          /**< Read-only: device identifier.       */
#define DIAGSWC_DID_ODOMETER 0xFD01u           /**< Read-only: lifetime distance, mm.   */
#define DIAGSWC_DID_TRIP 0xFD02u               /**< Read-only: trip distance, mm.       */
#define DIAGSWC_DID_HEALTH_SUMMARY 0xFD03u     /**< Read-only: the health counters.     */
#define DIAGSWC_DID_RESET_REASON 0xFD04u       /**< Read-only: last reset cause.        */
#define DIAGSWC_DID_TYRE_DIAMETER 0xFD10u      /**< Writable: tyre diameter, milli-inch. */
#define DIAGSWC_DID_GEAR_RATIO 0xFD11u         /**< Writable: gear ratio x 1000.        */
#define DIAGSWC_DID_VBATT_TRIM 0xFD12u         /**< Writable: voltage offset trim, mV.  */
#define DIAGSWC_DID_LOG_LEVEL 0xFD13u          /**< Writable: runtime log level.        */

/*==================================================================================================
 *  Routine identifiers
 *================================================================================================*/

#define DIAGSWC_ROUTINE_RESET_TRIP 0x0201u     /**< Zero the trip counter.              */
#define DIAGSWC_ROUTINE_SELF_TEST_LEDS 0x0202u /**< Run the indicator self-test.        */
#define DIAGSWC_ROUTINE_REDISCOVER_PACKS 0x0203u /**< Re-run battery pack discovery.    */

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Diagnostic activity counters. */
typedef struct
{
    uint32 requestsReceived;   /**< Requests seen on the diagnostic topic.        */
    uint32 requestsServed;     /**< Requests answered positively.                 */
    uint32 requestsRejected;   /**< Requests answered with a negative response.   */
    uint32 persistCount;       /**< Times the DTC record has been written to NvM. */
    uint32 persistFailures;    /**< Writes that failed.                           */
    uint16 restoredDtcCount;   /**< DTCs restored from NvM at startup.            */
} DiagSwc_StatusType;

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Restore the persistent DTC record and subscribe to the diagnostic topic.
 *
 * Runs after ::Dem_Init, so that a fault confirmed before the last power cycle comes back confirmed.
 *
 * @return E_OK on success; E_NOT_OK if the NvM block was unreadable, in which case diagnostics start from
 *         a clean record and the NvM problem is itself reported as an event.
 */
CHECK_RETURN Std_ReturnType DiagSwc_Init(void);

/**
 * @brief Persist the DTC record if Dem has signalled a change.
 *
 * Driven cyclically by SchM. Deferred rather than written on every confirmation, so a fault reporting
 * repeatedly does not consume flash endurance.
 */
void DiagSwc_MainFunction(void);

/**
 * @brief Write the DTC record to NvM now, regardless of whether anything changed.
 *
 * Called by EcuM on the shutdown path, so a controlled power-down never loses a freshly confirmed fault.
 */
CHECK_RETURN Std_ReturnType DiagSwc_Persist(void);

/**
 * @brief Handle one diagnostic request and produce its response.
 *
 * Pure with respect to the transport: the caller supplies the request bytes and a response buffer, so the
 * whole service layer can be tested without a broker.
 *
 * @param[in]  request     Request bytes: service identifier followed by its parameters.
 * @param[in]  requestLen  Length of @p request.
 * @param[out] response    Response bytes, positive or negative.
 * @param[in]  responseCap Capacity of @p response.
 * @param[out] responseLen Bytes written.
 * @return E_OK if a response was produced, which includes a negative one -- a rejected request is still
 *         a request that was handled. E_NOT_OK only if no response would fit.
 */
CHECK_RETURN Std_ReturnType DiagSwc_HandleRequest(const uint8 *request, uint16 requestLen,
                                                  uint8 *response, uint16 responseCap,
                                                  uint16 *responseLen);

/**
 * @brief Supply Dem with the snapshot data it captures on confirmation.
 *
 * Installed as Dem's snapshot provider by EcuM. Lives here rather than in Dem so that Dem does not depend
 * on the odometer, the ADC and the clock, which would make it untestable in isolation and would invert
 * the layering.
 */
void DiagSwc_ProvideSnapshot(Dem_SnapshotType *snapshot);

/**
 * @brief Read the diagnostic activity counters.
 * @param[out] status Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType DiagSwc_GetStatus(DiagSwc_StatusType *status);

/**
 * @brief Return this component's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void DiagSwc_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* DIAGSWC_H */

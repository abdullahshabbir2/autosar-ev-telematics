/**
 * @file    Mcu.h
 * @brief   AUTOSAR MCU driver (SWS_MCUDriver) -- ESP32 adaptation.
 *
 * Owns everything about the core itself: reset cause, reset request, clock
 * information, RAM integrity and the unique device identity. Deliberately thin:
 * the ESP-IDF startup code has already configured the PLL and the cache by the time
 * ::Mcu_Init runs, so this module reports and resets rather than configures.
 *
 * @par Reset cause is load-bearing
 * ::Mcu_GetResetReason is read once during ::EcuM_Init and is what distinguishes a
 * clean power-on from a watchdog bite or a panic. That distinction drives the
 * crash-loop detector (see Restart handling in EcuM), which is the mechanism that
 * stops a unit that cannot mount its SD card from rebooting every ten minutes
 * forever.
 *
 * @req SWREQ-SYS-0001, SWREQ-SYS-0002, SWREQ-SAF-0007
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef MCU_H
#define MCU_H

#include "base/Autosar_ModuleIds.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *  Published information
 *================================================================================================*/

#define MCU_VENDOR_ID 0xFFFEu
#define MCU_AR_RELEASE_MAJOR_VERSION 4u
#define MCU_AR_RELEASE_MINOR_VERSION 4u
#define MCU_SW_MAJOR_VERSION 2u
#define MCU_SW_MINOR_VERSION 0u
#define MCU_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  API service IDs
 *================================================================================================*/

#define MCU_API_ID_INIT 0x00u
#define MCU_API_ID_GET_RESET_REASON 0x05u
#define MCU_API_ID_PERFORM_RESET 0x07u
#define MCU_API_ID_GET_DEVICE_ID 0x20u
#define MCU_API_ID_GET_HEAP_INFO 0x21u

/*==================================================================================================
 *  Development error codes
 *================================================================================================*/

#define MCU_E_UNINIT E_UNINIT
#define MCU_E_PARAM_POINTER E_PARAM_POINTER
#define MCU_E_BUFFER_TOO_SMALL 0x20u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/**
 * @brief Why the core last started executing from the reset vector.
 *
 * Mapped from the ESP32 @c esp_reset_reason_t. The grouping is chosen so that
 * EcuM can ask one question -- "was the previous run terminated abnormally?" --
 * by testing against ::MCU_RESET_POWER_ON and ::MCU_RESET_SOFTWARE.
 */
typedef enum
{
    MCU_RESET_POWER_ON = 0,   /**< Cold start: supply applied or brown-out release. */
    MCU_RESET_SOFTWARE = 1,   /**< Deliberate Mcu_PerformReset() or OTA activation. */
    MCU_RESET_WATCHDOG = 2,   /**< A watchdog expired -- previous run hung.         */
    MCU_RESET_PANIC = 3,      /**< Unhandled exception or assertion.                */
    MCU_RESET_BROWNOUT = 4,   /**< Supply dipped below the brown-out threshold.     */
    MCU_RESET_DEEPSLEEP = 5,  /**< Woken from deep sleep.                           */
    MCU_RESET_EXTERNAL = 6,   /**< EN pin pulled low.                               */
    MCU_RESET_UNKNOWN = 7     /**< Cause not reported by the hardware.              */
} Mcu_ResetReasonType;

/** Heap and stack headroom, sampled for the telemetry health record. */
typedef struct
{
    uint32 heapFreeBytes;        /**< Currently free heap.                        */
    uint32 heapMinFreeBytes;     /**< Lowest free heap since boot (high-water).   */
    uint32 heapLargestBlockBytes;/**< Largest contiguous block -- fragmentation.  */
    uint32 internalFreeBytes;    /**< Free internal (non-PSRAM) heap.             */
} Mcu_HeapInfoType;

/** Length in bytes of the factory MAC used as the ECU's unique identity. */
#define MCU_DEVICE_ID_LENGTH 6u

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Latch the reset cause and prepare the module.
 *
 * Must be the first MCAL call in the startup sequence: the reset cause register is
 * read here before anything can overwrite it.
 *
 * @return E_OK always; retained for interface uniformity.
 */
Std_ReturnType Mcu_Init(void);

/**
 * @brief Return the cause of the most recent reset.
 * @return One of ::Mcu_ResetReasonType; ::MCU_RESET_UNKNOWN before ::Mcu_Init.
 */
Mcu_ResetReasonType Mcu_GetResetReason(void);

/**
 * @brief Human-readable name of a reset reason, for logs and telemetry.
 * @param reason Value to render.
 * @return A pointer to a string literal; never NULL_PTR, never needs freeing.
 */
const char *Mcu_GetResetReasonName(Mcu_ResetReasonType reason);

/**
 * @brief Request an immediate software reset. Never returns.
 *
 * Callers must have already persisted anything they cannot lose: this function
 * does not flush NvM or the SD card. ::EcuM_ShutdownAndReset is the supported way
 * to reset with a clean tail.
 */
NORETURN void Mcu_PerformReset(void);

/**
 * @brief Copy the ECU's unique identity (factory MAC) into @p buffer.
 *
 * @param[out] buffer     Destination, at least ::MCU_DEVICE_ID_LENGTH bytes.
 * @param[in]  bufferSize Capacity of @p buffer.
 * @return E_OK on success; E_NOT_OK if @p buffer is NULL_PTR or too small.
 */
CHECK_RETURN Std_ReturnType Mcu_GetDeviceId(uint8 *buffer, uint8 bufferSize);

/**
 * @brief Render the device identity as 12 uppercase hex digits plus a terminator.
 *
 * This is the string used as the MQTT client ID and the provisioning key, so the
 * formatting is fixed here rather than at each call site: a lowercase or
 * colon-separated variant would silently create a second device record in the
 * cloud.
 *
 * @param[out] buffer     Destination, at least 13 bytes.
 * @param[in]  bufferSize Capacity of @p buffer.
 * @return E_OK on success; E_NOT_OK otherwise (buffer left untouched).
 */
CHECK_RETURN Std_ReturnType Mcu_GetDeviceIdString(char *buffer, uint8 bufferSize);

/**
 * @brief Sample heap and fragmentation figures.
 * @param[out] info Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Mcu_GetHeapInfo(Mcu_HeapInfoType *info);

/**
 * @brief Core frequency in Hz, as actually configured.
 */
uint32 Mcu_GetCpuFrequencyHz(void);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Mcu_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* MCU_H */

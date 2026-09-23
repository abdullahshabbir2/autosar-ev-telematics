/**
 * @file    Autosar_ModuleIds.h
 * @brief   Central registry of module IDs, instance IDs and API service IDs.
 *
 * Every Det_ReportError() and Dem_SetEventStatus() call in the project is
 * identified by the triple (ModuleId, InstanceId, ApiId). Keeping the whole triple
 * space in one file is what makes a captured error code decodable from a field log
 * without the sources at hand, and it prevents two modules from reusing an ID.
 *
 * Module IDs below 256 follow the AUTOSAR allocation in TR_BSWModuleList.
 * IDs from 0x0400 upward are project-private (ECU abstraction and application).
 *
 * @see tools/decode_det.py  Decodes a raw DET/DEM code back into symbol names.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef AUTOSAR_MODULEIDS_H
#define AUTOSAR_MODULEIDS_H

/*==================================================================================================
 *  Module IDs -- MCAL (AUTOSAR-allocated)
 *================================================================================================*/

#define MODULE_ID_MCU 101u  /**< Mcu  -- clocks, reset, reset cause. */
#define MODULE_ID_PORT 124u /**< Port -- pin direction and mode.     */
#define MODULE_ID_DIO 120u  /**< Dio  -- digital read/write.         */
#define MODULE_ID_ADC 123u  /**< Adc  -- analogue acquisition.       */
#define MODULE_ID_SPI 83u   /**< Spi  -- SPI handler/driver.         */
#define MODULE_ID_CAN 80u   /**< Can  -- CAN driver (MCP2515).       */
#define MODULE_ID_UART 82u  /**< Uart -- asynchronous serial.        */
#define MODULE_ID_GPT 100u  /**< Gpt  -- general purpose timer.      */
#define MODULE_ID_WDG 102u  /**< Wdg  -- hardware watchdog driver.   */
#define MODULE_ID_FLS 92u   /**< Fls  -- flash driver (NVS-backed).  */

/*==================================================================================================
 *  Module IDs -- Services (AUTOSAR-allocated)
 *================================================================================================*/

#define MODULE_ID_CRC 201u   /**< Crc   -- CRC routines.                       */
#define MODULE_ID_DET 15u    /**< Det   -- development error tracer.           */
#define MODULE_ID_DEM 54u    /**< Dem   -- diagnostic event manager.           */
#define MODULE_ID_NVM 20u    /**< NvM   -- NVRAM manager.                      */
#define MODULE_ID_MEMIF 22u  /**< MemIf -- memory abstraction interface.       */
#define MODULE_ID_FEE 21u    /**< Fee   -- flash EEPROM emulation.             */
#define MODULE_ID_WDGM 13u   /**< WdgM  -- watchdog manager.                   */
#define MODULE_ID_COMM 12u   /**< ComM  -- communication manager.              */
#define MODULE_ID_ECUM 10u   /**< EcuM  -- ECU state manager.                  */
#define MODULE_ID_BSWM 42u   /**< BswM  -- basic software mode manager.        */
#define MODULE_ID_SCHM 130u  /**< SchM  -- BSW scheduler.                      */
#define MODULE_ID_COM 50u    /**< Com   -- signal-based communication.         */
#define MODULE_ID_RTE 2u     /**< Rte   -- runtime environment.                */

/*==================================================================================================
 *  Module IDs -- Project-private
 *================================================================================================*/

#define MODULE_ID_CANIF 60u    /**< CanIf -- CAN interface (AUTOSAR-allocated).   */
#define MODULE_ID_RS485IF 0x0401u /**< Rs485If -- battery bus transport.          */
#define MODULE_ID_IOHWAB 0x0402u  /**< IoHwAb  -- I/O hardware abstraction.      */
#define MODULE_ID_FSABS 0x0403u   /**< FsAbs   -- SD filesystem abstraction.     */
#define MODULE_ID_TIMEABS 0x0404u /**< TimeAbs -- RTC / wall-clock abstraction.   */
#define MODULE_ID_GNSSIF 0x0405u  /**< GnssIf  -- GNSS receiver interface.        */
#define MODULE_ID_NETIF 0x0406u   /**< NetIf   -- IP bearer abstraction.          */
#define MODULE_ID_LOG 0x0407u     /**< Log     -- structured logging front end.   */
#define MODULE_ID_PROV 0x0408u    /**< Prov    -- device provisioning service.    */
#define MODULE_ID_OTA 0x0409u     /**< Ota     -- firmware update service.        */

#define MODULE_ID_ODOSWC 0x0801u   /**< OdoSwc   -- odometry component.           */
#define MODULE_ID_BATTSWC 0x0802u  /**< BattSwc  -- battery monitoring component. */
#define MODULE_ID_TELEMSWC 0x0803u /**< TelemSwc -- telemetry component.          */
#define MODULE_ID_DIAGSWC 0x0804u  /**< DiagSwc  -- diagnostics component.        */
#define MODULE_ID_HMISWC 0x0805u   /**< HmiSwc   -- status indication component.  */

/*==================================================================================================
 *  Instance IDs
 *
 *  A module that manages several identical hardware or logical units reports which
 *  one failed through the instance ID. Single-instance modules always pass 0.
 *================================================================================================*/

#define INSTANCE_ID_SINGLE 0u /**< The module has exactly one instance. */

#define INSTANCE_ID_BATTERY_1 1u /**< Battery pack in slot 1. */
#define INSTANCE_ID_BATTERY_2 2u /**< Battery pack in slot 2. */
#define INSTANCE_ID_BATTERY_3 3u /**< Battery pack in slot 3. */
#define INSTANCE_ID_BATTERY_4 4u /**< Battery pack in slot 4. */

#define INSTANCE_ID_BEARER_WIFI 1u /**< IP bearer: 802.11 station.  */
#define INSTANCE_ID_BEARER_GSM 2u  /**< IP bearer: GSM/GPRS modem.  */

/*==================================================================================================
 *  Common API service IDs
 *
 *  AUTOSAR reserves 0x00 for <Mip>_Init and 0x01 for <Mip>_DeInit in most modules,
 *  and 0x0F is conventionally <Mip>_GetVersionInfo. Module-specific service IDs are
 *  declared in the module's own header next to the functions they identify.
 *================================================================================================*/

#define API_ID_INIT 0x00u            /**< <Mip>_Init()            */
#define API_ID_DEINIT 0x01u          /**< <Mip>_DeInit()          */
#define API_ID_MAIN_FUNCTION 0x0Eu   /**< <Mip>_MainFunction()    */
#define API_ID_GET_VERSION_INFO 0x0Fu/**< <Mip>_GetVersionInfo()  */

/*==================================================================================================
 *  Common development error codes
 *
 *  AUTOSAR gives each module its own error enumeration, but four conditions recur
 *  in every module, so they are allocated once here with the canonical AUTOSAR
 *  values and reused. Module-specific codes start at 0x20 in the module header.
 *================================================================================================*/

#define E_UNINIT 0x0Du          /**< API called before <Mip>_Init().          */
#define E_ALREADY_INITIALIZED 0x0Eu /**< <Mip>_Init() called twice.           */
#define E_PARAM_POINTER 0x10u   /**< A NULL_PTR was passed for an out param.  */
#define E_PARAM_VALUE 0x11u     /**< A numeric argument was out of range.     */
#define E_PARAM_CONFIG 0x12u    /**< The configuration set is inconsistent.   */

#endif /* AUTOSAR_MODULEIDS_H */

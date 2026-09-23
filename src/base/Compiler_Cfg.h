/**
 * @file    Compiler_Cfg.h
 * @brief   Module-specific memory and pointer class configuration (SWS_COMPILER_00010).
 *
 * One block per BSW/application module, as required by AUTOSAR. On this target all
 * classes resolve to nothing because the ESP32 presents a single flat address
 * space and the linker script supplied by ESP-IDF already separates IRAM/DRAM
 * from flash. The declarations are retained so that section placement can be
 * introduced later without touching any module source file.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COMPILER_CFG_H
#define COMPILER_CFG_H

/* clang-format off */

/*---------------------------------- MCAL ------------------------------------*/
#define MCU_CODE
#define MCU_VAR
#define MCU_CONST
#define MCU_APPL_DATA

#define PORT_CODE
#define PORT_VAR
#define PORT_CONST

#define DIO_CODE
#define DIO_VAR
#define DIO_CONST

#define ADC_CODE
#define ADC_VAR
#define ADC_CONST
#define ADC_APPL_DATA

#define SPI_CODE
#define SPI_VAR
#define SPI_CONST
#define SPI_APPL_DATA

#define CAN_CODE
#define CAN_VAR
#define CAN_CONST
#define CAN_APPL_DATA

#define UART_CODE
#define UART_VAR
#define UART_CONST
#define UART_APPL_DATA

#define GPT_CODE
#define GPT_VAR
#define GPT_CONST

#define WDG_CODE
#define WDG_VAR
#define WDG_CONST

#define FLS_CODE
#define FLS_VAR
#define FLS_CONST
#define FLS_APPL_DATA

/*------------------------- ECU Abstraction Layer ----------------------------*/
#define CANIF_CODE
#define CANIF_VAR
#define CANIF_CONST
#define CANIF_APPL_DATA

#define RS485IF_CODE
#define RS485IF_VAR
#define RS485IF_CONST
#define RS485IF_APPL_DATA

#define IOHWAB_CODE
#define IOHWAB_VAR
#define IOHWAB_CONST

#define FSABS_CODE
#define FSABS_VAR
#define FSABS_CONST
#define FSABS_APPL_DATA

#define TIMEABS_CODE
#define TIMEABS_VAR
#define TIMEABS_CONST
#define TIMEABS_APPL_DATA

#define GNSSIF_CODE
#define GNSSIF_VAR
#define GNSSIF_CONST
#define GNSSIF_APPL_DATA

#define NETIF_CODE
#define NETIF_VAR
#define NETIF_CONST
#define NETIF_APPL_DATA

/*------------------------------ Services ------------------------------------*/
#define CRC_CODE
#define CRC_VAR
#define CRC_CONST
#define CRC_APPL_DATA

#define DET_CODE
#define DET_VAR
#define DET_CONST

#define DEM_CODE
#define DEM_VAR
#define DEM_CONST
#define DEM_APPL_DATA

#define NVM_CODE
#define NVM_VAR
#define NVM_CONST
#define NVM_APPL_DATA

#define MEMIF_CODE
#define MEMIF_VAR
#define MEMIF_CONST
#define MEMIF_APPL_DATA

#define FEE_CODE
#define FEE_VAR
#define FEE_CONST
#define FEE_APPL_DATA

#define WDGM_CODE
#define WDGM_VAR
#define WDGM_CONST

#define COMM_CODE
#define COMM_VAR
#define COMM_CONST

#define ECUM_CODE
#define ECUM_VAR
#define ECUM_CONST

#define BSWM_CODE
#define BSWM_VAR
#define BSWM_CONST

#define SCHM_CODE
#define SCHM_VAR
#define SCHM_CONST

#define COM_CODE
#define COM_VAR
#define COM_CONST
#define COM_APPL_DATA

#define LOG_CODE
#define LOG_VAR
#define LOG_CONST

#define PROV_CODE
#define PROV_VAR
#define PROV_CONST

#define OTA_CODE
#define OTA_VAR
#define OTA_CONST

/*-------------------------------- RTE ---------------------------------------*/
#define RTE_CODE
#define RTE_VAR
#define RTE_CONST
#define RTE_APPL_DATA

/*--------------------- Application software components ----------------------*/
#define ODOSWC_CODE
#define ODOSWC_VAR
#define ODOSWC_CONST

#define BATTSWC_CODE
#define BATTSWC_VAR
#define BATTSWC_CONST

#define TELEMSWC_CODE
#define TELEMSWC_VAR
#define TELEMSWC_CONST

#define DIAGSWC_CODE
#define DIAGSWC_VAR
#define DIAGSWC_CONST

#define HMISWC_CODE
#define HMISWC_VAR
#define HMISWC_CONST

/* clang-format on */

#endif /* COMPILER_CFG_H */

/**
 * @file    EcuM.c
 * @brief   ECU state manager implementation: the startup sequence and the shutdown path.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/EcuM/EcuM.h"

#include <string.h>

#include "mcal/Adc/Adc.h"
#include "app/BattSwc/BattSwc.h"
#include "mcal/Can/Can.h"
#include "ecuabs/CanIf/CanIf.h"
#include "services/Com/Com.h"
#include "services/ComM/ComM.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "app/DiagSwc/DiagSwc.h"
#include "mcal/Dio/Dio.h"
#include "services/Fee/Fee.h"
#include "mcal/Fls/Fls.h"
#include "ecuabs/FsAbs/FsAbs.h"
#include "ecuabs/GnssIf/GnssIf.h"
#include "mcal/Gpt/Gpt.h"
#include "app/HmiSwc/HmiSwc.h"
#include "ecuabs/IoHwAb/IoHwAb.h"
#include "services/Log/Log.h"
#include "mcal/Mcu/Mcu.h"
#include "ecuabs/NetIf/NetIf.h"
#include "services/NvM/NvM.h"
#include "app/OdoSwc/OdoSwc.h"
#include "mcal/Port/Port.h"
#include "ecuabs/Rs485If/Rs485If.h"
#include "services/SchM/SchM.h"
#include "mcal/Spi/Spi.h"
#include "app/TelemSwc/TelemSwc.h"
#include "ecuabs/TimeAbs/TimeAbs.h"
#include "mcal/Uart/Uart.h"
#include "services/WdgM/WdgM.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC EcuM_StatusType EcuM_Status;
STATIC boolean EcuM_Initialised = FALSE;
STATIC Gpt_TimestampType EcuM_StartupBeganMs;

/*==================================================================================================
 *  Crash-loop detection
 *================================================================================================*/

/**
 * @brief Read the restart record, decide whether this is a crash loop, and write the record back.
 *
 * The window is measured in wall-clock time where available and falls back to counting resets otherwise:
 * the whole point is to notice a unit resetting every few minutes, and such a unit frequently has no valid
 * clock either -- which is often the very reason it is resetting.
 */
STATIC void EcuM_EvaluateCrashLoop(void)
{
    NvM_RestartInfoType info;
    uint32 now = 0u;
    boolean timeValid = FALSE;
    boolean windowExpired;

    if (NvM_ReadBlock(NVM_BLOCK_RESTART_INFO, &info) != E_OK)
    {
        return;
    }

    STD_DISCARD(TimeAbs_GetUnixTime(&now, &timeValid));

    if (info.totalRestarts < 0xFFFFFFFFuL)
    {
        info.totalRestarts++;
    }

    /* A window that cannot be evaluated is treated as still open, so resets keep accumulating. Treating it
     * as expired would reset the counter on every start and the detector would never fire -- on exactly the
     * units that need it most. */
    windowExpired = FALSE;
    if ((timeValid != FALSE) && (info.windowStartUnixTime != 0u))
    {
        windowExpired = ((now - info.windowStartUnixTime) > (uint32)ECUM_CRASH_LOOP_WINDOW_S) ? TRUE
                                                                                             : FALSE;
    }

    if (windowExpired != FALSE)
    {
        info.restartCount = 1u;
        info.windowStartUnixTime = now;
    }
    else
    {
        if (info.restartCount < 0xFFFFu)
        {
            info.restartCount++;
        }
        if (info.windowStartUnixTime == 0u)
        {
            info.windowStartUnixTime = now;
        }
    }

    info.structVersion = NVM_STRUCT_VERSION;
    info.lastResetReason = (uint32)Mcu_GetResetReason();

    EcuM_Status.restartCount = info.restartCount;
    EcuM_Status.totalRestarts = info.totalRestarts;

#if (ECUM_DEGRADED_MODE_ENABLED == STD_ON)
    if (info.restartCount >= (uint16)ECUM_CRASH_LOOP_COUNT)
    {
        EcuM_Status.crashLoopDetected = TRUE;
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CRASH_LOOP, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));

        /* Which subsystem to skip is inferred from the previous run's reset cause. A watchdog bite or a
         * panic means something hung or trapped, and the two candidates on this ECU are the battery bus
         * -- a blocking half-duplex protocol -- and the card, whose driver can block for seconds. Both are
         * skipped, because startup cannot tell which of them it was and running without either still
         * produces a unit that reports its own fault. */
        if ((info.lastResetReason == (uint32)MCU_RESET_WATCHDOG) ||
            (info.lastResetReason == (uint32)MCU_RESET_PANIC))
        {
            EcuM_Status.degradedSubsystemMask |= (uint8)(ECUM_DEGRADED_PACKS | ECUM_DEGRADED_STORAGE);
        }
        else
        {
            /* A brown-out loop is a supply problem, not a software one. The radios are the largest current
             * draw, so skipping the network is what gives the supply a chance to hold up long enough to
             * report the fault. */
            EcuM_Status.degradedSubsystemMask |= (uint8)ECUM_DEGRADED_NETWORK;
        }

        LOG_ERROR(MODULE_ID_ECUM, "crash loop: %u resets, degrading mask 0x%02X",
                  (unsigned int)info.restartCount,
                  (unsigned int)EcuM_Status.degradedSubsystemMask);
    }
#endif

    /* Written immediately: its entire purpose is to be readable after the reset that incremented it, and
     * the NvM block is configured for immediate write-through for exactly this reason. */
    STD_DISCARD(NvM_WriteBlock(NVM_BLOCK_RESTART_INFO, &info));
}

boolean EcuM_IsSubsystemDegraded(uint8 subsystemMask)
{
    return ((EcuM_Status.degradedSubsystemMask & subsystemMask) != 0u) ? TRUE : FALSE;
}

void EcuM_ClearCrashLoopCounter(void)
{
    NvM_RestartInfoType info;
    uint32 now = 0u;
    boolean timeValid = FALSE;

    if (NvM_ReadBlock(NVM_BLOCK_RESTART_INFO, &info) != E_OK)
    {
        return;
    }

    STD_DISCARD(TimeAbs_GetUnixTime(&now, &timeValid));

    info.restartCount = 0u;
    info.windowStartUnixTime = now;
    STD_DISCARD(NvM_WriteBlock(NVM_BLOCK_RESTART_INFO, &info));

    EcuM_Status.restartCount = 0u;
    EcuM_Status.crashLoopDetected = FALSE;
    EcuM_Status.degradedSubsystemMask = 0u;

    STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CRASH_LOOP, INSTANCE_ID_SINGLE,
                                   DEM_EVENT_STATUS_PASSED));
}

/*==================================================================================================
 *  Startup
 *================================================================================================*/

/** Bring up the MCAL. Nothing here may fail non-fatally: without it, nothing else can run. */
STATIC Std_ReturnType EcuM_InitMcal(void)
{
    Std_ReturnType status = E_OK;

    /* Mcu first: it latches the reset cause, which is the input to crash-loop detection. */
    status |= Mcu_Init();
    status |= Gpt_Init();

    /* Det second, before anything that might report. A violation arriving earlier can only be counted, not
     * recorded with its module, API and error code -- and startup is exactly when a misconfiguration
     * surfaces. */
    Det_Init();

    status |= Log_Init();

    LOG_INFO(MODULE_ID_ECUM, "%s %s (%s, %s) reset=%s", "telematics-ecu", ECU_FIRMWARE_VERSION,
             ECU_BUILD_GIT_DESCRIBE, ECU_BUILD_TIMESTAMP,
             Mcu_GetResetReasonName(Mcu_GetResetReason()));

    /* Port before Spi: both chip selects must be driven high before the SPI peripheral starts clocking, or
     * an attached device reads the initialisation traffic as a command. */
    status |= Port_Init();
    status |= Spi_Init();
    status |= Adc_Init();
    status |= Uart_Init();

    return (status == E_OK) ? E_OK : E_NOT_OK;
}

/** Bring up the memory stack. A failure here means defaults, not a refusal to start. */
STATIC void EcuM_InitMemory(void)
{
    if (Fls_Init() != E_OK)
    {
        LOG_ERROR(MODULE_ID_ECUM, "flash driver unavailable; NV data will not persist");
        return;
    }

    if (Fee_Init() != E_OK)
    {
        LOG_ERROR(MODULE_ID_ECUM, "EEPROM emulation unavailable");
        return;
    }

    if (NvM_Init() == E_OK)
    {
        NvM_StatisticsType stats;

        EcuM_Status.subsystems.nvmValid = TRUE;
        if (NvM_GetStatistics(&stats) == E_OK)
        {
            if (stats.defaultsApplied > 0u)
            {
                /* Defaults are a working state, not a failure -- a new unit takes this path -- but a unit
                 * that has been in service should not, so it is reported. */
                EcuM_Status.subsystems.nvmValid = FALSE;
                LOG_WARN(MODULE_ID_ECUM, "%lu NV blocks fell back to defaults",
                         (unsigned long)stats.defaultsApplied);
                STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_NVM_INTEGRITY, INSTANCE_ID_SINGLE,
                                               DEM_EVENT_STATUS_FAILED));
            }
            else
            {
                STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_NVM_INTEGRITY, INSTANCE_ID_SINGLE,
                                               DEM_EVENT_STATUS_PASSED));
            }
        }
    }
}

/** Bring up the sensing subsystems, each failing independently. */
STATIC void EcuM_InitSensors(void)
{
    if (Can_Init() == E_OK)
    {
        EcuM_Status.subsystems.canAvailable = TRUE;
        STD_DISCARD(CanIf_Init());
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_INIT_FAILED, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_PASSED));
    }
    else
    {
        /* Continue without CAN. Battery, GNSS and voltage data are still worth recording, and a unit that
         * reports "no CAN" is far more useful than one rebooting every second -- which is what v1's
         * ESP.restart() on this path produced. */
        LOG_ERROR(MODULE_ID_ECUM, "CAN controller did not initialise; no odometry this run");
        STD_DISCARD(Dem_SetEventStatus(DEM_EVENT_CAN_INIT_FAILED, INSTANCE_ID_SINGLE,
                                       DEM_EVENT_STATUS_FAILED));
        STD_DISCARD(CanIf_Init());
    }

    Gpt_DelayMs(ECUM_SUBSYSTEM_SETTLE_MS);

    if (EcuM_IsSubsystemDegraded((uint8)ECUM_DEGRADED_PACKS) != FALSE)
    {
        LOG_WARN(MODULE_ID_ECUM, "battery bus skipped: crash-loop degraded mode");
    }
    else if (Rs485If_Init() == E_OK)
    {
        if (Rs485If_DiscoverPacks() == E_OK)
        {
            EcuM_Status.subsystems.packsAvailable = TRUE;
            LOG_INFO(MODULE_ID_ECUM, "%u battery packs discovered",
                     (unsigned int)Rs485If_GetPresentPackCount());
        }
        else
        {
            LOG_ERROR(MODULE_ID_ECUM, "no battery packs answered discovery");
        }
        STD_DISCARD(BattSwc_Init());
    }
    else
    {
        LOG_ERROR(MODULE_ID_ECUM, "battery bus would not open");
    }

    Gpt_DelayMs(ECUM_SUBSYSTEM_SETTLE_MS);

#if (ECU_FEATURE_GNSS == STD_ON)
    if (EcuM_IsSubsystemDegraded((uint8)ECUM_DEGRADED_GNSS) == FALSE)
    {
        if (GnssIf_Init() == E_OK)
        {
            EcuM_Status.subsystems.gnssAvailable = TRUE;
        }
        else
        {
            LOG_ERROR(MODULE_ID_ECUM, "GNSS link would not open");
        }
    }
#endif

    if (IoHwAb_Init() != E_OK)
    {
        LOG_ERROR(MODULE_ID_ECUM, "analogue calibration unusable");
    }
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType EcuM_Init(void)
{
    (void)memset(&EcuM_Status, 0, sizeof(EcuM_Status));
    EcuM_Status.state = ECUM_STATE_STARTUP;

    if (EcuM_InitMcal() != E_OK)
    {
        /* The MCAL is the one layer with no degraded mode: without a time base or a configured port there
         * is nothing to run. Reported and then reset, so the next start gets a clean attempt. */
        Det_Panic(MODULE_ID_ECUM, ECUM_API_ID_INIT, ECUM_E_STARTUP_FAILED);
    }

    EcuM_StartupBeganMs = Gpt_GetMonotonicMs();
    EcuM_Status.lastResetReason = (uint8)Mcu_GetResetReason();

    /* Slow mode: startup legitimately blocks for tens of seconds mounting the card and attaching to GPRS.
     * Arming the fast timeout here would reset a unit that is merely starting up slowly. */
    if (WdgM_Init() != E_OK)
    {
        LOG_ERROR(MODULE_ID_ECUM, "watchdog would not arm");
    }

    STD_DISCARD(Dem_Init());
    STD_DISCARD(Com_Init());

    EcuM_InitMemory();

    /* The clock is established before crash-loop evaluation, because the window is measured in wall-clock
     * time where one is available. */
    if (TimeAbs_Init() == E_OK)
    {
        EcuM_Status.subsystems.clockValid = TRUE;
    }
    else
    {
        LOG_WARN(MODULE_ID_ECUM, "no valid wall clock; records will carry uptime only");
    }

    EcuM_EvaluateCrashLoop();

    STD_DISCARD(DiagSwc_Init());
    STD_DISCARD(OdoSwc_Init());
    STD_DISCARD(HmiSwc_Init());
    HmiSwc_SelfTest();

    EcuM_InitSensors();

#if (ECU_FEATURE_SD == STD_ON)
    if (EcuM_IsSubsystemDegraded((uint8)ECUM_DEGRADED_STORAGE) != FALSE)
    {
        LOG_WARN(MODULE_ID_ECUM, "storage skipped: crash-loop degraded mode");
    }
    else if (FsAbs_Init() == E_OK)
    {
        EcuM_Status.subsystems.storageAvailable = TRUE;
    }
    else
    {
        /* Live publishing continues without a store-and-forward buffer. v1 waited ten minutes and then
         * restarted, which guaranteed nothing was ever logged rather than merely not buffered. */
        LOG_ERROR(MODULE_ID_ECUM, "card would not mount; no store-and-forward buffer");
    }
#endif

    if (EcuM_IsSubsystemDegraded((uint8)ECUM_DEGRADED_NETWORK) == FALSE)
    {
        if (NetIf_Init() == E_OK)
        {
            STD_DISCARD(ComM_Init());
            STD_DISCARD(TelemSwc_Init());
            STD_DISCARD(ComM_RequestMode(COMM_FULL_COMMUNICATION));
        }
        else
        {
            LOG_ERROR(MODULE_ID_ECUM, "network stack would not initialise");
        }
    }
    else
    {
        LOG_WARN(MODULE_ID_ECUM, "network skipped: crash-loop degraded mode");
    }

    if (SchM_Init() != E_OK)
    {
        Det_Panic(MODULE_ID_ECUM, ECUM_API_ID_INIT, ECUM_E_STARTUP_FAILED);
    }

    if (SchM_StartTasks() != E_OK)
    {
        /* The one genuinely fatal startup failure. An ECU missing a task silently stops doing part of its
         * job, and nothing downstream can tell that apart from a vehicle that is not moving. */
        LOG_ERROR(MODULE_ID_ECUM, "tasks could not be created");
        Det_Panic(MODULE_ID_ECUM, ECUM_API_ID_INIT, ECUM_E_STARTUP_FAILED);
    }

    /* Supervision last, now that the tasks genuinely exist. Entities start deactivated, so activating them
     * earlier would have every one report an alive violation on the first cycle. */
    if (WdgM_ActivateSupervision() != E_OK)
    {
        LOG_ERROR(MODULE_ID_ECUM, "supervision would not activate");
    }

    Dem_StartOperationCycle();

    EcuM_Status.startupDurationMs = Gpt_ElapsedSince(EcuM_StartupBeganMs);
    EcuM_Status.state = (EcuM_Status.degradedSubsystemMask == 0u) ? ECUM_STATE_RUN
                                                                 : ECUM_STATE_RUN_DEGRADED;
    EcuM_Initialised = TRUE;

    LOG_INFO(MODULE_ID_ECUM, "startup complete in %lu ms, state=%u, can=%u packs=%u gnss=%u sd=%u",
             (unsigned long)EcuM_Status.startupDurationMs, (unsigned int)EcuM_Status.state,
             (unsigned int)EcuM_Status.subsystems.canAvailable,
             (unsigned int)EcuM_Status.subsystems.packsAvailable,
             (unsigned int)EcuM_Status.subsystems.gnssAvailable,
             (unsigned int)EcuM_Status.subsystems.storageAvailable);

    return E_OK;
}

void EcuM_ShutdownAndReset(void)
{
    EcuM_Status.state = ECUM_STATE_SHUTDOWN;

    LOG_INFO(MODULE_ID_ECUM, "shutting down");

    /* Supervision is stopped first, so that the flushing below -- which involves several flash writes and
     * may take a second -- cannot be mistaken for a hung task and reset the ECU part way through. */
    STD_DISCARD(WdgM_DeactivateEntity(WDGM_SE_SCHEDULER));
    STD_DISCARD(WdgM_DeactivateEntity(WDGM_SE_ACQUISITION));
    STD_DISCARD(WdgM_DeactivateEntity(WDGM_SE_STORAGE));
    STD_DISCARD(WdgM_DeactivateEntity(WDGM_SE_TELEMETRY));

    /* The odometer first: it is the one value whose loss cannot be recovered from anywhere else. */
    STD_DISCARD(OdoSwc_Persist());
    STD_DISCARD(DiagSwc_Persist());
    STD_DISCARD(NvM_WriteAll());

    /* A final health record, so the fleet learns that this reset was deliberate. Without it, a controlled
     * reset is indistinguishable from a crash. */
    TelemSwc_PublishHealth();

    IoHwAb_AllIndicatorsOff();
    Rs485If_DeInit();
    GnssIf_DeInit();
    Can_DeInit();

    Mcu_PerformReset();
}

EcuM_StateType EcuM_GetState(void)
{
    return EcuM_Status.state;
}

Std_ReturnType EcuM_GetStatus(EcuM_StatusType *status)
{
    DET_CHECK_RETURN(status != NULL_PTR, MODULE_ID_ECUM, INSTANCE_ID_SINGLE,
                     ECUM_API_ID_GET_STATE, ECUM_E_PARAM_POINTER, E_NOT_OK);

    *status = EcuM_Status;
    return E_OK;
}

void EcuM_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = ECUM_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_ECUM;
        versioninfo->sw_major_version = ECUM_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = ECUM_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = ECUM_SW_PATCH_VERSION;
    }
}

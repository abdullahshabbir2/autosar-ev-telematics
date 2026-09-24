/**
 * @file    DiagSwc_Cfg.h
 * @brief   Diagnostics configuration.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DIAGSWC_CFG_H
#define DIAGSWC_CFG_H

#include "services/Dem/Dem_Cfg.h"
#include "base/Std_Types.h"

#define DIAGSWC_DEV_ERROR_DETECT STD_ON

/**
 * @brief Interval between checks for a pending DTC persist, in milliseconds.
 *
 * 5000 ms. Long enough that a fault reporting on every cycle produces at most one write per five seconds;
 * short enough that a fault confirmed shortly before an uncontrolled power loss is usually already stored.
 * The controlled shutdown path writes unconditionally, so this only bounds the uncontrolled case.
 */
#define DIAGSWC_PERSIST_CHECK_MS 5000uL

/**
 * @brief Largest diagnostic request this ECU will parse, in bytes.
 *
 * 64. The largest defined request is a WriteDataByIdentifier with a four-byte value, so 64 is ample and
 * bounds the work any single inbound message can cause.
 */
#define DIAGSWC_MAX_REQUEST_SIZE 64u

/**
 * @brief Largest diagnostic response, in bytes.
 *
 * 256. The largest is ReadDTCInformation listing every confirmed DTC: at four bytes each across
 * DEM_EVENT_COUNT events that is 100 bytes, plus the header.
 */
#define DIAGSWC_MAX_RESPONSE_SIZE 256u

/** DTCs the persistent record can hold. Every configured event, so nothing is ever dropped. */
#define DIAGSWC_PERSISTENT_DTC_SLOTS DEM_EVENT_COUNT

/**
 * @brief Whether an ECU reset may be requested remotely.
 *
 * On. A remote reset is the only recovery available for a unit that has entered a degraded mode and is
 * physically inaccessible. It is guarded by the shutdown path, which flushes NvM and the DTC record first,
 * so a reset never costs data -- which is what makes allowing it reasonable.
 */
#define DIAGSWC_ALLOW_REMOTE_RESET STD_ON

#endif /* DIAGSWC_CFG_H */

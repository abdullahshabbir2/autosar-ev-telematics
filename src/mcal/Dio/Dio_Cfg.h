/**
 * @file    Dio_Cfg.h
 * @brief   DIO channel configuration -- logical channel to GPIO mapping.
 *
 * Channel IDs are the GPIO numbers themselves. That is unusual for AUTOSAR, which
 * normally renumbers channels 0..n, but it is the right call here: the ESP32 has a
 * flat GPIO matrix with no ports, so a renumbering layer would add a lookup table
 * and an indirection that buys nothing, while making a Det report harder to read
 * against a schematic. The symbolic names below are what call sites use; the numeric
 * value is an implementation detail that only ::Dio_ChannelIsConfigured cares about.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DIO_CFG_H
#define DIO_CFG_H

#include "Ecu_PinMap.h"
#include "Std_Types.h"

/** Report an invalid channel ID to Det rather than writing an unmapped pin. */
#define DIO_DEV_ERROR_DETECT STD_ON

/*------------------------------- Output channels ----------------------------*/

#define DIO_CHANNEL_LED_ACQ ((Dio_ChannelType)PIN_LED_ACQ)
#define DIO_CHANNEL_LED_STORAGE ((Dio_ChannelType)PIN_LED_STORAGE)
#define DIO_CHANNEL_LED_LINK ((Dio_ChannelType)PIN_LED_LINK)
#define DIO_CHANNEL_LED_CLOUD ((Dio_ChannelType)PIN_LED_CLOUD)
#define DIO_CHANNEL_LED_HEARTBEAT ((Dio_ChannelType)PIN_LED_HEARTBEAT)

#define DIO_CHANNEL_RS485_DE ((Dio_ChannelType)PIN_RS485_DE)
#define DIO_CHANNEL_CAN_CS ((Dio_ChannelType)PIN_CAN_CS)
#define DIO_CHANNEL_SD_CS ((Dio_ChannelType)PIN_SD_CS)
#define DIO_CHANNEL_GSM_PWRKEY ((Dio_ChannelType)PIN_GSM_PWRKEY)

/*------------------------------- Input channels -----------------------------*/

#define DIO_CHANNEL_CAN_INT ((Dio_ChannelType)PIN_CAN_INT)
#define DIO_CHANNEL_GSM_STATUS ((Dio_ChannelType)PIN_GSM_STATUS)

/** Number of channels the driver manages. */
#define DIO_CONFIGURED_CHANNEL_COUNT 11u

/** Highest GPIO number this target exposes; channel IDs above it are rejected. */
#define DIO_MAX_CHANNEL_ID 39u

/**
 * @brief Whether an LED is lit by a high or a low level.
 *
 * Active high, because GPIO2 and GPIO15 are strapping pins that must read low at
 * reset; an anode-to-pin LED satisfies that. See the note in Ecu_PinMap.h.
 */
#define DIO_LED_ACTIVE_LEVEL STD_HIGH
#define DIO_LED_INACTIVE_LEVEL STD_LOW

#endif /* DIO_CFG_H */

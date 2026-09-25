/**
 * @file    TimeAbs_Esp32.cpp
 * @brief   ESP32 platform leaf of the wall-clock abstraction: DS3231 over I2C, SNTP over lwIP.
 *
 * @par The DS3231 is addressed directly, not through RTClib
 * v1 used Adafruit RTClib, which returns a @c DateTime object and reports a missing device by returning a
 * default-constructed one -- so an unplugged RTC read as 1 January 2000 with no error. That value then went
 * into log file names and record timestamps, and the only symptom was a day's data filed under the wrong date.
 *
 * Talking to the four BCD registers directly is about thirty lines and lets the two outcomes be distinguished:
 * a bus error is reported as one, and an implausible but well-formed reading is rejected by TimeAbs.c's
 * plausibility window. The device is simple enough that a library buys nothing here.
 *
 * @par What is *not* in this file
 * The calendar arithmetic -- days per month, leap years, the Unix epoch conversion -- is in TimeAbs.c, where
 * the host suite tests it against known dates including the 2100 non-leap-year case. Only the register access
 * and the SNTP call are here, because they are the only parts that need hardware.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <Arduino.h>
#include <Wire.h>
#include <time.h>

#include "Ecu_PinMap.h"
#include "mcal/Gpt/Gpt.h"
#include "ecuabs/TimeAbs/TimeAbs.h"
#include "ecuabs/TimeAbs/TimeAbs_Platform.h"

/*==================================================================================================
 *  DS3231 register map
 *================================================================================================*/

#define DS3231_I2C_ADDRESS 0x68u

#define DS3231_REG_SECONDS 0x00u
#define DS3231_REG_STATUS 0x0Fu

/** Bit 7 of the status register: set while the oscillator has stopped since it was last cleared. */
#define DS3231_STATUS_OSF 0x80u

/** I2C clock. 100 kHz rather than 400 kHz: the bus runs to a separate module on this board. */
#define DS3231_I2C_CLOCK_HZ 100000uL

/** Bound on any single I2C exchange. The DS3231 answers in microseconds; 50 ms only expires on a fault. */
#define DS3231_I2C_TIMEOUT_MS 50uL

static boolean TimeAbs_RtcPresent = FALSE;

/*==================================================================================================
 *  BCD conversion
 *================================================================================================*/

/**
 * @brief Convert a packed BCD byte to binary.
 *
 * Returns 0xFF for a byte whose nibbles are not valid decimal digits, which the caller treats as a bus error.
 * A DS3231 whose backup cell has failed returns 0xFF in every register, and without this check that reads as a
 * plausible-looking time rather than as the missing device it is.
 */
static uint8 TimeAbs_BcdToBinary(uint8 bcd)
{
    const uint8 high = (uint8)((bcd >> 4u) & 0x0Fu);
    const uint8 low = (uint8)(bcd & 0x0Fu);

    if ((high > 9u) || (low > 9u))
    {
        return 0xFFu;
    }

    return (uint8)((high * 10u) + low);
}

/** Convert a binary value in 0..99 to packed BCD. */
static uint8 TimeAbs_BinaryToBcd(uint8 value)
{
    return (uint8)(((value / 10u) << 4u) | (value % 10u));
}

/*==================================================================================================
 *  Register access
 *================================================================================================*/

/** Read @p count consecutive registers from @p startRegister. */
static Std_ReturnType TimeAbs_RtcReadRegisters(uint8 startRegister, uint8 *buffer, uint8 count)
{
    uint8 i;

    Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
    (void)Wire.write(startRegister);

    /* endTransmission(false) leaves the bus held for the repeated start the read needs. Releasing it here --
     * which is what endTransmission() with no argument does -- lets another master, or a bus glitch, land
     * between the address write and the data read, and the returned bytes then come from an unknown register. */
    if (Wire.endTransmission(false) != 0u)
    {
        return E_NOT_OK;
    }

    if (Wire.requestFrom((uint8_t)DS3231_I2C_ADDRESS, (uint8_t)count, (uint8_t)true) != (size_t)count)
    {
        return E_NOT_OK;
    }

    for (i = 0u; i < count; i++)
    {
        const int value = Wire.read();

        if (value < 0)
        {
            return E_NOT_OK;
        }

        buffer[i] = (uint8)value;
    }

    return E_OK;
}

/** Write @p count consecutive registers starting at @p startRegister. */
static Std_ReturnType TimeAbs_RtcWriteRegisters(uint8 startRegister, const uint8 *buffer, uint8 count)
{
    uint8 i;

    Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
    (void)Wire.write(startRegister);

    for (i = 0u; i < count; i++)
    {
        (void)Wire.write(buffer[i]);
    }

    return (Wire.endTransmission(true) == 0u) ? E_OK : E_NOT_OK;
}

/*==================================================================================================
 *  Platform leaf
 *================================================================================================*/

extern "C" Std_ReturnType TimeAbs_PlatformRtcInit(void)
{
    uint8 status;

    if (Wire.begin((int)PIN_I2C_SDA, (int)PIN_I2C_SCL, DS3231_I2C_CLOCK_HZ) == false)
    {
        return E_NOT_OK;
    }

    /* A bounded timeout, and clear-on-timeout enabled. Without it the ESP32 I2C driver can wait forever for a
     * clock line held low by a device that reset mid-transfer, which on a shared bus is a realistic way for
     * one faulty peripheral to hang a task that has nothing else wrong with it. */
    Wire.setTimeOut((uint16_t)DS3231_I2C_TIMEOUT_MS);

    /* The presence check is a register read, not a bare address probe. Some devices acknowledge their address
     * while unable to return data, and a probe that only checks the ACK reports them as healthy. */
    if (TimeAbs_RtcReadRegisters(DS3231_REG_STATUS, &status, 1u) != E_OK)
    {
        TimeAbs_RtcPresent = FALSE;
        return E_NOT_OK;
    }

    if ((status & (uint8)DS3231_STATUS_OSF) != 0u)
    {
        /* The oscillator stopped at some point since this flag was last cleared, so whatever the registers hold
         * has no defined relationship to real time. The flag is cleared and E_NOT_OK returned: the device is
         * present and usable, but its current reading must not be trusted, and TimeAbs falls back to NTP or to
         * the last persisted timestamp. Reporting success here is how v1 came to timestamp records with the
         * moment its backup cell died. */
        const uint8 cleared = (uint8)(status & (uint8)~DS3231_STATUS_OSF);

        (void)TimeAbs_RtcWriteRegisters(DS3231_REG_STATUS, &cleared, 1u);
        TimeAbs_RtcPresent = TRUE;
        return E_NOT_OK;
    }

    TimeAbs_RtcPresent = TRUE;
    return E_OK;
}

extern "C" Std_ReturnType TimeAbs_PlatformRtcRead(uint32 *unixTime)
{
    uint8 registers[7];
    TimeAbs_DateTimeType dateTime;

    if ((unixTime == NULL_PTR) || (TimeAbs_RtcPresent == FALSE))
    {
        return E_NOT_OK;
    }

    if (TimeAbs_RtcReadRegisters(DS3231_REG_SECONDS, registers, 7u) != E_OK)
    {
        return E_NOT_OK;
    }

    dateTime.second = TimeAbs_BcdToBinary((uint8)(registers[0] & 0x7Fu));
    dateTime.minute = TimeAbs_BcdToBinary((uint8)(registers[1] & 0x7Fu));

    /* Bit 6 of the hours register selects 12-hour mode. This driver always writes 24-hour mode, but a device
     * that was previously set by other software may still be in 12-hour mode, and misreading that gives an
     * error of up to twelve hours -- which is plausible enough to pass every downstream sanity check. */
    if ((registers[2] & 0x40u) != 0u)
    {
        const uint8 hour12 = TimeAbs_BcdToBinary((uint8)(registers[2] & 0x1Fu));
        const boolean isPm = ((registers[2] & 0x20u) != 0u) ? TRUE : FALSE;

        if (hour12 == 0xFFu)
        {
            return E_NOT_OK;
        }

        if (isPm != FALSE)
        {
            dateTime.hour = (hour12 == 12u) ? 12u : (uint8)(hour12 + 12u);
        }
        else
        {
            dateTime.hour = (hour12 == 12u) ? 0u : hour12;
        }
    }
    else
    {
        dateTime.hour = TimeAbs_BcdToBinary((uint8)(registers[2] & 0x3Fu));
    }

    dateTime.day = TimeAbs_BcdToBinary((uint8)(registers[4] & 0x3Fu));
    dateTime.month = TimeAbs_BcdToBinary((uint8)(registers[5] & 0x1Fu));
    dateTime.year = (uint16)(2000u + TimeAbs_BcdToBinary(registers[6]));

    /* The DS3231's own weekday counter is carried through so the structure is fully initialised, but nothing
     * relies on it: ::TimeAbs_ToDateTime derives the weekday from the date, and this register is whatever the
     * last writer happened to put there. Register 3 counts 1..7 with no defined mapping to a particular day;
     * the structure's convention is 0 = Sunday, so an out-of-range value becomes 0 rather than a wrong day. */
    {
        const uint8 rtcWeekday = (uint8)(registers[3] & 0x07u);

        dateTime.weekday = (rtcWeekday >= 1u) ? (uint8)(rtcWeekday - 1u) : 0u;
    }

    /* One 0xFF from the BCD converter means the register held something that is not a decimal number, which on
     * this device means the bus returned rubbish. Checked as a group so that no single malformed field can slip
     * through into the calendar conversion. */
    if ((dateTime.second == 0xFFu) || (dateTime.minute == 0xFFu) || (dateTime.hour == 0xFFu)
        || (dateTime.day == 0xFFu) || (dateTime.month == 0xFFu) || (dateTime.year == (2000u + 0xFFu)))
    {
        return E_NOT_OK;
    }

    /* The calendar conversion, and the plausibility window that rejects a well-formed but impossible date, both
     * belong to TimeAbs.c and are tested on the host. */
    return TimeAbs_FromDateTime(&dateTime, unixTime);
}

extern "C" Std_ReturnType TimeAbs_PlatformRtcWrite(uint32 unixTime)
{
    TimeAbs_DateTimeType dateTime;
    uint8 registers[7];

    if (TimeAbs_RtcPresent == FALSE)
    {
        return E_NOT_OK;
    }

    if (TimeAbs_ToDateTime(unixTime, &dateTime) != E_OK)
    {
        return E_NOT_OK;
    }

    registers[0] = TimeAbs_BinaryToBcd(dateTime.second);
    registers[1] = TimeAbs_BinaryToBcd(dateTime.minute);
    /* Bit 6 left clear: 24-hour mode, always. */
    registers[2] = TimeAbs_BinaryToBcd(dateTime.hour);
    /* Day of week. The DS3231 only counts it and never derives anything from it, and nothing in this ECU reads
     * it, so a fixed 1 is written rather than computing a value that would then be the only unverified field. */
    registers[3] = 1u;
    registers[4] = TimeAbs_BinaryToBcd(dateTime.day);
    registers[5] = TimeAbs_BinaryToBcd(dateTime.month);
    registers[6] = TimeAbs_BinaryToBcd((uint8)(dateTime.year - 2000u));

    return TimeAbs_RtcWriteRegisters(DS3231_REG_SECONDS, registers, 7u);
}

extern "C" Std_ReturnType TimeAbs_PlatformNtpFetch(uint32 *unixTime, uint32 timeoutMs)
{
    const Gpt_TimestampType start = Gpt_GetMonotonicMs();

    if (unixTime == NULL_PTR)
    {
        return E_NOT_OK;
    }

    /* UTC with no offset and no DST rule. Every timestamp this ECU produces is UTC, and local time is applied
     * by whoever displays it. A device that applies a timezone itself produces a log whose ordering breaks
     * twice a year, and nothing downstream can repair it because the offset was never recorded. */
    configTime(0L, 0, TIMEABS_NTP_SERVER);

    /* Polling rather than an SNTP callback, because this is called from a state machine that already owns its
     * own timing and must not be re-entered from lwIP's thread. */
    while (Gpt_HasElapsed(start, timeoutMs) == FALSE)
    {
        const time_t now = time(NULL);

        /* The epoch test is the acceptance criterion: the ESP32 clock starts at 1970 and lwIP only steps it
         * once a response has been validated, so a value past the configured floor is proof that SNTP
         * answered. Waiting for a specific callback would be equivalent, and this needs no lwIP internals. */
        if ((uint32)now >= (uint32)TIMEABS_MIN_PLAUSIBLE_UNIX)
        {
            *unixTime = (uint32)now;
            return E_OK;
        }

        Gpt_DelayMs(100u);
    }

    return E_TIMEOUT;
}

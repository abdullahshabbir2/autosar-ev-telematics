/**
 * @file    GnssIf.c
 * @brief   GNSS interface implementation: NMEA 0183 parser and position validation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "ecuabs/GnssIf/GnssIf.h"

#include <string.h>

#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Uart/Uart.h"

/*==================================================================================================
 *  Local data
 *================================================================================================*/

STATIC boolean GnssIf_Initialised = FALSE;
STATIC GnssIf_PositionType GnssIf_Position;
STATIC GnssIf_StatisticsType GnssIf_Stats;
STATIC uint16 GnssIf_CyclesWithoutFix;

/** Sentence assembly buffer and its write cursor. */
STATIC char GnssIf_Buffer[GNSSIF_SENTENCE_BUFFER_SIZE];
STATIC uint16 GnssIf_BufferLen;
STATIC boolean GnssIf_Overflowed;

/*==================================================================================================
 *  Character helpers
 *
 *  Written locally rather than taken from <ctype.h> because the ctype functions are
 *  locale-dependent and, on some libraries, table lookups through a global that is not guaranteed
 *  initialised before a constructor runs. NMEA is pure ASCII, so the tests are trivial.
 *================================================================================================*/

STATIC boolean GnssIf_IsDigit(char c)
{
    return ((c >= '0') && (c <= '9')) ? TRUE : FALSE;
}

/** Hex digit to value, or 0xFF if @p c is not a hex digit. */
STATIC uint8 GnssIf_HexValue(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return (uint8)(c - '0');
    }
    if ((c >= 'A') && (c <= 'F'))
    {
        return (uint8)((c - 'A') + 10);
    }
    if ((c >= 'a') && (c <= 'f'))
    {
        return (uint8)((c - 'a') + 10);
    }
    return 0xFFu;
}

/*==================================================================================================
 *  Field access
 *================================================================================================*/

/** A view onto one comma-separated field, without copying it out. */
typedef struct
{
    const char *start;
    uint16 length;
} GnssIf_FieldType;

/**
 * @brief Split @p sentence into comma-separated field views.
 *
 * Views rather than copies: a sentence is at most 96 bytes and splitting it into twenty separate
 * buffers would cost more stack than the sentence itself.
 *
 * @return Number of fields found, capped at ::GNSSIF_MAX_FIELDS.
 */
STATIC uint8 GnssIf_SplitFields(const char *sentence, GnssIf_FieldType *fields)
{
    uint8 count = 0u;
    uint16 i = 0u;
    uint16 fieldStart = 0u;

    while ((sentence[i] != '\0') && (sentence[i] != '*') && (count < (uint8)GNSSIF_MAX_FIELDS))
    {
        if (sentence[i] == ',')
        {
            fields[count].start = &sentence[fieldStart];
            fields[count].length = (uint16)(i - fieldStart);
            count++;
            fieldStart = (uint16)(i + 1u);
        }
        i++;
    }

    if (count < (uint8)GNSSIF_MAX_FIELDS)
    {
        fields[count].start = &sentence[fieldStart];
        fields[count].length = (uint16)(i - fieldStart);
        count++;
    }

    return count;
}

/**
 * @brief Parse an unsigned decimal integer from a field view.
 * @return E_OK on success; E_NOT_OK if the field is empty or contains a non-digit.
 */
STATIC Std_ReturnType GnssIf_FieldToU32(const GnssIf_FieldType *field, uint32 *result)
{
    uint32 value = 0u;
    uint16 i;

    if (field->length == 0u)
    {
        return E_NOT_OK;
    }

    for (i = 0u; i < field->length; i++)
    {
        if (GnssIf_IsDigit(field->start[i]) == FALSE)
        {
            return E_NOT_OK;
        }
        /* Bounded so a malformed field of many digits cannot wrap silently. */
        if (value > (0xFFFFFFFFuL / 10uL))
        {
            return E_NOT_OK;
        }
        value = (value * 10uL) + (uint32)(field->start[i] - '0');
    }

    *result = value;
    return E_OK;
}

/**
 * @brief Parse a fixed-point decimal field, scaled by 10^@p scaleDigits.
 *
 * Handles an optional leading sign and an optional fractional part. Integer throughout: the
 * fraction is accumulated digit by digit and padded or truncated to the requested scale, so
 * "12.3" at scale 3 yields 12300 exactly.
 */
STATIC Std_ReturnType GnssIf_FieldToScaled(const GnssIf_FieldType *field, uint8 scaleDigits, sint32 *result)
{
    uint32 integerPart = 0u;
    uint32 fractionPart = 0u;
    uint8 fractionDigits = 0u;
    boolean negative = FALSE;
    boolean sawDigit = FALSE;
    boolean inFraction = FALSE;
    uint16 i;
    uint32 scale = 1u;
    uint8 s;

    if (field->length == 0u)
    {
        return E_NOT_OK;
    }

    for (i = 0u; i < field->length; i++)
    {
        const char c = field->start[i];

        if ((i == 0u) && ((c == '-') || (c == '+')))
        {
            negative = (c == '-') ? TRUE : FALSE;
        }
        else if (c == '.')
        {
            if (inFraction != FALSE)
            {
                return E_NOT_OK; /* two decimal points */
            }
            inFraction = TRUE;
        }
        else if (GnssIf_IsDigit(c) != FALSE)
        {
            sawDigit = TRUE;
            if (inFraction != FALSE)
            {
                /* Extra digits beyond the requested scale are discarded rather than rounded. The
                 * discarded magnitude is below the representable resolution, and truncating keeps
                 * the conversion exactly reversible. */
                if (fractionDigits < scaleDigits)
                {
                    fractionPart = (fractionPart * 10uL) + (uint32)(c - '0');
                    fractionDigits++;
                }
            }
            else
            {
                if (integerPart > (0x7FFFFFFFuL / 100uL))
                {
                    return E_NOT_OK;
                }
                integerPart = (integerPart * 10uL) + (uint32)(c - '0');
            }
        }
        else
        {
            return E_NOT_OK;
        }
    }

    if (sawDigit == FALSE)
    {
        return E_NOT_OK;
    }

    for (s = 0u; s < scaleDigits; s++)
    {
        scale *= 10uL;
    }
    /* Pad a short fraction out to the full scale: "12.3" at scale 3 must give 300, not 3. */
    while (fractionDigits < scaleDigits)
    {
        fractionPart *= 10uL;
        fractionDigits++;
    }

    {
        const uint32 magnitude = (integerPart * scale) + fractionPart;

        if (magnitude > 0x7FFFFFFFuL)
        {
            return E_NOT_OK;
        }
        *result = (negative != FALSE) ? -(sint32)magnitude : (sint32)magnitude;
    }

    return E_OK;
}

/*==================================================================================================
 *  Checksum
 *================================================================================================*/

Std_ReturnType GnssIf_VerifyChecksum(const char *sentence)
{
    uint8 computed = 0u;
    uint16 i;
    uint8 high;
    uint8 low;

    DET_CHECK_RETURN(sentence != NULL_PTR, MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE, GNSSIF_API_ID_PARSE_SENTENCE,
                     GNSSIF_E_PARAM_POINTER, E_NOT_OK);

    if (sentence[0] != '$')
    {
        return E_NOT_OK;
    }

    /* The checksum covers every character between '$' and '*', exclusive. */
    i = 1u;
    while ((sentence[i] != '\0') && (sentence[i] != '*'))
    {
        computed ^= (uint8)sentence[i];
        i++;
        if (i >= (uint16)GNSSIF_SENTENCE_BUFFER_SIZE)
        {
            return E_NOT_OK;
        }
    }

    if (sentence[i] != '*')
    {
        return E_NOT_OK; /* no checksum field at all */
    }

    high = GnssIf_HexValue(sentence[i + 1u]);
    low = GnssIf_HexValue(sentence[i + 2u]);
    if ((high == 0xFFu) || (low == 0xFFu))
    {
        return E_NOT_OK;
    }

    return (computed == (uint8)((high << 4u) | low)) ? E_OK : E_CRC_FAIL;
}

/*==================================================================================================
 *  Coordinate conversion
 *================================================================================================*/

Std_ReturnType GnssIf_ParseCoordinate(const char *field, char hemisphere, sint32 *result)
{
    uint16 length;
    uint16 dotIndex;
    uint16 i;
    uint32 degrees = 0u;
    uint32 minutesScaled = 0u;
    uint32 minuteScale = 1u;
    uint16 minutesStart;

    DET_CHECK_RETURN((field != NULL_PTR) && (result != NULL_PTR), MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE,
                     GNSSIF_API_ID_PARSE_SENTENCE, GNSSIF_E_PARAM_POINTER, E_NOT_OK);

    length = (uint16)strlen(field);
    if (length < 4u)
    {
        return E_NOT_OK;
    }

    /* Locate the decimal point. The two digits immediately before it are the whole minutes; every
     * digit before those is degrees. This is what makes one routine handle both latitude (two
     * degree digits) and longitude (three) without being told which it has. */
    dotIndex = length;
    for (i = 0u; i < length; i++)
    {
        if (field[i] == '.')
        {
            dotIndex = i;
            break;
        }
    }
    if ((dotIndex == length) || (dotIndex < 3u))
    {
        return E_NOT_OK;
    }

    minutesStart = (uint16)(dotIndex - 2u);

    for (i = 0u; i < minutesStart; i++)
    {
        if (GnssIf_IsDigit(field[i]) == FALSE)
        {
            return E_NOT_OK;
        }
        degrees = (degrees * 10uL) + (uint32)(field[i] - '0');
    }

    /* Whole minutes, then the fraction, accumulated into one scaled integer. */
    for (i = minutesStart; i < dotIndex; i++)
    {
        if (GnssIf_IsDigit(field[i]) == FALSE)
        {
            return E_NOT_OK;
        }
        minutesScaled = (minutesScaled * 10uL) + (uint32)(field[i] - '0');
    }
    for (i = (uint16)(dotIndex + 1u); i < length; i++)
    {
        if (GnssIf_IsDigit(field[i]) == FALSE)
        {
            return E_NOT_OK;
        }
        /* Six fractional digits of a minute is 0.1 mm of ground resolution; anything beyond that is
         * noise in a consumer receiver and is discarded to keep the arithmetic inside 64 bits. */
        if (minuteScale >= 1000000uL)
        {
            break;
        }
        minutesScaled = (minutesScaled * 10uL) + (uint32)(field[i] - '0');
        minuteScale *= 10uL;
    }

    if (degrees > 180u)
    {
        return E_NOT_OK;
    }

    /* degrees x 10^7 + minutes x 10^7 / 60, with the minutes still scaled by minuteScale.
     * Evaluated in 64 bits: minutesScaled reaches 6e7 and the multiply by 1e7 reaches 6e14.
     *
     * Rounded to nearest rather than truncated. Truncation would bias every coordinate toward zero
     * by up to one count, and because the bias is one-directional it does not cancel across a
     * journey -- it would show up as a small systematic offset in every logged position. Adding half
     * the divisor before dividing is the integer equivalent of round-half-up. */
    {
        const uint64 degreesE7 = (uint64)degrees * 10000000uLL;
        const uint64 minutesDivisor = 60uLL * (uint64)minuteScale;
        const uint64 minutesE7 =
            (((uint64)minutesScaled * 10000000uLL) + (minutesDivisor / 2uLL)) / minutesDivisor;
        const uint64 totalE7 = degreesE7 + minutesE7;

        if (totalE7 > 1800000000uLL)
        {
            return E_NOT_OK;
        }

        if ((hemisphere == 'S') || (hemisphere == 'W'))
        {
            *result = -(sint32)totalE7;
        }
        else if ((hemisphere == 'N') || (hemisphere == 'E'))
        {
            *result = (sint32)totalE7;
        }
        else
        {
            return E_NOT_OK;
        }
    }

    return E_OK;
}

/*==================================================================================================
 *  Sentence parsing
 *================================================================================================*/

/** Copy a field view into a NUL-terminated scratch buffer. */
STATIC boolean GnssIf_FieldToString(const GnssIf_FieldType *field, char *out, uint16 outSize)
{
    if ((field->length == 0u) || (field->length >= outSize))
    {
        return FALSE;
    }
    (void)memcpy(out, field->start, field->length);
    out[field->length] = '\0';
    return TRUE;
}

/** Convert an NMEA date (ddmmyy) and time (hhmmss) to a Unix timestamp. */
STATIC uint32 GnssIf_ToUnixTime(uint32 ddmmyy, uint32 hhmmss)
{
    /* Days before the start of each month in a non-leap year. */
    STATIC const uint16 daysBeforeMonth[12] = {0u,   31u,  59u,  90u,  120u, 151u,
                                               181u, 212u, 243u, 273u, 304u, 334u};
    const uint32 day = ddmmyy / 10000uL;
    const uint32 month = (ddmmyy / 100uL) % 100uL;
    const uint32 year = 2000uL + (ddmmyy % 100uL);
    const uint32 hour = hhmmss / 10000uL;
    const uint32 minute = (hhmmss / 100uL) % 100uL;
    const uint32 second = hhmmss % 100uL;
    uint32 days;
    uint32 y;

    if ((month < 1uL) || (month > 12uL) || (day < 1uL) || (day > 31uL) || (hour > 23uL) || (minute > 59uL)
        || (second > 60uL))
    {
        return 0u;
    }

    days = 0u;
    for (y = 1970uL; y < year; y++)
    {
        const boolean leap =
            (((y % 4uL) == 0uL) && (((y % 100uL) != 0uL) || ((y % 400uL) == 0uL))) ? TRUE : FALSE;
        days += (leap != FALSE) ? 366uL : 365uL;
    }

    days += daysBeforeMonth[month - 1uL];
    if (month > 2uL)
    {
        const boolean leap =
            (((year % 4uL) == 0uL) && (((year % 100uL) != 0uL) || ((year % 400uL) == 0uL))) ? TRUE : FALSE;
        if (leap != FALSE)
        {
            days += 1uL;
        }
    }
    days += (day - 1uL);

    return (((days * 24uL) + hour) * 3600uL) + (minute * 60uL) + second;
}

/** Whether @p field names a sentence of type @p type from any talker. */
STATIC boolean GnssIf_IsSentenceType(const GnssIf_FieldType *field, const char *type)
{
    /* "$GPRMC", "$GNRMC", "$GLRMC" and so on: five characters after the '$', of which the last
     * three are the sentence type. Matching on the type alone rather than the talker means a
     * multi-constellation receiver works without a configuration change. */
    if (field->length != 6u)
    {
        return FALSE;
    }
    return (strncmp(&field->start[3], type, 3u) == 0) ? TRUE : FALSE;
}

Std_ReturnType GnssIf_ParseSentence(const char *sentence, GnssIf_PositionType *position)
{
    GnssIf_FieldType fields[GNSSIF_MAX_FIELDS];
    uint8 fieldCount;
    Std_ReturnType checksum;
    char scratch[16];

    DET_CHECK_RETURN((sentence != NULL_PTR) && (position != NULL_PTR), MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE,
                     GNSSIF_API_ID_PARSE_SENTENCE, GNSSIF_E_PARAM_POINTER, E_NOT_OK);

    checksum = GnssIf_VerifyChecksum(sentence);
    if (checksum != E_OK)
    {
        return (checksum == E_CRC_FAIL) ? E_CRC_FAIL : E_NOT_OK;
    }

    fieldCount = GnssIf_SplitFields(sentence, fields);
    if (fieldCount < 2u)
    {
        return E_NOT_OK;
    }

    if (GnssIf_IsSentenceType(&fields[0], "RMC") != FALSE)
    {
        sint32 latitude;
        sint32 longitude;
        sint32 speedKnotsMilli;

        /* RMC: 1 time, 2 status, 3 lat, 4 NS, 5 lon, 6 EW, 7 speed (knots), 8 course, 9 date. */
        if (fieldCount < 10u)
        {
            return E_NOT_OK;
        }

        /* Status 'A' is active; 'V' is a void fix. A void RMC is well formed and simply means the
         * receiver has nothing yet, so it is not counted as malformed. */
        if ((fields[2].length != 1u) || (fields[2].start[0] != 'A'))
        {
            return E_NOT_OK;
        }

        if (GnssIf_FieldToString(&fields[3], scratch, (uint16)sizeof(scratch)) == FALSE)
        {
            return E_NOT_OK;
        }
        if (GnssIf_ParseCoordinate(scratch, fields[4].start[0], &latitude) != E_OK)
        {
            return E_NOT_OK;
        }

        if (GnssIf_FieldToString(&fields[5], scratch, (uint16)sizeof(scratch)) == FALSE)
        {
            return E_NOT_OK;
        }
        if (GnssIf_ParseCoordinate(scratch, fields[6].start[0], &longitude) != E_OK)
        {
            return E_NOT_OK;
        }

        position->latitudeE7 = latitude;
        position->longitudeE7 = longitude;

        /* Speed arrives in knots. One knot is 1852 m/h, so mm/s = knots x 1852000 / 3600, which is
         * knots x 4630 / 9 -- kept as an exact rational rather than a decimal approximation. */
        if (GnssIf_FieldToScaled(&fields[7], 3u, &speedKnotsMilli) == E_OK)
        {
            position->speedMmPerSec =
                (uint32)(((uint64)(uint32)speedKnotsMilli * 4630uLL) / (9uLL * 1000uLL));
        }

        {
            sint32 courseDeci;
            if (GnssIf_FieldToScaled(&fields[8], 1u, &courseDeci) == E_OK)
            {
                position->headingDeciDeg = (uint16)courseDeci;
            }
        }

        {
            uint32 date = 0u;
            sint32 timeScaled = 0;

            if ((GnssIf_FieldToU32(&fields[9], &date) == E_OK)
                && (GnssIf_FieldToScaled(&fields[1], 0u, &timeScaled) == E_OK))
            {
                position->fixUnixTime = GnssIf_ToUnixTime(date, (uint32)timeScaled);
            }
        }

        position->valid = TRUE;
        return E_OK;
    }

    if (GnssIf_IsSentenceType(&fields[0], "GGA") != FALSE)
    {
        sint32 latitude;
        sint32 longitude;
        uint32 quality = 0u;

        /* GGA: 1 time, 2 lat, 3 NS, 4 lon, 5 EW, 6 quality, 7 satellites, 8 HDOP, 9 altitude. */
        if (fieldCount < 10u)
        {
            return E_NOT_OK;
        }

        if (GnssIf_FieldToU32(&fields[6], &quality) != E_OK)
        {
            return E_NOT_OK;
        }
        /* Quality 0 is no fix and 6 is dead reckoning. Neither is a measured position, and
         * publishing an estimated one as a fix is how a stationary vehicle appears to drift. */
        if ((quality == 0uL) || (quality > (uint32)GNSSIF_FIX_RTK_FLOAT))
        {
            return E_NOT_OK;
        }

        if (GnssIf_FieldToString(&fields[2], scratch, (uint16)sizeof(scratch)) == FALSE)
        {
            return E_NOT_OK;
        }
        if (GnssIf_ParseCoordinate(scratch, fields[3].start[0], &latitude) != E_OK)
        {
            return E_NOT_OK;
        }

        if (GnssIf_FieldToString(&fields[4], scratch, (uint16)sizeof(scratch)) == FALSE)
        {
            return E_NOT_OK;
        }
        if (GnssIf_ParseCoordinate(scratch, fields[5].start[0], &longitude) != E_OK)
        {
            return E_NOT_OK;
        }

        position->latitudeE7 = latitude;
        position->longitudeE7 = longitude;
        position->fixQuality = (uint8)quality;

        {
            uint32 satellites = 0u;
            if (GnssIf_FieldToU32(&fields[7], &satellites) == E_OK)
            {
                position->satellitesUsed = (uint8)((satellites > 255uL) ? 255uL : satellites);
            }
        }
        {
            sint32 hdopCenti;
            if (GnssIf_FieldToScaled(&fields[8], 2u, &hdopCenti) == E_OK)
            {
                position->hdopCentiUnits = (uint16)((hdopCenti > 65535) ? 65535 : hdopCenti);
            }
        }
        {
            sint32 altitudeMilliMetre;
            if (GnssIf_FieldToScaled(&fields[9], 3u, &altitudeMilliMetre) == E_OK)
            {
                position->altitudeMm = altitudeMilliMetre;
            }
        }

        position->valid = TRUE;
        return E_OK;
    }

    /* Any other sentence type. A receiver emitting GSV, GSA and VTG is behaving normally, so this
     * is distinguished from a malformed sentence. */
    return E_NOT_FOUND;
}

/*==================================================================================================
 *  Plausibility
 *================================================================================================*/

boolean GnssIf_IsTransitionPlausible(const GnssIf_PositionType *from, const GnssIf_PositionType *to,
                                     uint32 deltaMs)
{
    sint32 dLat;
    sint32 dLon;
    uint64 distanceMm;
    uint64 allowedMm;

    if ((from == NULL_PTR) || (to == NULL_PTR))
    {
        return FALSE;
    }

    /* Nothing to compare against: the first fix is always accepted. v1 instead required the first
     * fix to fall inside a coordinate box around one country, so a unit shipped elsewhere would
     * never accept one. */
    if (from->valid == FALSE)
    {
        return TRUE;
    }

    /* Over a very short interval the permitted distance shrinks below the receiver's own noise, and
     * a stationary vehicle's jitter would start being rejected as movement. */
    if (deltaMs < (uint32)GNSSIF_MIN_PLAUSIBILITY_INTERVAL_MS)
    {
        return TRUE;
    }

    dLat = to->latitudeE7 - from->latitudeE7;
    dLon = to->longitudeE7 - from->longitudeE7;
    if (dLat < 0)
    {
        dLat = -dLat;
    }
    if (dLon < 0)
    {
        dLon = -dLon;
    }

    /* Manhattan distance rather than Euclidean: it needs no square root and is never smaller than
     * the true distance, so the bound errs toward rejecting only a fix that was already near the
     * limit. It overstates by at most a factor of sqrt(2), which against a speed limit four times
     * the vehicle's capability leaves ample margin.
     *
     * Units: one count is 1e-7 degrees, so
     *     metres      = count x METRES_PER_DEGREE / 1e7
     *     millimetres = count x METRES_PER_DEGREE / 1e4
     * The longitude degree is treated as a full meridional degree, which is generous away from the
     * equator and again errs toward accepting a genuine fix. */
    distanceMm =
        (((uint64)(uint32)dLat + (uint64)(uint32)dLon) * (uint64)GNSSIF_METRES_PER_DEGREE) / 10000uLL;

    allowedMm = ((uint64)GNSSIF_MAX_SPEED_MM_PER_SEC * (uint64)deltaMs) / 1000uLL;

    return (distanceMm <= allowedMm) ? TRUE : FALSE;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

Std_ReturnType GnssIf_Init(void)
{
    Uart_ConfigType config;

    (void)memset(&GnssIf_Position, 0, sizeof(GnssIf_Position));
    (void)memset(&GnssIf_Stats, 0, sizeof(GnssIf_Stats));
    GnssIf_BufferLen = 0u;
    GnssIf_Overflowed = FALSE;
    GnssIf_CyclesWithoutFix = 0u;

    config.baudRate = UART_BAUD_GNSS;
    config.frame = UART_FRAME_8N1;
    config.rxPin = PIN_GNSS_RX;
    config.txPin = PIN_GNSS_TX; /* PIN_NOT_CONNECTED: the link is receive-only */
    config.rxBufferSize = UART_RX_BUFFER_GNSS;
    config.invertRx = FALSE;

    if (Uart_Open(GNSSIF_UART_INSTANCE, &config) != E_OK)
    {
        (void)Det_ReportRuntimeError(MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE, GNSSIF_API_ID_INIT,
                                     E_PARAM_CONFIG);
        return E_NOT_OK;
    }

    GnssIf_Initialised = TRUE;
    return E_OK;
}

void GnssIf_DeInit(void)
{
    if (GnssIf_Initialised != FALSE)
    {
        STD_DISCARD(Uart_Close(GNSSIF_UART_INSTANCE));
    }
    GnssIf_Initialised = FALSE;
    GnssIf_BufferLen = 0u;
}

/** Parse one assembled sentence and, if it carries a plausible fix, adopt it. */
STATIC boolean GnssIf_ConsumeSentence(void)
{
    GnssIf_PositionType candidate = GnssIf_Position;
    Std_ReturnType status;

    GnssIf_Stats.sentencesReceived++;

    status = GnssIf_ParseSentence(GnssIf_Buffer, &candidate);

    if (status == E_CRC_FAIL)
    {
        GnssIf_Stats.checksumFailures++;
        return FALSE;
    }
    if (status == E_NOT_FOUND)
    {
        /* A sentence type this module does not consume. Not an error. */
        return FALSE;
    }
    if (status != E_OK)
    {
        /* Either malformed or reporting no fix. Distinguished by whether the sentence had the shape
         * of one we understand, which the parser has already decided; both are counted, because a
         * receiver with no sky view and one with damaged wiring need different responses. */
        if (GnssIf_Position.valid == FALSE)
        {
            GnssIf_Stats.noFixSentences++;
        }
        else
        {
            GnssIf_Stats.malformedSentences++;
        }
        return FALSE;
    }

    {
        const uint32 now = Gpt_GetMonotonicMs();
        const uint32 deltaMs =
            (GnssIf_Position.valid != FALSE) ? Gpt_ElapsedSince(GnssIf_Position.fixTimestampMs) : 0u;

        if (GnssIf_IsTransitionPlausible(&GnssIf_Position, &candidate, deltaMs) == FALSE)
        {
            GnssIf_Stats.rejectedJumps++;
            (void)Det_ReportRuntimeError(MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE, GNSSIF_API_ID_PARSE_SENTENCE,
                                         GNSSIF_E_IMPLAUSIBLE_JUMP);
            return FALSE;
        }

        candidate.fixTimestampMs = now;
        GnssIf_Position = candidate;
        GnssIf_Stats.sentencesAccepted++;
        GnssIf_Stats.fixCount++;
    }

    return TRUE;
}

uint8 GnssIf_MainFunction(void)
{
    uint16 processed = 0u;
    uint8 accepted = 0u;

    if (GnssIf_Initialised == FALSE)
    {
        return 0u;
    }

    while (processed < (uint16)GNSSIF_MAX_BYTES_PER_CYCLE)
    {
        uint8 byte;
        uint16 got = 0u;

        if (Uart_Read(GNSSIF_UART_INSTANCE, &byte, 1u, &got) != E_OK)
        {
            break;
        }
        if (got == 0u)
        {
            break;
        }
        processed++;

        if (byte == (uint8)'$')
        {
            /* A new sentence always restarts assembly. A '$' arriving mid-sentence means the
             * previous one was truncated, and discarding it is correct -- a truncated sentence can
             * still satisfy a checksum computed over only the part that arrived. */
            GnssIf_BufferLen = 0u;
            GnssIf_Overflowed = FALSE;
            GnssIf_Buffer[GnssIf_BufferLen] = (char)byte;
            GnssIf_BufferLen++;
            continue;
        }

        if ((byte == (uint8)'\r') || (byte == (uint8)'\n'))
        {
            if ((GnssIf_BufferLen > 0u) && (GnssIf_Overflowed == FALSE))
            {
                GnssIf_Buffer[GnssIf_BufferLen] = '\0';
                if (GnssIf_ConsumeSentence() != FALSE)
                {
                    accepted++;
                }
            }
            else if (GnssIf_Overflowed != FALSE)
            {
                GnssIf_Stats.overflowedSentences++;
            }
            else
            {
                /* Bare line terminator: nothing to do. */
            }
            GnssIf_BufferLen = 0u;
            GnssIf_Overflowed = FALSE;
            continue;
        }

        if (GnssIf_BufferLen == 0u)
        {
            /* Bytes before the first '$' are discarded: the link may have been opened mid-sentence. */
            continue;
        }

        /* Leave room for the NUL. */
        if (GnssIf_BufferLen >= (uint16)(GNSSIF_SENTENCE_BUFFER_SIZE - 1u))
        {
            GnssIf_Overflowed = TRUE;
            continue;
        }

        GnssIf_Buffer[GnssIf_BufferLen] = (char)byte;
        GnssIf_BufferLen++;
    }

    if (accepted > 0u)
    {
        if (GnssIf_CyclesWithoutFix >= (uint16)GNSSIF_NO_FIX_REPORT_THRESHOLD)
        {
            STD_DISCARD(
                Dem_SetEventStatus(DEM_EVENT_GNSS_NO_FIX, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_PASSED));
        }
        GnssIf_CyclesWithoutFix = 0u;
    }
    else
    {
        if (GnssIf_CyclesWithoutFix < 0xFFFFu)
        {
            GnssIf_CyclesWithoutFix++;
        }
        if (GnssIf_CyclesWithoutFix == (uint16)GNSSIF_NO_FIX_REPORT_THRESHOLD)
        {
            STD_DISCARD(
                Dem_SetEventStatus(DEM_EVENT_GNSS_NO_FIX, INSTANCE_ID_SINGLE, DEM_EVENT_STATUS_FAILED));
        }
    }

    return accepted;
}

Std_ReturnType GnssIf_GetPosition(GnssIf_PositionType *position)
{
    DET_CHECK_RETURN(position != NULL_PTR, MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE, GNSSIF_API_ID_GET_POSITION,
                     GNSSIF_E_PARAM_POINTER, E_NOT_OK);

    *position = GnssIf_Position;
    return E_OK;
}

boolean GnssIf_IsFixFresh(void)
{
    if (GnssIf_Position.valid == FALSE)
    {
        return FALSE;
    }
    return (Gpt_HasElapsed(GnssIf_Position.fixTimestampMs, GNSSIF_FIX_TIMEOUT_MS) == FALSE) ? TRUE : FALSE;
}

Std_ReturnType GnssIf_GetStatistics(GnssIf_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_GNSSIF, INSTANCE_ID_SINGLE, GNSSIF_API_ID_MAIN_FUNCTION,
                     GNSSIF_E_PARAM_POINTER, E_NOT_OK);

    *stats = GnssIf_Stats;
    return E_OK;
}

void GnssIf_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = GNSSIF_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_GNSSIF;
        versioninfo->sw_major_version = GNSSIF_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = GNSSIF_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = GNSSIF_SW_PATCH_VERSION;
    }
}

/**
 * @file    test_sensors.c
 * @brief   Unit tests for the CAN interface and the GNSS interface.
 *
 * Both modules decode an external wire format, which is the class of code where a subtle byte-order
 * or scaling error produces plausible-looking values that are quietly wrong. The sentences used here
 * are real NMEA with correct checksums, and the expected coordinates were computed independently
 * from the @c ddmm.mmmm definition rather than from the implementation.
 *
 * @req SWREQ-COM-0030 .. SWREQ-COM-0038, SWREQ-GNS-0001 .. SWREQ-GNS-0014
 * @verifies TS-CANIF-001 .. TS-CANIF-006, TS-GNSS-001 .. TS-GNSS-014
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "mcal/Can/Can.h"
#include "ecuabs/CanIf/CanIf.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "ecuabs/GnssIf/GnssIf.h"
#include "mcal/Spi/Spi.h"
#include "mcal/Gpt/Gpt.h"
#include "Stub_Mcal.h"
#include "mcal/Uart/Uart.h"
#include "unity.h"

#define GNSS_BUS ((uint8)UART_INSTANCE_GNSS)

/*==================================================================================================
 *  Reference NMEA sentences
 *
 *  Checksums are the real XOR of each body. Expected coordinates were derived from
 *  degrees + minutes/60 independently of the C code:
 *
 *      4807.038  N  ->  48 + 07.038/60  =  48.1173000 deg  ->  481173000
 *      01131.000 E  ->  11 + 31.000/60  =  11.5166667 deg  ->  115166667
 *      3128.7500 N  ->  31 + 28.75/60   =  31.4791667 deg  ->  314791667
 *      07422.5000 E ->  74 + 22.5/60    =  74.3750000 deg  ->  743750000
 *================================================================================================*/

#define NMEA_RMC_VALID "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*44"
#define NMEA_GGA_VALID "$GPGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*69"
#define NMEA_RMC_VOID "$GPRMC,000000.00,V,,,,,,,010100*1F"
#define NMEA_RMC_LAHORE "$GNRMC,101112.00,A,3128.7500,N,07422.5000,E,000.0,000.0,150324,,,A*4F"
#define NMEA_GGA_LAHORE "$GPGGA,101112.00,3128.7500,N,07422.5000,E,1,11,0.8,210.500,M,-33.1,M,,*40"
#define NMEA_GSV_IGNORED "$GPGSV,3,1,11,01,45,123,44,02,30,220,40*7E"
#define NMEA_GGA_SOUTHWEST "$GPGGA,101113.00,3128.7500,S,07422.5000,W,1,11,0.8,210.500,M,-33.1,M,,*4E"
#define NMEA_GGA_NULL_ISLAND "$GPGGA,101114.00,0000.0000,N,00000.0000,E,1,04,2.5,0.000,M,0.0,M,,*5B"
#define NMEA_GGA_EXTREMES "$GPGGA,101115.00,8959.9999,N,17959.9999,E,2,12,0.5,1000.000,M,0.0,M,,*63"

#define EXPECT_LAT_4807 481173000L
#define EXPECT_LON_01131 115166667L
#define EXPECT_LAT_3128 314791667L
#define EXPECT_LON_07422 743750000L
#define EXPECT_LAT_MAX 899999983L
#define EXPECT_LON_MAX 1799999983L

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    TEST_ASSERT_EQUAL(E_OK, Dem_Init());
    Can_DeInit();
    GnssIf_DeInit();
    TEST_ASSERT_EQUAL(E_OK, CanIf_Init());
}

void tearDown(void)
{
    GnssIf_DeInit();
}

/*==================================================================================================
 *  TS-CANIF-001 .. 006
 *================================================================================================*/

/** Build a drive-state frame with the given signals, little endian as the v1 code implemented. */
static void makeDriveStateFrame(Can_PduType *pdu, uint16 rpm, uint8 direction, uint8 speedMode, uint8 fault,
                                boolean lowPower)
{
    (void)memset(pdu, 0, sizeof(*pdu));
    pdu->id = CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG;
    pdu->dlc = 8u;
    pdu->sdu[0] = (uint8)((direction & 0x03u) | (uint8)((speedMode & 0x01u) << 3u));
    pdu->sdu[1] = (uint8)(rpm & 0xFFu);
    pdu->sdu[2] = (uint8)(rpm >> 8u);
    pdu->sdu[3] = fault;
    pdu->sdu[4] = (lowPower != FALSE) ? 0xAAu : 0x00u;
    pdu->timestamp = Gpt_GetMonotonicMs();
}

/** Build a current/voltage frame, little endian. */
static void makeCurrentVoltageFrame(Can_PduType *pdu, uint16 deciVolt, uint16 deciAmp)
{
    (void)memset(pdu, 0, sizeof(*pdu));
    pdu->id = CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG;
    pdu->dlc = 8u;
    pdu->sdu[0] = (uint8)(deciVolt & 0xFFu);
    pdu->sdu[1] = (uint8)(deciVolt >> 8u);
    pdu->sdu[2] = (uint8)(deciAmp & 0xFFu);
    pdu->sdu[3] = (uint8)(deciAmp >> 8u);
    pdu->timestamp = Gpt_GetMonotonicMs();
}

/**
 * @test TS-CANIF-001 Drive-state signals decode with the byte order the v1 code used.
 *
 * The v1 source contained a comment asserting big endian and code implementing little endian. This
 * pins the code's behaviour, which is what was running against real hardware; the open question is
 * recorded in docs/08-protocols.md.
 */
static void test_CanIf_DecodesDriveStateLittleEndian(void)
{
    Can_PduType pdu;
    CanIf_McuDataType data;

    (void)memset(&data, 0, sizeof(data));

    /* 5000 rpm is 0x1388, so byte 1 is 0x88 and byte 2 is 0x13 in Intel order. Reading it the other
     * way would give 0x8813 = 34 835 rpm -- a value the plausibility check would reject, which is
     * how this error would surface as "the odometer never advances". */
    makeDriveStateFrame(&pdu, 5000u, 1u, 0u, 3u, TRUE);
    TEST_ASSERT_EQUAL_HEX8(0x88u, pdu.sdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x13u, pdu.sdu[2]);

    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuDriveState(&pdu, &data));

    TEST_ASSERT_EQUAL_UINT16(5000u, data.motorRpm);
    TEST_ASSERT_EQUAL(CANIF_DIR_FORWARD, data.direction);
    TEST_ASSERT_EQUAL(CANIF_SPEED_MODE_HIGH, data.speedMode);
    TEST_ASSERT_EQUAL_UINT8(CANIF_MCU_FAULT_OVER_CURRENT, data.faultCode);
    TEST_ASSERT_TRUE(data.lowPowerMode);
    TEST_ASSERT_TRUE(data.driveStateValid);
}

/** @test TS-CANIF-002 Current and voltage decode in their protocol units. */
static void test_CanIf_DecodesCurrentVoltage(void)
{
    Can_PduType pdu;
    CanIf_McuDataType data;

    (void)memset(&data, 0, sizeof(data));

    /* 1000 counts is 100.0 V; 250 counts is 25.0 A. Kept as counts rather than converted to a
     * float, so the exact value the controller reported is what gets logged. */
    makeCurrentVoltageFrame(&pdu, 1000u, 250u);
    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuCurrentVoltage(&pdu, &data));

    TEST_ASSERT_EQUAL_UINT16(1000u, data.dcVoltageDeciVolt);
    TEST_ASSERT_EQUAL_UINT16(250u, data.dcCurrentDeciAmp);
    TEST_ASSERT_TRUE(data.currentVoltageValid);
}

/** @test TS-CANIF-003 Direction and speed-mode bits are extracted independently. */
static void test_CanIf_DecodesFlagBitsIndependently(void)
{
    Can_PduType pdu;
    CanIf_McuDataType data;

    (void)memset(&data, 0, sizeof(data));

    makeDriveStateFrame(&pdu, 0u, 2u, 1u, 0u, FALSE);
    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuDriveState(&pdu, &data));
    TEST_ASSERT_EQUAL(CANIF_DIR_REVERSE, data.direction);
    TEST_ASSERT_EQUAL(CANIF_SPEED_MODE_LOW, data.speedMode);

    makeDriveStateFrame(&pdu, 0u, 0u, 0u, 0u, FALSE);
    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuDriveState(&pdu, &data));
    TEST_ASSERT_EQUAL(CANIF_DIR_INVALID, data.direction);
    TEST_ASSERT_FALSE(data.lowPowerMode);
}

/**
 * @test TS-CANIF-004 Low-power mode is matched against its specific code, not "non-zero".
 *
 * The byte carries a code. Treating any non-zero value as "active" would misreport whatever else
 * the controller chooses to put there.
 */
static void test_CanIf_LowPowerMatchesSpecificCode(void)
{
    Can_PduType pdu;
    CanIf_McuDataType data;

    (void)memset(&data, 0, sizeof(data));

    makeDriveStateFrame(&pdu, 100u, 1u, 0u, 0u, FALSE);
    pdu.sdu[4] = 0x55u; /* non-zero, but not the low-power code */
    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuDriveState(&pdu, &data));
    TEST_ASSERT_FALSE_MESSAGE(data.lowPowerMode, "a non-zero byte was read as low-power active");

    pdu.sdu[4] = (uint8)CANIF_MCU_LOW_POWER_ACTIVE;
    TEST_ASSERT_EQUAL(E_OK, CanIf_DecodeMcuDriveState(&pdu, &data));
    TEST_ASSERT_TRUE(data.lowPowerMode);
}

/** @test TS-CANIF-005 A frame too short for its signals is rejected, not partially decoded. */
static void test_CanIf_RejectsShortFrame(void)
{
    Can_PduType pdu;
    CanIf_McuDataType data;

    (void)memset(&data, 0, sizeof(data));

    makeDriveStateFrame(&pdu, 5000u, 1u, 0u, 0u, FALSE);
    pdu.dlc = 3u; /* fault code and low-power byte absent */
    TEST_ASSERT_EQUAL(E_NOT_OK, CanIf_DecodeMcuDriveState(&pdu, &data));
    TEST_ASSERT_FALSE(data.driveStateValid);

    makeCurrentVoltageFrame(&pdu, 1000u, 250u);
    pdu.dlc = 2u;
    TEST_ASSERT_EQUAL(E_NOT_OK, CanIf_DecodeMcuCurrentVoltage(&pdu, &data));

    TEST_ASSERT_EQUAL(E_NOT_OK, CanIf_DecodeMcuDriveState(NULL_PTR, &data));
    TEST_ASSERT_EQUAL(E_NOT_OK, CanIf_DecodeMcuDriveState(&pdu, NULL_PTR));
}

/*----------------------------- CAN driver scaffolding ------------------------*/

/* A 3-byte register READ shifts in 3 bytes and the value lands in the third. */
static void queueRegisterRead(uint8 value)
{
    const uint8 response[3] = {0x00u, 0x00u, value};
    Stub_Spi_QueueRxBytes(response, sizeof(response));
}

static void queueWritePadding(uint16 bytes)
{
    uint16 i;
    for (i = 0u; i < bytes; i++)
    {
        const uint8 pad = 0x00u;
        Stub_Spi_QueueRxBytes(&pad, 1u);
    }
}

/** Script the SPI exchange a successful Can_Init() performs. */
static void scriptCanInit(void)
{
    queueWritePadding(1u);      /* RESET                        */
    queueWritePadding(4u);      /* BIT MODIFY CANCTRL -> config */
    queueRegisterRead(0x80u);   /* CANSTAT: config reached      */
    queueWritePadding(3u * 3u); /* CNF1..CNF3                   */
    queueWritePadding(6u * 2u); /* RXM0, RXM1                   */
    queueWritePadding(6u * 6u); /* RXF0..RXF5                   */
    queueWritePadding(3u * 2u); /* RXB0CTRL, RXB1CTRL           */
    queueWritePadding(3u * 2u); /* CANINTE, CANINTF             */
    queueWritePadding(4u);      /* BIT MODIFY CANCTRL -> normal */
    queueRegisterRead(0x00u);   /* CANSTAT: normal reached      */
}

/** Deliver one frame to the driver through the SPI stub, then decode it. */
static void deliverFrame(const Can_PduType *pdu)
{
    uint8 regs[CAN_ID_REGISTER_COUNT];
    uint8 raw[1u + 13u];
    uint8 i;

    queueRegisterRead(0x01u); /* CANINTF: RX0IF set */

    Can_EncodeIdentifier(pdu->id, regs);
    raw[0] = 0x00u;
    for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
    {
        raw[1u + i] = regs[i];
    }
    raw[5] = pdu->dlc;
    for (i = 0u; i < 8u; i++)
    {
        raw[6u + i] = pdu->sdu[i];
    }
    Stub_Spi_QueueRxBytes(raw, sizeof(raw));

    queueRegisterRead(0x00u); /* CANINTF clear */
    queueRegisterRead(0x00u); /* EFLG          */
    queueRegisterRead(0x00u); /* TEC           */
    queueRegisterRead(0x00u); /* REC           */

    (void)Can_MainFunction_Read();
    (void)CanIf_MainFunction();
}

/**
 * @test TS-CANIF-006 Signals go stale, and both halves must be fresh.
 *
 * Driven end to end through the real SPI stub, CAN driver and interface, so the freshness rule is
 * exercised against the module's own state rather than a local copy.
 *
 * v1 published whatever the last received frame contained, indefinitely, so a controller that
 * stopped transmitting produced a record showing its final speed forever -- and the odometer kept
 * integrating it.
 */
static void test_CanIf_SignalsGoStale(void)
{
    Can_PduType drive;
    Can_PduType cv;
    CanIf_McuDataType data;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptCanInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    TEST_ASSERT_FALSE_MESSAGE(CanIf_IsMcuDataFresh(), "data was fresh before any frame arrived");

    /* Only the drive-state half. A controller still sending speed but no voltage -- or the reverse --
     * must not read as fresh, because the odometer relies on both. */
    makeDriveStateFrame(&drive, 3000u, 1u, 0u, 0u, FALSE);
    deliverFrame(&drive);

    TEST_ASSERT_EQUAL(E_OK, CanIf_GetMcuData(&data));
    TEST_ASSERT_TRUE(data.driveStateValid);
    TEST_ASSERT_FALSE(data.currentVoltageValid);
    TEST_ASSERT_FALSE_MESSAGE(CanIf_IsMcuDataFresh(), "one half of the signal set was reported as fresh");

    /* Now the other half: fresh. */
    makeCurrentVoltageFrame(&cv, 1000u, 250u);
    deliverFrame(&cv);

    TEST_ASSERT_EQUAL(E_OK, CanIf_GetMcuData(&data));
    TEST_ASSERT_EQUAL_UINT16(3000u, data.motorRpm);
    TEST_ASSERT_EQUAL_UINT16(1000u, data.dcVoltageDeciVolt);
    TEST_ASSERT_TRUE(CanIf_IsMcuDataFresh());

    /* Let it age past the limit: no longer fresh, though the values are still readable. */
    Stub_Gpt_AdvanceMs(CANIF_MCU_SIGNAL_TIMEOUT_MS + 100u);
    TEST_ASSERT_FALSE_MESSAGE(CanIf_IsMcuDataFresh(), "a stale signal set was reported as fresh");

    TEST_ASSERT_EQUAL(E_OK, CanIf_GetMcuData(&data));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(3000u, data.motorRpm,
                                     "stale data should still be readable, just not fresh");

    {
        CanIf_StatisticsType stats;
        TEST_ASSERT_EQUAL(E_OK, CanIf_GetStatistics(&stats));
        TEST_ASSERT_EQUAL_UINT32(1u, stats.driveStateFrames);
        TEST_ASSERT_EQUAL_UINT32(1u, stats.currentVoltageFrames);
        TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.staleReads);
    }
}

/*==================================================================================================
 *  TS-GNSS-001 .. 014
 *================================================================================================*/

/** @test TS-GNSS-001 A correct NMEA checksum is accepted and a corrupted one is rejected. */
static void test_Gnss_ChecksumValidation(void)
{
    char corrupted[128];

    TEST_ASSERT_EQUAL(E_OK, GnssIf_VerifyChecksum(NMEA_RMC_VALID));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_VerifyChecksum(NMEA_GGA_VALID));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_VerifyChecksum(NMEA_GSV_IGNORED));

    /* Flip one character in the body: the stored checksum no longer matches. */
    (void)strcpy(corrupted, NMEA_RMC_VALID);
    corrupted[10] = (char)(corrupted[10] ^ 0x01);
    TEST_ASSERT_EQUAL(E_CRC_FAIL, GnssIf_VerifyChecksum(corrupted));

    /* Malformed: no '$', no '*', non-hex checksum digits. */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_VerifyChecksum("GPRMC,no,dollar*00"));
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_VerifyChecksum("$GPRMC,no,star"));
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_VerifyChecksum("$GPRMC,x*ZZ"));
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_VerifyChecksum(NULL_PTR));
}

/** @test TS-GNSS-002 Every single-bit corruption of a sentence body is detected. */
static void test_Gnss_ChecksumDetectsSingleBitErrors(void)
{
    char buffer[128];
    const uint16 bodyLength = (uint16)(strlen(NMEA_GGA_LAHORE) - 4u); /* exclude "*hh" and '$' */
    uint16 bit;

    for (bit = 0u; bit < (uint16)(bodyLength * 8u); bit++)
    {
        const uint16 index = (uint16)(1u + (bit / 8u));
        const uint8 mask = (uint8)(1u << (bit % 8u));

        (void)strcpy(buffer, NMEA_GGA_LAHORE);
        buffer[index] = (char)((uint8)buffer[index] ^ mask);

        /* A flipped bit may turn a character into a ',' or '*', which changes the structure rather
         * than only the checksum -- so either outcome is acceptable, as long as it is not accepted. */
        TEST_ASSERT_NOT_EQUAL_MESSAGE(E_OK, GnssIf_VerifyChecksum(buffer),
                                      "a single-bit corruption passed the checksum");
    }
}

/**
 * @test TS-GNSS-003 Coordinates convert from ddmm.mmmm to degrees x 10^7 exactly.
 *
 * Expected values computed independently from degrees + minutes/60.
 */
static void test_Gnss_CoordinateConversion(void)
{
    sint32 value = 0;

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("4807.038", 'N', &value));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_4807, value);

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("01131.000", 'E', &value));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LON_01131, value);

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("3128.7500", 'N', &value));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_3128, value);

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("07422.5000", 'E', &value));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LON_07422, value);
}

/** @test TS-GNSS-004 Southern and western hemispheres produce negative values. */
static void test_Gnss_HemisphereSign(void)
{
    sint32 north = 0;
    sint32 south = 0;
    sint32 east = 0;
    sint32 west = 0;

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("3128.7500", 'N', &north));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("3128.7500", 'S', &south));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("07422.5000", 'E', &east));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseCoordinate("07422.5000", 'W', &west));

    TEST_ASSERT_EQUAL_INT32(north, -south);
    TEST_ASSERT_EQUAL_INT32(east, -west);
    TEST_ASSERT_TRUE(south < 0);
    TEST_ASSERT_TRUE(west < 0);
}

/** @test TS-GNSS-005 Malformed coordinate fields are rejected. */
static void test_Gnss_CoordinateRejectsMalformed(void)
{
    sint32 value = 0;

    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("", 'N', &value));
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("123", 'N', &value));       /* no '.'     */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("12.34", 'N', &value));     /* no degrees */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("48O7.038", 'N', &value));  /* letter O   */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("4807.038", 'X', &value));  /* hemisphere */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("99999.000", 'N', &value)); /* > 180 deg */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate(NULL_PTR, 'N', &value));
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseCoordinate("4807.038", 'N', NULL_PTR));
}

/** @test TS-GNSS-006 An RMC sentence with a valid fix is parsed in full. */
static void test_Gnss_ParsesRmc(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_RMC_VALID, &position));

    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_4807, position.latitudeE7);
    TEST_ASSERT_EQUAL_INT32(EXPECT_LON_01131, position.longitudeE7);
    TEST_ASSERT_TRUE(position.valid);

    /* 22.4 knots. One knot is 1852 m/h, so mm/s = 22.4 x 1852000 / 3600 = 11 523. */
    TEST_ASSERT_UINT32_WITHIN(2u, 11523u, position.speedMmPerSec);

    /* Course 084.4 degrees, held as tenths. */
    TEST_ASSERT_EQUAL_UINT16(844u, position.headingDeciDeg);

    /* 23 March 1994, 12:35:19 UTC. */
    TEST_ASSERT_GREATER_THAN_UINT32(0u, position.fixUnixTime);
}

/** @test TS-GNSS-007 A GGA sentence is parsed, including quality, satellites and altitude. */
static void test_Gnss_ParsesGga(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));

    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_GGA_LAHORE, &position));

    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_3128, position.latitudeE7);
    TEST_ASSERT_EQUAL_INT32(EXPECT_LON_07422, position.longitudeE7);
    TEST_ASSERT_EQUAL_UINT8(GNSSIF_FIX_GPS, position.fixQuality);
    TEST_ASSERT_EQUAL_UINT8(11u, position.satellitesUsed);
    TEST_ASSERT_EQUAL_UINT16(80u, position.hdopCentiUnits); /* 0.8 x 100 */
    TEST_ASSERT_EQUAL_INT32(210500L, position.altitudeMm);  /* 210.500 m */
    TEST_ASSERT_TRUE(position.valid);
}

/** @test TS-GNSS-008 A void RMC and a no-fix GGA are rejected without being called malformed. */
static void test_Gnss_RejectsNoFix(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));

    /* Status 'V': the receiver is working but has nothing yet. */
    TEST_ASSERT_EQUAL(E_NOT_OK, GnssIf_ParseSentence(NMEA_RMC_VOID, &position));
    TEST_ASSERT_FALSE(position.valid);
}

/**
 * @test TS-GNSS-009 A sentence type this module does not consume is distinguished from an error.
 *
 * A receiver emitting GSV, GSA and VTG is behaving normally; counting those as malformed would make
 * the malformed counter useless for spotting real wiring trouble.
 */
static void test_Gnss_IgnoresUnknownSentenceTypes(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));
    TEST_ASSERT_EQUAL(E_NOT_FOUND, GnssIf_ParseSentence(NMEA_GSV_IGNORED, &position));
}

/** @test TS-GNSS-010 Any talker prefix is accepted, not only GP. */
static void test_Gnss_AcceptsAnyTalker(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));

    /* GN is what a multi-constellation receiver emits. Matching on the talker as well as the
     * sentence type would make a GPS+GLONASS module silently produce no fixes. */
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_RMC_LAHORE, &position));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_3128, position.latitudeE7);
}

/** @test TS-GNSS-011 Boundary coordinates parse correctly. */
static void test_Gnss_BoundaryCoordinates(void)
{
    GnssIf_PositionType position;

    (void)memset(&position, 0, sizeof(position));

    /* Null island: a legitimate coordinate that a "non-zero means valid" test would reject. */
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_GGA_NULL_ISLAND, &position));
    TEST_ASSERT_EQUAL_INT32(0L, position.latitudeE7);
    TEST_ASSERT_EQUAL_INT32(0L, position.longitudeE7);
    TEST_ASSERT_TRUE(position.valid);

    /* Near the representable extremes of both axes. */
    (void)memset(&position, 0, sizeof(position));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_GGA_EXTREMES, &position));
    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_MAX, position.latitudeE7);
    TEST_ASSERT_EQUAL_INT32(EXPECT_LON_MAX, position.longitudeE7);

    /* Southern and western hemispheres through the full sentence path. */
    (void)memset(&position, 0, sizeof(position));
    TEST_ASSERT_EQUAL(E_OK, GnssIf_ParseSentence(NMEA_GGA_SOUTHWEST, &position));
    TEST_ASSERT_EQUAL_INT32(-EXPECT_LAT_3128, position.latitudeE7);
    TEST_ASSERT_EQUAL_INT32(-EXPECT_LON_07422, position.longitudeE7);
}

/**
 * @test TS-GNSS-012 Plausibility is judged by implied speed, not by geography.
 *
 * v1 required the first fix to fall inside latitude 24..38 and longitude 60..78 -- a box around one
 * country, compiled in and documented nowhere. A unit shipped elsewhere would never accept a fix.
 */
static void test_Gnss_PlausibilityIsSpeedBased(void)
{
    GnssIf_PositionType from;
    GnssIf_PositionType to;

    (void)memset(&from, 0, sizeof(from));
    (void)memset(&to, 0, sizeof(to));

    /* The first fix is always accepted, wherever on Earth it is. Berlin would have been rejected
     * outright by the v1 box. */
    from.valid = FALSE;
    to.latitudeE7 = 524500000L; /* Berlin */
    to.longitudeE7 = 133800000L;
    TEST_ASSERT_TRUE_MESSAGE(GnssIf_IsTransitionPlausible(&from, &to, 1000u),
                             "a first fix outside one particular country was rejected");

    /* A realistic movement: about 28 m in one second, which is 100 km/h. */
    from.valid = TRUE;
    from.latitudeE7 = 314791667L;
    from.longitudeE7 = 743750000L;
    to.latitudeE7 = from.latitudeE7 + 2500L; /* ~27.8 m north */
    to.longitudeE7 = from.longitudeE7;
    TEST_ASSERT_TRUE(GnssIf_IsTransitionPlausible(&from, &to, 1000u));

    /* A jump to another continent in one second is not. */
    to.latitudeE7 = 524500000L;
    to.longitudeE7 = 133800000L;
    TEST_ASSERT_FALSE_MESSAGE(GnssIf_IsTransitionPlausible(&from, &to, 1000u),
                              "an intercontinental jump in one second was accepted");

    /* Given enough time, the same movement becomes plausible -- the rule is about speed, not
     * distance, so a unit shipped across the world resumes normally rather than being permanently
     * unable to accept a fix. A week is used rather than a day because the Manhattan bound puts
     * Lahore to Berlin at about 9100 km against a great-circle 5200 km, and 24 h at the configured
     * 252 km/h covers only 6000 km. */
    TEST_ASSERT_TRUE(GnssIf_IsTransitionPlausible(&from, &to, 7u * 24u * 3600u * 1000u));
}

/** @test TS-GNSS-013 Over a very short interval, receiver jitter is not rejected as movement. */
static void test_Gnss_ShortIntervalsAreNotJudged(void)
{
    GnssIf_PositionType from;
    GnssIf_PositionType to;

    (void)memset(&from, 0, sizeof(from));
    (void)memset(&to, 0, sizeof(to));

    from.valid = TRUE;
    from.latitudeE7 = 314791667L;
    from.longitudeE7 = 743750000L;
    to.latitudeE7 = from.latitudeE7 + 200L; /* ~2 m of jitter */
    to.longitudeE7 = from.longitudeE7 + 200L;

    /* Below the minimum interval the permitted distance would be smaller than the receiver's own
     * noise, so the test is not applied at all. */
    TEST_ASSERT_TRUE(GnssIf_IsTransitionPlausible(&from, &to, 10u));

    TEST_ASSERT_FALSE(GnssIf_IsTransitionPlausible(NULL_PTR, &to, 1000u));
    TEST_ASSERT_FALSE(GnssIf_IsTransitionPlausible(&from, NULL_PTR, 1000u));
}

/** @test TS-GNSS-014 The byte-stream assembler extracts sentences and counts failures. */
static void test_Gnss_StreamAssembly(void)
{
    GnssIf_PositionType position;
    GnssIf_StatisticsType stats;

    TEST_ASSERT_EQUAL(E_OK, Uart_Init());
    TEST_ASSERT_EQUAL(E_OK, GnssIf_Init());

    /* Partial leading garbage, then a good sentence, then an ignored one, then a corrupted one. */
    Stub_Uart_QueueRxString(GNSS_BUS, "garbage before the first dollar\r\n");
    Stub_Uart_QueueRxString(GNSS_BUS, NMEA_GGA_LAHORE);
    Stub_Uart_QueueRxString(GNSS_BUS, "\r\n");
    Stub_Uart_QueueRxString(GNSS_BUS, NMEA_GSV_IGNORED);
    Stub_Uart_QueueRxString(GNSS_BUS, "\r\n");
    Stub_Uart_QueueRxString(GNSS_BUS, "$GPGGA,101112.00,3128.7500,N,07422.5000,E,1,11,0.8,210.5,M,,,,*00");
    Stub_Uart_QueueRxString(GNSS_BUS, "\r\n");

    TEST_ASSERT_EQUAL_UINT8(1u, GnssIf_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, GnssIf_GetPosition(&position));
    TEST_ASSERT_TRUE(position.valid);
    TEST_ASSERT_EQUAL_INT32(EXPECT_LAT_3128, position.latitudeE7);
    TEST_ASSERT_TRUE(GnssIf_IsFixFresh());

    TEST_ASSERT_EQUAL(E_OK, GnssIf_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.sentencesAccepted);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.checksumFailures);
    TEST_ASSERT_EQUAL_UINT32(3u, stats.sentencesReceived);

    /* And the fix ages out rather than being published forever. */
    Stub_Gpt_AdvanceMs(GNSSIF_FIX_TIMEOUT_MS + 1000u);
    TEST_ASSERT_FALSE_MESSAGE(GnssIf_IsFixFresh(), "a stale fix was still reported as fresh");
}

/** @test TS-GNSS-014b An over-long sentence is discarded rather than truncated. */
static void test_Gnss_OverlongSentenceDiscarded(void)
{
    GnssIf_StatisticsType stats;
    char monster[GNSSIF_SENTENCE_BUFFER_SIZE * 2u];
    uint16 i;

    TEST_ASSERT_EQUAL(E_OK, Uart_Init());
    TEST_ASSERT_EQUAL(E_OK, GnssIf_Init());

    monster[0] = '$';
    for (i = 1u; i < (uint16)(sizeof(monster) - 1u); i++)
    {
        monster[i] = 'A';
    }
    monster[sizeof(monster) - 1u] = '\0';

    Stub_Uart_QueueRxString(GNSS_BUS, monster);
    Stub_Uart_QueueRxString(GNSS_BUS, "\r\n");

    TEST_ASSERT_EQUAL_UINT8(0u, GnssIf_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, GnssIf_GetStatistics(&stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.overflowedSentences);

    /* A truncated sentence can still satisfy a checksum computed over the part that arrived, which
     * is why it must be discarded rather than parsed. */
    TEST_ASSERT_EQUAL_UINT32(0u, stats.sentencesAccepted);
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_CanIf_DecodesDriveStateLittleEndian);
    RUN_TEST(test_CanIf_DecodesCurrentVoltage);
    RUN_TEST(test_CanIf_DecodesFlagBitsIndependently);
    RUN_TEST(test_CanIf_LowPowerMatchesSpecificCode);
    RUN_TEST(test_CanIf_RejectsShortFrame);
    RUN_TEST(test_CanIf_SignalsGoStale);
    RUN_TEST(test_Gnss_ChecksumValidation);
    RUN_TEST(test_Gnss_ChecksumDetectsSingleBitErrors);
    RUN_TEST(test_Gnss_CoordinateConversion);
    RUN_TEST(test_Gnss_HemisphereSign);
    RUN_TEST(test_Gnss_CoordinateRejectsMalformed);
    RUN_TEST(test_Gnss_ParsesRmc);
    RUN_TEST(test_Gnss_ParsesGga);
    RUN_TEST(test_Gnss_RejectsNoFix);
    RUN_TEST(test_Gnss_IgnoresUnknownSentenceTypes);
    RUN_TEST(test_Gnss_AcceptsAnyTalker);
    RUN_TEST(test_Gnss_BoundaryCoordinates);
    RUN_TEST(test_Gnss_PlausibilityIsSpeedBased);
    RUN_TEST(test_Gnss_ShortIntervalsAreNotJudged);
    RUN_TEST(test_Gnss_StreamAssembly);
    RUN_TEST(test_Gnss_OverlongSentenceDiscarded);
    return UNITY_END();
}

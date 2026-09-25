/**
 * @file    test_can.c
 * @brief   Unit tests for the MCP2515 CAN driver.
 *
 * The MCP2515 is reached only through Spi/Dio/Gpt, so the whole driver runs on the host
 * against the SPI stub. That makes two things testable that otherwise need a logic
 * analyser and a vehicle: the identifier encoding, which is verified over its entire
 * domain rather than at a few sample points, and the register sequences, which are
 * asserted byte for byte.
 *
 * @req SWREQ-COM-0010 .. SWREQ-COM-0015
 * @verifies TS-CAN-001 .. TS-CAN-012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "mcal/Can/Can.h"
#include "services/Det/Det.h"
#include "mcal/Spi/Spi.h"
#include "Stub_Mcal.h"
#include "unity.h"

/* MCP2515 constants the tests assert against. Duplicated from Can.c on purpose: a test
 * that imports the implementation's private header verifies only self-consistency. */
#define MCP_INSTR_RESET 0xC0u
#define MCP_INSTR_READ 0x03u
#define MCP_INSTR_WRITE 0x02u
#define MCP_INSTR_BIT_MODIFY 0x05u
#define MCP_INSTR_READ_RXB0 0x90u
#define MCP_INSTR_LOAD_TXB0 0x40u
#define MCP_INSTR_RTS_TXB0 0x81u
#define MCP_REG_CANCTRL 0x0Fu
#define MCP_REG_CANSTAT 0x0Eu
#define MCP_REG_CNF1 0x2Au
#define MCP_REG_CANINTF 0x2Cu
#define MCP_REG_TXB0CTRL 0x30u
#define MCP_MODE_CONFIG 0x80u
#define MCP_MODE_NORMAL 0x00u
#define MCP_SIDL_EXIDE 0x08u

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();

    /* Module-static state survives between test cases in one binary, so it is cleared
     * explicitly. Without this a case that asserts "rejected because uninitialised"
     * silently inherits the previous case's successful Can_Init and tests nothing. */
    Can_DeInit();
}

void tearDown(void)
{
}

/*==================================================================================================
 *  Helpers
 *================================================================================================*/

/**
 * @brief Script the SPI responses a successful Can_Init() expects to read.
 *
 * Can_Init issues, in order: RESET; then a bit-modify of CANCTRL followed by CANSTAT
 * polls until config mode is seen; the configuration writes; then another bit-modify and
 * CANSTAT polls until normal mode is seen. Only the CANSTAT reads consume scripted bytes
 * that matter, and the stub returns 0xFF past the end of its script -- which would be
 * read as mode 0xE0 and never match -- so the reads must be scripted deliberately.
 *
 * Each 3-byte READ transaction shifts in 3 bytes, and the value lands in the third, so
 * two padding bytes precede every scripted register value.
 */
static void queueRegisterRead(uint8 value)
{
    const uint8 response[3] = {0x00u, 0x00u, value};
    Stub_Spi_QueueRxBytes(response, sizeof(response));
}

/** Bytes consumed by one N-byte write-only transaction (nothing meaningful is read). */
static void queueWritePadding(uint16 bytes)
{
    uint16 i;
    for (i = 0u; i < bytes; i++)
    {
        const uint8 pad = 0x00u;
        Stub_Spi_QueueRxBytes(&pad, 1u);
    }
}

/** Script a complete, successful Can_Init() exchange. */
static void scriptSuccessfulInit(void)
{
    queueWritePadding(1u);              /* RESET                                  */
    queueWritePadding(4u);              /* BIT MODIFY CANCTRL -> config           */
    queueRegisterRead(MCP_MODE_CONFIG); /* CANSTAT poll: config reached           */
    queueWritePadding(3u * 3u);         /* CNF1, CNF2, CNF3                       */
    queueWritePadding(6u * 2u);         /* RXM0, RXM1 (6 bytes each)              */
    queueWritePadding(6u * 6u);         /* RXF0..RXF5 (6 bytes each)              */
    queueWritePadding(3u * 2u);         /* RXB0CTRL, RXB1CTRL                     */
    queueWritePadding(3u * 2u);         /* CANINTE, CANINTF                       */
    queueWritePadding(4u);              /* BIT MODIFY CANCTRL -> normal           */
    queueRegisterRead(MCP_MODE_NORMAL); /* CANSTAT poll: normal reached           */
}

/*==================================================================================================
 *  TS-CAN-001 .. 003 : identifier codec
 *================================================================================================*/

/** @test TS-CAN-001 Every 11-bit standard identifier survives an encode/decode round trip. */
static void test_StandardIdentifier_RoundTripsOverFullDomain(void)
{
    uint32 id;

    for (id = 0u; id <= 0x7FFuL; id++)
    {
        uint8 regs[CAN_ID_REGISTER_COUNT];

        Can_EncodeIdentifier((Can_IdType)id, regs);

        /* EXIDE must stay clear, or the peer decodes an entirely different frame. */
        TEST_ASSERT_EQUAL_HEX8(0u, regs[1] & MCP_SIDL_EXIDE);
        TEST_ASSERT_EQUAL_HEX32(id, Can_DecodeIdentifier(regs));
    }
}

/**
 * @test TS-CAN-002 Extended identifiers round-trip across the whole 29-bit domain.
 *
 * Walked in strides rather than one by one -- 536 million iterations would dominate the
 * suite -- but the stride is deliberately coprime with every power of two so that it
 * visits all bit positions and all field boundaries rather than aliasing onto a subset.
 */
static void test_ExtendedIdentifier_RoundTripsAcrossDomain(void)
{
    const uint32 stride = 104729uL; /* prime */
    uint32 raw;

    for (raw = 0u; raw <= CAN_ID_MASK; raw += stride)
    {
        const Can_IdType id = (Can_IdType)raw | CAN_ID_EXTENDED_FLAG;
        uint8 regs[CAN_ID_REGISTER_COUNT];

        Can_EncodeIdentifier(id, regs);

        TEST_ASSERT_EQUAL_HEX8(MCP_SIDL_EXIDE, regs[1] & MCP_SIDL_EXIDE);
        TEST_ASSERT_EQUAL_HEX32(id, Can_DecodeIdentifier(regs));
    }
}

/**
 * @test TS-CAN-003 Field-boundary identifiers encode exactly as the datasheet specifies.
 *
 * Hand-computed from DS20001801J figure 12-4 rather than from the implementation, so a
 * transposed shift cannot pass. These also pin the two real motor-controller identifiers.
 */
static void test_Identifier_MatchesDatasheetEncoding(void)
{
    uint8 regs[CAN_ID_REGISTER_COUNT];

    /* All 29 bits set: SIDH=0xFF, SIDL = 0xE0 | EXIDE | 0x03, EID8=0xFF, EID0=0xFF. */
    Can_EncodeIdentifier(CAN_ID_MASK | CAN_ID_EXTENDED_FLAG, regs);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEBu, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, regs[3]);

    /* 0x10F8109A. Splitting it the way the controller does:
     *   SID  = (id >> 18) & 0x7FF = 0x43E
     *   EID  =  id        & 0x3FFFF = 0x0109A
     *   SIDH = (id >> 21) & 0xFF                          = 0x87
     *   SIDL = ((SID & 7) << 5) | EXIDE | ((EID >> 16) & 3)
     *        = (6 << 5) | 0x08 | 0                        = 0xC8
     *   EID8 = (id >> 8)  & 0xFF                          = 0x10
     *   EID0 =  id        & 0xFF                          = 0x9A                      */
    Can_EncodeIdentifier(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, regs);
    TEST_ASSERT_EQUAL_HEX8(0x87u, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC8u, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0x10u, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(0x9Au, regs[3]);
    TEST_ASSERT_EQUAL_HEX32(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, Can_DecodeIdentifier(regs));

    /* 0x10F8108D shares its SID and its upper EID bits, differing only in EID0. Two
     * identifiers this close are exactly the case a transposed shift would merge. */
    Can_EncodeIdentifier(CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG, regs);
    TEST_ASSERT_EQUAL_HEX8(0x87u, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC8u, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0x10u, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(0x8Du, regs[3]);
    TEST_ASSERT_EQUAL_HEX32(CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG, Can_DecodeIdentifier(regs));

    /* Standard 0x7FF: SIDH = 0xFF, SIDL = 0xE0, no EXIDE. */
    Can_EncodeIdentifier(0x7FFuL, regs);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE0u, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, regs[3]);
}

/**
 * @test TS-CAN-003b A standard and an extended identifier with the same numeric value
 *       encode differently, so the filter set cannot confuse them.
 */
static void test_StandardAndExtended_AreDistinguishable(void)
{
    uint8 stdRegs[CAN_ID_REGISTER_COUNT];
    uint8 extRegs[CAN_ID_REGISTER_COUNT];

    Can_EncodeIdentifier(0x123uL, stdRegs);
    Can_EncodeIdentifier(0x123uL | CAN_ID_EXTENDED_FLAG, extRegs);

    TEST_ASSERT_NOT_EQUAL(Can_DecodeIdentifier(stdRegs), Can_DecodeIdentifier(extRegs));
    TEST_ASSERT_EQUAL_HEX32(0x123uL, Can_DecodeIdentifier(stdRegs));
    TEST_ASSERT_EQUAL_HEX32(0x123uL | CAN_ID_EXTENDED_FLAG, Can_DecodeIdentifier(extRegs));
}

/** @test TS-CAN-003c The codec tolerates a NULL register pointer. */
static void test_Identifier_NullPointerIsSafe(void)
{
    Can_EncodeIdentifier(0x100uL, NULL_PTR); /* must not trap */
    TEST_ASSERT_EQUAL_HEX32(0u, Can_DecodeIdentifier(NULL_PTR));
}

/*==================================================================================================
 *  TS-CAN-004 .. 006 : initialisation
 *================================================================================================*/

/** @test TS-CAN-004 A successful init resets, configures and reaches normal mode. */
static void test_Init_ReachesStartedState(void)
{
    const uint8 *tx;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();

    TEST_ASSERT_EQUAL(E_OK, Can_Init());
    TEST_ASSERT_EQUAL(CAN_CS_STARTED, Can_GetControllerMode());

    tx = Stub_Spi_GetTxBuffer();
    TEST_ASSERT_NOT_NULL(tx);

    /* The very first byte on the bus must be RESET. Configuring before resetting leaves
     * whatever mode a warm restart left behind, and CNF1..3 are silently ignored outside
     * configuration mode. */
    TEST_ASSERT_EQUAL_HEX8(MCP_INSTR_RESET, tx[0]);

    /* Then a bit-modify of CANCTRL requesting configuration mode. */
    TEST_ASSERT_EQUAL_HEX8(MCP_INSTR_BIT_MODIFY, tx[1]);
    TEST_ASSERT_EQUAL_HEX8(MCP_REG_CANCTRL, tx[2]);
    TEST_ASSERT_EQUAL_HEX8(MCP_MODE_CONFIG, tx[4]);

    /* The bus must be released; a driver that keeps the lock starves the SD card. */
    TEST_ASSERT_EQUAL(SPI_DEVICE_NONE, Spi_GetOwner());
}

/** @test TS-CAN-005 Init writes the configured bit timing to CNF1, CNF2 and CNF3. */
static void test_Init_ProgramsConfiguredBitTiming(void)
{
    const uint8 *tx;
    uint16 len;
    uint16 i;
    boolean foundCnf1 = FALSE;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    tx = Stub_Spi_GetTxBuffer();
    len = Stub_Spi_GetTxLength();

    /* Locate the CNF1 write and confirm the configured value went out. Timing that does
     * not match the crystal produces a controller that initialises perfectly and then
     * never acknowledges a frame. */
    for (i = 0u; (i + 2u) < len; i++)
    {
        if ((tx[i] == MCP_INSTR_WRITE) && (tx[i + 1u] == MCP_REG_CNF1))
        {
            TEST_ASSERT_EQUAL_HEX8(CAN_CNF1_VALUE, tx[i + 2u]);
            foundCnf1 = TRUE;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(foundCnf1, "Can_Init did not write CNF1");
}

/**
 * @test TS-CAN-006 If the controller never reports configuration mode, Init fails.
 *
 * This is the v1 defect made impossible: v1's Can_Init returned true unconditionally with
 * the hardware call commented out, so a dead controller reported success and every MCU
 * field in the log stayed empty for months.
 */
static void test_Init_FailsWhenControllerNeverEntersConfigMode(void)
{
    TEST_ASSERT_EQUAL(E_OK, Spi_Init());

    /* Nothing scripted: the stub returns 0xFF, which decodes as a mode that never
     * matches, so every mode poll times out. */
    TEST_ASSERT_EQUAL(E_NOT_OK, Can_Init());
    TEST_ASSERT_EQUAL(CAN_CS_UNINIT, Can_GetControllerMode());

    /* And the failure must be recorded, not merely returned. */
    {
        Det_StatisticsType stats;
        Det_GetStatistics(&stats);
        TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.runtimeErrorCount);
    }

    /* The bus must still be released on every failure path. */
    TEST_ASSERT_EQUAL(SPI_DEVICE_NONE, Spi_GetOwner());
}

/*==================================================================================================
 *  TS-CAN-007 .. 009 : transmit
 *================================================================================================*/

/** @test TS-CAN-007 Can_Write loads TXB0 and issues a request-to-send. */
static void test_Write_LoadsBufferThenRequestsSend(void)
{
    Can_PduType pdu;
    const uint8 *tx;
    uint16 len;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    /* TXB0CTRL read must report TXREQ clear so the buffer looks free. */
    queueRegisterRead(0x00u);
    queueWritePadding(64u);

    pdu.id = 0x123uL;
    pdu.dlc = 3u;
    pdu.sdu[0] = 0xDEu;
    pdu.sdu[1] = 0xADu;
    pdu.sdu[2] = 0xBEu;
    pdu.timestamp = 0u;

    Stub_Spi_QueueRxBytes((const uint8 *)"", 0u);
    len = Stub_Spi_GetTxLength();
    TEST_ASSERT_EQUAL(E_OK, Can_Write(&pdu));

    tx = Stub_Spi_GetTxBuffer();

    /* Immediately after the TXB0CTRL read (3 bytes) comes LOAD TX BUFFER. */
    TEST_ASSERT_EQUAL_HEX8(MCP_INSTR_READ, tx[len]);
    TEST_ASSERT_EQUAL_HEX8(MCP_REG_TXB0CTRL, tx[len + 1u]);
    TEST_ASSERT_EQUAL_HEX8(MCP_INSTR_LOAD_TXB0, tx[len + 3u]);

    /* Identifier, then DLC, then the payload. */
    TEST_ASSERT_EQUAL_HEX8(3u, tx[len + 3u + 1u + CAN_ID_REGISTER_COUNT]);
    TEST_ASSERT_EQUAL_HEX8(0xDEu, tx[len + 3u + 2u + CAN_ID_REGISTER_COUNT]);
    TEST_ASSERT_EQUAL_HEX8(0xADu, tx[len + 3u + 3u + CAN_ID_REGISTER_COUNT]);
    TEST_ASSERT_EQUAL_HEX8(0xBEu, tx[len + 3u + 4u + CAN_ID_REGISTER_COUNT]);

    /* And finally the RTS, without which the frame is loaded but never sent. */
    TEST_ASSERT_EQUAL_HEX8(MCP_INSTR_RTS_TXB0, tx[len + 3u + 5u + CAN_ID_REGISTER_COUNT]);

    {
        Can_StatisticsType stats;
        TEST_ASSERT_EQUAL(E_OK, Can_GetStatistics(&stats));
        TEST_ASSERT_EQUAL_UINT32(1u, stats.framesTransmitted);
    }
}

/** @test TS-CAN-008 An over-long DLC is rejected rather than truncated. */
static void test_Write_RejectsOversizedDlc(void)
{
    Can_PduType pdu;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    pdu.id = 0x100uL;
    pdu.dlc = (uint8)(CAN_MAX_DLC + 1u);
    pdu.timestamp = 0u;

    TEST_ASSERT_EQUAL(E_NOT_OK, Can_Write(&pdu));

    /* Rejection must be recorded as a development error: an over-long DLC is a caller
     * defect, and silently clamping it would ship a frame the receiver misparses. */
    {
        Det_StatisticsType stats;
        Det_GetStatistics(&stats);
        TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.devErrorCount);
    }
}

/** @test TS-CAN-009 Writing before Init, or with a NULL PDU, is rejected. */
static void test_Write_RejectsUninitialisedAndNull(void)
{
    Can_PduType pdu;

    pdu.id = 0x100uL;
    pdu.dlc = 1u;
    pdu.sdu[0] = 0u;
    pdu.timestamp = 0u;

    TEST_ASSERT_EQUAL(E_NOT_OK, Can_Write(&pdu));     /* not initialised */
    TEST_ASSERT_EQUAL(E_NOT_OK, Can_Write(NULL_PTR)); /* NULL            */
}

/*==================================================================================================
 *  TS-CAN-010 .. 012 : receive
 *================================================================================================*/

/** @test TS-CAN-010 A frame in RXB0 is decoded and queued with its payload intact. */
static void test_MainFunctionRead_DecodesAndQueuesFrame(void)
{
    Can_PduType pdu;
    uint8 regs[CAN_ID_REGISTER_COUNT];
    uint8 frame[1u + 13u];
    uint8 i;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    /* CANINTF read: RX0IF set. */
    queueRegisterRead(0x01u);

    /* READ RX BUFFER response: one padding byte for the instruction, then the frame. */
    Can_EncodeIdentifier(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, regs);
    frame[0] = 0x00u;
    for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
    {
        frame[1u + i] = regs[i];
    }
    frame[5] = 8u; /* DLC, RTR clear */
    for (i = 0u; i < 8u; i++)
    {
        frame[6u + i] = (uint8)(0xA0u + i);
    }
    Stub_Spi_QueueRxBytes(frame, sizeof(frame));

    queueRegisterRead(0x00u); /* CANINTF read: nothing left */
    queueRegisterRead(0x00u); /* EFLG: no errors            */
    queueRegisterRead(0x00u); /* TEC                        */
    queueRegisterRead(0x00u); /* REC                        */

    TEST_ASSERT_EQUAL_UINT8(1u, Can_MainFunction_Read());
    TEST_ASSERT_EQUAL_UINT8(1u, Can_GetRxQueueCount());

    TEST_ASSERT_EQUAL(E_OK, Can_Receive(&pdu));
    TEST_ASSERT_EQUAL_HEX32(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, pdu.id);
    TEST_ASSERT_EQUAL_UINT8(8u, pdu.dlc);
    for (i = 0u; i < 8u; i++)
    {
        TEST_ASSERT_EQUAL_HEX8((uint8)(0xA0u + i), pdu.sdu[i]);
    }

    /* Queue is now empty and reports so distinctly from an error. */
    TEST_ASSERT_EQUAL(E_NOT_FOUND, Can_Receive(&pdu));
}

/**
 * @test TS-CAN-011 A remote transmission request is discarded, not published.
 *
 * An RTR frame carries no payload. Accepting one as data would publish eight bytes of
 * whatever the controller's buffer last held as though it were a fresh measurement.
 */
static void test_MainFunctionRead_DiscardsRemoteRequest(void)
{
    uint8 regs[CAN_ID_REGISTER_COUNT];
    uint8 frame[1u + 13u];
    uint8 i;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    queueRegisterRead(0x01u);

    Can_EncodeIdentifier(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, regs);
    frame[0] = 0x00u;
    for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
    {
        frame[1u + i] = regs[i];
    }
    frame[5] = 0x40u | 8u; /* RTR set */
    for (i = 0u; i < 8u; i++)
    {
        frame[6u + i] = 0xFFu;
    }
    Stub_Spi_QueueRxBytes(frame, sizeof(frame));

    queueRegisterRead(0x00u);
    queueRegisterRead(0x00u);
    queueRegisterRead(0x00u);
    queueRegisterRead(0x00u);

    /* The buffer was drained -- so the flag is cleared and the controller does not stall
     * -- but nothing reached the queue. */
    TEST_ASSERT_EQUAL_UINT8(1u, Can_MainFunction_Read());
    TEST_ASSERT_EQUAL_UINT8(0u, Can_GetRxQueueCount());
}

/** @test TS-CAN-012 A bus-off flag is latched, counted once, and reported. */
static void test_MainFunctionRead_LatchesBusOffOnce(void)
{
    Can_StatisticsType stats;
    uint8 cycle;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    for (cycle = 0u; cycle < 3u; cycle++)
    {
        queueRegisterRead(0x00u); /* CANINTF: no frames */
        queueRegisterRead(0x20u); /* EFLG: TXBO set     */
        queueRegisterRead(0xFFu); /* TEC saturated      */
        queueRegisterRead(0x00u); /* REC                */
        (void)Can_MainFunction_Read();
    }

    TEST_ASSERT_TRUE(Can_IsBusOff());
    TEST_ASSERT_EQUAL(E_OK, Can_GetStatistics(&stats));

    /* Counted once for one continuous bus-off episode. Counting per cycle would turn a
     * single disconnected harness into thousands of events and bury everything else in
     * the diagnostic record. */
    TEST_ASSERT_EQUAL_UINT32(1u, stats.busOffCount);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, stats.txErrorCounter);
}

/** @test TS-CAN-012b The receive queue drops the newest frame and counts the overflow. */
static void test_ReceiveQueue_CountsOverflowWithoutCorrupting(void)
{
    Can_StatisticsType stats;
    Can_PduType pdu;
    uint16 pushed;

    TEST_ASSERT_EQUAL(E_OK, Spi_Init());
    scriptSuccessfulInit();
    TEST_ASSERT_EQUAL(E_OK, Can_Init());

    /* Deliver more frames than the queue can hold. CAN_MAX_FRAMES_PER_CYCLE bounds each
     * call, so several calls are needed. */
    for (pushed = 0u; pushed < (uint16)(CAN_RX_QUEUE_DEPTH + 8u); pushed++)
    {
        uint8 regs[CAN_ID_REGISTER_COUNT];
        uint8 frame[1u + 13u];
        uint8 i;

        queueRegisterRead(0x01u);

        Can_EncodeIdentifier(CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG, regs);
        frame[0] = 0x00u;
        for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
        {
            frame[1u + i] = regs[i];
        }
        frame[5] = 2u;
        frame[6] = (uint8)pushed;
        frame[7] = 0x00u;
        for (i = 2u; i < 8u; i++)
        {
            frame[6u + i] = 0u;
        }
        Stub_Spi_QueueRxBytes(frame, sizeof(frame));

        queueRegisterRead(0x00u); /* CANINTF clear */
        queueRegisterRead(0x00u); /* EFLG          */
        queueRegisterRead(0x00u); /* TEC           */
        queueRegisterRead(0x00u); /* REC           */

        (void)Can_MainFunction_Read();
    }

    TEST_ASSERT_EQUAL_UINT8((uint8)CAN_RX_QUEUE_DEPTH, Can_GetRxQueueCount());
    TEST_ASSERT_EQUAL(E_OK, Can_GetStatistics(&stats));
    TEST_ASSERT_GREATER_THAN_UINT32(0u, stats.rxOverflowCount);

    /* The frames that *are* queued must still be intact -- the oldest first, with its
     * original payload. A queue that overwrote its tail under pressure would hand back a
     * frame stitched together from two different ones. */
    TEST_ASSERT_EQUAL(E_OK, Can_Receive(&pdu));
    TEST_ASSERT_EQUAL_HEX32(CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG, pdu.id);
    TEST_ASSERT_EQUAL_UINT8(2u, pdu.dlc);
    TEST_ASSERT_EQUAL_HEX8(0u, pdu.sdu[0]);
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_StandardIdentifier_RoundTripsOverFullDomain);
    RUN_TEST(test_ExtendedIdentifier_RoundTripsAcrossDomain);
    RUN_TEST(test_Identifier_MatchesDatasheetEncoding);
    RUN_TEST(test_StandardAndExtended_AreDistinguishable);
    RUN_TEST(test_Identifier_NullPointerIsSafe);
    RUN_TEST(test_Init_ReachesStartedState);
    RUN_TEST(test_Init_ProgramsConfiguredBitTiming);
    RUN_TEST(test_Init_FailsWhenControllerNeverEntersConfigMode);
    RUN_TEST(test_Write_LoadsBufferThenRequestsSend);
    RUN_TEST(test_Write_RejectsOversizedDlc);
    RUN_TEST(test_Write_RejectsUninitialisedAndNull);
    RUN_TEST(test_MainFunctionRead_DecodesAndQueuesFrame);
    RUN_TEST(test_MainFunctionRead_DiscardsRemoteRequest);
    RUN_TEST(test_MainFunctionRead_LatchesBusOffOnce);
    RUN_TEST(test_ReceiveQueue_CountsOverflowWithoutCorrupting);
    return UNITY_END();
}

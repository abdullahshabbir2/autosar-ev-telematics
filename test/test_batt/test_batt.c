/**
 * @file    test_batt.c
 * @brief   Unit tests for the battery software component's aggregation and health logic.
 *
 * @par What these tests drive
 * Real code all the way down. Response frames are built here, injected through the UART stub, and
 * read back by the actual Rs485If transport, so BattSwc is exercised against the same decode path it
 * uses on the vehicle -- not against a hand-filled state structure. That matters because half the
 * behaviour under test is about *which* packs answered, and a mock of Rs485If would be a restatement
 * of the assumption rather than a test of it.
 *
 * Frames are CRC-stamped with ::Crc_CalculateCRC16, the production implementation. Using it to build
 * fixtures is sound precisely because test_crc has already verified it against the published
 * CCITT-FALSE check value 0x29B1 -- an independently established fact, not this suite's own output.
 *
 * @req SWREQ-BAT-0030 .. SWREQ-BAT-0048
 * @verifies TS-BATT-001 .. TS-BATT-014
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "app/BattSwc/BattSwc.h"
#include "app/BattSwc/BattSwc_Cfg.h"
#include "ecuabs/Rs485If/Rs485If.h"
#include "services/Crc/Crc.h"
#include "services/Dem/Dem.h"
#include "services/Det/Det.h"
#include "mcal/Uart/Uart.h"
#include "Rs485_TestVectors.h"
#include "Stub_Mcal.h"
#include "unity.h"

/** The RS485 bus, as the UART stub indexes its instances. */
#define TB_BUS ((uint8)UART_INSTANCE_RS485)

/*==================================================================================================
 *  Frame construction
 *
 *  Offsets are repeated here rather than shared with Rs485If.c, which keeps them module-private.
 *  That duplication is deliberate: if the layout changes, this suite fails, and a test that would
 *  silently follow an implementation change is not testing the layout at all.
 *================================================================================================*/

#define TB_OFF_START 0u
#define TB_OFF_DIR 1u
#define TB_OFF_LEN_HI 2u
#define TB_OFF_LEN_LO 3u
#define TB_OFF_TYPE 4u
#define TB_OFF_SERIAL 5u
#define TB_OFF_CMD 9u

/** Serial numbers given to the four slots. Arbitrary but distinct, so a misrouted frame shows up. */
static const uint32 TbSerials[RS485IF_PACK_COUNT] = {
    0x11110001uL,
    0x22220002uL,
    0x33330003uL,
    0x44440004uL,
};

static void TbWriteU16(uint8 *at, uint16 value)
{
    at[0] = (uint8)(value >> 8u);
    at[1] = (uint8)(value & 0xFFu);
}

static void TbWriteU32(uint8 *at, uint32 value)
{
    at[0] = (uint8)(value >> 24u);
    at[1] = (uint8)((value >> 16u) & 0xFFu);
    at[2] = (uint8)((value >> 8u) & 0xFFu);
    at[3] = (uint8)(value & 0xFFu);
}

/** Fill the common response header and stamp the CRC. */
static void TbFinishFrame(uint8 *frame, uint8 size, uint32 serial, uint8 command)
{
    frame[TB_OFF_START] = RS485IF_START_BYTE;
    frame[TB_OFF_DIR] = RS485IF_DIR_RESPONSE;
    TbWriteU16(&frame[TB_OFF_LEN_HI], (uint16)size);
    frame[TB_OFF_TYPE] = RS485IF_TYPE_ADDRESSED;
    TbWriteU32(&frame[TB_OFF_SERIAL], serial);
    frame[TB_OFF_CMD] = command;

    TbWriteU16(&frame[size - RS485IF_CRC_SIZE],
               Crc_CalculateCRC16(frame, (uint32)(size - RS485IF_CRC_SIZE), 0u, TRUE));
}

static void TbBuildSerialResponse(uint8 *frame, uint32 serial)
{
    (void)memset(frame, 0, RS485IF_SERIAL_RESPONSE_SIZE);
    TbFinishFrame(frame, RS485IF_SERIAL_RESPONSE_SIZE, serial, RS485IF_CMD_SERIAL_NUMBER);
}

/** Pack-level response. Only the fields the aggregate uses are set; the rest stay zero. */
static void TbBuildPackResponse(uint8 *frame, uint32 serial, uint16 voltage, sint32 current,
                                sint16 temperature, uint8 stateOfCharge, uint8 stateOfHealth)
{
    uint8 *p;

    (void)memset(frame, 0, RS485IF_BATTERY_RESPONSE_SIZE);
    p = &frame[RS485IF_PAYLOAD_OFFSET];

    /* Offsets follow Rs485If_ParsePackData. */
    TbWriteU16(&p[0], voltage);
    TbWriteU32(&p[6], (uint32)current);
    TbWriteU16(&p[10], (uint16)temperature); /* pack temperature      */
    TbWriteU16(&p[12], (uint16)temperature); /* highest sensor        */
    TbWriteU16(&p[14], (uint16)temperature); /* lowest sensor         */
    p[16] = stateOfCharge;
    p[17] = stateOfHealth;

    TbFinishFrame(frame, RS485IF_BATTERY_RESPONSE_SIZE, serial, RS485IF_CMD_BATTERY_PARAMS);
}

/**
 * @brief Cell-level response with every cell at @p base except cell @p weakIndex at @p base - @p sag.
 *
 * Shaping it this way makes the imbalance exactly @p sag and the weakest cell exactly @p weakIndex,
 * so both can be asserted against a value computed here rather than read back from the component.
 */
static void TbBuildCellResponse(uint8 *frame, uint32 serial, uint16 base, uint8 weakIndex, uint16 sag,
                                sint16 temperature)
{
    uint8 *p;
    uint8 i;

    (void)memset(frame, 0, RS485IF_CELL_RESPONSE_SIZE);
    p = &frame[RS485IF_PAYLOAD_OFFSET];

    for (i = 0u; i < RS485IF_CELLS_PER_PACK; i++)
    {
        const uint16 value = (i == weakIndex) ? (uint16)(base - sag) : base;

        TbWriteU16(&p[(uint16)i * 2u], value);
    }

    for (i = 0u; i < RS485IF_TEMPS_PER_PACK; i++)
    {
        TbWriteU16(&p[50u + ((uint16)i * 2u)], (uint16)temperature);
    }

    TbFinishFrame(frame, RS485IF_CELL_RESPONSE_SIZE, serial, RS485IF_CMD_CELL_PARAMS);
}

/*==================================================================================================
 *  Bus simulation
 *
 *  Rs485If issues one request per (pack, object) and reads a fixed-length reply. The stub answers
 *  each transmitted request with the next queued response, so the script below has to be in the
 *  order the transport asks -- which is itself worth asserting, and TS-BATT-001 does.
 *================================================================================================*/

/** One pack's intended readings. */
typedef struct
{
    boolean answersSerial; /**< FALSE: the pack is absent entirely.            */
    boolean answersPack;   /**< FALSE: discovered but silent this cycle.       */
    boolean answersCells;  /**< FALSE: pack data but no cell data.             */
    uint16 voltage;
    sint32 current;
    sint16 temperature;
    uint8 stateOfCharge;
    uint8 stateOfHealth;
    uint16 cellBase;
    uint8 weakIndex;
    uint16 sag;
} TbPackScript;

/**
 * @brief Queue the whole discovery exchange for a scripted bus.
 *
 * The order is not arbitrary and has to match Rs485If_DiscoverPacks exactly, because a scripted reply
 * is delivered to whichever request happens to be next. Discovery transmits:
 *
 *     switch(none)                          -- all packs off, so replies cannot collide
 *     for each slot:  switch(slot)          -- enable exactly this one
 *                     serial request        -- the pack answers, or does not
 *                     switch(none)          -- off again
 *     switch(present)                       -- re-enable only what answered
 *
 * Writing it out this way makes the test sensitive to that sequence changing, which is the point: the
 * one-at-a-time enable is what stops four packs answering an unaddressed request simultaneously, and
 * a refactor that lost it would still pass a test that only checked the serial replies.
 */
static void TbQueueDiscovery(const TbPackScript *script)
{
    uint8 slot;

    Stub_Uart_QueueTxResponse(TB_BUS, Rs485_Tv_ResponseSwitchAck, RS485IF_REQUEST_FRAME_SIZE_EXT);

    for (slot = 0u; slot < (uint8)RS485IF_PACK_COUNT; slot++)
    {
        Stub_Uart_QueueTxResponse(TB_BUS, Rs485_Tv_ResponseSwitchAck, RS485IF_REQUEST_FRAME_SIZE_EXT);

        if (script[slot].answersSerial != FALSE)
        {
            uint8 frame[RS485IF_SERIAL_RESPONSE_SIZE];

            TbBuildSerialResponse(frame, TbSerials[slot]);
            Stub_Uart_QueueTxResponse(TB_BUS, frame, RS485IF_SERIAL_RESPONSE_SIZE);
        }
        else
        {
            /* A silent entry, not an omitted one: the replies after it must stay aligned with the
             * requests that follow, or an absent pack would shift every later pack's data onto the
             * wrong slot -- which is a far more confusing failure than the one being modelled. */
            Stub_Uart_QueueTxSilence(TB_BUS);
        }

        Stub_Uart_QueueTxResponse(TB_BUS, Rs485_Tv_ResponseSwitchAck, RS485IF_REQUEST_FRAME_SIZE_EXT);
    }

    Stub_Uart_QueueTxResponse(TB_BUS, Rs485_Tv_ResponseSwitchAck, RS485IF_REQUEST_FRAME_SIZE_EXT);
}

/**
 * @brief Queue one polling round: pack data then cell data, for each discovered pack in slot order.
 *
 * Only discovered packs are polled, so a slot that failed discovery contributes no entries at all --
 * unlike a discovered pack that goes silent, which contributes two silent turns.
 */
static void TbQueueRound(const TbPackScript *script)
{
    uint8 slot;

    for (slot = 0u; slot < (uint8)RS485IF_PACK_COUNT; slot++)
    {
        const TbPackScript *s = &script[slot];

        if (s->answersSerial == FALSE)
        {
            continue;
        }

        if (s->answersPack != FALSE)
        {
            uint8 frame[RS485IF_BATTERY_RESPONSE_SIZE];

            TbBuildPackResponse(frame, TbSerials[slot], s->voltage, s->current, s->temperature,
                                s->stateOfCharge, s->stateOfHealth);
            Stub_Uart_QueueTxResponse(TB_BUS, frame, RS485IF_BATTERY_RESPONSE_SIZE);
        }
        else
        {
            Stub_Uart_QueueTxSilence(TB_BUS);
        }

        if (s->answersCells != FALSE)
        {
            uint8 frame[RS485IF_CELL_RESPONSE_SIZE];

            TbBuildCellResponse(frame, TbSerials[slot], s->cellBase, s->weakIndex, s->sag, s->temperature);
            Stub_Uart_QueueTxResponse(TB_BUS, frame, RS485IF_CELL_RESPONSE_SIZE);
        }
        else
        {
            Stub_Uart_QueueTxSilence(TB_BUS);
        }
    }
}

/** A healthy four-pack bus: identical packs, small and equal imbalance. */
static void TbHealthyScript(TbPackScript *script)
{
    uint8 slot;

    for (slot = 0u; slot < (uint8)RS485IF_PACK_COUNT; slot++)
    {
        script[slot].answersSerial = TRUE;
        script[slot].answersPack = TRUE;
        script[slot].answersCells = TRUE;
        script[slot].voltage = 7000u;
        script[slot].current = 4300;
        script[slot].temperature = 3500;
        script[slot].stateOfCharge = 80u;
        script[slot].stateOfHealth = 95u;
        script[slot].cellBase = 3650u;
        script[slot].weakIndex = 0u;
        script[slot].sag = 10u;
    }
}

/** Bring the bus up, discover, and run one polling round. */
static void TbRunCycle(const TbPackScript *script)
{
    TbQueueRound(script);
    TEST_ASSERT_EQUAL(E_OK, BattSwc_MainFunction());
}

/*==================================================================================================
 *  Fixture
 *================================================================================================*/

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Det_Init();
    Rs485If_DeInit();
    TEST_ASSERT_EQUAL(E_OK, Dem_Init());
    TEST_ASSERT_EQUAL(E_OK, Rs485If_Init());
    TEST_ASSERT_EQUAL(E_OK, BattSwc_Init());
}

void tearDown(void)
{
    /* Rs485If caches discovered packs, so without this a later case inherits the previous one's
     * bus population and passes or fails depending on execution order. */
    Rs485If_DeInit();
}

/*==================================================================================================
 *  TS-BATT-001 .. 003  Discovery and the responding count
 *================================================================================================*/

/** TS-BATT-001: a fully populated bus reports four present and four responding. */
static void test_Batt_HealthyBusReportsAllPacks(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT8(RS485IF_PACK_COUNT, aggregate.packsPresent);
    TEST_ASSERT_EQUAL_UINT8(RS485IF_PACK_COUNT, aggregate.packsResponding);
    TEST_ASSERT_TRUE(aggregate.aggregateValid);
}

/**
 * TS-BATT-002: SWREQ-BAT-0030 -- aggregates cover the responding packs only.
 *
 * The case that matters. Three packs at 80 % state of charge and one silent must report 80 %, not
 * 60 %. Averaging a missing pack in as zero is what made three healthy packs look like a fault in
 * v1, and the arithmetic is indistinguishable from a genuine discharge.
 */
static void test_Batt_AggregateExcludesSilentPack(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    /* Slot 4 was discovered but goes silent this cycle. */
    script[3].answersPack = FALSE;
    script[3].answersCells = FALSE;

    TbQueueRound(script);
    STD_DISCARD(BattSwc_MainFunction()); /* E_NOT_OK: one pack did not answer */

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT8(RS485IF_PACK_COUNT, aggregate.packsPresent);
    TEST_ASSERT_EQUAL_UINT8(3u, aggregate.packsResponding);

    /* The lowest state of charge is 80, from the three that answered -- not 0 from the fourth. */
    TEST_ASSERT_EQUAL_UINT16(80u, aggregate.lowestStateOfCharge);
    TEST_ASSERT_EQUAL_UINT16(95u, aggregate.lowestStateOfHealth);

    /* Current sums over responders only: three packs at 4300, not four. */
    TEST_ASSERT_EQUAL_INT32(3 * 4300, aggregate.totalCurrent);
}

/**
 * TS-BATT-003: SWREQ-BAT-0048 -- a silent pack is marked invalid, not zeroed.
 *
 * `dataValid` is what lets a consumer tell "0 V" from "no reading". Zero volts is an alarm; the
 * absence of a reading is a different fact.
 */
static void test_Batt_SilentPackIsInvalidNotZero(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_PackHealthType health;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[1].answersPack = FALSE;
    script[1].answersCells = FALSE;

    TbQueueRound(script);
    STD_DISCARD(BattSwc_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(2u, &health));
    TEST_ASSERT_FALSE(health.dataValid);

    /* And a pack that did answer is valid, so the flag is discriminating rather than always false. */
    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, &health));
    TEST_ASSERT_TRUE(health.dataValid);
}

/*==================================================================================================
 *  TS-BATT-004 .. 008  Cell imbalance
 *================================================================================================*/

/** TS-BATT-004: imbalance is highest minus lowest, and the weakest cell is identified. */
static void test_Batt_ImbalanceAndWeakestCell(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_PackHealthType health;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    /* Cell 7 of pack 1 sags by 120 raw units below a 3650 base. */
    script[0].cellBase = 3650u;
    script[0].weakIndex = 7u;
    script[0].sag = 120u;

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, &health));
    TEST_ASSERT_TRUE(health.dataValid);
    TEST_ASSERT_EQUAL_UINT16(3650u, health.cellVoltageHighest);
    TEST_ASSERT_EQUAL_UINT16(3650u - 120u, health.cellVoltageLowest);
    TEST_ASSERT_EQUAL_UINT16(120u, health.cellImbalance);
    TEST_ASSERT_EQUAL_UINT8(7u, health.weakestCellIndex);
}

/**
 * TS-BATT-005: SWREQ-BAT-0035 -- the warning fires above the limit and not at it.
 *
 * Both sides of the boundary, because an off-by-one here either cries wolf on every healthy pack or
 * never fires at all, and neither is visible without testing the exact threshold.
 */
static void test_Batt_ImbalanceWarningBoundary(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_PackHealthType health;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    /* Exactly at the limit: not a warning. */
    script[0].sag = BATTSWC_IMBALANCE_WARN_RAW;
    TbRunCycle(script);
    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, &health));
    TEST_ASSERT_EQUAL_UINT16(BATTSWC_IMBALANCE_WARN_RAW, health.cellImbalance);
    TEST_ASSERT_FALSE(health.imbalanceWarning);

    /* One unit above: a warning. */
    script[0].sag = BATTSWC_IMBALANCE_WARN_RAW + 1u;
    TbRunCycle(script);
    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, &health));
    TEST_ASSERT_TRUE(health.imbalanceWarning);
}

/** TS-BATT-006: the worst imbalance across the bus is reported with which pack has it. */
static void test_Batt_WorstImbalanceIdentifiesPack(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[0].sag = 40u;
    script[1].sag = 250u; /* the worst */
    script[2].sag = 90u;
    script[3].sag = 15u;

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT16(250u, aggregate.worstImbalance);
    TEST_ASSERT_EQUAL_UINT8(2u, aggregate.worstImbalancePack); /* 1-based */
}

/**
 * TS-BATT-007: a silent pack cannot win the worst-imbalance comparison.
 *
 * A pack with no data has no imbalance. If an invalid pack's stale or zeroed cells were compared,
 * the reported worst offender would be whichever pack had just gone quiet -- pointing a technician
 * at the one pack that is definitely not the problem.
 */
static void test_Batt_SilentPackDoesNotWinWorstImbalance(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    /* Give pack 3 a large imbalance, then have it answer pack data but not cell data. */
    script[2].sag = 900u;
    script[2].answersCells = FALSE;
    script[1].sag = 200u;

    TbQueueRound(script);
    STD_DISCARD(BattSwc_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT16(200u, aggregate.worstImbalance);
    TEST_ASSERT_EQUAL_UINT8(2u, aggregate.worstImbalancePack);
}

/** TS-BATT-008: a perfectly balanced pack reports zero imbalance and no warning. */
static void test_Batt_BalancedPackReportsZeroImbalance(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_PackHealthType health;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[0].sag = 0u;
    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, &health));
    TEST_ASSERT_EQUAL_UINT16(0u, health.cellImbalance);
    TEST_ASSERT_FALSE(health.imbalanceWarning);
}

/*==================================================================================================
 *  TS-BATT-009 .. 011  Temperature and extremes
 *================================================================================================*/

/** TS-BATT-009: the highest temperature across responding packs wins. */
static void test_Batt_HighestTemperatureAcrossPacks(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[0].temperature = 3000;
    script[1].temperature = 4200; /* the highest */
    script[2].temperature = 3100;
    script[3].temperature = 2900;

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_INT16(4200, aggregate.highestTemperature);
}

/**
 * TS-BATT-010: a negative temperature is carried as negative.
 *
 * The raw field is signed and a cold vehicle is a real operating condition. An unsigned read would
 * turn -5 degC into something near 65 000, which passes a range check on the wrong side.
 */
static void test_Batt_NegativeTemperatureIsSigned(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[0].temperature = -1500;
    script[1].temperature = -1200;
    script[2].temperature = -1800;
    script[3].temperature = -1400;

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_INT16(-1200, aggregate.highestTemperature);
}

/**
 * TS-BATT-011: a negative pack current sums correctly.
 *
 * Charging and discharging differ only in sign, and a sum that treated the field as unsigned would
 * report a charging pack as a very large discharge.
 */
static void test_Batt_NegativeCurrentSumsSigned(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    script[0].current = -2000;
    script[1].current = -2000;
    script[2].current = 1000;
    script[3].current = 1000;

    TbRunCycle(script);

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_INT32(-2000, aggregate.totalCurrent);
}

/*==================================================================================================
 *  TS-BATT-012 .. 014  Whole-bus failure and contract
 *================================================================================================*/

/**
 * TS-BATT-012: with no pack responding, the aggregate is invalid rather than zero.
 *
 * An all-zero aggregate is a readable set of numbers that happens to describe a catastrophic battery.
 * `aggregateValid` is what stops it being published as one.
 */
static void test_Batt_NoResponseMakesAggregateInvalid(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;
    uint8 slot;

    TbHealthyScript(script);
    TbQueueDiscovery(script);
    TEST_ASSERT_EQUAL(E_OK, Rs485If_DiscoverPacks());

    for (slot = 0u; slot < (uint8)RS485IF_PACK_COUNT; slot++)
    {
        script[slot].answersPack = FALSE;
        script[slot].answersCells = FALSE;
    }

    TbQueueRound(script); /* queues nothing */
    STD_DISCARD(BattSwc_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT8(0u, aggregate.packsResponding);
    TEST_ASSERT_FALSE(aggregate.aggregateValid);
}

/** TS-BATT-013: an entirely absent bus is discovered as zero packs, and that is not a crash. */
static void test_Batt_EmptyBusIsHandled(void)
{
    TbPackScript script[RS485IF_PACK_COUNT];
    BattSwc_AggregateType aggregate;
    uint8 slot;

    TbHealthyScript(script);
    for (slot = 0u; slot < (uint8)RS485IF_PACK_COUNT; slot++)
    {
        script[slot].answersSerial = FALSE;
    }

    TbQueueDiscovery(script);
    STD_DISCARD(Rs485If_DiscoverPacks());
    STD_DISCARD(BattSwc_MainFunction());

    TEST_ASSERT_EQUAL(E_OK, BattSwc_GetAggregate(&aggregate));
    TEST_ASSERT_EQUAL_UINT8(0u, aggregate.packsPresent);
    TEST_ASSERT_EQUAL_UINT8(0u, aggregate.packsResponding);
    TEST_ASSERT_FALSE(aggregate.aggregateValid);
}

/** TS-BATT-014: the accessors reject an out-of-range slot and a NULL destination. */
static void test_Batt_AccessorsRejectBadArguments(void)
{
    BattSwc_PackHealthType health;

    TEST_ASSERT_NOT_EQUAL(E_OK, BattSwc_GetPackHealth(0u, &health));
    TEST_ASSERT_NOT_EQUAL(E_OK, BattSwc_GetPackHealth((Rs485If_SlotType)(RS485IF_PACK_COUNT + 1u), &health));
    TEST_ASSERT_NOT_EQUAL(E_OK, BattSwc_GetPackHealth(1u, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, BattSwc_GetAggregate(NULL_PTR));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Batt_HealthyBusReportsAllPacks);
    RUN_TEST(test_Batt_AggregateExcludesSilentPack);
    RUN_TEST(test_Batt_SilentPackIsInvalidNotZero);

    RUN_TEST(test_Batt_ImbalanceAndWeakestCell);
    RUN_TEST(test_Batt_ImbalanceWarningBoundary);
    RUN_TEST(test_Batt_WorstImbalanceIdentifiesPack);
    RUN_TEST(test_Batt_SilentPackDoesNotWinWorstImbalance);
    RUN_TEST(test_Batt_BalancedPackReportsZeroImbalance);

    RUN_TEST(test_Batt_HighestTemperatureAcrossPacks);
    RUN_TEST(test_Batt_NegativeTemperatureIsSigned);
    RUN_TEST(test_Batt_NegativeCurrentSumsSigned);

    RUN_TEST(test_Batt_NoResponseMakesAggregateInvalid);
    RUN_TEST(test_Batt_EmptyBusIsHandled);
    RUN_TEST(test_Batt_AccessorsRejectBadArguments);

    return UNITY_END();
}

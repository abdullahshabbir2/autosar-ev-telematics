/**
 * @file    Can.c
 * @brief   MCP2515 CAN controller driver.
 *
 * Reaches the controller only through Spi, Dio and Gpt, so this file contains no
 * platform code at all and is compiled unchanged for the host test build. Every
 * register sequence below is therefore verifiable off-target against the SPI stub,
 * which is the only practical way to gain confidence in a register protocol without a
 * logic analyser permanently attached.
 *
 * Register and instruction names follow the MCP2515 datasheet (DS20001801J).
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "mcal/Can/Can.h"

#include "services/Det/Det.h"
#include "mcal/Gpt/Gpt.h"
#include "mcal/Spi/Spi.h"

/*==================================================================================================
 *  MCP2515 SPI instruction set (datasheet table 12-1)
 *================================================================================================*/

#define MCP_INSTR_RESET 0xC0u
#define MCP_INSTR_READ 0x03u
#define MCP_INSTR_WRITE 0x02u
#define MCP_INSTR_READ_STATUS 0xA0u
#define MCP_INSTR_RX_STATUS 0xB0u
#define MCP_INSTR_BIT_MODIFY 0x05u

/** READ RX BUFFER, 0b1001 0nm0. nm = 00 selects RXB0SIDH, 10 selects RXB1SIDH. */
#define MCP_INSTR_READ_RXB0 0x90u
#define MCP_INSTR_READ_RXB1 0x94u

/** LOAD TX BUFFER, 0b0100 0abc. abc = 000 selects TXB0SIDH. */
#define MCP_INSTR_LOAD_TXB0 0x40u

/** REQUEST TO SEND, 0b1000 0nnn, one bit per transmit buffer. */
#define MCP_INSTR_RTS_TXB0 0x81u

/*==================================================================================================
 *  MCP2515 registers
 *================================================================================================*/

#define MCP_REG_RXF0SIDH 0x00u
#define MCP_REG_RXF1SIDH 0x04u
#define MCP_REG_RXF2SIDH 0x08u
#define MCP_REG_RXF3SIDH 0x10u
#define MCP_REG_RXF4SIDH 0x14u
#define MCP_REG_RXF5SIDH 0x18u
#define MCP_REG_TEC 0x1Cu
#define MCP_REG_REC 0x1Du
#define MCP_REG_RXM0SIDH 0x20u
#define MCP_REG_RXM1SIDH 0x24u
#define MCP_REG_CNF3 0x28u
#define MCP_REG_CNF2 0x29u
#define MCP_REG_CNF1 0x2Au
#define MCP_REG_CANINTE 0x2Bu
#define MCP_REG_CANINTF 0x2Cu
#define MCP_REG_EFLG 0x2Du
#define MCP_REG_CANCTRL 0x0Fu
#define MCP_REG_CANSTAT 0x0Eu
#define MCP_REG_TXB0CTRL 0x30u
#define MCP_REG_RXB0CTRL 0x60u
#define MCP_REG_RXB1CTRL 0x70u

/*---------------------------------- CANCTRL ---------------------------------*/

#define MCP_CANCTRL_REQOP_MASK 0xE0u
#define MCP_MODE_NORMAL 0x00u
#define MCP_MODE_SLEEP 0x20u
#define MCP_MODE_LOOPBACK 0x40u
#define MCP_MODE_LISTENONLY 0x60u
#define MCP_MODE_CONFIG 0x80u

/*---------------------------------- CANSTAT ---------------------------------*/

#define MCP_CANSTAT_OPMOD_MASK 0xE0u

/*---------------------------------- CANINTF ---------------------------------*/

#define MCP_CANINTF_RX0IF 0x01u
#define MCP_CANINTF_RX1IF 0x02u
#define MCP_CANINTF_ERRIF 0x20u
#define MCP_CANINTF_MERRF 0x80u

/*---------------------------------- CANINTE ---------------------------------*/

#define MCP_CANINTE_RX0IE 0x01u
#define MCP_CANINTE_RX1IE 0x02u
#define MCP_CANINTE_ERRIE 0x20u

/*------------------------------------ EFLG ----------------------------------*/

#define MCP_EFLG_EWARN 0x01u
#define MCP_EFLG_RXWAR 0x02u
#define MCP_EFLG_TXWAR 0x04u
#define MCP_EFLG_RXEP 0x08u
#define MCP_EFLG_TXEP 0x10u
#define MCP_EFLG_TXBO 0x20u
#define MCP_EFLG_RX0OVR 0x40u
#define MCP_EFLG_RX1OVR 0x80u

/*--------------------------------- RXBnCTRL ---------------------------------*/

/** RXM = 00: accept only identifiers that pass the acceptance filters. */
#define MCP_RXBCTRL_RXM_FILTERS 0x00u

/**
 * @brief BUKT: a frame for RXB0 rolls over into RXB1 when RXB0 is still full.
 *
 * This is what turns the two receive buffers into a 2-deep FIFO. The motor controller
 * emits its two frames back to back, so without rollover the second one is dropped
 * whenever the software read is even slightly late.
 */
#define MCP_RXB0CTRL_BUKT 0x04u

/*--------------------------------- TXBnCTRL ---------------------------------*/

#define MCP_TXBCTRL_TXREQ 0x08u

/*----------------------------- Identifier fields ----------------------------*/

/** SIDL bit 3: set when the identifier is in 29-bit extended format. */
#define MCP_SIDL_EXIDE 0x08u

/** Bytes read back from a receive buffer: 4 identifier + DLC + 8 data. */
#define MCP_RX_FRAME_BYTES 13u

/** RXBnDLC bit 6: the frame was a remote transmission request. */
#define MCP_DLC_RTR 0x40u
#define MCP_DLC_MASK 0x0Fu

/*==================================================================================================
 *  Timing
 *================================================================================================*/

/** Bound on a mode-change handshake, in milliseconds. */
#define CAN_MODE_CHANGE_TIMEOUT_MS 20u

/*==================================================================================================
 *  Local data
 *================================================================================================*/

/**
 * @brief Software receive queue.
 *
 * A single-producer/single-consumer ring: ::Can_MainFunction_Read writes, ::Can_Receive
 * reads. Head and tail are only ever advanced by their own side, so no lock is needed
 * on a 32-bit core where the index loads and stores are atomic.
 */
STATIC Can_PduType Can_RxQueue[CAN_RX_QUEUE_DEPTH];
STATIC uint8 Can_RxHead;  /* next slot to write */
STATIC uint8 Can_RxTail;  /* next slot to read  */
STATIC uint8 Can_RxCount;

STATIC Can_ControllerStateType Can_State = CAN_CS_UNINIT;
STATIC Can_StatisticsType Can_Stats;
STATIC boolean Can_BusOffLatched;

/*==================================================================================================
 *  Identifier codec -- pure, exhaustively tested off-target
 *================================================================================================*/

void Can_EncodeIdentifier(Can_IdType id, uint8 *regs)
{
    if (regs == NULL_PTR)
    {
        return;
    }

    if ((id & CAN_ID_EXTENDED_FLAG) != 0u)
    {
        const uint32 raw = id & CAN_ID_MASK;

        /* A 29-bit identifier splits as SID[10:0] followed by EID[17:0]. SIDL carries
         * the low three SID bits in its top three, the EXIDE selector in bit 3, and the
         * top two EID bits in its low two -- which is why this cannot be written as a
         * simple byte split. */
        regs[0] = (uint8)((raw >> 21u) & 0xFFu);
        regs[1] = (uint8)((uint8)((raw >> 13u) & 0xE0u) | (uint8)MCP_SIDL_EXIDE |
                          (uint8)((raw >> 16u) & 0x03u));
        regs[2] = (uint8)((raw >> 8u) & 0xFFu);
        regs[3] = (uint8)(raw & 0xFFu);
    }
    else
    {
        const uint32 raw = id & 0x7FFuL;

        regs[0] = (uint8)((raw >> 3u) & 0xFFu);
        regs[1] = (uint8)((raw & 0x07uL) << 5u); /* EXIDE clear */
        regs[2] = 0x00u;
        regs[3] = 0x00u;
    }
}

Can_IdType Can_DecodeIdentifier(const uint8 *regs)
{
    Can_IdType id;

    if (regs == NULL_PTR)
    {
        return 0u;
    }

    if ((regs[1] & MCP_SIDL_EXIDE) != 0u)
    {
        id = ((Can_IdType)regs[0] << 21u) | (((Can_IdType)regs[1] & 0xE0uL) << 13u) |
             (((Can_IdType)regs[1] & 0x03uL) << 16u) | ((Can_IdType)regs[2] << 8u) |
             (Can_IdType)regs[3];
        id |= CAN_ID_EXTENDED_FLAG;
    }
    else
    {
        id = ((Can_IdType)regs[0] << 3u) | ((Can_IdType)regs[1] >> 5u);
    }

    return id;
}

/*==================================================================================================
 *  Low-level SPI access
 *
 *  Every helper assumes the caller already holds the bus lock. Locking is taken once per
 *  public API call rather than per register access: an MCP2515 mode change is a
 *  read-modify-write followed by a poll, and releasing the bus between those steps would
 *  let the SD driver interleave a multi-millisecond block write into the middle of the
 *  handshake.
 *================================================================================================*/

STATIC Std_ReturnType Can_Mcp_Reset(void)
{
    const uint8 instruction = MCP_INSTR_RESET;

    return Spi_Transfer(SPI_DEVICE_CAN, &instruction, NULL_PTR, 1u);
}

STATIC Std_ReturnType Can_Mcp_ReadRegister(uint8 address, uint8 *value)
{
    uint8 tx[3];
    uint8 rx[3];

    tx[0] = MCP_INSTR_READ;
    tx[1] = address;
    tx[2] = 0x00u;

    if (Spi_Transfer(SPI_DEVICE_CAN, tx, rx, (uint16)sizeof(tx)) != E_OK)
    {
        return E_NOT_OK;
    }
    *value = rx[2];
    return E_OK;
}

STATIC Std_ReturnType Can_Mcp_WriteRegister(uint8 address, uint8 value)
{
    uint8 tx[3];

    tx[0] = MCP_INSTR_WRITE;
    tx[1] = address;
    tx[2] = value;

    return Spi_Transfer(SPI_DEVICE_CAN, tx, NULL_PTR, (uint16)sizeof(tx));
}

STATIC Std_ReturnType Can_Mcp_BitModify(uint8 address, uint8 mask, uint8 value)
{
    uint8 tx[4];

    tx[0] = MCP_INSTR_BIT_MODIFY;
    tx[1] = address;
    tx[2] = mask;
    tx[3] = value;

    return Spi_Transfer(SPI_DEVICE_CAN, tx, NULL_PTR, (uint16)sizeof(tx));
}

/**
 * @brief Write four consecutive identifier registers starting at @p address.
 *
 * Uses the auto-incrementing WRITE instruction so the four bytes go out in one
 * transaction: the controller latches an identifier atomically only if it is written
 * without an intervening chip-select release.
 */
STATIC Std_ReturnType Can_Mcp_WriteIdRegisters(uint8 address, const uint8 *regs)
{
    uint8 tx[2u + CAN_ID_REGISTER_COUNT];
    uint8 i;

    tx[0] = MCP_INSTR_WRITE;
    tx[1] = address;
    for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
    {
        tx[2u + i] = regs[i];
    }

    return Spi_Transfer(SPI_DEVICE_CAN, tx, NULL_PTR, (uint16)sizeof(tx));
}

/**
 * @brief Read a whole receive buffer using the READ RX BUFFER instruction.
 *
 * That instruction is used rather than four ordinary register reads because it clears
 * the matching RXnIF flag as a side effect of the transfer. Clearing the flag separately
 * leaves a window in which a frame arriving between the read and the clear is discarded
 * along with the flag -- a lost-frame bug that appears only under load.
 */
STATIC Std_ReturnType Can_Mcp_ReadRxBuffer(uint8 instruction, uint8 *frame)
{
    uint8 tx[1u + MCP_RX_FRAME_BYTES];
    uint8 rx[1u + MCP_RX_FRAME_BYTES];
    uint8 i;

    tx[0] = instruction;
    for (i = 1u; i < (uint8)sizeof(tx); i++)
    {
        tx[i] = (uint8)SPI_DUMMY_BYTE;
    }

    if (Spi_Transfer(SPI_DEVICE_CAN, tx, rx, (uint16)sizeof(tx)) != E_OK)
    {
        return E_NOT_OK;
    }

    for (i = 0u; i < MCP_RX_FRAME_BYTES; i++)
    {
        frame[i] = rx[1u + i];
    }
    return E_OK;
}

/**
 * @brief Enter @p mode and confirm the controller actually got there.
 *
 * The confirmation is the point. A mode request is not a command but a request: the
 * controller finishes any frame in progress first, and a bus held dominant by a faulty
 * node can make a request to leave configuration mode never complete. v1 assumed the
 * transition always succeeded -- in fact it never issued one at all -- so a controller
 * stuck in configuration mode was indistinguishable from a working one.
 */
STATIC Std_ReturnType Can_Mcp_SetMode(uint8 mode)
{
    Gpt_TimestampType started;

    if (Can_Mcp_BitModify(MCP_REG_CANCTRL, MCP_CANCTRL_REQOP_MASK, mode) != E_OK)
    {
        return E_NOT_OK;
    }

    started = Gpt_GetMonotonicMs();
    for (;;)
    {
        uint8 status = 0u;

        if (Can_Mcp_ReadRegister(MCP_REG_CANSTAT, &status) != E_OK)
        {
            return E_NOT_OK;
        }
        if ((status & MCP_CANSTAT_OPMOD_MASK) == mode)
        {
            return E_OK;
        }
        if (Gpt_HasElapsed(started, CAN_MODE_CHANGE_TIMEOUT_MS) != FALSE)
        {
            return E_TIMEOUT;
        }
        Gpt_DelayUs(100u);
    }
}

/**
 * @brief Program bit timing and the acceptance filter set. Requires configuration mode.
 */
STATIC Std_ReturnType Can_Mcp_Configure(void)
{
    uint8 idRegs[CAN_ID_REGISTER_COUNT];
    uint8 maskRegs[CAN_ID_REGISTER_COUNT];
    Std_ReturnType status = E_OK;

    /* Bit timing. CNF1/2/3 are only writable in configuration mode. */
    status |= Can_Mcp_WriteRegister(MCP_REG_CNF1, CAN_CNF1_VALUE);
    status |= Can_Mcp_WriteRegister(MCP_REG_CNF2, CAN_CNF2_VALUE);
    status |= Can_Mcp_WriteRegister(MCP_REG_CNF3, CAN_CNF3_VALUE);

    /* Both masks demand an exact match on all 29 identifier bits. The EXIDE bit is set
     * in the mask registers too, so a standard frame whose 11 bits happen to coincide
     * with the top of an extended identifier is rejected. */
    Can_EncodeIdentifier(CAN_FILTER_MASK_EXTENDED | CAN_ID_EXTENDED_FLAG, maskRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXM0SIDH, maskRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXM1SIDH, maskRegs);

    /* RXB0 takes the two identifiers through RXF0 and RXF1. */
    Can_EncodeIdentifier(CAN_ID_MCU_DRIVE_STATE | CAN_ID_EXTENDED_FLAG, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF0SIDH, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF2SIDH, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF4SIDH, idRegs);

    Can_EncodeIdentifier(CAN_ID_MCU_CURRENT_VOLTAGE | CAN_ID_EXTENDED_FLAG, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF1SIDH, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF3SIDH, idRegs);
    status |= Can_Mcp_WriteIdRegisters(MCP_REG_RXF5SIDH, idRegs);

    /* Filters on, and rollover enabled so the pair of frames cannot be clipped. */
    status |= Can_Mcp_WriteRegister(MCP_REG_RXB0CTRL,
                                    (uint8)(MCP_RXBCTRL_RXM_FILTERS | MCP_RXB0CTRL_BUKT));
    status |= Can_Mcp_WriteRegister(MCP_REG_RXB1CTRL, MCP_RXBCTRL_RXM_FILTERS);

    /* Interrupt flags are enabled even though the INT line is only polled: CANINTF is
     * read as the "is there a frame" indication, and the enable bits gate it. */
    status |= Can_Mcp_WriteRegister(
        MCP_REG_CANINTE, (uint8)(MCP_CANINTE_RX0IE | MCP_CANINTE_RX1IE | MCP_CANINTE_ERRIE));
    status |= Can_Mcp_WriteRegister(MCP_REG_CANINTF, 0x00u);

    return (status == E_OK) ? E_OK : E_NOT_OK;
}

/*==================================================================================================
 *  Receive queue
 *================================================================================================*/

STATIC void Can_QueuePush(const Can_PduType *pdu)
{
    if (Can_RxCount >= (uint8)CAN_RX_QUEUE_DEPTH)
    {
        /* Drop the newest rather than overwrite the oldest. For a signal stream the
         * freshest sample is the valuable one, so this looks like the wrong choice --
         * but overwriting the tail while Can_Receive is reading it would hand the
         * consumer a half-updated frame, and the queue only ever fills when the
         * scheduler is already late. Counting the loss is what makes the condition
         * visible. */
        Can_Stats.rxOverflowCount++;
        return;
    }

    Can_RxQueue[Can_RxHead] = *pdu;
    Can_RxHead = (uint8)((Can_RxHead + 1u) % (uint8)CAN_RX_QUEUE_DEPTH);
    Can_RxCount++;
}

/*==================================================================================================
 *  API
 *================================================================================================*/

void Can_DeInit(void)
{
    Can_RxHead = 0u;
    Can_RxTail = 0u;
    Can_RxCount = 0u;
    Can_BusOffLatched = FALSE;
    Can_State = CAN_CS_UNINIT;
    Can_Stats.framesReceived = 0u;
    Can_Stats.framesTransmitted = 0u;
    Can_Stats.rxOverflowCount = 0u;
    Can_Stats.txFailureCount = 0u;
    Can_Stats.busOffCount = 0u;
    Can_Stats.errorWarningCount = 0u;
    Can_Stats.rxErrorCounter = 0u;
    Can_Stats.txErrorCounter = 0u;
}

Std_ReturnType Can_Init(void)
{
    uint8 attempt;

    Can_RxHead = 0u;
    Can_RxTail = 0u;
    Can_RxCount = 0u;
    Can_BusOffLatched = FALSE;
    Can_Stats.framesReceived = 0u;
    Can_Stats.framesTransmitted = 0u;
    Can_Stats.rxOverflowCount = 0u;
    Can_Stats.txFailureCount = 0u;
    Can_Stats.busOffCount = 0u;
    Can_Stats.errorWarningCount = 0u;
    Can_Stats.rxErrorCounter = 0u;
    Can_Stats.txErrorCounter = 0u;
    Can_State = CAN_CS_UNINIT;

    for (attempt = 0u; attempt < (uint8)CAN_INIT_RETRY_COUNT; attempt++)
    {
        Std_ReturnType status;

        if (Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS) != E_OK)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_INIT,
                                         CAN_E_SPI_FAILURE);
            continue;
        }

        status = Can_Mcp_Reset();
        if (status == E_OK)
        {
            /* The reset command puts the device into configuration mode, but it needs
             * time to get there. Polling CANSTAT immediately reads the pre-reset value
             * and passes, which is how a half-configured controller ends up looking
             * healthy. */
            Gpt_DelayMs(CAN_RESET_SETTLE_MS);
            status = Can_Mcp_SetMode(MCP_MODE_CONFIG);
        }
        if (status == E_OK)
        {
            status = Can_Mcp_Configure();
        }
        if (status == E_OK)
        {
            status = Can_Mcp_SetMode(MCP_MODE_NORMAL);
        }

        STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));

        if (status == E_OK)
        {
            Can_State = CAN_CS_STARTED;
            return E_OK;
        }

        /* The MCP2515 occasionally needs a second attempt when its supply rises
         * alongside the MCU's, which is exactly the condition at vehicle power-on. */
        Gpt_DelayMs(CAN_RESET_SETTLE_MS);
    }

    (void)Det_ReportRuntimeError(MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_INIT,
                                 CAN_E_INIT_FAILED);
    return E_NOT_OK;
}

Std_ReturnType Can_SetControllerMode(Can_ControllerStateType state)
{
    uint8 mcpMode;
    Std_ReturnType status;

    DET_CHECK_RETURN(Can_State != CAN_CS_UNINIT, MODULE_ID_CAN, INSTANCE_ID_SINGLE,
                     CAN_API_ID_SET_CONTROLLER_MODE, CAN_E_UNINIT, E_NOT_OK);

    switch (state)
    {
    case CAN_CS_STARTED:
        mcpMode = MCP_MODE_NORMAL;
        break;
    case CAN_CS_STOPPED:
        mcpMode = MCP_MODE_CONFIG;
        break;
    case CAN_CS_SLEEP:
        mcpMode = MCP_MODE_SLEEP;
        break;
    case CAN_CS_UNINIT:
    default:
        (void)Det_ReportError(MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_SET_CONTROLLER_MODE,
                              CAN_E_TRANSITION);
        return E_NOT_OK;
    }

    if (Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS) != E_OK)
    {
        return E_BUSY;
    }
    status = Can_Mcp_SetMode(mcpMode);
    STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));

    if (status == E_OK)
    {
        Can_State = state;
    }
    return status;
}

Can_ControllerStateType Can_GetControllerMode(void)
{
    return Can_State;
}

Std_ReturnType Can_Write(const Can_PduType *pdu)
{
    uint8 tx[2u + CAN_ID_REGISTER_COUNT + 1u + CAN_MAX_DLC];
    uint8 idRegs[CAN_ID_REGISTER_COUNT];
    uint8 ctrl = 0u;
    uint8 i;
    uint16 frameLen;
    Std_ReturnType status;

    DET_CHECK_RETURN(pdu != NULL_PTR, MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_WRITE,
                     CAN_E_PARAM_POINTER, E_NOT_OK);
    DET_CHECK_RETURN(pdu->dlc <= CAN_MAX_DLC, MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_WRITE,
                     CAN_E_PARAM_DLC, E_NOT_OK);
    DET_CHECK_RETURN(Can_State == CAN_CS_STARTED, MODULE_ID_CAN, INSTANCE_ID_SINGLE,
                     CAN_API_ID_WRITE, CAN_E_TRANSITION, E_NOT_OK);

    if (Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS) != E_OK)
    {
        return E_BUSY;
    }

    /* Only TXB0 is used. Three buffers would allow priority ordering, but this ECU
     * transmits nothing time-critical -- diagnostics only -- and a single buffer removes
     * the possibility of frames leaving in an order the receiver does not expect. */
    if (Can_Mcp_ReadRegister(MCP_REG_TXB0CTRL, &ctrl) != E_OK)
    {
        STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));
        return E_NOT_OK;
    }
    if ((ctrl & MCP_TXBCTRL_TXREQ) != 0u)
    {
        STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));
        return E_BUSY;
    }

    Can_EncodeIdentifier(pdu->id, idRegs);

    tx[0] = MCP_INSTR_LOAD_TXB0;
    for (i = 0u; i < CAN_ID_REGISTER_COUNT; i++)
    {
        tx[1u + i] = idRegs[i];
    }
    tx[1u + CAN_ID_REGISTER_COUNT] = (uint8)(pdu->dlc & MCP_DLC_MASK);
    for (i = 0u; i < pdu->dlc; i++)
    {
        tx[2u + CAN_ID_REGISTER_COUNT + i] = pdu->sdu[i];
    }
    frameLen = (uint16)(2u + CAN_ID_REGISTER_COUNT + pdu->dlc);

    status = Spi_Transfer(SPI_DEVICE_CAN, tx, NULL_PTR, frameLen);
    if (status == E_OK)
    {
        const uint8 rts = MCP_INSTR_RTS_TXB0;
        status = Spi_Transfer(SPI_DEVICE_CAN, &rts, NULL_PTR, 1u);
    }

    STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));

    if (status != E_OK)
    {
        Can_Stats.txFailureCount++;
        return E_NOT_OK;
    }

    Can_Stats.framesTransmitted++;
    return E_OK;
}

uint8 Can_MainFunction_Read(void)
{
    uint8 moved = 0u;
    uint8 intf = 0u;
    uint8 eflg = 0u;

    if (Can_State != CAN_CS_STARTED)
    {
        return 0u;
    }

    if (Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS) != E_OK)
    {
        /* The SD driver holds the bus during a slow write. Frames stay in the
         * controller's two buffers and are collected on the next cycle; that is what the
         * rollover configuration is for. Not an error. */
        return 0u;
    }

    if (Can_Mcp_ReadRegister(MCP_REG_CANINTF, &intf) != E_OK)
    {
        STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));
        (void)Det_ReportRuntimeError(MODULE_ID_CAN, INSTANCE_ID_SINGLE,
                                     CAN_API_ID_MAIN_FUNCTION_READ, CAN_E_SPI_FAILURE);
        return 0u;
    }

    while ((moved < (uint8)CAN_MAX_FRAMES_PER_CYCLE) &&
           ((intf & (MCP_CANINTF_RX0IF | MCP_CANINTF_RX1IF)) != 0u))
    {
        uint8 raw[MCP_RX_FRAME_BYTES];
        const uint8 instruction =
            ((intf & MCP_CANINTF_RX0IF) != 0u) ? MCP_INSTR_READ_RXB0 : MCP_INSTR_READ_RXB1;

        if (Can_Mcp_ReadRxBuffer(instruction, raw) != E_OK)
        {
            (void)Det_ReportRuntimeError(MODULE_ID_CAN, INSTANCE_ID_SINGLE,
                                         CAN_API_ID_MAIN_FUNCTION_READ, CAN_E_SPI_FAILURE);
            break;
        }

        /* Remote transmission requests carry no payload and nothing on this bus uses
         * them; accepting one as a data frame would publish eight bytes of stale buffer
         * content as if it were a measurement. */
        if ((raw[4] & MCP_DLC_RTR) == 0u)
        {
            Can_PduType pdu;
            uint8 i;

            pdu.id = Can_DecodeIdentifier(raw);
            pdu.dlc = (uint8)(raw[4] & MCP_DLC_MASK);
            if (pdu.dlc > CAN_MAX_DLC)
            {
                pdu.dlc = CAN_MAX_DLC;
            }
            for (i = 0u; i < CAN_MAX_DLC; i++)
            {
                pdu.sdu[i] = (i < pdu.dlc) ? raw[5u + i] : 0u;
            }
            pdu.timestamp = Gpt_GetMonotonicMs();

            Can_QueuePush(&pdu);
            Can_Stats.framesReceived++;
        }

        moved++;

        if (Can_Mcp_ReadRegister(MCP_REG_CANINTF, &intf) != E_OK)
        {
            break;
        }
    }

    /* Error flags are sampled on every cycle rather than only on failure, so that a
     * degrading bus is visible as a rising counter before it reaches bus-off. */
    if (Can_Mcp_ReadRegister(MCP_REG_EFLG, &eflg) == E_OK)
    {
        if ((eflg & MCP_EFLG_TXBO) != 0u)
        {
            if (Can_BusOffLatched == FALSE)
            {
                Can_BusOffLatched = TRUE;
                Can_Stats.busOffCount++;
            }
        }
        else
        {
            Can_BusOffLatched = FALSE;
        }

        if ((eflg & MCP_EFLG_EWARN) != 0u)
        {
            Can_Stats.errorWarningCount++;
        }
        if ((eflg & (MCP_EFLG_RX0OVR | MCP_EFLG_RX1OVR)) != 0u)
        {
            Can_Stats.rxOverflowCount++;
            /* Overflow flags are sticky and must be cleared explicitly, or every later
             * cycle re-reports the same single event. */
            (void)Can_Mcp_BitModify(MCP_REG_EFLG,
                                    (uint8)(MCP_EFLG_RX0OVR | MCP_EFLG_RX1OVR), 0x00u);
        }
    }

    (void)Can_Mcp_ReadRegister(MCP_REG_TEC, &Can_Stats.txErrorCounter);
    (void)Can_Mcp_ReadRegister(MCP_REG_REC, &Can_Stats.rxErrorCounter);

    STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));
    return moved;
}

Std_ReturnType Can_Receive(Can_PduType *pdu)
{
    DET_CHECK_RETURN(pdu != NULL_PTR, MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_RECEIVE,
                     CAN_E_PARAM_POINTER, E_NOT_OK);

    if (Can_RxCount == 0u)
    {
        return E_NOT_FOUND;
    }

    *pdu = Can_RxQueue[Can_RxTail];
    Can_RxTail = (uint8)((Can_RxTail + 1u) % (uint8)CAN_RX_QUEUE_DEPTH);
    Can_RxCount--;

    return E_OK;
}

uint8 Can_GetRxQueueCount(void)
{
    return Can_RxCount;
}

Std_ReturnType Can_GetStatistics(Can_StatisticsType *stats)
{
    DET_CHECK_RETURN(stats != NULL_PTR, MODULE_ID_CAN, INSTANCE_ID_SINGLE, CAN_API_ID_RECEIVE,
                     CAN_E_PARAM_POINTER, E_NOT_OK);

    *stats = Can_Stats;
    return E_OK;
}

boolean Can_IsBusOff(void)
{
    return Can_BusOffLatched;
}

Std_ReturnType Can_RecoverBusOff(void)
{
    Std_ReturnType status;

    if (Can_BusOffLatched == FALSE)
    {
        return E_OK;
    }

    if (Spi_Lock(SPI_DEVICE_CAN, SPI_LOCK_TIMEOUT_MS) != E_OK)
    {
        return E_BUSY;
    }

    /* A round trip through configuration mode clears the error counters, which is the
     * only way out of bus-off on this controller. */
    status = Can_Mcp_SetMode(MCP_MODE_CONFIG);
    if (status == E_OK)
    {
        status = Can_Mcp_SetMode(MCP_MODE_NORMAL);
    }
    if (status == E_OK)
    {
        (void)Can_Mcp_BitModify(MCP_REG_EFLG, MCP_EFLG_TXBO, 0x00u);
    }

    STD_DISCARD(Spi_Unlock(SPI_DEVICE_CAN));

    if (status == E_OK)
    {
        Can_BusOffLatched = FALSE;
        Can_State = CAN_CS_STARTED;
    }
    return status;
}

void Can_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = CAN_VENDOR_ID;
        versioninfo->moduleID = (uint16)MODULE_ID_CAN;
        versioninfo->sw_major_version = CAN_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = CAN_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = CAN_SW_PATCH_VERSION;
    }
}

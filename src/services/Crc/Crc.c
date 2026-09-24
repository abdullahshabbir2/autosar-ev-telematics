/**
 * @file    Crc.c
 * @brief   AUTOSAR CRC Library implementation.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include "services/Crc/Crc.h"

/*==================================================================================================
 *  Local constants
 *================================================================================================*/

#define CRC8_POLYNOMIAL 0x1Du      /* SAE J1850                                  */
#define CRC8_FINAL_XOR 0xFFu
#define CRC8H2F_POLYNOMIAL 0x2Fu
#define CRC8H2F_FINAL_XOR 0xFFu
#define CRC16_POLYNOMIAL 0x1021u   /* CCITT-FALSE, MSB-first                     */
#define CRC16ARC_POLY_REFLECTED 0xA001u /* ARC, 0x8005 reflected, LSB-first      */
#define CRC32_POLY_REFLECTED 0xEDB88320uL      /* 0x04C11DB7 reflected           */
#define CRC32P4_POLY_REFLECTED 0xC8DF352FuL    /* 0xF4ACFB13 reflected           */
#define CRC32_FINAL_XOR 0xFFFFFFFFuL

#define CRC_BITS_PER_BYTE 8u
#define CRC8_MSB_MASK 0x80u
#define CRC16_MSB_MASK 0x8000u
#define CRC_TABLE_SIZE 256u

/*==================================================================================================
 *  Local data -- reflected CRC-32 lookup tables
 *
 *  Built once at first use rather than stored as 2 KiB of .rodata: flash on this
 *  part is the scarcer resource (see the size budget in docs/02-architecture.md),
 *  and 2 x 1 KiB of DRAM is affordable. Construction is idempotent and produces
 *  the same bytes from any caller, so a race between two tasks arriving here
 *  simultaneously cannot yield a wrong table -- only duplicated work. That makes a
 *  lock unnecessary and keeps these routines callable from an ISR.
 *================================================================================================*/

STATIC uint32 Crc_Crc32Table[CRC_TABLE_SIZE];
STATIC uint32 Crc_Crc32P4Table[CRC_TABLE_SIZE];
STATIC boolean Crc_Crc32TableValid = FALSE;
STATIC boolean Crc_Crc32P4TableValid = FALSE;

/**
 * @brief Populate a reflected CRC-32 lookup table for @p reflectedPoly.
 */
STATIC void Crc_BuildTable32(uint32 *table, uint32 reflectedPoly)
{
    uint32 index;

    for (index = 0u; index < CRC_TABLE_SIZE; index++)
    {
        uint32 remainder = index;
        uint8 bit;

        for (bit = 0u; bit < CRC_BITS_PER_BYTE; bit++)
        {
            if ((remainder & 0x00000001uL) != 0u)
            {
                remainder = (remainder >> 1u) ^ reflectedPoly;
            }
            else
            {
                remainder >>= 1u;
            }
        }
        table[index] = remainder;
    }
}

/*==================================================================================================
 *  CRC-8 family (bit-at-a-time, MSB-first, non-reflected)
 *================================================================================================*/

/**
 * @brief Shared MSB-first CRC-8 kernel.
 *
 * @param dataPtr    Bytes to fold in. May be NULL_PTR only when @p length is zero.
 * @param length     Number of bytes at @p dataPtr.
 * @param register8  Register state on entry, already un-XORed by the caller.
 * @param polynomial Generator polynomial in non-reflected form.
 */
STATIC uint8 Crc_Kernel8(const uint8 *dataPtr, uint32 length, uint8 register8, uint8 polynomial)
{
    uint32 byteIndex;

    for (byteIndex = 0u; byteIndex < length; byteIndex++)
    {
        uint8 bit;

        register8 ^= dataPtr[byteIndex];
        for (bit = 0u; bit < CRC_BITS_PER_BYTE; bit++)
        {
            if ((register8 & CRC8_MSB_MASK) != 0u)
            {
                register8 = (uint8)(((uint8)(register8 << 1u)) ^ polynomial);
            }
            else
            {
                register8 = (uint8)(register8 << 1u);
            }
        }
    }
    return register8;
}

uint8 Crc_CalculateCRC8(const uint8 *dataPtr, uint32 length, uint8 startValue, boolean isFirstCall)
{
    uint8 reg;

    /* A NULL buffer with a non-zero length is a caller defect, not a CRC of zero.
     * Returning the initial value would look like a valid result, so the profile's
     * "all ones" value is returned instead, which no real payload produces. */
    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC8_INITIAL_VALUE;
    }

    /* AUTOSAR streaming contract: a continuation undoes the previous final XOR so
     * that the register resumes exactly where the last call left it. */
    reg = (isFirstCall != FALSE) ? (uint8)CRC8_INITIAL_VALUE : (uint8)(startValue ^ CRC8_FINAL_XOR);

    reg = Crc_Kernel8(dataPtr, length, reg, CRC8_POLYNOMIAL);

    return (uint8)(reg ^ CRC8_FINAL_XOR);
}

uint8 Crc_CalculateCRC8H2F(const uint8 *dataPtr, uint32 length, uint8 startValue,
                           boolean isFirstCall)
{
    uint8 reg;

    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC8H2F_INITIAL_VALUE;
    }

    reg = (isFirstCall != FALSE) ? (uint8)CRC8H2F_INITIAL_VALUE
                                 : (uint8)(startValue ^ CRC8H2F_FINAL_XOR);

    reg = Crc_Kernel8(dataPtr, length, reg, CRC8H2F_POLYNOMIAL);

    return (uint8)(reg ^ CRC8H2F_FINAL_XOR);
}

/*==================================================================================================
 *  CRC-16/CCITT-FALSE (bit-at-a-time, MSB-first, non-reflected, no final XOR)
 *================================================================================================*/

uint16 Crc_CalculateCRC16(const uint8 *dataPtr, uint32 length, uint16 startValue,
                          boolean isFirstCall)
{
    uint16 reg;
    uint32 byteIndex;

    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC16_INITIAL_VALUE;
    }

    /* No final XOR in this profile, so a continuation resumes from startValue as-is. */
    reg = (isFirstCall != FALSE) ? (uint16)CRC16_INITIAL_VALUE : startValue;

    for (byteIndex = 0u; byteIndex < length; byteIndex++)
    {
        uint8 bit;

        reg ^= (uint16)((uint16)dataPtr[byteIndex] << CRC_BITS_PER_BYTE);
        for (bit = 0u; bit < CRC_BITS_PER_BYTE; bit++)
        {
            if ((reg & CRC16_MSB_MASK) != 0u)
            {
                reg = (uint16)(((uint16)(reg << 1u)) ^ (uint16)CRC16_POLYNOMIAL);
            }
            else
            {
                reg = (uint16)(reg << 1u);
            }
        }
    }
    return reg;
}

/*==================================================================================================
 *  CRC-16/ARC (bit-at-a-time, LSB-first, reflected in and out)
 *================================================================================================*/

uint16 Crc_CalculateCRC16ARC(const uint8 *dataPtr, uint32 length, uint16 startValue,
                             boolean isFirstCall)
{
    uint16 reg;
    uint32 byteIndex;

    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC16ARC_INITIAL_VALUE;
    }

    /* Reflected form: the register is already held in reflected order throughout,
     * so both the input reflection and the output reflection cancel out and no
     * explicit bit-reversal step is needed. */
    reg = (isFirstCall != FALSE) ? (uint16)CRC16ARC_INITIAL_VALUE : startValue;

    for (byteIndex = 0u; byteIndex < length; byteIndex++)
    {
        uint8 bit;

        reg ^= (uint16)dataPtr[byteIndex];
        for (bit = 0u; bit < CRC_BITS_PER_BYTE; bit++)
        {
            if ((reg & 0x0001u) != 0u)
            {
                reg = (uint16)((uint16)(reg >> 1u) ^ (uint16)CRC16ARC_POLY_REFLECTED);
            }
            else
            {
                reg = (uint16)(reg >> 1u);
            }
        }
    }
    return reg;
}

/*==================================================================================================
 *  CRC-32 family (table-driven, reflected)
 *================================================================================================*/

/**
 * @brief Shared reflected CRC-32 kernel.
 */
STATIC uint32 Crc_Kernel32(const uint8 *dataPtr, uint32 length, uint32 register32,
                           const uint32 *table)
{
    uint32 byteIndex;

    for (byteIndex = 0u; byteIndex < length; byteIndex++)
    {
        const uint8 tableIndex = (uint8)((uint8)(register32 & 0xFFuL) ^ dataPtr[byteIndex]);
        register32 = (register32 >> CRC_BITS_PER_BYTE) ^ table[tableIndex];
    }
    return register32;
}

uint32 Crc_CalculateCRC32(const uint8 *dataPtr, uint32 length, uint32 startValue,
                          boolean isFirstCall)
{
    uint32 reg;

    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC32_INITIAL_VALUE;
    }

    if (Crc_Crc32TableValid == FALSE)
    {
        Crc_BuildTable32(Crc_Crc32Table, CRC32_POLY_REFLECTED);
        Crc_Crc32TableValid = TRUE;
    }

    reg = (isFirstCall != FALSE) ? (uint32)CRC32_INITIAL_VALUE : (startValue ^ CRC32_FINAL_XOR);

    reg = Crc_Kernel32(dataPtr, length, reg, Crc_Crc32Table);

    return reg ^ CRC32_FINAL_XOR;
}

uint32 Crc_CalculateCRC32P4(const uint8 *dataPtr, uint32 length, uint32 startValue,
                            boolean isFirstCall)
{
    uint32 reg;

    if ((dataPtr == NULL_PTR) && (length != 0u))
    {
        return CRC32P4_INITIAL_VALUE;
    }

    if (Crc_Crc32P4TableValid == FALSE)
    {
        Crc_BuildTable32(Crc_Crc32P4Table, CRC32P4_POLY_REFLECTED);
        Crc_Crc32P4TableValid = TRUE;
    }

    reg = (isFirstCall != FALSE) ? (uint32)CRC32P4_INITIAL_VALUE : (startValue ^ CRC32_FINAL_XOR);

    reg = Crc_Kernel32(dataPtr, length, reg, Crc_Crc32P4Table);

    return reg ^ CRC32_FINAL_XOR;
}

/*==================================================================================================
 *  Version information
 *================================================================================================*/

void Crc_GetVersionInfo(Std_VersionInfoType *versioninfo)
{
    if (versioninfo != NULL_PTR)
    {
        versioninfo->vendorID = CRC_VENDOR_ID;
        versioninfo->moduleID = (uint16)CRC_MODULE_ID;
        versioninfo->sw_major_version = CRC_SW_MAJOR_VERSION;
        versioninfo->sw_minor_version = CRC_SW_MINOR_VERSION;
        versioninfo->sw_patch_version = CRC_SW_PATCH_VERSION;
    }
}

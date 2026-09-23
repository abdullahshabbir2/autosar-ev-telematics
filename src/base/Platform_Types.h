/**
 * @file    Platform_Types.h
 * @brief   AUTOSAR platform-specific type definitions (SWS_Platform).
 *
 * Implements the AUTOSAR Classic Platform "Platform Types" specification for the
 * Xtensa LX6 (ESP32) target and for the host (x86-64) used by the off-target unit
 * test suite. Only fixed-width, explicitly-signed types are permitted anywhere in
 * this project; plain @c int / @c long are forbidden by
 * @ref docs/10-coding-standard.md (rule CS-TYPE-01).
 *
 * @note    CPU_TYPE is 32 for the ESP32 target. The host build keeps CPU_TYPE at
 *          32 as well so that integer promotion behaviour observed by the unit
 *          tests matches the target; @c sint64 / @c uint64 remain available but
 *          are reserved for time accumulators only.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef PLATFORM_TYPES_H
#define PLATFORM_TYPES_H

/*==================================================================================================
 *  SECTION 1 : Published information (SWS_Platform_00027)
 *================================================================================================*/

#define PLATFORM_VENDOR_ID 0xFFFEu /**< Private / unregistered vendor ID. */
#define PLATFORM_MODULE_ID 199u    /**< AUTOSAR module ID for Platform types. */

#define PLATFORM_AR_RELEASE_MAJOR_VERSION 4u
#define PLATFORM_AR_RELEASE_MINOR_VERSION 4u
#define PLATFORM_AR_RELEASE_REVISION_VERSION 0u

#define PLATFORM_SW_MAJOR_VERSION 2u
#define PLATFORM_SW_MINOR_VERSION 0u
#define PLATFORM_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  SECTION 2 : CPU / endianness abstraction (SWS_Platform_00061 .. 00045)
 *================================================================================================*/

#define CPU_TYPE_8 8u
#define CPU_TYPE_16 16u
#define CPU_TYPE_32 32u
#define CPU_TYPE_64 64u

#define MSB_FIRST 0u /**< Most significant bit is bit 0.  */
#define LSB_FIRST 1u /**< Least significant bit is bit 0. */

#define HIGH_BYTE_FIRST 0u /**< Big endian byte order.    */
#define LOW_BYTE_FIRST 1u  /**< Little endian byte order. */

/** Register width of the target CPU. */
#define CPU_TYPE CPU_TYPE_32

/** Bit order within a register. Xtensa LX6 numbers bit 0 as the LSB. */
#define CPU_BIT_ORDER LSB_FIRST

/** Byte order in memory. Both Xtensa LX6 and x86-64 are little endian. */
#define CPU_BYTE_ORDER LOW_BYTE_FIRST

/*==================================================================================================
 *  SECTION 3 : Boolean (SWS_Platform_00026, SWS_Platform_00034)
 *================================================================================================*/

#ifndef TRUE
#define TRUE 1u
#endif

#ifndef FALSE
#define FALSE 0u
#endif

/*==================================================================================================
 *  SECTION 4 : Integer and float types (SWS_Platform_00013 .. 00041)
 *================================================================================================*/

#include <stdint.h>

typedef unsigned char boolean; /**< TRUE / FALSE only. Never compared with ==TRUE. */

typedef int8_t sint8;   /**< -128            .. +127            */
typedef uint8_t uint8;  /**<  0              .. 255             */
typedef int16_t sint16; /**< -32768          .. +32767          */
typedef uint16_t uint16;/**<  0              .. 65535           */
typedef int32_t sint32; /**< -2147483648     .. +2147483647     */
typedef uint32_t uint32;/**<  0              .. 4294967295      */
typedef int64_t sint64; /**< reserved for monotonic time only   */
typedef uint64_t uint64;/**< reserved for monotonic time only   */

/* Optimised ("least"/"fast") variants, SWS_Platform_00050 .. 00057. On a 32-bit
 * core the widest natural register is used so that no masking code is emitted. */
typedef uint_least8_t uint8_least;
typedef uint_least16_t uint16_least;
typedef uint_least32_t uint32_least;
typedef int_least8_t sint8_least;
typedef int_least16_t sint16_least;
typedef int_least32_t sint32_least;

typedef float float32;  /**< IEEE-754 binary32. */
typedef double float64; /**< IEEE-754 binary64. */

/*==================================================================================================
 *  SECTION 5 : Compile-time guarantees
 *================================================================================================*/

/* A silently-wrong type width would corrupt every serialised frame in the
 * project, so the assumption is asserted rather than documented. */
#if defined(__cplusplus)
#define PLATFORM_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define PLATFORM_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
#define PLATFORM_STATIC_ASSERT(cond, msg) /* not supported by this compiler */
#endif

PLATFORM_STATIC_ASSERT(sizeof(uint8) == 1u, "uint8 must be exactly 1 byte");
PLATFORM_STATIC_ASSERT(sizeof(uint16) == 2u, "uint16 must be exactly 2 bytes");
PLATFORM_STATIC_ASSERT(sizeof(uint32) == 4u, "uint32 must be exactly 4 bytes");
PLATFORM_STATIC_ASSERT(sizeof(uint64) == 8u, "uint64 must be exactly 8 bytes");
PLATFORM_STATIC_ASSERT(sizeof(float32) == 4u, "float32 must be IEEE-754 binary32");
PLATFORM_STATIC_ASSERT(sizeof(float64) == 8u, "float64 must be IEEE-754 binary64");

#endif /* PLATFORM_TYPES_H */

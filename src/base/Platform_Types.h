/**
 * @file    Platform_Types.h
 * @brief   AUTOSAR platform-specific type definitions (SWS_Platform).
 *
 * Implements the AUTOSAR Classic Platform "Platform Types" specification for the
 * Xtensa LX6 (ESP32) target and for the host (x86-64) used by the off-target unit
 * test suite. Only fixed-width, explicitly-signed types are permitted anywhere in
 * this project; plain @c int / @c long are forbidden by
 * [10-coding-standard.md](docs/10-coding-standard.md) (rule CS-TYPE-01).
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

/**
 * @brief AUTOSAR boolean (SWS_Platform_00026). Holds ::TRUE or ::FALSE only; never compared with @c ==TRUE.
 *
 * @par The Arduino core defines this name too
 * `Arduino.h` contains `typedef bool boolean;`, so every platform leaf -- which must include both -- would
 * otherwise fail to compile on a conflicting declaration. The name cannot be changed here: AUTOSAR specifies
 * it, and the whole point of this header is to provide the specified names.
 *
 * So where `Arduino.h` has already defined it, its definition stands. That is safe, and it is worth being
 * precise about why rather than assuming it:
 *
 *  - **Layout.** `sizeof(bool) == sizeof(unsigned char) == 1` with 1-byte alignment on both Xtensa LX6 and
 *    x86-64, so a struct containing a `boolean` has identical layout whether the translation unit is C or
 *    C++. The static assertion below enforces that rather than trusting it, and will fail the build on a
 *    toolchain where it does not hold.
 *  - **Values.** Every assignment in this project uses ::TRUE or ::FALSE, which are 1 and 0. `bool`
 *    normalises any other value to 1; `unsigned char` does not. The difference is only observable for a value
 *    that is neither -- which coding-standard rule CS-BOOL-01 forbids producing, and which is why the rule
 *    exists.
 *  - **Comparisons.** `!= FALSE` is the required form throughout, and it is correct under both definitions.
 *    `== TRUE` would not be, on a C translation unit holding a value of 2; that is the other half of
 *    CS-BOOL-01.
 *
 * The consequence for a reader: do not assume a `boolean` can carry a bit pattern other than 0 or 1, and do
 * not write one.
 */
#if defined(__cplusplus) && defined(ARDUINO)
/* Arduino.h has already provided it; see above. */
#else
typedef unsigned char boolean;
#endif

typedef int8_t sint8;    /**< -128            .. +127            */
typedef uint8_t uint8;   /**<  0              .. 255             */
typedef int16_t sint16;  /**< -32768          .. +32767          */
typedef uint16_t uint16; /**<  0              .. 65535           */
typedef int32_t sint32;  /**< -2147483648     .. +2147483647     */
typedef uint32_t uint32; /**<  0              .. 4294967295      */
typedef int64_t sint64;  /**< reserved for monotonic time only   */
typedef uint64_t uint64; /**< reserved for monotonic time only   */

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

/* The one that makes the boolean accommodation above safe. A C translation unit gets `unsigned char` and a
 * C++ one with the Arduino core gets `bool`; if those ever differed in width, a structure holding a boolean
 * would have two different layouts in one program and every frame containing one would be misaligned across
 * the C/C++ boundary. Asserted in both languages, so the failure is a build error rather than a field fault. */
PLATFORM_STATIC_ASSERT(sizeof(boolean) == 1u, "boolean must be exactly 1 byte in both C and C++");

#endif /* PLATFORM_TYPES_H */

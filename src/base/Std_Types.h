/**
 * @file    Std_Types.h
 * @brief   AUTOSAR standard type definitions (SWS_StandardTypes).
 *
 * Every BSW module, ECU abstraction module and software component in this project
 * returns ::Std_ReturnType and reports its version through ::Std_VersionInfoType.
 * Nothing in the stack above the MCAL is permitted to invent its own status enum.
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef STD_TYPES_H
#define STD_TYPES_H

#include "base/Compiler.h"
#include "base/Platform_Types.h"

/*==================================================================================================
 *  Published information (SWS_Std_00014)
 *================================================================================================*/

#define STD_VENDOR_ID 0xFFFEu
#define STD_MODULE_ID 197u

#define STD_AR_RELEASE_MAJOR_VERSION 4u
#define STD_AR_RELEASE_MINOR_VERSION 4u
#define STD_AR_RELEASE_REVISION_VERSION 0u

#define STD_SW_MAJOR_VERSION 2u
#define STD_SW_MINOR_VERSION 0u
#define STD_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  Std_ReturnType (SWS_Std_00005 .. 00006)
 *================================================================================================*/

/**
 * @brief Standard API return type.
 *
 * Values 0x00 and 0x01 are reserved by AUTOSAR. Values 0x02..0x3F may be
 * allocated by a module for a more specific outcome; this project uses the range
 * as defined below so that a single value can be logged and traced unambiguously
 * across layers.
 */
typedef uint8 Std_ReturnType;

#ifndef STATUS_TYPE_DEFINED
#define E_OK ((Std_ReturnType)0x00u)     /**< Operation succeeded.                      */
#define E_NOT_OK ((Std_ReturnType)0x01u) /**< Operation failed, no further detail.       */
#endif

#define E_PENDING ((Std_ReturnType)0x02u)      /**< Accepted; result available later.    */
#define E_BUSY ((Std_ReturnType)0x03u)         /**< Resource is in use, retry later.     */
#define E_TIMEOUT ((Std_ReturnType)0x04u)      /**< Peer did not answer within deadline. */
#define E_CRC_FAIL ((Std_ReturnType)0x05u)     /**< Integrity check mismatch.            */
#define E_NOT_FOUND ((Std_ReturnType)0x06u)    /**< Requested object does not exist.     */
#define E_NO_SPACE ((Std_ReturnType)0x07u)     /**< Destination is full.                 */
#define E_INVALID_PARAM ((Std_ReturnType)0x08u)/**< Caller passed an out-of-range value. */
#define E_NOT_INITIALISED ((Std_ReturnType)0x09u) /**< Module used before its Init().    */
#define E_UNSUPPORTED ((Std_ReturnType)0x0Au)  /**< Feature absent in this build.        */

/*==================================================================================================
 *  Std_VersionInfoType (SWS_Std_00015)
 *================================================================================================*/

/** Version information returned by every module's `<Mip>_GetVersionInfo()`. */
typedef struct
{
    uint16 vendorID;         /**< Vendor identifier.               */
    uint16 moduleID;         /**< AUTOSAR module identifier.       */
    uint8 sw_major_version;  /**< Implementation major version.    */
    uint8 sw_minor_version;  /**< Implementation minor version.    */
    uint8 sw_patch_version;  /**< Implementation patch version.    */
} Std_VersionInfoType;

/*==================================================================================================
 *  Symbol definitions (SWS_Std_00007 .. 00013)
 *================================================================================================*/

#ifndef STD_HIGH
#define STD_HIGH 0x01u /**< Physical state 5V or 3.3V. */
#define STD_LOW 0x00u  /**< Physical state 0V.         */
#endif

#ifndef STD_ACTIVE
#define STD_ACTIVE 0x01u /**< Logical state active.   */
#define STD_IDLE 0x00u   /**< Logical state idle.     */
#endif

#ifndef STD_ON
#define STD_ON 0x01u  /**< Compile-time feature enabled.  */
#define STD_OFF 0x00u /**< Compile-time feature disabled. */
#endif

/** Message type for a transformer error (SWS_Std_91001). Unused, kept for completeness. */
#define STD_MESSAGETYPE_REQUEST 0x00u
#define STD_MESSAGETYPE_RESPONSE 0x01u

#define STD_MESSAGERESULT_OK 0x00u
#define STD_MESSAGERESULT_ERROR 0x01u

/*==================================================================================================
 *  Project-wide helper macros
 *================================================================================================*/

/**
 * @brief Number of elements in a statically-sized array.
 *
 * Deliberately not applied to pointers: the division would silently yield a wrong
 * count. Call sites are checked by the STD_ARRAY_SIZE cppcheck rule.
 */
#define STD_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/** Clamp @p v into the inclusive range [@p lo, @p hi]. */
#define STD_CLAMP(v, lo, hi) (((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))

/** Smaller of two values. Arguments must be side-effect free. */
#define STD_MIN(a, b) (((a) < (b)) ? (a) : (b))

/** Larger of two values. Arguments must be side-effect free. */
#define STD_MAX(a, b) (((a) > (b)) ? (a) : (b))

/**
 * @brief Deliberately discard the result of a ::CHECK_RETURN function.
 *
 * A plain @c (void) cast does not satisfy GCC's @c warn_unused_result -- by design,
 * since casting away a status is exactly the mistake the attribute exists to catch.
 * Binding the value to a named object does satisfy it, so this macro makes an
 * intentional discard both possible and conspicuous: a reviewer grepping for
 * STD_DISCARD sees every place a status was dropped on purpose.
 *
 * Legitimate uses are narrow, and all of them are cleanup paths where the caller has
 * already decided to fail and has nothing left to do with a second error -- releasing a
 * bus lock while unwinding, for instance. Anywhere else, propagate the status.
 */
#define STD_DISCARD(expr)                        \
    do                                           \
    {                                            \
        const Std_ReturnType stdDiscarded_ = (expr); \
        (void)stdDiscarded_;                     \
    } while (0)

#endif /* STD_TYPES_H */

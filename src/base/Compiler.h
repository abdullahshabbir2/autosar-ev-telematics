/**
 * @file    Compiler.h
 * @brief   AUTOSAR compiler abstraction (SWS_COMPILER).
 *
 * Provides the keyword abstractions every BSW module uses in its declarations so
 * that the source is free of compiler-specific extensions. Targets supported:
 *   - Xtensa GCC (ESP32, xtensa-esp32-elf-gcc)
 *   - Host GCC / Clang / MSVC (off-target unit tests)
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef COMPILER_H
#define COMPILER_H

#include "base/Compiler_Cfg.h"

/*==================================================================================================
 *  Published information
 *================================================================================================*/

#define COMPILER_VENDOR_ID 0xFFFEu
#define COMPILER_MODULE_ID 198u

#define COMPILER_AR_RELEASE_MAJOR_VERSION 4u
#define COMPILER_AR_RELEASE_MINOR_VERSION 4u
#define COMPILER_AR_RELEASE_REVISION_VERSION 0u

/*==================================================================================================
 *  Toolchain identification
 *================================================================================================*/

#if defined(__XTENSA__)
#define _GCC_C_XTENSA_ /**< Xtensa GCC, on-target build. */
#elif defined(_MSC_VER)
#define _MSVC_C_X86_64_ /**< MSVC, host build. */
#elif defined(__clang__)
#define _CLANG_C_HOST_ /**< Clang, host build. */
#elif defined(__GNUC__)
#define _GCC_C_HOST_ /**< GCC, host build. */
#else
#error "Compiler.h: unsupported toolchain. Add an abstraction block before building."
#endif

/*==================================================================================================
 *  Storage class and pointer qualifier abstractions (SWS_COMPILER_00001 .. 00007)
 *================================================================================================*/

/** Abstraction of the @c static keyword for module-local objects and functions. */
#define STATIC static

/** Abstraction of the @c inline keyword. */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L))
#define INLINE inline
#elif defined(_MSC_VER)
#define INLINE __inline
#else
#define INLINE
#endif

/**
 * @brief Abstraction of the @c NULL pointer constant (SWS_COMPILER_00051).
 *
 * `(void *)0` in C, `nullptr` in C++. The distinction is not cosmetic: C++ refuses to convert `void *` to a
 * function pointer, so the C form cannot initialise a callback member -- which every platform leaf that
 * registers a callback has. `nullptr` converts to any pointer type, including a function pointer, and is also
 * what lets a comparison against an overloaded type (an Arduino `File`, say) resolve unambiguously.
 *
 * Both forms compare equal to a null pointer of any type, so no call site has to know which it got.
 */
#ifndef NULL_PTR
#ifdef __cplusplus
#define NULL_PTR nullptr
#else
#define NULL_PTR ((void *)0)
#endif
#endif

/**
 * @brief Declares a function that never returns.
 *
 * Used by Det_Panic() and Mcu_PerformReset() so the optimiser does not emit dead
 * epilogue code and so static analysis stops tracking paths past the call.
 */
#if defined(_GCC_C_XTENSA_) || defined(_GCC_C_HOST_) || defined(_CLANG_C_HOST_)
#define NORETURN __attribute__((noreturn))
#elif defined(_MSVC_C_X86_64_)
#define NORETURN __declspec(noreturn)
#else
#define NORETURN
#endif

/** Marks a parameter or object as deliberately unused (silences MISRA 2.7). */
#define COMPILER_UNUSED(x) ((void)(x))

/**
 * @brief Marks a function result that the caller must not discard.
 *
 * Applied to every BSW entry point returning Std_ReturnType: dropping an
 * E_NOT_OK on an NvM write or a Crc verification is the single most common class
 * of latent defect in code of this kind, so the compiler is made to reject it.
 */
#if defined(_GCC_C_XTENSA_) || defined(_GCC_C_HOST_) || defined(_CLANG_C_HOST_)
#define CHECK_RETURN __attribute__((warn_unused_result))
#else
#define CHECK_RETURN
#endif

/** Requests that a function always be expanded inline (hot ISR-side helpers). */
#if defined(_GCC_C_XTENSA_) || defined(_GCC_C_HOST_) || defined(_CLANG_C_HOST_)
#define FORCE_INLINE INLINE __attribute__((always_inline))
#else
#define FORCE_INLINE INLINE
#endif

/** printf-style format checking for the logging front end. */
#if defined(_GCC_C_XTENSA_) || defined(_GCC_C_HOST_) || defined(_CLANG_C_HOST_)
#define PRINTF_LIKE(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#define PRINTF_LIKE(fmtIdx, argIdx)
#endif

/*==================================================================================================
 *  Memory and pointer class abstractions
 *
 *  AUTOSAR requires each module to qualify its declarations with a memory class so
 *  that a linker script can place code and data in specific sections. On this
 *  single-address-space target every class expands to nothing, but the qualifiers
 *  are kept in the source: they document intent, and they are the hook a future
 *  port to a memory-protected MCU needs.
 *================================================================================================*/

#define AUTOMATIC       /**< Object with automatic (stack) storage duration. */
#define TYPEDEF         /**< Used in typedefs of pointer types.             */
#define _STATIC_ static /**< AUTOSAR spelling of module-local storage.       */

#define FUNC(rettype, memclass) rettype
#define FUNC_P2CONST(rettype, ptrclass, memclass) const rettype *
#define FUNC_P2VAR(rettype, ptrclass, memclass) rettype *

#define P2VAR(ptrtype, memclass, ptrclass) ptrtype *
#define P2CONST(ptrtype, memclass, ptrclass) const ptrtype *
#define CONSTP2VAR(ptrtype, memclass, ptrclass) ptrtype *const
#define CONSTP2CONST(ptrtype, memclass, ptrclass) const ptrtype *const
#define P2FUNC(rettype, ptrclass, fctname) rettype(*fctname)
#define CONSTP2FUNC(rettype, ptrclass, fctname) rettype(*const fctname)

#define CONST(consttype, memclass) const consttype
#define VAR(vartype, memclass) vartype

#endif /* COMPILER_H */

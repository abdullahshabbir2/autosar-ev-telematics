/**
 * @file    Det.h
 * @brief   AUTOSAR Default Error Tracer (SWS_DET).
 *
 * Det is the single sink for "this call violated my contract" reports from every
 * layer. It exists so that a contract violation is *recorded* rather than silently
 * absorbed by a defensive @c if, which is what makes an integration fault
 * diagnosable in the field instead of only under a debugger.
 *
 * @par Three severities, three fates
 *  - ::Det_ReportError -- a caller passed something illegal (NULL out-parameter,
 *    out-of-range index, API used before Init). The callee has already rejected the
 *    call; Det records it. In a production build the report is counted and the most
 *    recent entries are kept in a RAM ring so ::Det_GetHistory can publish them
 *    over the diagnostic channel.
 *  - ::Det_ReportRuntimeError -- an operational failure that is expected to happen
 *    occasionally (a CRC mismatch on a noisy bus, a socket refusing a connection).
 *    Never fatal. Counted separately so that a rising rate is visible as a trend.
 *  - ::Det_ReportTransientFault -- a fault the caller has already recovered from,
 *    recorded only for statistics.
 *
 * @par Why development-error detection stays enabled in production here
 * AUTOSAR expects @c DetEnableDevErrorDetect to be switched off for series
 * production. This ECU keeps it on (see [ADR-0004](docs/adr/0004-keep-det-enabled-in-production.md)):
 * the units are field-deployed on vehicles with no debug access, the checks cost
 * ~1.4 KiB of flash and a handful of cycles on paths that are not
 * throughput-critical, and a report reaching the cloud is the only realistic way a
 * latent integration defect gets noticed. What *is* disabled in production is the
 * halt-on-error behaviour: ::DET_HALT_ON_ERROR is only set for the unit-test and
 * bench builds, where stopping at the first violation is what makes the cause
 * obvious.
 *
 * @req SWREQ-DIAG-0001, SWREQ-DIAG-0002, SWREQ-DIAG-0003
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef DET_H
#define DET_H

#include "base/Autosar_ModuleIds.h"
#include "services/Det/Det_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *  Published information
 *================================================================================================*/

#define DET_VENDOR_ID 0xFFFEu
#define DET_MODULE_ID MODULE_ID_DET

#define DET_AR_RELEASE_MAJOR_VERSION 4u
#define DET_AR_RELEASE_MINOR_VERSION 4u
#define DET_AR_RELEASE_REVISION_VERSION 0u

#define DET_SW_MAJOR_VERSION 2u
#define DET_SW_MINOR_VERSION 0u
#define DET_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Classification of a Det report. */
typedef enum
{
    DET_SEVERITY_DEV = 0,      /**< API contract violation by the caller.     */
    DET_SEVERITY_RUNTIME = 1,  /**< Expected operational failure.             */
    DET_SEVERITY_TRANSIENT = 2 /**< Fault already recovered; statistics only. */
} Det_SeverityType;

/** One captured report. Packed to 12 bytes so the ring stays cheap. */
typedef struct
{
    uint32 timestamp;   /**< Milliseconds since boot (Gpt_GetMonotonicMs()). */
    uint16 moduleId;    /**< Reporting module, see Autosar_ModuleIds.h.      */
    uint8 instanceId;   /**< Which instance of that module.                  */
    uint8 apiId;        /**< Which API of that module.                       */
    uint8 errorId;      /**< Module-specific error code.                     */
    uint8 severity;     /**< ::Det_SeverityType.                             */
    uint16 occurrences; /**< Times this exact triple has been reported.      */
} Det_EntryType;

/** Aggregate counters, published in the telemetry health record. */
typedef struct
{
    uint32 devErrorCount;       /**< Total DET_SEVERITY_DEV reports.       */
    uint32 runtimeErrorCount;   /**< Total DET_SEVERITY_RUNTIME reports.   */
    uint32 transientFaultCount; /**< Total DET_SEVERITY_TRANSIENT reports. */
    uint16 distinctEntryCount;  /**< Distinct triples currently held.      */
    uint16 ringOverflowCount;   /**< Times a new triple evicted an old one.*/
} Det_StatisticsType;

/**
 * @brief Optional hook invoked on every report, before any halt.
 *
 * Set by EcuM so that Log can print the report and Dem can promote selected
 * development errors to DTCs. Kept as a function pointer rather than a direct call
 * so that Det has no dependency on Log or Dem and can therefore be unit-tested on
 * its own.
 */
typedef void (*Det_NotificationFctType)(const Det_EntryType *entry);

/*==================================================================================================
 *  API
 *================================================================================================*/

/**
 * @brief Initialise Det. Clears the ring and all counters.
 *
 * Must run before any other module's Init(), because those Inits may themselves
 * report. Reports arriving before this call are counted in a pre-init tally that
 * ::Det_GetStatistics adds in, so an early violation is not lost.
 */
void Det_Init(void);

/**
 * @brief Register the notification hook. Pass NULL_PTR to remove it.
 */
void Det_SetNotificationHook(Det_NotificationFctType hook);

/**
 * @brief Report a development error (API contract violation).
 *
 * @param moduleId   Reporting module ID.
 * @param instanceId Instance within the module, or ::INSTANCE_ID_SINGLE.
 * @param apiId      API service ID within the module.
 * @param errorId    Module-specific error code.
 * @return Always E_OK. AUTOSAR fixes this signature; the value carries no meaning
 *         and exists only so the call can sit inside a comma expression.
 *
 * @note Reentrant and ISR-safe.
 */
Std_ReturnType Det_ReportError(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId);

/**
 * @brief Report an operational failure that is not a caller defect.
 * @copydetails Det_ReportError
 */
Std_ReturnType Det_ReportRuntimeError(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 errorId);

/**
 * @brief Report a fault from which the caller has already recovered.
 *
 * Recorded for statistics only: by the time this is called the condition is over, so there is nothing for
 * a caller to act on. The value is in the count -- a bus that recovers from a fault fifty times an hour is
 * failing even though every individual exchange succeeded.
 *
 * Documented in full rather than copied from ::Det_ReportError. The copy named a parameter `errorId` where
 * this function's is `faultId`, so the documentation told a reader to pass something that does not exist.
 *
 * @param moduleId   Reporting module, see Autosar_ModuleIds.h.
 * @param instanceId Which instance of that module.
 * @param apiId      Which API of that module.
 * @param faultId    Module-specific fault code.
 * @return E_OK if the report was recorded.
 */
Std_ReturnType Det_ReportTransientFault(uint16 moduleId, uint8 instanceId, uint8 apiId, uint8 faultId);

/**
 * @brief Read the aggregate counters.
 * @param[out] stats Destination. Ignored if NULL_PTR.
 */
void Det_GetStatistics(Det_StatisticsType *stats);

/**
 * @brief Read captured reports, most recent first.
 *
 * @param[out] buffer   Destination array.
 * @param[in]  maxCount Capacity of @p buffer in entries.
 * @return Number of entries written, 0 if @p buffer is NULL_PTR or @p maxCount is 0.
 */
uint16 Det_GetHistory(Det_EntryType *buffer, uint16 maxCount);

/**
 * @brief Discard all captured reports and zero all counters.
 *
 * Reachable from the diagnostic channel (service ClearDiagnosticInformation) so
 * that a technician can confirm a repair produced a clean run.
 */
void Det_ClearHistory(void);

/**
 * @brief Report an unrecoverable condition and stop the ECU.
 *
 * Used where continuing would be worse than resetting: a corrupt configuration
 * set, a failed static assertion about layout, a stack canary breach. Records the
 * report, gives Log one chance to flush, then asks Mcu for a reset. Never returns.
 *
 * @param moduleId Reporting module.
 * @param apiId    Reporting API.
 * @param errorId  Module-specific error code.
 */
NORETURN void Det_Panic(uint16 moduleId, uint8 apiId, uint8 errorId);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Det_GetVersionInfo(Std_VersionInfoType *versioninfo);

/*==================================================================================================
 *  Convenience macros
 *
 *  Used by every module so that a parameter check reads as one line and compiles to
 *  nothing when development error detection is configured off.
 *================================================================================================*/

#if (DET_ENABLE_DEV_ERROR_DETECT == STD_ON)

/**
 * @brief If @p cond is false, report @p errId and return @p retval from the caller.
 *
 * The @c do/while(0) wrapper keeps the macro usable as a single statement in an
 * unbraced @c if, which MISRA would otherwise flag.
 */
#define DET_CHECK_RETURN(cond, modId, instId, apiId, errId, retval)     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            (void)Det_ReportError((modId), (instId), (apiId), (errId)); \
            return (retval);                                            \
        }                                                               \
    } while (0)

/** As ::DET_CHECK_RETURN but for a @c void function. */
#define DET_CHECK_RETURN_VOID(cond, modId, instId, apiId, errId)        \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            (void)Det_ReportError((modId), (instId), (apiId), (errId)); \
            return;                                                     \
        }                                                               \
    } while (0)

#else /* development error detection disabled */

#define DET_CHECK_RETURN(cond, modId, instId, apiId, errId, retval) \
    do                                                              \
    {                                                               \
        if (!(cond))                                                \
        {                                                           \
            return (retval);                                        \
        }                                                           \
    } while (0)

#define DET_CHECK_RETURN_VOID(cond, modId, instId, apiId, errId) \
    do                                                           \
    {                                                            \
        if (!(cond))                                             \
        {                                                        \
            return;                                              \
        }                                                        \
    } while (0)

#endif /* DET_ENABLE_DEV_ERROR_DETECT */

#ifdef __cplusplus
}
#endif

#endif /* DET_H */

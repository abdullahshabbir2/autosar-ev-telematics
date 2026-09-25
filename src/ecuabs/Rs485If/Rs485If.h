/**
 * @file    Rs485If.h
 * @brief   RS485 battery-pack bus transport and protocol handler.
 *
 * Talks to up to four battery packs over a shared half-duplex RS485 bus at 4800 baud,
 * 8E1. Sits in the ECU Abstraction Layer: it owns the wire protocol and the transceiver
 * direction line, and hands decoded, validated measurements upward as plain structs.
 * Nothing above this module knows the frame layout, and nothing in this module knows what
 * the values are used for.
 *
 * @par Protocol summary
 * Strict request/response, master-initiated, no unsolicited traffic. Every frame is
 * @c 0xFF, a direction byte, a 16-bit length, a type byte, a 32-bit pack serial number,
 * a command, a payload, and a CRC-16/CCITT-FALSE transmitted most-significant byte first.
 * The full byte-level specification is in [08-protocols.md.](docs/08-protocols.md.)
 *
 * @par Four defects in the v1 implementation that this interface prevents
 *
 * 1. **Stack buffer overflow on two frame types.** @c switchOnOffBatteries and
 *    @c sendAuthenticationSeed declared @c byte requestPacket[] with fifteen
 *    initialisers -- so a fifteen-byte array -- then called
 *    @c appendCRC16(requestPacket, 15), which writes indices 15 and 16, and finally
 *    @c sendPacket(requestPacket, 17), which reads them back. The CRC was written to and
 *    read from the same two bytes past the end of the array, which is why it appeared to
 *    work: the value transmitted was usually correct, and the corruption landed on
 *    whatever the compiler happened to place next on the stack. Any change to register
 *    allocation, optimisation level or an added local turns it into a wrong CRC or a
 *    corrupted neighbouring variable. Here every frame is built in a buffer whose size is
 *    derived from the frame definition and checked with a static assertion.
 *
 * 2. **Receive buffer never purged before a request.** v1 called @c serial.flush(),
 *    which drains the *transmit* buffer. A late reply from the previous pack therefore
 *    sat in the receive FIFO and was parsed as the current pack's reply -- and because
 *    the protocol echoes the serial number, that mis-attribution was detectable but never
 *    checked. ::Rs485If_ReadBatteryParameters purges with ::Uart_DiscardRx and validates
 *    the echoed serial number.
 *
 * 3. **Driver-enable dropped before the line was idle.** v1 deasserted nothing at all --
 *    it had no direction control in software -- but the equivalent hazard is released by
 *    ::Uart_DrainTx here, because @c write() returns when bytes are buffered, not sent.
 *
 * 4. **Misplaced casts in the field decoders.** v1 wrote
 *    @c (int32_t)(buffer[n] << 24) | (buffer[n+1] << 16) | ..., where the cast applies
 *    only to the first term and @c buffer[n] << 24 is undefined once the top bit is set.
 *    Fields are assembled here as unsigned and reinterpreted once, at the end, which is
 *    defined behaviour for every input.
 *
 * @req SWREQ-BAT-0001 .. SWREQ-BAT-0020
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#ifndef RS485IF_H
#define RS485IF_H

#include "base/Autosar_ModuleIds.h"
#include "ecuabs/Rs485If/Rs485If_Cfg.h"
#include "base/Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485IF_VENDOR_ID 0xFFFEu
#define RS485IF_SW_MAJOR_VERSION 2u
#define RS485IF_SW_MINOR_VERSION 0u
#define RS485IF_SW_PATCH_VERSION 0u

/*==================================================================================================
 *  API service IDs
 *================================================================================================*/

#define RS485IF_API_ID_INIT 0x00u
#define RS485IF_API_ID_READ_SERIAL 0x20u
#define RS485IF_API_ID_READ_BATTERY 0x21u
#define RS485IF_API_ID_READ_CELLS 0x22u
#define RS485IF_API_ID_SWITCH_PACKS 0x23u
#define RS485IF_API_ID_DISCOVER 0x24u
#define RS485IF_API_ID_BUILD_FRAME 0x25u
#define RS485IF_API_ID_PARSE_FRAME 0x26u

/*==================================================================================================
 *  Development and runtime error codes
 *================================================================================================*/

#define RS485IF_E_UNINIT E_UNINIT
#define RS485IF_E_PARAM_POINTER E_PARAM_POINTER
#define RS485IF_E_PARAM_SLOT 0x20u      /**< Pack slot outside 1 .. 4.               */
#define RS485IF_E_NO_RESPONSE 0x21u     /**< Nothing arrived within the deadline.     */
#define RS485IF_E_SHORT_RESPONSE 0x22u  /**< Fewer bytes than the frame requires.     */
#define RS485IF_E_CRC_MISMATCH 0x23u    /**< Frame check failed.                      */
#define RS485IF_E_BAD_HEADER 0x24u      /**< Start byte or direction byte wrong.      */
#define RS485IF_E_BAD_LENGTH 0x25u      /**< Declared length disagrees with the type. */
#define RS485IF_E_SERIAL_MISMATCH 0x26u /**< Reply echoed a different pack's serial.  */
#define RS485IF_E_STALE_RX 0x27u        /**< Bytes were pending before a request.     */
#define RS485IF_E_TX_FAILED 0x28u       /**< The UART refused the request.            */

/*==================================================================================================
 *  Wire-format constants
 *================================================================================================*/

#define RS485IF_START_BYTE 0xFFu   /**< First byte of every frame.            */
#define RS485IF_DIR_REQUEST 0x00u  /**< Second byte, master to pack.          */
#define RS485IF_DIR_RESPONSE 0x55u /**< Second byte, pack to master.          */

#define RS485IF_TYPE_ADDRESSED 0x01u /**< Frame carries a pack serial number.   */
#define RS485IF_TYPE_BROADCAST 0x03u /**< Frame addresses the pack controller.  */

#define RS485IF_CMD_SERIAL_NUMBER 0x00u  /**< Read the pack serial number.        */
#define RS485IF_CMD_BATTERY_PARAMS 0x01u /**< Read pack-level measurements.       */
#define RS485IF_CMD_CELL_PARAMS 0x05u    /**< Read per-cell measurements.         */
#define RS485IF_CMD_AUTH_SEED 0x08u      /**< Begin the authentication exchange.  */

/** Bytes before the payload: start, direction, length(2), type, serial(4), command(4). */
#define RS485IF_PAYLOAD_OFFSET 13u

/** Bytes occupied by the frame check. */
#define RS485IF_CRC_SIZE 2u

/** Total length of a request that carries no payload. */
#define RS485IF_REQUEST_FRAME_SIZE 15u

/** Total length of a request that carries a two-byte payload (switch, auth seed). */
#define RS485IF_REQUEST_FRAME_SIZE_EXT 17u

/** Total length of a serial-number response. */
#define RS485IF_SERIAL_RESPONSE_SIZE 15u

/** Total length of a pack-parameter (BATT0100) response. */
#define RS485IF_BATTERY_RESPONSE_SIZE 53u

/** Total length of a cell-parameter (BATT0500) response. */
#define RS485IF_CELL_RESPONSE_SIZE 81u

/** Longest frame in either direction; sizes every buffer in this module. */
#define RS485IF_MAX_FRAME_SIZE RS485IF_CELL_RESPONSE_SIZE

/** Cells reported per pack. */
#define RS485IF_CELLS_PER_PACK 23u

/** Temperature sensors reported per pack. */
#define RS485IF_TEMPS_PER_PACK 4u

/*==================================================================================================
 *  Types
 *================================================================================================*/

/** Pack slot, 1 .. ::RS485IF_PACK_COUNT. Slot 0 is not a pack. */
typedef uint8 Rs485If_SlotType;

/**
 * @brief Pack-level measurements (protocol object BATT0100).
 *
 * Fields hold the pack's raw integers exactly as transmitted. No scaling is applied here
 * and none should be: the divisor is a property of the BMS firmware revision, and
 * converting to engineering units at this layer would discard the exact value the pack
 * reported, making a later correction to the scaling impossible to apply retrospectively
 * to already-logged data.
 *
 * @warning The physical scaling of these fields is **not confirmed**. The v1 firmware
 *          applied none, and its protocol notes were second-hand; the only samples
 *          available are its hand-authored simulation fixtures, whose values are
 *          consistent with a centi-unit convention (voltage 7000 -> 70.00 V, current
 *          4300 -> 43.00 A, temperature 3500 -> 35.00 degC) but which prove nothing about
 *          the real device. Scaling is applied in exactly one place -- BattSwc's
 *          conversion table -- and is flagged as an open item in
 *          [08-protocols.md.](docs/08-protocols.md.) It must be confirmed against the BMS datasheet or a
 *          bus capture before any engineering-unit value is relied upon. Until then the
 *          raw integers are the authoritative record, which is why they are what gets
 *          logged.
 */
typedef struct
{
    uint16 voltage;           /**< Pack voltage, raw.                           */
    uint16 voltageHighest;    /**< Highest cell voltage, raw.                    */
    uint16 voltageLowest;     /**< Lowest cell voltage, raw.                     */
    sint32 current;           /**< Pack current, raw. Sign convention unconfirmed.*/
    sint16 temperature;       /**< Pack temperature, raw.                        */
    sint16 temperatureHigh;   /**< Highest sensor, raw.                          */
    sint16 temperatureLow;    /**< Lowest sensor, raw.                           */
    uint8 stateOfCharge;      /**< State of charge, percent (0 .. 100).          */
    uint8 stateOfHealth;      /**< State of health, percent (0 .. 100).          */
    uint32 chargeEnergyWh;    /**< Lifetime charge throughput, raw.              */
    uint32 dischargeEnergyWh; /**< Lifetime discharge throughput, raw.           */
    uint32 chargeTimeSec;     /**< Lifetime charging time, raw.                  */
    uint32 dischargeTimeSec;  /**< Lifetime discharging time, raw.               */
    uint32 statusFlags;       /**< BATT0201 status and fault bits.               */
} Rs485If_PackDataType;

/**
 * @brief Per-cell measurements (protocol object BATT0500).
 * @copydetails Rs485If_PackDataType
 */
typedef struct
{
    uint16 cellVoltage[RS485IF_CELLS_PER_PACK]; /**< Cell voltages, raw.         */
    sint32 current;                             /**< Pack current, raw.          */
    sint16 temperature[RS485IF_TEMPS_PER_PACK]; /**< Sensors, raw.               */
    uint32 statusFlags1;                        /**< BATT0202 word 1.            */
    uint32 statusFlags2;                        /**< BATT0202 word 2.            */
} Rs485If_CellDataType;

/** Everything known about one pack after a polling round. */
typedef struct
{
    uint32 serialNumber;        /**< 0 if the pack has not been discovered.      */
    Rs485If_PackDataType pack;  /**< Valid only while @c packDataValid is TRUE.   */
    Rs485If_CellDataType cells; /**< Valid only while @c cellDataValid is TRUE.   */
    boolean present;            /**< The pack answered a serial-number request.  */
    boolean packDataValid;      /**< The last BATT0100 read succeeded.           */
    boolean cellDataValid;      /**< The last BATT0500 read succeeded.           */
    uint16 consecutiveFailures; /**< Reads failed in a row; drives the DTC.      */
    uint32 totalRequests;       /**< Requests issued to this pack.               */
    uint32 totalFailures;       /**< Requests that did not yield a valid frame.  */
} Rs485If_PackStateType;

/** Bus-level counters, published as diagnostic data. */
typedef struct
{
    uint32 framesSent;       /**< Requests transmitted.                        */
    uint32 framesReceived;   /**< Well-formed responses accepted.               */
    uint32 crcFailures;      /**< Responses rejected by the frame check.        */
    uint32 timeouts;         /**< Requests that drew no reply at all.           */
    uint32 shortFrames;      /**< Replies that stopped before the frame ended.  */
    uint32 headerFailures;   /**< Replies with a wrong start or direction byte. */
    uint32 serialMismatches; /**< Replies attributed to the wrong pack.         */
    uint32 staleRxBytes;     /**< Bytes purged before a request was issued.     */
} Rs485If_StatisticsType;

/*==================================================================================================
 *  API -- bus lifecycle
 *================================================================================================*/

/**
 * @brief Open the bus and put the transceiver into receive.
 * @return E_OK on success; E_NOT_OK if the UART could not be opened.
 */
CHECK_RETURN Std_ReturnType Rs485If_Init(void);

/** Close the bus and release the UART. */
void Rs485If_DeInit(void);

/*==================================================================================================
 *  API -- frame codec
 *
 *  Exposed so the codec can be tested against captured frames without a bus. These are
 *  pure functions: no I/O, no module state.
 *================================================================================================*/

/**
 * @brief Build an addressed request frame.
 *
 * @param[out] frame        Destination, at least ::RS485IF_REQUEST_FRAME_SIZE bytes.
 * @param[in]  frameSize    Capacity of @p frame.
 * @param[in]  serialNumber Pack serial number, or 0 to broadcast.
 * @param[in]  command      One of the RS485IF_CMD_* values.
 * @return E_OK on success; E_NOT_OK if @p frame is NULL_PTR or too small. A short buffer
 *         is rejected rather than truncated -- this is the exact hazard that made the v1
 *         frame builders overflow their arrays.
 */
CHECK_RETURN Std_ReturnType Rs485If_BuildRequest(uint8 *frame, uint8 frameSize, uint32 serialNumber,
                                                 uint8 command);

/**
 * @brief Build a request frame carrying a two-byte payload.
 *
 * @param[out] frame     Destination, at least ::RS485IF_REQUEST_FRAME_SIZE_EXT bytes.
 * @param[in]  frameSize Capacity of @p frame.
 * @param[in]  serialNumber Pack serial number, or 0 to broadcast.
 * @param[in]  command   One of the RS485IF_CMD_* values.
 * @param[in]  payloadHigh First payload byte.
 * @param[in]  payloadLow  Second payload byte.
 * @return E_OK on success; E_NOT_OK if @p frame is NULL_PTR or too small.
 */
CHECK_RETURN Std_ReturnType Rs485If_BuildRequestExt(uint8 *frame, uint8 frameSize, uint32 serialNumber,
                                                    uint8 command, uint8 payloadHigh, uint8 payloadLow);

/**
 * @brief Validate a received frame's header, declared length and CRC.
 *
 * Checks, in order: the start byte, the response direction byte, that the declared length
 * equals @p expectedSize, and the CRC. Validating the header before the CRC means a frame
 * from a foreign device on a shared bus is rejected as a header error rather than
 * inflating the CRC-failure counter, which is what a bus-quality assessment relies on.
 *
 * @param[in] frame        Received bytes.
 * @param[in] frameSize    Bytes actually received.
 * @param[in] expectedSize Length the frame type requires.
 * @return E_OK if the frame is well-formed; ::E_CRC_FAIL, E_NOT_OK or E_INVALID_PARAM
 *         with a Det runtime report identifying which check failed.
 */
CHECK_RETURN Std_ReturnType Rs485If_ValidateResponse(const uint8 *frame, uint8 frameSize, uint8 expectedSize);

/**
 * @brief Read the pack serial number echoed in a response frame.
 * @return The serial number, or 0 if @p frame is NULL_PTR.
 */
uint32 Rs485If_ParseSerialNumber(const uint8 *frame);

/**
 * @brief Decode a BATT0100 payload.
 * @param[in]  frame Validated response of ::RS485IF_BATTERY_RESPONSE_SIZE bytes.
 * @param[out] data  Destination.
 * @return E_OK on success; E_NOT_OK if either pointer is NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Rs485If_ParsePackData(const uint8 *frame, Rs485If_PackDataType *data);

/**
 * @brief Decode a BATT0500 payload.
 * @param[in]  frame Validated response of ::RS485IF_CELL_RESPONSE_SIZE bytes.
 * @param[out] data  Destination.
 * @return E_OK on success; E_NOT_OK if either pointer is NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Rs485If_ParseCellData(const uint8 *frame, Rs485If_CellDataType *data);

/*==================================================================================================
 *  API -- transactions
 *================================================================================================*/

/**
 * @brief Enable or disable packs by bit mask.
 *
 * @param maskHigh Slots 9 .. 16, unused on this vehicle and always 0.
 * @param maskLow  Bit 0 enables slot 1, bit 1 slot 2, and so on.
 * @return E_OK if the controller acknowledged; E_TIMEOUT or E_NOT_OK otherwise.
 */
CHECK_RETURN Std_ReturnType Rs485If_SwitchPacks(uint8 maskHigh, uint8 maskLow);

/**
 * @brief Discover which slots are populated and learn their serial numbers.
 *
 * Enables one slot at a time, because the packs share the bus and answer an unaddressed
 * serial-number request simultaneously otherwise. Costs roughly
 * ::RS485IF_PACK_COUNT x (2 x ::RS485IF_SWITCH_SETTLE_MS) and so runs once, during
 * startup, with the watchdog in its slow mode.
 *
 * @return E_OK if at least one pack answered; E_NOT_FOUND if none did.
 */
CHECK_RETURN Std_ReturnType Rs485If_DiscoverPacks(void);

/**
 * @brief Read pack-level measurements from @p slot into its cached state.
 * @return E_OK on success; a specific failure code otherwise. On failure the cached
 *         @c packDataValid is cleared so a stale reading cannot be published as current.
 */
CHECK_RETURN Std_ReturnType Rs485If_ReadPackData(Rs485If_SlotType slot);

/**
 * @brief Read per-cell measurements from @p slot into its cached state.
 * @copydetails Rs485If_ReadPackData
 */
CHECK_RETURN Std_ReturnType Rs485If_ReadCellData(Rs485If_SlotType slot);

/**
 * @brief Poll every present pack once, for both objects.
 *
 * @return E_OK if every present pack answered both requests; E_NOT_OK if any did not. A
 *         partial round is not rolled back: the packs that did answer keep their fresh
 *         data, because three good packs and one silent one is far more useful than four
 *         discarded readings.
 */
CHECK_RETURN Std_ReturnType Rs485If_PollAllPacks(void);

/**
 * @brief Read a pack's cached state.
 * @param[in]  slot  Pack slot.
 * @param[out] state Destination.
 * @return E_OK on success; E_NOT_OK for an invalid slot or a NULL pointer.
 */
CHECK_RETURN Std_ReturnType Rs485If_GetPackState(Rs485If_SlotType slot, Rs485If_PackStateType *state);

/** Number of slots that answered discovery. */
uint8 Rs485If_GetPresentPackCount(void);

/**
 * @brief Read the bus counters.
 * @param[out] stats Destination. E_NOT_OK if NULL_PTR.
 */
CHECK_RETURN Std_ReturnType Rs485If_GetStatistics(Rs485If_StatisticsType *stats);

/**
 * @brief Return this module's version information.
 * @param[out] versioninfo Destination. Ignored if NULL_PTR.
 */
void Rs485If_GetVersionInfo(Std_VersionInfoType *versioninfo);

#ifdef __cplusplus
}
#endif

#endif /* RS485IF_H */

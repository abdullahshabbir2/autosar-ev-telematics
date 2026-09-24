#!/usr/bin/env python3
"""Reference model and test-vector generator for the RS485 battery-pack protocol.

Builds request and response frames from the protocol definition in
``docs/08-protocols.md`` and emits them as a C header for ``test_rs485``.  Frames are
constructed here independently of the C implementation, so a disagreement points at the
C code rather than at a shared misunderstanding.

Provenance of the golden frames
-------------------------------
The v1 firmware embedded three hand-authored response frames as its ``DUMMY_DATA``
fixtures.  Two of them carry a CRC that this model reproduces exactly, which
cross-validates the frame layout, the length field and the CRC parameters against code
that was demonstrably talking to real hardware:

    Dummy_SN_response   (15 bytes)  CRC 0xAC56  -> reproduced
    Dummy_CELL_response (81 bytes)  CRC 0xB047  -> reproduced
    Dummy_BATT_response (53 bytes)  CRC 0x2B76  -> does NOT match; correct CRC is 0xF831

The third is simply wrong.  It went unnoticed because v1's ``readResponse`` returned
``true`` unconditionally on the ``DUMMY_DATA`` path, before reaching its own CRC check --
so simulation mode exercised neither the CRC nor any other validation, and could not have
caught a regression in either.  Both variants are emitted below: the corrected frame as a
positive vector, and the v1 frame verbatim as a negative vector proving the validator
rejects a real-world bad CRC.

Usage
-----
    python tools/rs485_reference.py --check   # self-check against the golden frames
    python tools/rs485_reference.py --emit    # write test/support/Rs485_TestVectors.h

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
"""

from __future__ import annotations

import argparse
import sys

sys.path.insert(0, __file__.rsplit("rs485_reference.py", 1)[0])
from crc_reference import PROFILES, crc  # noqa: E402

CRC16 = next(p for p in PROFILES if p.name == "CRC16")

# ---------------------------------------------------------------------------
# Protocol constants (mirror of Rs485If.h -- kept literal, not imported)
# ---------------------------------------------------------------------------

START_BYTE = 0xFF
DIR_REQUEST = 0x00
DIR_RESPONSE = 0x55
TYPE_ADDRESSED = 0x01
TYPE_BROADCAST = 0x03

CMD_SERIAL_NUMBER = 0x00
CMD_BATTERY_PARAMS = 0x01
CMD_CELL_PARAMS = 0x05

REQUEST_SIZE = 15
REQUEST_SIZE_EXT = 17
SERIAL_RESPONSE_SIZE = 15
BATTERY_RESPONSE_SIZE = 53
CELL_RESPONSE_SIZE = 81
PAYLOAD_OFFSET = 13


def append_crc(frame: list[int]) -> list[int]:
    """Return ``frame`` with its final two bytes replaced by the CRC of the rest."""
    body = bytes(frame[:-2])
    value = crc(CRC16, body)
    return frame[:-2] + [(value >> 8) & 0xFF, value & 0xFF]


def build_request(serial: int, command: int) -> list[int]:
    """A 15-byte addressed request carrying no payload."""
    frame = [0x00] * REQUEST_SIZE
    frame[0] = START_BYTE
    frame[1] = DIR_REQUEST
    frame[2] = (REQUEST_SIZE >> 8) & 0xFF
    frame[3] = REQUEST_SIZE & 0xFF
    frame[4] = TYPE_ADDRESSED
    frame[5:9] = [(serial >> s) & 0xFF for s in (24, 16, 8, 0)]
    frame[9] = command
    return append_crc(frame)


def build_request_ext(serial: int, command: int, hi: int, lo: int) -> list[int]:
    """A 17-byte broadcast request carrying a two-byte payload."""
    frame = [0x00] * REQUEST_SIZE_EXT
    frame[0] = START_BYTE
    frame[1] = DIR_REQUEST
    frame[2] = (REQUEST_SIZE_EXT >> 8) & 0xFF
    frame[3] = REQUEST_SIZE_EXT & 0xFF
    frame[4] = TYPE_BROADCAST
    frame[5:9] = [(serial >> s) & 0xFF for s in (24, 16, 8, 0)]
    frame[9] = command
    frame[11] = 0x00
    frame[12] = 0x02
    frame[13] = hi
    frame[14] = lo
    return append_crc(frame)


def build_response(serial: int, command: int, payload: list[int], size: int) -> list[int]:
    """A response frame of exactly ``size`` bytes wrapping ``payload``."""
    frame = [0x00] * size
    frame[0] = START_BYTE
    frame[1] = DIR_RESPONSE
    frame[2] = (size >> 8) & 0xFF
    frame[3] = size & 0xFF
    frame[4] = TYPE_ADDRESSED
    frame[5:9] = [(serial >> s) & 0xFF for s in (24, 16, 8, 0)]
    frame[9] = command
    expected = size - PAYLOAD_OFFSET - 2
    if len(payload) != expected:
        raise ValueError(f"payload must be {expected} bytes, got {len(payload)}")
    frame[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(payload)] = payload
    return append_crc(frame)


def u16(v: int) -> list[int]:
    return [(v >> 8) & 0xFF, v & 0xFF]


def u32(v: int) -> list[int]:
    return [(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]


def s16(v: int) -> list[int]:
    return u16(v & 0xFFFF)


def s32(v: int) -> list[int]:
    return u32(v & 0xFFFFFFFF)


# ---------------------------------------------------------------------------
# Golden frames from the v1 firmware
# ---------------------------------------------------------------------------

V1_SN = [0xFF, 0x55, 0x00, 0x0F, 0x01, 0x12, 0x34, 0x56, 0x78,
         0x00, 0x00, 0x00, 0x00, 0xAC, 0x56]

V1_BATT = [0xFF, 0x55, 0x00, 0x35, 0x01, 0x12, 0x34, 0x56, 0x78, 0x01, 0x00, 0x00,
           0x26, 0x1B, 0x58, 0x01, 0x64, 0x01, 0x0D, 0x00, 0x00, 0x10, 0xCC, 0x0D,
           0xAC, 0x0F, 0xA0, 0x0B, 0xB8, 0x3B, 0x60, 0x00, 0x01, 0x38, 0x84, 0x00,
           0x03, 0x68, 0xEC, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x75, 0x34, 0x00,
           0x00, 0x00, 0x03, 0x2B, 0x76]

V1_CELL = [0xFF, 0x55, 0x00, 0x51, 0x01, 0x12, 0x34, 0x56, 0x78, 0x05, 0x00, 0x00,
           0x42, 0x03, 0xE8, 0x03, 0xF2, 0x11, 0x22, 0x33, 0x44, 0x04, 0x04, 0x04,
           0x0E, 0x04, 0x18, 0x04, 0x22, 0x04, 0x2C, 0x04, 0x36, 0x04, 0x40, 0x04,
           0x4A, 0x04, 0x54, 0x04, 0x5E, 0x04, 0x68, 0x04, 0x72, 0x04, 0x7C, 0x04,
           0x86, 0x04, 0x90, 0x04, 0x9A, 0x04, 0xA4, 0x04, 0xAE, 0x04, 0xB8, 0x04,
           0xC2, 0x04, 0xCC, 0x04, 0xD6, 0x13, 0x88, 0x00, 0xFA, 0x01, 0x04, 0x01,
           0x0E, 0x01, 0x18, 0x87, 0x65, 0x43, 0x21, 0xB0, 0x47]

TEST_SERIAL = 0x12345678


def self_check() -> bool:
    """Reproduce the two trustworthy golden CRCs and confirm the third is wrong."""
    ok = True

    for name, frame, expect_match in (
        ("Dummy_SN_response", V1_SN, True),
        ("Dummy_CELL_response", V1_CELL, True),
        ("Dummy_BATT_response", V1_BATT, False),
    ):
        embedded = (frame[-2] << 8) | frame[-1]
        computed = crc(CRC16, bytes(frame[:-2]))
        matched = embedded == computed
        verdict = "ok" if matched == expect_match else "UNEXPECTED"
        if matched != expect_match:
            ok = False
        note = "" if expect_match else "  (v1 CRC is wrong -- expected)"
        print(f"  {name:22} len={len(frame):3} embedded=0x{embedded:04X} "
              f"computed=0x{computed:04X}  [{verdict}]{note}")

    # And the declared length field must equal the actual frame length in all three.
    for name, frame in (("SN", V1_SN), ("BATT", V1_BATT), ("CELL", V1_CELL)):
        declared = (frame[2] << 8) | frame[3]
        if declared != len(frame):
            print(f"  {name}: length field {declared} != actual {len(frame)}", file=sys.stderr)
            ok = False

    return ok


def battery_payload_nominal() -> list[int]:
    """A plausible pack reading: 38 bytes."""
    return (u16(7000) + u16(3560) + u16(3480) + s32(4300) + s16(2750) + s16(2810)
            + s16(2690) + [59, 96] + u32(80004) + u32(223468) + u32(3600) + u32(30004)
            + u32(0x00000003))


def battery_payload_negative() -> list[int]:
    """A discharging pack below freezing: exercises every signed field's negative range."""
    return (u16(6820) + u16(3410) + u16(3390) + s32(-12750) + s16(-150) + s16(-90)
            + s16(-220) + [42, 91] + u32(0) + u32(1) + u32(0) + u32(7) + u32(0xDEADBEEF))


def battery_payload_extremes() -> list[int]:
    """Saturated fields: the boundaries where a misplaced cast changes the value."""
    return (u16(0xFFFF) + u16(0xFFFF) + u16(0xFFFF) + s32(-2147483648) + s16(-32768)
            + s16(32767) + s16(-32768) + [100, 100] + u32(0xFFFFFFFF) + u32(0xFFFFFFFF)
            + u32(0xFFFFFFFF) + u32(0xFFFFFFFF) + u32(0xFFFFFFFF))


def cell_payload_nominal() -> list[int]:
    """23 cell voltages, current, 4 temperatures, 2 flag words: 66 bytes."""
    payload: list[int] = []
    for i in range(23):
        payload += u16(3300 + (i * 7))
    payload += s32(-5000)
    for i in range(4):
        payload += s16(2500 + (i * 25))
    payload += u32(0x87654321)
    payload += u32(0x0F0F0F0F)
    return payload


def emit_header() -> str:
    """Render every vector as a C header."""
    lines: list[str] = [
        "/**",
        " * @file    Rs485_TestVectors.h",
        " * @brief   Generated RS485 protocol test vectors.",
        " *",
        " * GENERATED by tools/rs485_reference.py -- do not edit by hand.",
        " * The generator reproduces the CRC of two frames taken from the v1 firmware, which",
        " * cross-validates the frame layout and CRC parameters against code that was talking",
        " * to real hardware. See that script's docstring for the third frame's story.",
        " *",
        " * @copyright",
        " * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.",
        " * SPDX-License-Identifier: Proprietary",
        " */",
        "",
        "#ifndef RS485_TESTVECTORS_H",
        "#define RS485_TESTVECTORS_H",
        "",
        '#include "base/Std_Types.h"',
        "",
        f"#define RS485_TV_SERIAL 0x{TEST_SERIAL:08X}uL",
        "",
    ]

    def emit_array(name: str, data: list[int], comment: str) -> None:
        lines.append(f"/** {comment} */")
        lines.append(f"static const uint8 {name}[{len(data)}] = {{")
        for i in range(0, len(data), 12):
            chunk = ", ".join(f"0x{b:02X}u" for b in data[i:i + 12])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    emit_array("Rs485_Tv_RequestSerial", build_request(TEST_SERIAL, CMD_SERIAL_NUMBER),
               "Request: read serial number, addressed to RS485_TV_SERIAL.")
    emit_array("Rs485_Tv_RequestBattery", build_request(TEST_SERIAL, CMD_BATTERY_PARAMS),
               "Request: read pack parameters (BATT0100).")
    emit_array("Rs485_Tv_RequestCells", build_request(TEST_SERIAL, CMD_CELL_PARAMS),
               "Request: read cell parameters (BATT0500).")
    emit_array("Rs485_Tv_RequestSwitchAll",
               build_request_ext(0, CMD_BATTERY_PARAMS, 0x00, 0x0F),
               "Request: enable all four pack slots (broadcast, 17 bytes).")
    emit_array("Rs485_Tv_RequestSwitchNone",
               build_request_ext(0, CMD_BATTERY_PARAMS, 0x00, 0x00),
               "Request: disable every pack slot.")

    emit_array("Rs485_Tv_ResponseSwitchAck",
               build_response(0, CMD_BATTERY_PARAMS, [0x00, 0x0F], REQUEST_SIZE_EXT),
               "Response: a valid 17-byte acknowledgement of a pack-switch command.")
    emit_array("Rs485_Tv_ResponseSerialOther",
               build_response(0x87654321, CMD_SERIAL_NUMBER, [], SERIAL_RESPONSE_SIZE),
               "Response: serial number of a *different* pack. Negative vector for the "
               "echoed-serial check.")

    emit_array("Rs485_Tv_ResponseSerial", V1_SN,
               "Response: serial number. Verbatim from v1; CRC independently reproduced.")
    emit_array("Rs485_Tv_ResponseCellsV1", V1_CELL,
               "Response: cell parameters. Verbatim from v1; CRC independently reproduced.")
    emit_array("Rs485_Tv_ResponseBatteryV1Corrupt", V1_BATT,
               "Response: v1 pack-parameter frame with its original WRONG CRC (0x2B76). "
               "Negative vector: the validator must reject this.")

    emit_array("Rs485_Tv_ResponseBatteryNominal",
               build_response(TEST_SERIAL, CMD_BATTERY_PARAMS, battery_payload_nominal(),
                              BATTERY_RESPONSE_SIZE),
               "Response: nominal pack reading, correct CRC.")
    emit_array("Rs485_Tv_ResponseBatteryNegative",
               build_response(TEST_SERIAL, CMD_BATTERY_PARAMS, battery_payload_negative(),
                              BATTERY_RESPONSE_SIZE),
               "Response: discharging below freezing -- all signed fields negative.")
    emit_array("Rs485_Tv_ResponseBatteryExtremes",
               build_response(TEST_SERIAL, CMD_BATTERY_PARAMS, battery_payload_extremes(),
                              BATTERY_RESPONSE_SIZE),
               "Response: every field saturated to its representable limit.")
    emit_array("Rs485_Tv_ResponseCellsNominal",
               build_response(TEST_SERIAL, CMD_CELL_PARAMS, cell_payload_nominal(),
                              CELL_RESPONSE_SIZE),
               "Response: nominal cell reading, correct CRC.")

    # Expected decoded values, so the test asserts against numbers derived here.
    lines += [
        "/* Expected decode of Rs485_Tv_ResponseBatteryNominal. */",
        "#define RS485_TV_NOM_VOLTAGE 7000u",
        "#define RS485_TV_NOM_VOLTAGE_HIGH 3560u",
        "#define RS485_TV_NOM_VOLTAGE_LOW 3480u",
        "#define RS485_TV_NOM_CURRENT 4300L",
        "#define RS485_TV_NOM_TEMP 2750",
        "#define RS485_TV_NOM_TEMP_HIGH 2810",
        "#define RS485_TV_NOM_TEMP_LOW 2690",
        "#define RS485_TV_NOM_SOC 59u",
        "#define RS485_TV_NOM_SOH 96u",
        "#define RS485_TV_NOM_CHARGE_WH 80004uL",
        "#define RS485_TV_NOM_DISCHARGE_WH 223468uL",
        "#define RS485_TV_NOM_CHARGE_SEC 3600uL",
        "#define RS485_TV_NOM_DISCHARGE_SEC 30004uL",
        "#define RS485_TV_NOM_FLAGS 0x00000003uL",
        "",
        "/* Expected decode of Rs485_Tv_ResponseBatteryNegative. */",
        "#define RS485_TV_NEG_CURRENT (-12750L)",
        "#define RS485_TV_NEG_TEMP (-150)",
        "#define RS485_TV_NEG_TEMP_HIGH (-90)",
        "#define RS485_TV_NEG_TEMP_LOW (-220)",
        "#define RS485_TV_NEG_FLAGS 0xDEADBEEFuL",
        "",
        "/* Expected decode of Rs485_Tv_ResponseBatteryExtremes. */",
        "#define RS485_TV_EXT_CURRENT (-2147483647L - 1L)",
        "#define RS485_TV_EXT_TEMP (-32768)",
        "#define RS485_TV_EXT_TEMP_HIGH 32767",
        "",
        "/* Expected decode of Rs485_Tv_ResponseCellsNominal. */",
        "#define RS485_TV_CELL_BASE_MV 3300u",
        "#define RS485_TV_CELL_STEP_MV 7u",
        "#define RS485_TV_CELL_CURRENT (-5000L)",
        "#define RS485_TV_CELL_TEMP_BASE 2500",
        "#define RS485_TV_CELL_TEMP_STEP 25",
        "#define RS485_TV_CELL_FLAGS1 0x87654321uL",
        "#define RS485_TV_CELL_FLAGS2 0x0F0F0F0FuL",
        "",
        "/* The CRC the v1 BATT fixture should have carried. */",
        f"#define RS485_TV_V1_BATT_CORRECT_CRC 0x{crc(CRC16, bytes(V1_BATT[:-2])):04X}u",
        f"#define RS485_TV_V1_BATT_EMBEDDED_CRC 0x{(V1_BATT[-2] << 8) | V1_BATT[-1]:04X}u",
        "",
        "/** Serial number carried by Rs485_Tv_ResponseSerialOther. */",
        "#define RS485_TV_SERIAL_OTHER 0x87654321uL",
        "",
        "#endif /* RS485_TESTVECTORS_H */",
    ]

    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="self-check only")
    parser.add_argument("--emit", action="store_true",
                        help="write test/support/Rs485_TestVectors.h")
    args = parser.parse_args()

    print("RS485 protocol reference self-check against v1 golden frames:")
    if not self_check():
        print("\nFAIL: golden frame verification did not behave as documented.",
              file=sys.stderr)
        return 1
    print("Frame layout, length field and CRC parameters confirmed.\n")

    if args.check:
        return 0

    header = emit_header()
    if args.emit:
        with open("test/support/Rs485_TestVectors.h", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(header)
        print("Wrote test/support/Rs485_TestVectors.h")
    else:
        print(header)
    return 0


if __name__ == "__main__":
    sys.exit(main())

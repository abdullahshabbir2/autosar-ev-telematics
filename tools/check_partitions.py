#!/usr/bin/env python3
"""Validate the flash partition table against the constraints the firmware depends on.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

The partition table is a separate artefact from the firmware and the two can disagree: a unit can be
flashed with a table that does not match the image it is running. `Fls_Init` checks at runtime that
`nvdata` exists at the expected size, which is the right place for the check that matters in the
field -- but a table that is wrong should fail the build, not the vehicle.

Checks, each of which corresponds to something that breaks if it does not hold:

  1. Every offset and size is 4 KiB aligned. Flash erases a whole sector whatever is asked for, so an
     unaligned partition shares its first or last sector with a neighbour and erasing one damages the
     other.
  2. No two partitions overlap.
  3. Nothing extends past the end of the device.
  4. `nvdata` exists, has the subtype Fls_Cfg.h expects, and is at least FLS_PARTITION_SIZE.
  5. `nvdata` is a whole number of sectors and at least two of them, because Fee ping-pongs between
     sectors so that a garbage collection never has a moment with no valid copy of a block.
  6. `app0` and `app1` are the same size, or A/B OTA cannot roll back -- an image that fits the
     active slot but not the other one makes the rollback path unusable exactly when it is needed.

Exit status is 0 if every check passes, 1 otherwise, with each failure named.
"""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TABLE = REPO_ROOT / "partitions" / "odo_partitions.csv"
FLS_CFG = REPO_ROOT / "src" / "mcal" / "Fls" / "Fls_Cfg.h"

SECTOR_SIZE = 4096
DEVICE_SIZE = 4 * 1024 * 1024


class Partition:
    def __init__(self, name: str, ptype: str, subtype: str, offset: int, size: int):
        self.name = name
        self.ptype = ptype
        self.subtype = subtype
        self.offset = offset
        self.size = size

    @property
    def end(self) -> int:
        return self.offset + self.size

    def __str__(self) -> str:
        return f"{self.name} @ {self.offset:#x}..{self.end:#x} ({self.size:#x})"


def parse_number(text: str) -> int:
    text = text.strip()
    return int(text, 16) if text.lower().startswith("0x") else int(text, 10)


def read_table(path: Path) -> list[Partition]:
    partitions: list[Partition] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#") or not row[0].strip():
                continue
            if len(row) < 5:
                sys.exit(f"{path.name}: malformed row: {row}")
            partitions.append(
                Partition(
                    row[0].strip(),
                    row[1].strip(),
                    row[2].strip(),
                    parse_number(row[3]),
                    parse_number(row[4]),
                )
            )
    return partitions


def read_fls_expectations(path: Path) -> tuple[str, int, int]:
    """Pull the partition name, subtype and minimum size out of Fls_Cfg.h.

    Read from the header rather than duplicated here, so the two cannot drift. A check that keeps its
    own copy of the value it is checking stops being a check the first time one side changes.
    """
    text = path.read_text(encoding="utf-8")

    def find(pattern: str) -> str:
        match = re.search(pattern, text)
        if not match:
            sys.exit(f"{path.name}: could not find {pattern}")
        return match.group(1)

    name = find(r'#define\s+FLS_PARTITION_NAME\s+"([^"]+)"')
    subtype = parse_number(find(r"#define\s+FLS_PARTITION_SUBTYPE\s+(0[xX][0-9a-fA-F]+|\d+)"))
    sector_count = parse_number(find(r"#define\s+FLS_SECTOR_COUNT\s+(\d+)u?"))
    return name, subtype, sector_count * SECTOR_SIZE


def main() -> int:
    if not TABLE.exists():
        sys.exit(f"missing {TABLE}")

    partitions = read_table(TABLE)
    if not partitions:
        sys.exit(f"{TABLE.name}: no partitions found")

    fls_name, fls_subtype, fls_min_size = read_fls_expectations(FLS_CFG)
    failures: list[str] = []

    # 1. Alignment.
    for p in partitions:
        if p.offset % SECTOR_SIZE:
            failures.append(f"{p.name}: offset {p.offset:#x} is not {SECTOR_SIZE}-byte aligned")
        if p.size % SECTOR_SIZE:
            failures.append(f"{p.name}: size {p.size:#x} is not a whole number of sectors")

    # 2. Overlap, and 3. device bounds.
    ordered = sorted(partitions, key=lambda p: p.offset)
    for earlier, later in zip(ordered, ordered[1:]):
        if earlier.end > later.offset:
            failures.append(f"{earlier.name} overlaps {later.name}: {earlier} vs {later}")
    for p in partitions:
        if p.end > DEVICE_SIZE:
            failures.append(f"{p.name} extends past the {DEVICE_SIZE:#x} device: {p}")

    by_name = {p.name: p for p in partitions}

    # 4 and 5. The partition Fls owns.
    nvdata = by_name.get(fls_name)
    if nvdata is None:
        failures.append(f"no '{fls_name}' partition; Fls_Init will fail at runtime")
    else:
        if parse_number(nvdata.subtype) != fls_subtype:
            failures.append(
                f"{fls_name}: subtype {nvdata.subtype} but Fls_Cfg.h expects {fls_subtype:#x}"
            )
        if nvdata.ptype != "data":
            failures.append(f"{fls_name}: type '{nvdata.ptype}', expected 'data'")
        if nvdata.size < fls_min_size:
            failures.append(
                f"{fls_name}: {nvdata.size:#x} is smaller than FLS_PARTITION_SIZE {fls_min_size:#x}"
            )
        if nvdata.size // SECTOR_SIZE < 2:
            failures.append(
                f"{fls_name}: only {nvdata.size // SECTOR_SIZE} sector(s); Fee needs at least 2 to "
                "ping-pong, or a garbage collection has a window with no valid copy of a block"
            )

    # 6. Equal OTA slots.
    app0, app1 = by_name.get("app0"), by_name.get("app1")
    if app0 and app1 and app0.size != app1.size:
        failures.append(
            f"app0 ({app0.size:#x}) and app1 ({app1.size:#x}) differ; A/B rollback needs both slots "
            "to hold the same image"
        )

    # ------------------------------------------------------------------ report
    width = max(len(p.name) for p in partitions)
    print(f"{TABLE.relative_to(REPO_ROOT)}  ({DEVICE_SIZE // (1024 * 1024)} MiB device)")
    for p in ordered:
        print(
            f"  {p.name:<{width}}  {p.offset:#09x} .. {p.end:#09x}"
            f"  {p.size // 1024:>6} KiB  {p.ptype}/{p.subtype}"
        )
    used = sum(p.size for p in partitions)
    print(f"  {'':<{width}}  allocated {used // 1024} KiB of {DEVICE_SIZE // 1024} KiB")

    if failures:
        print()
        for message in failures:
            print(f"FAIL  {message}")
        return 1

    print("\nOK  every partition check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

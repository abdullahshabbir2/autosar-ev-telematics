#!/usr/bin/env python3
"""Host (off-target) unit test runner for the EV telematics ECU.

Why this exists alongside ``pio test``
--------------------------------------
Every ``.c`` file in ``src/`` belongs to a layer that is defined to be free of any
platform dependency: the MCAL's platform bindings live in ``.cpp`` files, and
nothing above the MCAL may include an Arduino, ESP-IDF or FreeRTOS header.  This
runner enforces that rule mechanically -- it compiles **all** ``src/**/*.c`` with a
plain host compiler and links them against the stubs in ``test/support/``.  A
platform dependency leaking upward does not produce a subtle runtime bug months
later; it fails the build here, immediately, with an unresolved symbol.

It also means the whole suite runs with nothing but ``gcc`` and Unity -- no
embedded toolchain, no board, no network -- which is what makes it usable as a
pre-commit gate and in CI.

Usage
-----
    python tools/run_native_tests.py                 # build and run every suite
    python tools/run_native_tests.py test_crc        # run selected suites
    python tools/run_native_tests.py --list          # list discovered suites
    python tools/run_native_tests.py --coverage      # add gcov instrumentation
    python tools/run_native_tests.py --verbose       # echo every compiler command

Exit status is 0 only if every selected suite builds and passes.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO_ROOT / "src"
TEST_ROOT = REPO_ROOT / "test"
BUILD_ROOT = REPO_ROOT / ".build" / "native"

# Unity is resolved in this order: a vendored copy, then the PlatformIO global
# library store, then a UNITY_DIR override.  CI vendors it so the run is hermetic.
UNITY_SEARCH_PATHS = [
    REPO_ROOT / "test" / "vendor" / "Unity" / "src",
    Path.home() / ".platformio" / "lib" / "Unity" / "src",
]

# Warnings are errors.  This list is deliberately close to the strictest practical
# setting for C: the MISRA-adjacent rules in docs/10-coding-standard.md assume that
# implicit conversions and shadowed declarations cannot reach review.
CFLAGS = [
    "-std=c11",
    "-O1",  # -O1 so the optimiser runs (catches UB the tests would otherwise hide)
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wshadow",
    "-Wconversion",
    "-Wsign-conversion",
    "-Wcast-qual",
    "-Wcast-align",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-Wpointer-arith",
    "-Wundef",
    "-Wswitch-default",
    "-Wswitch-enum",
    "-Wfloat-equal",
    "-Wdouble-promotion",
    "-Wredundant-decls",
    "-Wwrite-strings",
    "-Winit-self",
    "-Wlogical-op",
    "-Wformat=2",
    "-fno-common",
    "-DUNIT_TEST=1",
    "-DDET_ENABLE_DEV_ERROR_DETECT=1",
]

# Unity itself is third-party and is not held to the project's warning policy.
UNITY_CFLAGS = ["-std=c11", "-O1", "-g", "-w", "-DUNITY_INCLUDE_DOUBLE"]

SANITISER_FLAGS = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


class Colour:
    """ANSI colours, disabled when stdout is not a terminal or NO_COLOR is set."""

    _on = sys.stdout.isatty() and "NO_COLOR" not in os.environ

    GREEN = "\033[32m" if _on else ""
    RED = "\033[31m" if _on else ""
    YELLOW = "\033[33m" if _on else ""
    CYAN = "\033[36m" if _on else ""
    DIM = "\033[2m" if _on else ""
    BOLD = "\033[1m" if _on else ""
    RESET = "\033[0m" if _on else ""


def find_unity() -> Path:
    """Locate Unity's source directory or exit with an actionable message."""
    override = os.environ.get("UNITY_DIR")
    candidates = ([Path(override)] if override else []) + UNITY_SEARCH_PATHS
    for candidate in candidates:
        if (candidate / "unity.c").is_file():
            return candidate
    sys.exit(
        "Unity not found. Install it with:\n"
        "    pio pkg install -g -l 'throwtheswitch/Unity@^2.5.2'\n"
        "or vendor it into test/vendor/Unity, or set UNITY_DIR."
    )


def find_compiler() -> str:
    """Return the host C compiler, honouring $CC."""
    for name in (os.environ.get("CC"), "gcc", "clang", "cc"):
        if name and shutil.which(name):
            return name
    sys.exit("No host C compiler found. Install gcc or clang, or set $CC.")


def include_dirs(unity_dir: Path) -> list[str]:
    """The include search path: three roots, not one directory per module.

    Project headers are referenced by their layer-qualified path -- ``"services/Crc/Crc.h"``, not
    ``"Crc.h"`` -- so only the roots need to be on the search path.  That is not a style choice.  On
    a case-insensitive filesystem a flat namespace makes the AUTOSAR module names collide with the
    Arduino framework's headers (``Spi.h``/``SPI.h``, ``Can.h``/``can.h``, ``Uart.h``/``uart.h``),
    and the collision resolves differently in each direction depending on include order.  The
    reasoning is in full at the top of platformio.ini; the two builds must agree, so the layout is
    the same here.

    It also gives the host build something useful: because the path has exactly these roots, a
    ``#include "Spi.h"`` left over anywhere fails to compile rather than silently finding a
    different file, so the target and host builds cannot drift apart on this.

    Unity and ``test/support`` keep flat includes -- the stub headers are named ``Stub_*`` and
    collide with nothing.
    """
    dirs = [unity_dir, SRC_ROOT, REPO_ROOT / "config", TEST_ROOT / "support"]
    return [f"-iquote{d}" for d in (str(x) for x in dirs)]


def discover_suites() -> dict[str, Path]:
    """Find every ``test/test_*`` directory containing at least one .c file."""
    suites = {}
    for path in sorted(TEST_ROOT.iterdir()):
        if path.is_dir() and path.name.startswith("test_") and any(path.glob("*.c")):
            suites[path.name] = path
    return suites


def production_sources() -> list[Path]:
    """All platform-independent production sources (every ``src/**/*.c``).

    ``.cpp`` files are excluded by construction: they are the platform bindings.
    """
    return sorted(SRC_ROOT.rglob("*.c"))


def support_sources() -> list[Path]:
    """Test doubles and stubs shared by all suites."""
    return sorted((TEST_ROOT / "support").rglob("*.c"))


def compile_one(cc: str, source: Path, obj: Path, flags: list[str], incs: list[str],
                verbose: bool) -> tuple[bool, str]:
    """Compile a single translation unit. Returns (ok, diagnostics)."""
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc, *flags, *incs, "-c", str(source), "-o", str(obj)]
    if verbose:
        print(f"{Colour.DIM}{' '.join(cmd)}{Colour.RESET}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode == 0, (proc.stdout + proc.stderr)


def build_and_run(
    cc: str,
    suite: str,
    suite_dir: Path,
    unity_dir: Path,
    incs: list[str],
    extra_cflags: list[str],
    extra_ldflags: list[str],
    verbose: bool,
) -> tuple[bool, str, float]:
    """Compile and execute one suite. Returns (passed, output, seconds)."""
    started = time.monotonic()
    out_dir = BUILD_ROOT / suite
    objects: list[Path] = []
    cflags = CFLAGS + extra_cflags

    # Unity, then the production .c layer, then the stubs, then the suite itself.
    units: list[tuple[Path, list[str]]] = [(unity_dir / "unity.c", UNITY_CFLAGS + extra_cflags)]
    units += [(s, cflags) for s in production_sources()]
    units += [(s, cflags) for s in support_sources()]
    units += [(s, cflags) for s in sorted(suite_dir.glob("*.c"))]

    for source, flags in units:
        try:
            rel = source.relative_to(REPO_ROOT)
        except ValueError:
            rel = Path("vendor") / source.name  # Unity lives outside the repo
        obj = out_dir / rel.with_suffix(".o")
        ok, diag = compile_one(cc, source, obj, flags, incs, verbose)
        if not ok:
            return False, f"compile failed: {rel}\n{diag}", time.monotonic() - started
        if diag.strip():
            return False, f"warnings treated as errors in {rel}\n{diag}", time.monotonic() - started
        objects.append(obj)

    binary = out_dir / (suite + (".exe" if os.name == "nt" else ""))
    link_cmd = [cc, *[str(o) for o in objects], "-o", str(binary), *extra_ldflags, "-lm"]
    if verbose:
        print(f"{Colour.DIM}{' '.join(link_cmd)}{Colour.RESET}")
    link = subprocess.run(link_cmd, capture_output=True, text=True)
    if link.returncode != 0:
        return False, "link failed\n" + link.stdout + link.stderr, time.monotonic() - started

    run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=out_dir)
    return run.returncode == 0, (run.stdout + run.stderr), time.monotonic() - started


def parse_unity_totals(output: str) -> tuple[int, int, int]:
    """Extract (tests, failures, ignored) from Unity's summary line."""
    for line in reversed(output.splitlines()):
        parts = line.split()
        if len(parts) >= 6 and parts[1] == "Tests" and parts[3] == "Failures":
            try:
                return int(parts[0]), int(parts[2]), int(parts[4])
            except ValueError:
                break
    return 0, 0, 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suites", nargs="*", help="suite names to run (default: all)")
    parser.add_argument("--list", action="store_true", help="list suites and exit")
    parser.add_argument("--coverage", action="store_true", help="instrument for gcov")
    parser.add_argument("--sanitize", action="store_true",
                        help="enable ASan/UBSan (not available on every host)")
    parser.add_argument("--verbose", "-v", action="store_true", help="echo compiler commands")
    parser.add_argument("--clean", action="store_true", help="remove build output first")
    args = parser.parse_args()

    suites = discover_suites()
    if args.list:
        for name in suites:
            print(name)
        return 0
    if not suites:
        print("No test suites found under test/.", file=sys.stderr)
        return 1

    if args.suites:
        unknown = [s for s in args.suites if s not in suites]
        if unknown:
            print(f"Unknown suite(s): {', '.join(unknown)}", file=sys.stderr)
            print(f"Available: {', '.join(suites)}", file=sys.stderr)
            return 1
        selected = {name: suites[name] for name in args.suites}
    else:
        selected = suites

    if args.clean and BUILD_ROOT.exists():
        shutil.rmtree(BUILD_ROOT)

    cc = find_compiler()
    unity_dir = find_unity()
    incs = include_dirs(unity_dir)

    extra_cflags: list[str] = []
    extra_ldflags: list[str] = []
    if args.coverage:
        extra_cflags += ["--coverage"]
        extra_ldflags += ["--coverage"]
    if args.sanitize:
        extra_cflags += SANITISER_FLAGS
        extra_ldflags += SANITISER_FLAGS

    print(f"{Colour.BOLD}Host unit tests{Colour.RESET}  cc={cc}  unity={unity_dir}")
    print(f"{len(production_sources())} production sources, {len(selected)} suite(s)\n")

    total_tests = total_failures = total_ignored = 0
    failed_suites: list[str] = []

    for name, path in selected.items():
        passed, output, elapsed = build_and_run(
            cc, name, path, unity_dir, incs, extra_cflags, extra_ldflags, args.verbose
        )
        tests, failures, ignored = parse_unity_totals(output)
        total_tests += tests
        total_failures += failures
        total_ignored += ignored

        if passed:
            print(f"  {Colour.GREEN}PASS{Colour.RESET}  {name:<28} "
                  f"{tests:>3} tests  {elapsed:5.2f}s")
        else:
            failed_suites.append(name)
            print(f"  {Colour.RED}FAIL{Colour.RESET}  {name:<28} "
                  f"{tests:>3} tests  {elapsed:5.2f}s")
            for line in output.rstrip().splitlines():
                print(f"        {line}")

    print()
    summary = (f"{total_tests} tests, {total_failures} failures, {total_ignored} ignored "
               f"across {len(selected)} suite(s)")
    if failed_suites:
        print(f"{Colour.RED}{Colour.BOLD}FAILED{Colour.RESET} {summary}")
        print(f"{Colour.RED}Failing suites: {', '.join(failed_suites)}{Colour.RESET}")
        return 1

    print(f"{Colour.GREEN}{Colour.BOLD}OK{Colour.RESET} {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

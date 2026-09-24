#!/usr/bin/env python3
"""Generate docs/06-traceability.md from the source, the tests and the requirements.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

A traceability matrix maintained by hand is wrong within a month: it is the document nobody updates
when they rename a module, and its value depends entirely on being right. So it is generated from the
three places the information actually lives:

  * docs/01-requirements.md   -- headings define the requirements
  * src/**/*.h                -- Doxygen `@req` tags say which module implements what
  * test/**/*.c               -- case comments say which test verifies what

Run it after changing any of those. CI regenerates and diffs, so a stale matrix fails the build.

    python tools/gen_traceability.py            # write the document
    python tools/gen_traceability.py --check     # exit 1 if it would change

The gaps it finds are the point. A requirement with no implementation, or one implemented but not
tested, appears in its own table rather than being absent from the matrix -- which is the difference
between a document that shows coverage and one that merely asserts it.

Range notation is understood: a header saying `@req SWREQ-SYS-0060 .. SWREQ-SYS-0085` claims every
defined requirement between those endpoints, so a module does not need thirty tags to claim thirty
requirements it genuinely implements.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS = REPO_ROOT / "docs"
REQUIREMENTS = DOCS / "01-requirements.md"
OUTPUT = DOCS / "06-traceability.md"
SRC = REPO_ROOT / "src"
TEST = REPO_ROOT / "test"

REQ_RE = re.compile(r"\bSWREQ-([A-Z]{2,4})-(\d{4})\b")
RANGE_RE = re.compile(r"\b(SWREQ-[A-Z]{2,4}-\d{4})\s*\.\.\s*(SWREQ-[A-Z]{2,4}-\d{4})\b")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+?)\s*#*$", re.MULTILINE)
REQ_TAG_RE = re.compile(r"@req\s+(?P<body>[^\n*]*(?:\n\s*\*\s+[^\n@*][^\n]*)*)", re.MULTILINE)

AREA_ORDER = [
    "SYS", "ODO", "BAT", "COM", "NVM", "STO",
    "TEL", "DIAG", "SAF", "INT", "GNS", "SNS", "HMI", "SEC",
]


def sort_key(req: str) -> tuple[int, int]:
    match = REQ_RE.match(req)
    if not match:
        return (len(AREA_ORDER), 0)
    area, number = match.group(1), int(match.group(2))
    index = AREA_ORDER.index(area) if area in AREA_ORDER else len(AREA_ORDER)
    return (index, number)


# ------------------------------------------------------------------ requirements
def read_requirements() -> dict[str, str]:
    """Requirement id -> its one-line title, from the headings of 01-requirements.md."""
    text = REQUIREMENTS.read_text(encoding="utf-8")
    found: dict[str, str] = {}
    for match in HEADING_RE.finditer(text):
        title = match.group(1)
        ids = REQ_RE.findall(title)
        if len(ids) == 1:
            req = f"SWREQ-{ids[0][0]}-{ids[0][1]}"
            # Headings read "SWREQ-SYS-0001 — Reset cause is recorded".
            summary = re.sub(r"^\s*SWREQ-[A-Z]{2,4}-\d{4}\s*[—–-]\s*", "", title).strip()
            found[req] = summary
    return found


def expand(text: str, defined: set[str]) -> set[str]:
    """Every requirement a tag body claims, expanding `A .. B` against what is defined."""
    claimed: set[str] = set()

    for start, end in RANGE_RE.findall(text):
        lo, hi = sort_key(start), sort_key(end)
        if lo > hi:
            lo, hi = hi, lo
        claimed.update(r for r in defined if lo <= sort_key(r) <= hi)

    # Bare identifiers, including the endpoints themselves.
    without_ranges = RANGE_RE.sub(" ", text)
    for area, number in REQ_RE.findall(without_ranges):
        claimed.add(f"SWREQ-{area}-{number}")
    for start, end in RANGE_RE.findall(text):
        claimed.update({start, end})

    return claimed


# ------------------------------------------------------- implementations and tests
def read_implementations(defined: set[str]) -> dict[str, set[str]]:
    """Requirement -> the modules whose headers claim it."""
    mapping: dict[str, set[str]] = defaultdict(set)
    for header in sorted(SRC.rglob("*.h")):
        text = header.read_text(encoding="utf-8")
        module = header.stem
        for tag in REQ_TAG_RE.finditer(text):
            for req in expand(tag.group("body"), defined):
                mapping[req].add(module)
    return mapping


def read_tests(defined: set[str]) -> dict[str, set[str]]:
    """Requirement -> the test suites whose cases cite it."""
    mapping: dict[str, set[str]] = defaultdict(set)
    for source in sorted(TEST.rglob("*.c")):
        text = source.read_text(encoding="utf-8")
        suite = source.parent.name
        for req in expand(text, defined):
            mapping[req].add(suite)
    return mapping


# ------------------------------------------------------------------------ render
def render(requirements: dict[str, str],
           implementations: dict[str, set[str]],
           tests: dict[str, set[str]]) -> str:
    ordered = sorted(requirements, key=sort_key)
    unimplemented = [r for r in ordered if not implementations.get(r)]
    untested = [r for r in ordered if implementations.get(r) and not tests.get(r)]
    covered = len(ordered) - len(unimplemented) - len(untested)

    out: list[str] = []
    add = out.append

    add("<!-- GENERATED by tools/gen_traceability.py -- do not edit by hand. -->")
    add("")
    add("# Traceability")
    add("")
    add("**Audience:** reviewers checking that every requirement is implemented and verified, and")
    add("engineers looking for where a requirement lives in the code.")
    add("")
    add("Generated from [01-requirements.md](01-requirements.md) (headings), the `@req` tags in")
    add("`src/**/*.h`, and the requirement citations in `test/**/*.c`. CI regenerates this file and")
    add("fails if it differs, so it cannot drift from the code the way a hand-written matrix does.")
    add("")
    add("Regenerate with:")
    add("")
    add("```bash")
    add("python tools/gen_traceability.py")
    add("```")
    add("")
    add("---")
    add("")
    add("## Coverage")
    add("")
    add("| | Count |")
    add("|---|---|")
    add(f"| Requirements defined | {len(ordered)} |")
    add(f"| Implemented and tested | {covered} |")
    add(f"| Implemented, not directly tested | {len(untested)} |")
    add(f"| Not implemented | {len(unimplemented)} |")
    add("")

    if unimplemented:
        add("### Not implemented")
        add("")
        add("No module's `@req` tag claims these. Either the requirement is not met, or the module that")
        add("meets it has not said so — both are worth resolving, and neither is visible without this.")
        add("")
        add("| Requirement | Statement |")
        add("|---|---|")
        for req in unimplemented:
            add(f"| `{req}` | {requirements[req]} |")
        add("")

    if untested:
        add("### Implemented but not directly tested")
        add("")
        add("Claimed by a module with no test citing it. Some of these are legitimately outside host")
        add("testing — anything whose verification needs the silicon — and")
        add("[05-test-strategy.md](05-test-strategy.md) §7 lists those with what covers them instead.")
        add("The rest are gaps.")
        add("")
        add("| Requirement | Statement | Implemented by |")
        add("|---|---|---|")
        for req in untested:
            modules = ", ".join(f"`{m}`" for m in sorted(implementations[req]))
            add(f"| `{req}` | {requirements[req]} | {modules} |")
        add("")

    add("---")
    add("")
    add("## Requirement → implementation → test")
    add("")

    current_area = None
    for req in ordered:
        area = REQ_RE.match(req).group(1)
        if area != current_area:
            current_area = area
            add("")
            add(f"### {area}")
            add("")
            add("| Requirement | Statement | Modules | Suites |")
            add("|---|---|---|---|")
        modules = ", ".join(f"`{m}`" for m in sorted(implementations.get(req, ()))) or "—"
        suites = ", ".join(f"`{s}`" for s in sorted(tests.get(req, ()))) or "—"
        add(f"| `{req}` | {requirements[req]} | {modules} | {suites} |")

    add("")
    add("---")
    add("")
    add("## Module → requirements")
    add("")
    add("The same data the other way round, for reading a module's header and wanting to know what it")
    add("is answerable for.")
    add("")

    by_module: dict[str, set[str]] = defaultdict(set)
    for req, modules in implementations.items():
        for module in modules:
            by_module[module].add(req)

    add("| Module | Requirements |")
    add("|---|---|")
    for module in sorted(by_module):
        reqs = ", ".join(f"`{r}`" for r in sorted(by_module[module], key=sort_key))
        add(f"| `{module}` | {reqs} |")

    add("")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the generated file would differ, without writing it")
    args = parser.parse_args()

    requirements = read_requirements()
    if not requirements:
        sys.exit(f"no requirement headings found in {REQUIREMENTS}")

    defined = set(requirements)
    implementations = read_implementations(defined)
    tests = read_tests(defined)
    rendered = render(requirements, implementations, tests)

    if args.check:
        existing = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if existing != rendered:
            print(f"FAIL  {OUTPUT.relative_to(REPO_ROOT)} is stale; run tools/gen_traceability.py")
            return 1
        print(f"OK  {OUTPUT.relative_to(REPO_ROOT)} is up to date")
        return 0

    OUTPUT.write_text(rendered, encoding="utf-8", newline="\n")

    unimplemented = sum(1 for r in requirements if not implementations.get(r))
    untested = sum(1 for r in requirements
                   if implementations.get(r) and not tests.get(r))
    print(f"wrote {OUTPUT.relative_to(REPO_ROOT)}")
    print(f"  {len(requirements)} requirements, "
          f"{len(requirements) - unimplemented - untested} implemented and tested, "
          f"{untested} untested, {unimplemented} unimplemented")
    return 0


if __name__ == "__main__":
    sys.exit(main())

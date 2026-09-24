#!/usr/bin/env python3
"""Check that every internal link in the documentation resolves.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

Documentation that points at files which no longer exist is worse than documentation that says
nothing, because a reader trusts it and then loses time. This runs in CI so a rename breaks the build
rather than the reader.

What is checked:

  * Markdown links to files in the repository, including `../` paths out of docs/ into the source.
  * Anchors within the same document (`#section-heading`), matched against GitHub's heading-to-anchor
    rules -- lowercase, spaces to hyphens, punctuation dropped.
  * Anchors in another document (`other.md#heading`).
  * Requirement identifiers of the form SWREQ-AREA-NNNN, which must be defined in 01-requirements.md
    if they are referenced anywhere. This is the check that keeps the traceability honest: a document
    or a source file citing a requirement that does not exist is a broken trace, and it is exactly
    the kind of thing that accumulates silently.

What is deliberately NOT checked:

  * External URLs. Fetching them makes CI depend on other people's uptime, so a job that is meant to
    catch our own mistakes starts failing for reasons nobody here can fix.

Exit status 0 if everything resolves, 1 otherwise, with each problem named and located.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_ROOT = REPO_ROOT / "docs"
REQUIREMENTS = DOCS_ROOT / "01-requirements.md"

# [text](target) -- the target group stops at whitespace or a closing paren.
LINK_RE = re.compile(r"\[(?P<text>[^\]]*)\]\((?P<target>[^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING_RE = re.compile(r"^(?P<hashes>#{1,6})\s+(?P<title>.+?)\s*#*$", re.MULTILINE)
# Area prefixes are two to four letters. Two matters: config/Ecu_PinMap.h once cited
# `SWREQ-HW-0001 .. SWREQ-HW-0006`, an area with no requirements defined anywhere, and a
# three-letter minimum silently ignored it -- so the checker reported everything resolving
# while a whole undefined area sat in a header.
REQ_RE = re.compile(r"\bSWREQ-[A-Z]{2,4}-\d{4}\b")

# Fenced code blocks, so a link inside an example is not treated as a real one.
FENCE_RE = re.compile(r"^```.*?^```", re.MULTILINE | re.DOTALL)


def github_anchor(title: str) -> str:
    """Convert a heading to the anchor GitHub generates for it.

    Lowercase, inline formatting stripped, non-word characters dropped, spaces to hyphens. Reproduced
    here rather than approximated, because an anchor check that does not match the renderer's rules
    reports failures on links that work and passes links that do not.
    """
    text = title.strip().lower()
    text = re.sub(r"`([^`]*)`", r"\1", text)           # inline code
    text = re.sub(r"\*\*([^*]*)\*\*", r"\1", text)     # bold
    text = re.sub(r"\*([^*]*)\*", r"\1", text)         # italic
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links keep their text
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"\s+", "-", text.strip())


def strip_code(text: str) -> str:
    """Blank out fenced blocks, preserving line count so reported line numbers stay right."""
    def blank(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    return FENCE_RE.sub(blank, text)


def anchors_of(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        if not path.exists():
            cache[path] = set()
        else:
            body = strip_code(path.read_text(encoding="utf-8"))
            cache[path] = {github_anchor(m.group("title")) for m in HEADING_RE.finditer(body)}
    return cache[path]


def defined_requirements() -> set[str]:
    if not REQUIREMENTS.exists():
        return set()
    text = REQUIREMENTS.read_text(encoding="utf-8")
    # A requirement is *defined* by a heading; a mention in prose is only a reference.
    return {
        req
        for m in HEADING_RE.finditer(text)
        for req in REQ_RE.findall(m.group("title"))
    }


def main() -> int:
    if not DOCS_ROOT.exists():
        print("no docs/ directory; nothing to check")
        return 0

    markdown = sorted(DOCS_ROOT.rglob("*.md"))
    if (REPO_ROOT / "README.md").exists():
        markdown.append(REPO_ROOT / "README.md")

    problems: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    checked = 0

    for doc in markdown:
        body = strip_code(doc.read_text(encoding="utf-8"))
        lines = body.split("\n")

        for number, line in enumerate(lines, start=1):
            for match in LINK_RE.finditer(line):
                target = match.group("target")

                if target.startswith(("http://", "https://", "mailto:")):
                    continue

                checked += 1
                where = f"{doc.relative_to(REPO_ROOT)}:{number}"

                # Same-document anchor.
                if target.startswith("#"):
                    wanted = target[1:].lower()
                    if wanted not in anchors_of(doc, anchor_cache):
                        problems.append(f"{where}: no heading matching anchor '{target}'")
                    continue

                path_part, _, anchor = target.partition("#")
                resolved = (doc.parent / path_part).resolve()

                if not resolved.exists():
                    problems.append(f"{where}: '{path_part}' does not exist")
                    continue

                if anchor and resolved.suffix == ".md":
                    if anchor.lower() not in anchors_of(resolved, anchor_cache):
                        problems.append(
                            f"{where}: '{path_part}' has no heading matching anchor '#{anchor}'"
                        )

    # Requirement identifiers, across documentation and source.
    defined = defined_requirements()
    if defined:
        referenced: dict[str, list[str]] = {}
        for path in [*markdown, *REPO_ROOT.joinpath("src").rglob("*.[ch]")]:
            if path == REQUIREMENTS:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for req in REQ_RE.findall(text):
                referenced.setdefault(req, []).append(str(path.relative_to(REPO_ROOT)))

        for req, places in sorted(referenced.items()):
            if req not in defined:
                shown = ", ".join(sorted(set(places))[:3])
                problems.append(f"{req} is referenced ({shown}) but not defined in 01-requirements.md")

    # ------------------------------------------------------------------- report
    print(f"checked {checked} internal link(s) across {len(markdown)} document(s)")
    print(f"found {len(defined)} requirement definition(s) in 01-requirements.md")

    if problems:
        print()
        for message in problems:
            print(f"FAIL  {message}")
        return 1

    print("\nOK  every internal link and requirement reference resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Inject build provenance into the firmware image as preprocessor definitions.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

Defines, all as string literals:

    ECU_BUILD_GIT_DESCRIBE   e.g. "v2.0.0-4-gb1c3f7a" or "v2.0.0-4-gb1c3f7a-dirty"
    ECU_BUILD_GIT_SHA        the full commit hash
    ECU_BUILD_TIMESTAMP      UTC, ISO 8601, second resolution
    ECU_BUILD_HOST           the machine that produced the image

These reach the health record and the diagnostic identification response, which is the entire point: a unit in
the field reports which commit it is running, and that report can be matched against a tag in the repository.

The "-dirty" suffix matters more than it looks. An image built from a working tree with uncommitted changes
cannot be reproduced from the repository, so a field report from one cannot be diagnosed against source. Marking
it in the image rather than trusting a convention is what makes that visible at the point it matters -- when
somebody is reading a fault report six months later.

Every value degrades to a defined placeholder rather than failing the build. A build from an exported tarball
with no .git directory is a legitimate thing to do, and it should produce a working image that honestly reports
it does not know its own provenance.
"""

import datetime
import platform
import subprocess

Import("env")  # noqa: F821  - injected by SCons


def _git(*args):
    """Run a git command, returning its stripped stdout or None if it cannot be run."""
    try:
        result = subprocess.run(
            ["git"] + list(args),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None

    if result.returncode != 0:
        return None

    value = result.stdout.strip()
    return value if value else None


def _describe():
    """A human-readable version, with a dirty marker when the tree has uncommitted changes."""
    # --always falls back to a bare hash when no tag is reachable, which is the normal state on a feature
    # branch. --dirty appends the marker. --tags considers non-annotated tags too, because a release tag
    # created through a web interface is not annotated and would otherwise be invisible here.
    described = _git("describe", "--tags", "--always", "--dirty")
    if described:
        return described

    # No tags and no commits reachable -- a repository with no history, or no git at all.
    return "unknown"


def _sha():
    return _git("rev-parse", "HEAD") or "unknown"


def _timestamp():
    # UTC, because an image built in one timezone and diagnosed in another must not require anyone to work out
    # which offset applied. Second resolution: finer would imply a precision the value does not have.
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _quote(value):
    r"""Wrap a value as a C string literal, escaped for the command line.

    The backslash-escaped quotes are required: the define travels through the shell and SCons before reaching
    the compiler, and an unescaped quote is consumed on the way.
    """
    # Defensive rather than expected: none of these values should contain a quote or a backslash, but a tag
    # name is user-supplied and a broken define here would fail the build with a message pointing at the
    # compiler rather than at the tag.
    cleaned = value.replace("\\", "").replace('"', "")
    return '\\"{}\\"'.format(cleaned)


env.Append(
    CPPDEFINES=[
        ("ECU_BUILD_GIT_DESCRIBE", _quote(_describe())),
        ("ECU_BUILD_GIT_SHA", _quote(_sha())),
        ("ECU_BUILD_TIMESTAMP", _quote(_timestamp())),
        ("ECU_BUILD_HOST", _quote(platform.node() or "unknown")),
    ]
)

print("Build provenance: {} ({})".format(_describe(), _timestamp()))

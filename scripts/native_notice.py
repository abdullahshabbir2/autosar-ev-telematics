"""Fail the `native` PlatformIO environment with a pointer to the real test runner.

Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.

The host tests are compiled by tools/run_native_tests.py rather than by PlatformIO, because that script links
every platform-independent source into each suite -- so a platform dependency leaking above the MCAL becomes an
undefined-reference error instead of something a reviewer has to notice. PlatformIO would satisfy those
references from the framework and the leak would pass unremarked.

This script exists only so that `pio run -e native` says that, rather than failing later with a compile error
about a missing Arduino header.
"""

import sys

sys.stderr.write(
    "\n"
    "The 'native' environment is not the host test runner.\n"
    "\n"
    "  python tools/run_native_tests.py            # every suite\n"
    "  python tools/run_native_tests.py test_odo   # one suite\n"
    "\n"
    "See the comment at the top of platformio.ini for why the tests are built directly\n"
    "rather than through PlatformIO.\n"
    "\n"
)
sys.exit(1)

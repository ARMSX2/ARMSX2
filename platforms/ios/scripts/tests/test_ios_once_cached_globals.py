#!/usr/bin/env python3
"""A dispatch_once cache in an MRC file must own what it stores.

ARMSX2Bridge.mm is compiled without -fobjc-arc, so a convenience constructor
assigned to a static inside dispatch_once is autoreleased: the first call reads a
live object, the pool drains, and every later call reads freed memory. It shipped
twice — the shader-pack accept-list and the skin-package one — and reached a device
as two different crashes from one site, a SIGSEGV and an unrecognized selector on
whatever reused the memory.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BRIDGE = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm"

# +1 already, or explicitly retained. Anything else a class returns is autoreleased.
OWNING = re.compile(r"\[\[\s*\w+\s+(alloc|new)\b|\bretain\s*\]")
# Collection and boxed literals are autoreleased. @"..." alone is a constant object.
AUTORELEASED_LITERAL = re.compile(r"=\s*@[\[{(]")
CONVENIENCE_CALL = re.compile(r"=\s*\[\s*\w+\s+\w+")


def once_blocks(text):
    """Yield (line_number, body) for each dispatch_once block."""
    for m in re.finditer(r"dispatch_once\s*\(\s*&\s*\w+\s*,\s*\^\s*\{", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield text.count("\n", 0, m.start()) + 1, text[m.end():i]


class OnceCachedGlobals(unittest.TestCase):
    def test_bridge_exists(self):
        self.assertTrue(BRIDGE.is_file(), f"missing {BRIDGE}")

    def test_bridge_is_still_mrc(self):
        """If the target ever gains ARC this guard is obsolete rather than wrong."""
        cmake = (ROOT / "platforms/ios/app/src/main/cpp/CMakeLists.txt").read_text()
        self.assertNotIn("fobjc-arc", cmake)
        self.assertNotIn("OBJC_ARC", cmake)

    def test_no_autoreleased_object_cached_in_a_once_block(self):
        text = BRIDGE.read_text()
        offenders = []
        for line_no, body in once_blocks(text):
            for offset, line in enumerate(body.split("\n")):
                code = line.split("//")[0]
                if "=" not in code or OWNING.search(code):
                    continue
                if AUTORELEASED_LITERAL.search(code) or CONVENIENCE_CALL.search(code):
                    offenders.append(f"  ARMSX2Bridge.mm:{line_no + offset}: {code.strip()}")
        self.assertEqual(
            offenders,
            [],
            "dispatch_once stored an autoreleased object in a static; it dangles once "
            "the pool drains. Use [[Class alloc] init...] instead:\n" + "\n".join(offenders),
        )


if __name__ == "__main__":
    unittest.main()

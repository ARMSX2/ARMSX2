#!/usr/bin/env python3
"""A bundled shader must not divide by an unguarded integer prescale.

Above roughly 1.5x internal resolution the source is taller than the output, so floor() of
the ratio is zero and the frame goes black. The guard is a max(..., 1.0) around the prescale.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
PRESETS = ROOT / "platforms/ios/app/src/main/assets/shaders/presets"

# A prescale is a floor() over the output/source height ratio, in either the divide or
# the reciprocal-multiply spelling. SourceSize.w and .zw are 1/height and 1/size.
PRESCALE = re.compile(
    r"floor\s*\([^;]*?OutputSize\.[xy]{1,2}[^;]*?"
    r"(?:/\s*[a-zA-Z_.]*SourceSize\.[xy]{1,2}|\*\s*[a-zA-Z_.]*SourceSize\.[zw]{1,2})",
    re.IGNORECASE,
)


def shaders():
    """Every file a preset can pull in, since a stage can #include the prescale from a header."""
    return sorted(
        path
        for suffix in ("*.slang", "*.inc", "*.h")
        for path in PRESETS.rglob(suffix)
    )


class PrescaleGuard(unittest.TestCase):
    def test_presets_exist(self):
        self.assertTrue(shaders(), f"no .slang files under {PRESETS}")

    def test_every_prescale_is_clamped(self):
        offenders = []
        for path in shaders():
            for number, line in enumerate(path.read_text().splitlines(), 1):
                code = line.split("//")[0]
                if not PRESCALE.search(code):
                    continue
                # clamp() carries its own lower bound; max() is the direct guard.
                if "max(" in code or "clamp(" in code:
                    continue
                offenders.append(f"  {path.relative_to(PRESETS)}:{number}: {code.strip()}")
        self.assertEqual(
            offenders,
            [],
            "a derived integer prescale is not clamped to at least 1. Wrap it in max(..., 1.0) "
            "and record the change in patches/slang-shaders-prescale-zero-guard.patch:\n"
            + "\n".join(offenders),
        )

    def test_the_two_known_shaders_still_carry_their_guard(self):
        """Named directly, and the clamp has to sit on the prescale line itself."""
        for relative in (
            "crt/shaders/crt-aperture.slang",
            "pixel-art-scaling/shaders/sharp-bilinear.slang",
        ):
            path = PRESETS / relative
            self.assertTrue(path.is_file(), f"missing {relative}")
            guarded = [
                line
                for line in path.read_text().splitlines()
                if PRESCALE.search(line.split("//")[0])
                and ("max(" in line or "clamp(" in line)
            ]
            self.assertTrue(
                guarded,
                f"{relative} has no clamped prescale line; see "
                "patches/slang-shaders-prescale-zero-guard.patch",
            )


if __name__ == "__main__":
    unittest.main()

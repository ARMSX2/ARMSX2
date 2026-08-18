#!/usr/bin/env python3
"""A bundled shader must not divide by an unguarded integer prescale.

Several slang shaders derive a whole-number prescale from output height over source
height and then divide by it. That is safe in RetroArch, where the source is always a
small console framebuffer being scaled up. PCSX2 renders internally at up to 8x, so
above roughly 1.5x the source is taller than the screen, the ratio falls under one,
floor() returns zero and the frame turns black. Reported on an iPhone SE 2 at 2x,
reproduced in the simulator at 3x.

The guard is a max(..., 1.0) around the prescale, which sharp-bilinear-simple and
crt-geom already carry upstream. This test exists because re-syncing a preset from
upstream would silently drop it again.
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
    return sorted(PRESETS.rglob("*.slang"))


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
            "a derived integer prescale is not clamped to at least 1. Above ~1.5x internal "
            "resolution it floors to zero and the frame goes black. Wrap it in max(..., 1.0) "
            "and record the change in patches/slang-shaders-prescale-zero-guard.patch:\n"
            + "\n".join(offenders),
        )

    def test_the_two_known_shaders_still_carry_their_guard(self):
        """Named directly, so a rename or a re-sync cannot quietly drop the fix."""
        for relative in (
            "crt/shaders/crt-aperture.slang",
            "pixel-art-scaling/shaders/sharp-bilinear.slang",
        ):
            path = PRESETS / relative
            self.assertTrue(path.is_file(), f"missing {relative}")
            self.assertIn(
                "max(",
                path.read_text(),
                f"{relative} lost its prescale guard; see "
                "patches/slang-shaders-prescale-zero-guard.patch",
            )


if __name__ == "__main__":
    unittest.main()

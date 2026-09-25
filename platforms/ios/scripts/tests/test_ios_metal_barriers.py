import re
import unittest
from pathlib import Path


# Repo root. The Metal renderer used to be vendored under platforms/ios; it is
# shared core now, so everything below is relative to the top of the tree.
ROOT = Path(__file__).resolve().parents[4]


class IOSMetalBarrierTests(unittest.TestCase):
    def test_ios_does_not_advertise_unsupported_texture_barriers(self):
        implementation = (
            ROOT / "pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm"
        ).read_text(encoding="utf-8")

        self.assertRegex(
            implementation,
            r"(?m)^#if TARGET_OS_IPHONE\n"
            r"\s*m_features\.texture_barrier = false;\n"
            r"#else\n"
            r"\s*m_features\.texture_barrier = true;\n"
            r"#endif$",
        )
        self.assertNotIn("[enc textureBarrier];", implementation)

    def test_bc_textures_follow_the_gpu(self):
        implementation = (
            ROOT / "pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm"
        ).read_text(encoding="utf-8")

        # Most iOS GPUs cannot sample BC, and creating a BC texture there aborts.
        self.assertTrue("supportsBCTextureCompression" in implementation, "BC support must come from the GPU")
        self.assertIsNone(re.search(r"m_features\.(dxt|bptc)_textures = true;", implementation))


if __name__ == "__main__":
    unittest.main()

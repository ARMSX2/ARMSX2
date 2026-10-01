import unittest

from ios_source import ROOT, block, without_comments


REPLACEMENTS = ROOT / "pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp"


class TextureCacheBudget(unittest.TestCase):
    def test_ios_sizes_the_cache_from_the_app_memory_limit(self):
        code = without_comments(REPLACEMENTS.read_text(encoding="utf-8"))
        budget = block(code, "size_t GSTextureReplacements::GetReplacementCacheBudget()")
        self.assertIn(
            "os_proc_available_memory()", budget,
            "iOS kills an app long before physical RAM runs out, so the budget has to come "
            "from what the app may still allocate")


if __name__ == "__main__":
    unittest.main()

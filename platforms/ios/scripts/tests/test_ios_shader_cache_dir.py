import re
import unittest

from ios_source import CPP, without_comments


DELEGATE = CPP / "IOS/AppDelegate.mm"
LIBRASHADER_CMAKE = CPP / "3rdparty/librashader/CMakeLists.txt"

CACHE_ENV = "XDG_CACHE_HOME"
REPORT = "PCSX2 iOS: shader cache "
LEVEL_CALL = "Log::SetConsoleOutputLevel"

# Markers AppDelegate.mm already carries; the test only fails on one outside this set.
KNOWN_TAGS = {
    "@@LOG_SINK@@", "@@LOG_UNIFIED@@", "@@BUNDLE_ID@@", "@@BUILD_ID@@", "@@TEST_MARKER@@",
    "@@FF_FIX@@", "@@DIAG_MODE@@", "@@BIOS_GATE@@", "@@CFG@@", "@@DYLD_MAP@@",
}


class ShaderCacheDirectoryPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = DELEGATE.read_text(encoding="utf-8")
        cls.code = without_comments(cls.source)

    def test_the_cache_directory_is_redirected_from_the_system_path(self):
        setenv = re.search(r'setenv\(\s*"' + CACHE_ENV + r'"\s*,\s*([^;]+)\)', self.code)
        self.assertIsNotNone(
            setenv,
            f"AppDelegate no longer sets {CACHE_ENV}, so librashader's cache lands outside "
            "Library/Caches")

        self.assertIn(
            "NSCachesDirectory", self.code,
            f"the {CACHE_ENV} value is no longer derived from NSCachesDirectory")
        self.assertNotRegex(
            setenv.group(1), r'"\s*/',
            f"{CACHE_ENV} is set from a literal path, but the container path changes on every "
            "install")

    def test_the_observation_line_survives_and_reaches_the_console_writer(self):
        i_report = self.source.find(REPORT)
        self.assertNotEqual(
            i_report, -1,
            f"the {REPORT!r} line is gone from AppDelegate.mm")

        i_level = self.source.find(LEVEL_CALL)
        self.assertNotEqual(i_level, -1, f"{LEVEL_CALL} is gone from AppDelegate.mm")
        self.assertLess(
            i_level, i_report,
            f"the {REPORT!r} line comes before {LEVEL_CALL}, so the console writer drops it")

        emitter = re.search(r"[^\n]*" + re.escape(REPORT) + r"[^\n]*", self.source).group(0)
        self.assertIn(
            "Console.", emitter,
            "the shader cache line is not written through Console.*")

    def test_no_new_stderr_marker(self):
        added = set(re.findall(r"@@[A-Z_]+@@", self.source)) - KNOWN_TAGS
        self.assertFalse(
            added,
            f"AppDelegate.mm has new stderr markers {sorted(added)} outside KNOWN_TAGS")

    def test_the_cargo_feature_set_still_builds_metal_only(self):
        cmake = LIBRASHADER_CMAKE.read_text(encoding="utf-8")
        code = "\n".join(l for l in cmake.split("\n") if not l.lstrip().startswith("#"))

        self.assertIn(
            "--no-default-features", code,
            "the librashader cargo build no longer passes --no-default-features, which turns on "
            "runtimes that use librashader-cache")
        features = set(re.findall(r"--features\s+(\S+)", code))
        self.assertEqual(
            features, {"runtime-metal"},
            f"the librashader cargo features are {sorted(features)}, not only runtime-metal")


if __name__ == "__main__":
    unittest.main()

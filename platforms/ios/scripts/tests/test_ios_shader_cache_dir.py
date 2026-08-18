import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
DELEGATE = ROOT / "platforms/ios/app/src/main/cpp/IOS/AppDelegate.mm"
LIBRASHADER_CMAKE = ROOT / "platforms/ios/app/src/main/cpp/3rdparty/librashader/CMakeLists.txt"

CACHE_ENV = "XDG_CACHE_HOME"
REPORT = "PCSX2 iOS: shader cache "
LEVEL_CALL = "Log::SetConsoleOutputLevel"

# Phase 1 owns the removal of these; ROADMAP.md:52 forbids the iOS release build growing the
# set, so this file asserts no addition rather than emptiness.
KNOWN_TAGS = {
    "@@LOG_SINK@@", "@@LOG_UNIFIED@@", "@@BUNDLE_ID@@", "@@BUILD_ID@@", "@@TEST_MARKER@@",
    "@@FF_FIX@@", "@@DIAG_MODE@@", "@@BIOS_GATE@@", "@@CFG@@", "@@DYLD_MAP@@",
}

SILENT = ("platform-dirs discards a relative XDG_CACHE_HOME without a diagnostic "
          "(platform-dirs-0.3.0/src/lib.rs:25-33, :88-90) and returns to $HOME/.cache, so a "
          "wrong value here looks exactly like a working one from the outside.")


def without_comments(source):
    """What the compiler sees, so prose naming a symbol cannot satisfy a check."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def fetched_librashader_trees():
    return sorted(ROOT.glob("platforms/ios/build-*/_deps/librashader_src-src"))


class ShaderCacheDirectoryPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = DELEGATE.read_text(encoding="utf-8")
        cls.code = without_comments(cls.source)

    def test_the_cache_directory_is_redirected_from_the_system_path(self):
        setenv = re.search(r'setenv\(\s*"' + CACHE_ENV + r'"\s*,\s*([^;]+)\)', self.code)
        self.assertIsNotNone(
            setenv,
            f"AppDelegate no longer sets {CACHE_ENV}. librashader resolves its cache through "
            "platform-dirs, whose Apple branch is gated on macOS alone "
            "(platform-dirs-0.3.0/src/lib.rs:37), so iOS falls to the XDG branch and the persy "
            "database lands at <container>/.cache/librashader — outside Library/Caches, which "
            "means the OS cannot purge it and iCloud does back it up.")

        self.assertIn(
            "NSCachesDirectory", self.code,
            f"the {CACHE_ENV} value is no longer derived from NSSearchPathForDirectoriesInDomains "
            "with NSCachesDirectory. " + SILENT)
        self.assertNotRegex(
            setenv.group(1), r'"\s*/',
            f"{CACHE_ENV} is being set from a literal path. The container UUID changes on every "
            "install, so a literal is wrong on the second launch. " + SILENT)

    def test_the_observation_line_survives_and_reaches_the_console_writer(self):
        i_report = self.source.find(REPORT)
        self.assertNotEqual(
            i_report, -1,
            f"the {REPORT!r} line is gone. Without it the claim that the cache moved is an "
            "unverifiable source assertion: a sideloaded container is not reachable by devicectl "
            "and log collect needs root, so the container's own report is the only witness.")

        i_level = self.source.find(LEVEL_CALL)
        self.assertNotEqual(i_level, -1, f"{LEVEL_CALL} is gone; update this test")
        self.assertLess(
            i_level, i_report,
            f"the {REPORT!r} line moved above {LEVEL_CALL}. The console writer is dead before that "
            "call, so the line is dropped in silence — which on device looks identical to the "
            "redirect having failed.")

        emitter = re.search(r"[^\n]*" + re.escape(REPORT) + r"[^\n]*", self.source).group(0)
        self.assertIn(
            "Console.", emitter,
            "the observation is no longer emitted through Console.*. ROADMAP.md:59 prescribes the "
            "Console route precisely so the iOS release build stops shipping raw stderr traces.")

    def test_this_change_added_no_stderr_marker(self):
        added = set(re.findall(r"@@[A-Z_]+@@", self.source)) - KNOWN_TAGS
        self.assertFalse(
            added,
            f"AppDelegate.mm grew a new stderr marker: {sorted(added)}. ROADMAP.md:52 requires the "
            "iOS release build to ship none of them, so adding one leaves Phase 1 with more to "
            "convert than it started with.")

    def test_the_cargo_feature_set_still_builds_metal_only(self):
        cmake = LIBRASHADER_CMAKE.read_text(encoding="utf-8")
        code = "\n".join(l for l in cmake.split("\n") if not l.lstrip().startswith("#"))

        self.assertIn(
            "--no-default-features", code,
            "the librashader cargo build no longer disables default features, which turns on "
            "every runtime including ones that cache, so the "
            "shader cache has stopped being dead code and needs observing on a device.")
        features = set(re.findall(r"--features\s+(\S+)", code))
        self.assertEqual(
            features, {"runtime-metal"},
            f"the librashader cargo features are now {sorted(features)}. Every runtime except "
            "Metal reaches librashader-cache, so the shader cache has "
            "stopped being dead code and needs observing on a device.")

    def test_the_metal_runtime_still_never_reaches_the_cache_crate(self):
        trees = fetched_librashader_trees()
        if not trees:
            self.skipTest("no fetched librashader tree on disk; configure an iOS build to run this")

        for tree in trees:
            mtl = tree / "librashader-runtime-mtl"
            if not mtl.is_dir():
                continue
            sources = list(mtl.rglob("*.rs")) + list(mtl.rglob("*.toml"))
            hits = [f"{p.relative_to(tree)}" for p in sources
                    if re.search(r"librashader[_-]cache", p.read_text(encoding="utf-8"))]
            self.assertEqual(
                hits, [],
                f"{tree.name}: librashader-runtime-mtl now references librashader-cache in {hits}. "
                "The pin bumped and the Metal runtime started caching, so the redirect in "
                "AppDelegate.mm is live rather than pre-emptive. Check a device and "
                "expect Library/Caches/librashader to exist this time.")


if __name__ == "__main__":
    unittest.main()

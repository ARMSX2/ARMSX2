"""SettingsStore.init() must not reach a `.shared` singleton, its own or anyone's.

`static let shared` runs init() inside swift_once, so a SettingsStore.shared read that
init() reaches re-enters the once token. ALLOWLIST holds the accesses that cannot.
"""

import re
import unittest

from ios_source import SWIFT, block, without_comments


MODELS = SWIFT / "Models"

SHARED_TOKEN = re.compile(r"([A-Z]\w*)\s*\.\s*shared(?:\.[a-zA-Z_]\w*)?")
BARE_START = re.compile(r"FrameTimeDynamicResolutionController\.shared\.setEnabled")
MIGRATE_CALL = re.compile(r"Self\s*\.\s*migrateFramePacingOptimalDefaultV1\s*\(\s*\)")

# A raw requestGraphicsApply(). The guarded one does not match: the paren has to
# follow `Apply` for this to fire.
RAW_APPLY = re.compile(r"\brequestGraphicsApply\(\)")


class TestIosSettingsStoreInitNoSharedAccess(unittest.TestCase):

    SETTINGS_STORE = MODELS / "SettingsStore.swift"
    FRAME_PACING = MODELS / "SettingsStore+FramePacing.swift"
    FTDRC = MODELS / "FrameTimeDynamicResolutionController.swift"
    VPAD_SKIN_LIB = MODELS / "VPadSkinLibraryStore.swift"

    # Each entry is a regex, the scan it answers for, and why the access cannot re-enter
    # the once token. The regex has to match over the token it excuses.
    ALLOWLIST = [
        # The deferral moves the call to the next run loop turn, by which point
        # swift_once has released and the instance is whole.
        (r"DispatchQueue\.main\.async\s*\{[^}]*FrameTimeDynamicResolutionController\.shared\.setEnabled",
         "init",
         "the adaptive controller starts on the next run loop turn, not inside the once body "
         "(held up by test_adaptive_controller_start_is_deferred)"),

        # VPadSkinLibraryStore declares no init of its own and names SettingsStore
        # nowhere, so touching its singleton starts a once token that cannot reach
        # back into this one.
        (r"VPadSkinLibraryStore\.shared\.adoptLegacySelection",
         "init",
         "VPadSkinLibraryStore has no init body and no SettingsStore reference, so its once "
         "token cannot re-enter this one; the callee body is walked below"),

        # Callee only: setEnabled's read is safe because its call site defers, and the
        # same read in the init body deadlocks.
        (r"SettingsStore\.shared\.upscaleMultiplier",
         "callee",
         "setEnabled reads it, which is safe only because the call site defers "
         "(held up by test_adaptive_controller_start_is_deferred)"),
    ]

    def setUp(self):
        for path in (self.SETTINGS_STORE, self.FRAME_PACING, self.FTDRC, self.VPAD_SKIN_LIB):
            self.assertTrue(path.exists(), "%s missing, so the repo root is wrong" % path.name)
        self.settings_store = self.SETTINGS_STORE.read_text(encoding="utf-8")
        self.frame_pacing = self.FRAME_PACING.read_text(encoding="utf-8")
        self.ftdrc = self.FTDRC.read_text(encoding="utf-8")
        self.vpad_skin_lib = self.VPAD_SKIN_LIB.read_text(encoding="utf-8")

    def _init_body(self):
        return block(self.settings_store, "private init()")

    def _migrate_body(self):
        return block(self.frame_pacing, "func migrateFramePacingOptimalDefaultV1(")

    def _walk_transitive_callees(self, init_body, depth=2):
        """Every `Self.foo()` and `X.shared.foo()` reachable from init, to `depth`.

        A callee with no Swift body in these four files is skipped.
        """
        owner_files = [
            ("SettingsStore+FramePacing", self.frame_pacing),
            ("SettingsStore", self.settings_store),
            ("FrameTimeDynamicResolutionController", self.ftdrc),
            ("VPadSkinLibraryStore", self.vpad_skin_lib),
        ]
        seen = set()
        results = []

        def find_callee_body(name):
            for label, src in owner_files:
                for prefix in (r"    static\s+func\s+", r"    func\s+"):
                    pat = (r"(?m)^" + prefix + re.escape(name)
                           + r"\s*(?:<[^>]*>)?\s*\([^)]*\)"
                           r"(?:\s*->\s*[^{]+?)?\s*\{(?P<body>.*?)^    \}\n")
                    m = re.search(pat, src, re.DOTALL)
                    if m:
                        return label, m.group("body")
            return "", ""

        def walk(body, current):
            if current > depth:
                return
            for m in re.finditer(r"\bSelf\s*\.\s*([a-zA-Z_]\w*)\s*\(", body):
                callee = m.group(1)
                if ("Self", callee) in seen:
                    continue
                seen.add(("Self", callee))
                label, callee_body = find_callee_body(callee)
                if callee_body:
                    results.append((label, "Self.%s" % callee, callee_body))
                    walk(callee_body, current + 1)
            for m in re.finditer(r"\b([A-Z]\w*)\s*\.\s*shared\s*\.\s*([a-zA-Z_]\w*)\s*\(", body):
                owner, callee = m.group(1), m.group(2)
                if (owner, callee) in seen:
                    continue
                seen.add((owner, callee))
                label, callee_body = find_callee_body(callee)
                if callee_body:
                    results.append((label or owner, "%s.shared.%s" % (owner, callee), callee_body))
                    walk(callee_body, current + 1)

        walk(init_body, 1)
        return results

    def _assert_no_unallowed_shared(self, body, where, scan):
        """Every `.shared` token needs an ALLOWLIST entry whose match spans the token.

        The window only has to reach a `DispatchQueue.main.async` opener above the token.
        """
        for match in SHARED_TOKEN.finditer(body):
            start = max(0, match.start() - 400)
            window = body[start:match.end() + 400]
            at, to = match.start() - start, match.end() - start
            covered = any(
                m.start() <= at and m.end() >= to
                for pat, entry_scan, _ in self.ALLOWLIST if entry_scan == scan
                for m in re.finditer(pat, window)
            )
            line = body[:match.start()].count("\n") + 1
            self.assertTrue(
                covered,
                "%s reaches `%s` at line +%d with no ALLOWLIST entry covering it"
                % (where, match.group(0), line),
            )

    def test_migration_function_does_not_reference_settings_store_shared(self):
        """The frame pacing migration runs from init, so it writes the INI by hand."""
        body = without_comments(self._migrate_body())
        for pattern in (r"SettingsStore\s*\.\s*shared",
                        r"Self\s*\.\s*shared",
                        r"\.shared\s*\.\s*applyFramePacingPreset",
                        r"\bshared\s*\.\s*applyFramePacingPreset"):
            self.assertNotRegex(
                body, pattern,
                "migrateFramePacingOptimalDefaultV1() matches %r, but it runs inside "
                "SettingsStore.init()" % pattern,
            )

    def test_migration_function_writes_optimal_table_directly_to_ini(self):
        """The migration writes the Optimal preset's four INI keys and scalar by hand."""
        body = self._migrate_body()
        for pattern, label in (
            (r'"EmuCore/GS"[\s\S]*?"VsyncQueueSize"[\s\S]*?value:\s*4', "VsyncQueueSize=4"),
            (r'"SPU2/Output"[\s\S]*?"OutputLatencyMS"[\s\S]*?value:\s*15', "OutputLatencyMS=15"),
            (r'"SPU2/Output"[\s\S]*?"BufferMS"[\s\S]*?value:\s*50', "BufferMS=50"),
            (r'"EmuCore/GS"[\s\S]*?"SyncToHostRefreshRate"[\s\S]*?value:\s*false',
             "SyncToHostRefreshRate=false"),
            (r'"Framerate"[\s\S]*?"NominalScalar"[\s\S]*?value:\s*1\.0', "NominalScalar=1.0"),
        ):
            self.assertRegex(
                body, pattern,
                "migrateFramePacingOptimalDefaultV1() does not write %s" % label,
            )

    def test_migration_is_called_at_top_of_init(self):
        """The migration writes INI values the rest of init then reads back, so it
        has to run before the first read or the stored properties are stale."""
        init_body = self._init_body()
        call = MIGRATE_CALL.search(init_body)
        self.assertIsNotNone(
            call,
            "SettingsStore.init() does not call Self.migrateFramePacingOptimalDefaultV1()",
        )
        first_read = re.search(r"ARMSX2Bridge\.getINI", init_body)
        if first_read is None:
            self.skipTest("init() reads no INI keys, so there is nothing to order against")
        self.assertLess(
            call.start(), first_read.start(),
            "Self.migrateFramePacingOptimalDefaultV1() runs after the first ARMSX2Bridge.getINI "
            "read in init()",
        )

    def test_init_does_not_call_migrate_twice(self):
        """A second call would write the INI again after init has read it."""
        calls = MIGRATE_CALL.findall(self.settings_store)
        self.assertEqual(
            len(calls), 1,
            "SettingsStore.swift calls Self.migrateFramePacingOptimalDefaultV1() %d times, "
            "expected once" % len(calls),
        )

    def test_init_body_has_no_unallowed_shared_access(self):
        """A `.shared` access written in the init body needs an ALLOWLIST entry."""
        self._assert_no_unallowed_shared(
            without_comments(self._init_body()), "SettingsStore.init()", "init")

    def test_setting_commit_routes_through_the_guarded_helper(self):
        """A setter must not reload GS settings while init is still loading them."""
        body = without_comments(block(self.settings_store, "private func commit<T>("))
        self.assertRegex(
            body, r"\brequestGraphicsApplyGuarded\(\)",
            "commit() does not call requestGraphicsApplyGuarded()",
        )
        self.assertNotRegex(
            body, RAW_APPLY,
            "commit() calls requestGraphicsApply() instead of requestGraphicsApplyGuarded()",
        )
        guard = without_comments(block(self.settings_store, "func requestGraphicsApplyGuarded("))
        self.assertRegex(
            guard, r"guard\s+!suppressINIWrites\s+else\s*\{\s*return\s*\}",
            "requestGraphicsApplyGuarded() does not return early while suppressINIWrites is set",
        )

    def test_adaptive_controller_start_is_deferred(self):
        """setEnabled reads SettingsStore.shared.upscaleMultiplier, so starting the
        controller from init has to wait for the next run loop turn."""
        init_body = self._init_body()
        starts = list(BARE_START.finditer(init_body))
        self.assertGreaterEqual(
            len(starts), 1,
            "SettingsStore.init() does not call FrameTimeDynamicResolutionController.shared."
            "setEnabled",
        )
        for m in starts:
            preceding = init_body[max(0, m.start() - 200):m.start()]
            opener = preceding.rfind("DispatchQueue.main.async")
            self.assertGreater(
                opener, preceding.rfind("}"),
                "a FrameTimeDynamicResolutionController.shared.setEnabled call in init() is not "
                "inside DispatchQueue.main.async",
            )

    def test_init_transitive_callees_have_no_unallowed_shared_access(self):
        """The functions init() reaches, two calls deep, need the same ALLOWLIST cover."""
        init_body = self._init_body()
        callees = self._walk_transitive_callees(init_body, depth=2)
        names = [name for _, name, _ in callees]
        self.assertTrue(
            any("migrateFramePacingOptimalDefaultV1" in name for name in names),
            "the walk did not reach Self.migrateFramePacingOptimalDefaultV1. Reached: %s" % names,
        )
        for label, name, body in callees:
            self._assert_no_unallowed_shared(
                without_comments(body), "`%s` in %s.swift" % (name, label), "callee")


if __name__ == "__main__":
    unittest.main()

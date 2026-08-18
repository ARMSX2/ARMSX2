"""SettingsStore.init() must not reach a `.shared` singleton, its own or anyone's.

`static let shared` runs init() inside swift_once. Anything init() reaches that
reads SettingsStore.shared re-enters that in-flight token: iOS 26 traps in
libdispatch, iOS 27 aborts on a selector the half-built instance does not answer
to yet. It shipped twice from two different lines of the same init, the second
time through a callee rather than the init body, so this walks the call graph
instead of naming one function the way the first fix did.

ALLOWLIST is the escape hatch. An entry says why the access cannot re-enter and
names what holds that reason up, and adding one is the deliberate act. The
default for a new `.shared` reference reachable from init() is to restructure.
"""

import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
MODELS = ROOT / "platforms/ios/app/src/main/swift/Models"

INIT_BODY = re.compile(r"(?m)^    private init\(\) \{(?P<body>.*?)^    \}\n", re.DOTALL)
MIGRATE_BODY = re.compile(
    r"(?m)^    static func migrateFramePacingOptimalDefaultV1\(\) \{(?P<body>.*?)^    \}\n",
    re.DOTALL,
)
COMMIT_BODY = re.compile(
    r"(?m)^    private func commit<T>\([^)]*\) \{(?P<body>.*?)^    \}\n", re.DOTALL
)
GUARDED_BODY = re.compile(
    r"(?m)^    func requestGraphicsApplyGuarded\(\) \{(?P<body>.*?)^    \}\n", re.DOTALL
)
SHARED_TOKEN = re.compile(r"([A-Z]\w*)\s*\.\s*shared(?:\.[a-zA-Z_]\w*)?")
DEFERRED_START = re.compile(
    r"DispatchQueue\.main\.async\s*\{[^}]*?"
    r"FrameTimeDynamicResolutionController\.shared\.setEnabled",
    re.DOTALL,
)
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

    # Each entry is a regex, the scan it answers for, and the reason it is allowed
    # to stay. The reason has to say why the access cannot re-enter the once token,
    # and name the thing that keeps that true, so the next reader can check it
    # rather than trust it. An entry has to match over the token it excuses, not
    # merely somewhere near it, or one entry ends up covering its neighbours.
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

        # This is the read that deadlocked. It is callee only on purpose: inside
        # setEnabled it survives because the call site defers, and written into the
        # init body it is the original crash again.
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

    def _body(self, pattern, text, what):
        """The brace-matched body of one declaration, or a failure naming it."""
        m = pattern.search(text)
        self.assertIsNotNone(m, "cannot find %s, so it was renamed, moved or re-indented "
                                "and this test needs revisiting" % what)
        return m.group("body")

    def _init_body(self):
        return self._body(INIT_BODY, self.settings_store, "private init() in SettingsStore.swift")

    def _migrate_body(self):
        return self._body(MIGRATE_BODY, self.frame_pacing,
                          "migrateFramePacingOptimalDefaultV1() in SettingsStore+FramePacing.swift")

    @staticmethod
    def _strip_line_comments(text):
        """Drop `//` tails. These functions name the forbidden call in prose to say
        why it is forbidden, and only live code should be flagged."""
        out = []
        for line in text.splitlines():
            i = line.find("//")
            out.append(line[:i] if i >= 0 else line)
        return "\n".join(out)

    def _walk_transitive_callees(self, init_body, depth=2):
        """Every `Self.foo()` and `X.shared.foo()` reachable from init, to `depth`.

        Regex rather than a parse: the call graph out of init is shallow, and the
        other guards here are stdlib only for the same reason. A callee with no
        Swift body in these four files is skipped, since a hazard in the C++ bridge
        or an enum namespace is not a swift_once one.
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
        """Every `.shared` token needs an ALLOWLIST entry spanning it.

        The entry has to match over the token's own position, not just land inside
        the window. Matching anywhere in the window lets an allowed access vouch for
        a new one written next to it, which is most of what the end of init is. The
        window itself is only there to reach the `DispatchQueue.main.async` opener
        above the token it excuses.
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
                "%s reaches `%s` at line +%d with no ALLOWLIST entry covering it. It runs "
                "inside swift_once for SettingsStore.shared, so either restructure it out of "
                "init, or add an entry saying why it cannot re-enter and what holds that up."
                % (where, match.group(0), line),
            )

    def test_migration_function_does_not_reference_settings_store_shared(self):
        """The frame pacing migration runs from init, so it writes the INI by hand."""
        body = self._strip_line_comments(self._migrate_body())
        for pattern in (r"SettingsStore\s*\.\s*shared",
                        r"Self\s*\.\s*shared",
                        r"\.shared\s*\.\s*applyFramePacingPreset",
                        r"\bshared\s*\.\s*applyFramePacingPreset"):
            self.assertNotRegex(
                body, pattern,
                "migrateFramePacingOptimalDefaultV1() matches %r. It runs inside "
                "SettingsStore.init(), so the store it wants is the one still being built."
                % pattern,
            )

    def test_migration_function_writes_optimal_table_directly_to_ini(self):
        """Writing by hand is only a fix if it still writes. The Optimal preset is
        four INI keys and a scalar, and delegating them away is what broke it."""
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
                "migrateFramePacingOptimalDefaultV1() no longer writes %s, so the migration "
                "is a no-op for the case it exists to handle." % label,
            )

    def test_migration_is_called_at_top_of_init(self):
        """The migration writes INI values the rest of init then reads back, so it
        has to run before the first read or the stored properties are stale."""
        init_body = self._init_body()
        call = MIGRATE_CALL.search(init_body)
        self.assertIsNotNone(
            call,
            "SettingsStore.init() does not call Self.migrateFramePacingOptimalDefaultV1(), "
            "so a fresh install never gets the Optimal frame pacing default.",
        )
        first_read = re.search(r"ARMSX2Bridge\.getINI", init_body)
        if first_read is None:
            self.skipTest("init() reads no INI keys, so there is nothing to order against")
        self.assertLess(
            call.start(), first_read.start(),
            "Self.migrateFramePacingOptimalDefaultV1() runs after the first ARMSX2Bridge.getINI "
            "read in init(), so the properties below it hold pre-migration values.",
        )

    def test_init_does_not_call_migrate_twice(self):
        """A second call left behind by an edit would rewrite the INI, and one put
        back at the bottom of init would undo the ordering above."""
        calls = MIGRATE_CALL.findall(self.settings_store)
        self.assertEqual(
            len(calls), 1,
            "SettingsStore.swift calls Self.migrateFramePacingOptimalDefaultV1() %d times, "
            "and it wants exactly one." % len(calls),
        )

    def test_init_body_has_no_unallowed_shared_access(self):
        """The init body itself. A new `FooBar.shared.baz()` here has nothing
        vouching for it and fails."""
        self._assert_no_unallowed_shared(
            self._strip_line_comments(self._init_body()), "SettingsStore.init()", "init")

    def test_setting_commit_routes_through_the_guarded_helper(self):
        """A setter must not reload GS settings while init is still loading them.

        Settings used to carry an onSet closure each and the guard was checked in
        all of them. They are one `commit()` now, with appliesGraphics on the
        descriptor deciding, so there is a single place left to get this wrong.
        """
        body = self._strip_line_comments(
            self._body(COMMIT_BODY, self.settings_store,
                       "private func commit<T> in SettingsStore.swift"))
        self.assertRegex(
            body, r"\brequestGraphicsApplyGuarded\(\)",
            "commit() no longer reaches requestGraphicsApplyGuarded(), so a setting written "
            "during init can reload GS settings on top of the values init is reading.",
        )
        self.assertNotRegex(
            body, RAW_APPLY,
            "commit() calls requestGraphicsApply() directly. The guarded one exists because "
            "that call has to be dropped while suppressINIWrites is up.",
        )
        guard = self._strip_line_comments(
            self._body(GUARDED_BODY, self.settings_store,
                       "func requestGraphicsApplyGuarded in SettingsStore.swift"))
        self.assertRegex(
            guard, r"guard\s+!suppressINIWrites\s+else\s*\{\s*return\s*\}",
            "requestGraphicsApplyGuarded() no longer checks suppressINIWrites, which is the "
            "whole of what it does that requestGraphicsApply() does not.",
        )

    def test_adaptive_controller_start_is_deferred(self):
        """setEnabled reads SettingsStore.shared.upscaleMultiplier, so starting the
        controller from init has to wait for the next run loop turn.

        This was the second crash: the first fix bounded itself to the migration
        function and this line sat four hundred lines below it, doing the same thing.
        """
        init_body = self._init_body()
        self.assertRegex(
            init_body, DEFERRED_START,
            "SettingsStore.init() starts FrameTimeDynamicResolutionController without a "
            "DispatchQueue.main.async around it. setEnabled reads "
            "SettingsStore.shared.upscaleMultiplier, which re-enters the once token this "
            "init is running inside: a libdispatch trap on iOS 26, an abort on iOS 27.",
        )
        starts = list(BARE_START.finditer(init_body))
        self.assertGreaterEqual(
            len(starts), 1,
            "SettingsStore.init() never calls FrameTimeDynamicResolutionController.shared."
            "setEnabled, so adaptive resolution no longer starts from its saved value and "
            "this test is watching nothing.",
        )
        for m in starts:
            preceding = init_body[max(0, m.start() - 200):m.start()]
            opener = preceding.rfind("DispatchQueue.main.async")
            self.assertGreater(
                opener, preceding.rfind("}"),
                "one FrameTimeDynamicResolutionController.shared.setEnabled call in init() sits "
                "outside the async block. The bare form re-enters the once token and hangs.",
            )

    def test_init_transitive_callees_have_no_unallowed_shared_access(self):
        """The callees, which is where the second crash actually lived. The init
        body read clean; setEnabled's body did not."""
        init_body = self._init_body()
        callees = self._walk_transitive_callees(init_body, depth=2)
        names = [name for _, name, _ in callees]
        self.assertTrue(
            any("migrateFramePacingOptimalDefaultV1" in name for name in names),
            "the walk did not reach Self.migrateFramePacingOptimalDefaultV1, so it is finding "
            "nothing and passing for the wrong reason. Reached: %s" % names,
        )
        for label, name, body in callees:
            self._assert_no_unallowed_shared(
                self._strip_line_comments(body), "`%s` in %s.swift" % (name, label), "callee")


if __name__ == "__main__":
    unittest.main()

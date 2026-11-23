"""No store's init may reach a `.shared` singleton, its own or anyone's.

`static let shared` runs SettingsStore.init() inside swift_once, and every other store is
built inside it, so a SettingsStore.shared read that an init reaches re-enters the once
token. ALLOWLIST holds the accesses that cannot.
"""

import re
import unittest
from collections import Counter

from ios_source import STORE_INITS, SWIFT, at, block, store_inits, store_text, swift, without_comments


SHARED_TOKEN = re.compile(r"([A-Z]\w*)\s*\.\s*shared(?:\.[a-zA-Z_]\w*)?")
BARE_START = re.compile(r"FrameTimeDynamicResolutionController\.shared\.setEnabled")
MIGRATE_CALL = re.compile(r"Self\s*\.\s*migrateFramePacingOptimalDefaultV1\s*\(\s*\)")

# A raw requestGraphicsApply(). The guarded one does not match: the paren has to
# follow `Apply` for this to fire.
RAW_APPLY = re.compile(r"\brequestGraphicsApply\(\)")

# Every SettingsStore*.swift there should be. A new store file has to be added here in
# the commit that creates it, so its init cannot slip past these checks.
STORE_FILES = [
    "SettingsStore+Appearance.swift",
    "SettingsStore+Audio.swift",
    "SettingsStore+CPU.swift",
    "SettingsStore+Controller.swift",
    "SettingsStore+DEV9.swift",
    "SettingsStore+Defaults.swift",
    "SettingsStore+FramePacing.swift",
    "SettingsStore+GameFixState.swift",
    "SettingsStore+GameFixes.swift",
    "SettingsStore+Graphics.swift",
    "SettingsStore+GraphicsRuntime.swift",
    "SettingsStore+GraphicsState.swift",
    "SettingsStore+Ranges.swift",
    "SettingsStore+RetroAchievements.swift",
    "SettingsStore+RumbleMigration.swift",
    "SettingsStore+Runtime.swift",
    "SettingsStore+Speedhacks.swift",
    "SettingsStore+UI.swift",
    "SettingsStore.swift",
    "SettingsStoreAppearance.swift",
    "SettingsStoreController.swift",
    "SettingsStoreNetwork.swift",
]

# Store methods an init may call after its closing load, when every assignment goes
# through the setter anyway.
AFTER_CLOSE = {
    "normalizeDEV9Settings": "writes the DEV9 INI keys back in their normal form and assigns "
                             "no stored property",
    "snapshotCustomOsd": "seeds the Custom OSD snapshot in the INI and assigns no stored property",
}

# The defer runs once the closing load is done, so these see a whole store.
DEFER_CALLS = {
    "synchronizeControllerMacroGameplayInput": "runs in the defer, after the closing load",
    "writeFrameLimiterSettings": "runs in the defer, after the closing load",
}

DECL = re.compile(
    r"^(?P<indent> *)(?:@\w+\s+|(?:private|fileprivate|internal|public)(?:\(set\))?\s+"
    r"|(?:lazy|weak|unowned|nonisolated)\s+)*(?P<kind>let|var)\s+(?P<name>[A-Za-z_]\w*)(?P<rest>.*)$")
ASSIGN = re.compile(
    r"(?m)^[ \t]*(?:self[ \t]*\.[ \t]*)?(?P<name>[A-Za-z_]\w*)(?P<chain>(?:[ \t]*\.[ \t]*\w+)*)"
    r"[ \t]*(?:[-+*/%|&^]|<<|>>)?=(?!=)")
EXTENSION = re.compile(r"(?m)^[ \t]*(?:@\w+\s+)*(?:(?:private|fileprivate|internal|public)\s+)?"
                       r"extension\s+SettingsStore\b")
LOCAL = re.compile(r"\b(?:let|var)\s+(?P<name>[A-Za-z_]\w*)")
LOCAL_TUPLE = re.compile(r"\b(?:let|var|for)\s*\((?P<names>[\w\s,]+)\)")
FUNC = re.compile(r"(?m)^\s*(?:@\w+\s+)*(?:(?:private|fileprivate|internal|public|static|"
                  r"nonisolated|override|final|mutating)\s+)*func\s+(?P<name>[A-Za-z_]\w*)")
BARE_CALL = re.compile(r"(?<![\w.])(?P<name>[a-z_]\w*)\s*\(")
MEMBER_CALL = re.compile(r"\b(?:controller|network|appearance|self)\s*\.\s*(?P<name>[a-z_]\w*)\s*\(")


class TestIosSettingsStoreInitNoSharedAccess(unittest.TestCase):
    """The last stored property an init assigns must have no default, or the loads after it fire didSet."""

    FTDRC = swift("FrameTimeDynamicResolutionController.swift")
    VPAD_SKIN_LIB = swift("VPadSkinLibraryStore.swift")

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
        for path in (self.FTDRC, self.VPAD_SKIN_LIB):
            self.assertTrue(path.exists(), "%s missing, so the repo root is wrong" % path.name)
        # A pinned file that is gone is reported by test_store_files_are_pinned.
        self.store_files = {}
        for name in STORE_FILES:
            for path in SWIFT.rglob(name):
                self.store_files[name] = path.read_text(encoding="utf-8")
        self.settings_store = self.store_files["SettingsStore.swift"]
        self.frame_pacing = self.store_files["SettingsStore+FramePacing.swift"]
        # Store extensions that live outside the glob still hold store methods an init can call.
        self.outside_extensions = {}
        for path in SWIFT.rglob("*.swift"):
            if path.name in STORE_FILES:
                continue
            src = path.read_text(encoding="utf-8")
            blocks = [block(src[m.start():], "extension") for m in EXTENSION.finditer(src)]
            if blocks:
                self.outside_extensions[path.name] = "\n".join(blocks)
        self.ftdrc = self.FTDRC.read_text(encoding="utf-8")
        self.vpad_skin_lib = self.VPAD_SKIN_LIB.read_text(encoding="utf-8")

    def _init_body(self):
        return block(self.settings_store, "private init()")

    def _migrate_body(self):
        return block(self.frame_pacing, "func migrateFramePacingOptimalDefaultV1(")

    def _walk_transitive_callees(self, init_body, depth=2):
        """Every `Self.foo()`, bare `foo()`, `X.shared.foo()` and sub-store `foo()` reachable from init, to `depth`.

        A callee with no Swift body in the store files or the two singletons is skipped.
        """
        owner_files = [(name[:-len(".swift")], src) for name, src in self.store_files.items()]
        owner_files += [(name[:-len(".swift")], src) for name, src in self.outside_extensions.items()]
        owner_files += [
            ("FrameTimeDynamicResolutionController", self.ftdrc),
            ("VPadSkinLibraryStore", self.vpad_skin_lib),
        ]
        callers = [
            (re.compile(r"\bSelf\s*\.\s*([a-zA-Z_]\w*)\s*\("), lambda m: "Self"),
            # A sub-store has to spell a parent static out; the lowercase start skips nested types.
            (re.compile(r"\bSettingsStore\s*\.\s*([a-z_]\w*)\s*\("), lambda m: "SettingsStore"),
            (re.compile(r"\b([A-Z]\w*)\s*\.\s*shared\s*\.\s*([a-zA-Z_]\w*)\s*\("),
             lambda m: "%s.shared" % m.group(1)),
            (re.compile(r"\b(?:Self\s*\.\s*)?(Appearance|Controller|Network)\s*\.\s*([a-zA-Z_]\w*)\s*\("),
             lambda m: m.group(1)),
            (re.compile(r"\b(controller|network|appearance)\s*\.\s*([a-zA-Z_]\w*)\s*\("),
             lambda m: m.group(1)),
        ]
        funcs = self._store_funcs()
        seen = set()
        results = []

        def find_callee_body(name):
            for label, src in owner_files:
                for indent in ("    ", "        "):
                    pat = (r"(?m)^" + indent + r"(?:private\s+)?(?:static\s+)?func\s+" + re.escape(name)
                           + r"\s*(?:<[^>]*>)?\s*\([^)]*\)"
                           r"(?:\s*->\s*[^{]+?)?\s*\{(?P<body>.*?)^" + indent + r"\}\n")
                    m = re.search(pat, src, re.DOTALL)
                    if m:
                        return label, m.group("body")
            return "", ""

        def walk(body, current):
            if current > depth:
                return
            calls = [(owner_of(m), m.group(m.lastindex))
                     for pattern, owner_of in callers for m in pattern.finditer(body)]
            # A bare call to a store method runs on the store being built.
            calls += [("Self", m.group("name")) for m in BARE_CALL.finditer(body)
                      if m.group("name") in funcs]
            for owner, callee in calls:
                if (owner, callee) in seen:
                    continue
                seen.add((owner, callee))
                label, callee_body = find_callee_body(callee)
                if callee_body:
                    results.append((label or owner, "%s.%s" % (owner, callee), callee_body))
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

    def _stored_properties(self, name, text, class_body):
        """{name: True if it starts with a value} for the class's own stored properties.

        An optional without `=` starts as nil, so it counts as having a value.
        """
        class_needle = STORE_INITS[name][0]
        line_start = text.rfind("\n", 0, at(text, class_needle, name)) + 1
        indent = re.match(r" *", text[line_start:]).group(0) + "    "
        lines = without_comments(class_body).split("\n")
        props = {}
        for i, line in enumerate(lines):
            m = DECL.match(line)
            if not m or m.group("indent") != indent:
                continue
            prop = m.group("name")
            head = m.group("rest").split("{", 1)
            declared = head[0]
            optional = re.match(r"\s*:\s*(?:Optional<|.*[?!]\s*$)", declared) is not None
            if m.group("kind") == "let" or "=" in declared:
                props[prop] = "=" in declared
            elif len(head) == 1:
                props[prop] = optional
            else:
                after = head[1] + "\n" + "\n".join(lines[i + 1:i + 3])
                if re.match(r"\s*(?:didSet|willSet)\b", after):
                    props[prop] = optional
        return props

    @staticmethod
    def _split_init(init_body):
        """The init body after its opening brace, without its defer and deferred-main blocks,
        and the defer blocks on their own."""
        body = without_comments(init_body)
        body = body[body.find("{") + 1:]
        deferred = []
        for needle in ("defer {", "DispatchQueue.main.async"):
            while needle in body:
                inner = block(body, needle)
                if needle == "defer {":
                    deferred.append(inner)
                body = body.replace(inner, "", 1)
        return body, "\n".join(deferred)

    def _store_funcs(self):
        sources = list(self.store_files.values()) + list(self.outside_extensions.values())
        return {m.group("name") for src in sources for m in FUNC.finditer(src)}

    def _stored_assignments(self, body, stored):
        return [m for m in ASSIGN.finditer(body)
                if m.group("name") in stored and not m.group("chain").strip()
                and m.group("name") != "suppressINIWrites"]

    def test_store_files_are_pinned(self):
        """The glob has to find exactly the pinned files, each once."""
        found = Counter(p.name for p in SWIFT.rglob("SettingsStore*.swift"))
        pinned = Counter(STORE_FILES)
        self.assertEqual(STORE_FILES, sorted(STORE_FILES), "STORE_FILES is not sorted")
        missing = sorted((pinned - found).elements())
        extra = sorted((found - pinned).elements())
        self.assertFalse(
            missing or extra,
            "SettingsStore*.swift differs from STORE_FILES: missing %s, extra %s" % (missing, extra),
        )

    def test_every_store_class_has_an_init_entry(self):
        """A store class whose init is not in STORE_INITS would go unchecked."""
        for name in STORE_INITS:
            self.assertIn(name, STORE_FILES, "%s is in STORE_INITS but not in STORE_FILES" % name)
        for name, src in self.store_files.items():
            if re.search(r"(?m)^\s*final class\s", without_comments(src)):
                self.assertIn(name, STORE_INITS,
                              "%s declares a final class but has no STORE_INITS entry" % name)

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
        self.assertIsNotNone(first_read, "SettingsStore.init() reads no INI key, so the order "
                                         "check would hold up nothing")
        self.assertLess(
            call.start(), first_read.start(),
            "Self.migrateFramePacingOptimalDefaultV1() runs after the first ARMSX2Bridge.getINI "
            "read in init()",
        )

    def test_init_does_not_call_migrate_twice(self):
        """A second call would write the INI again after init has read it."""
        calls = MIGRATE_CALL.findall(store_text())
        self.assertEqual(
            len(calls), 1,
            "the store files call Self.migrateFramePacingOptimalDefaultV1() %d times, "
            "expected once" % len(calls),
        )

    def test_init_body_has_no_unallowed_shared_access(self):
        """A `.shared` access written in an init body needs an ALLOWLIST entry."""
        for name, _, _, init_body in store_inits():
            with self.subTest(store=name):
                self._assert_no_unallowed_shared(
                    without_comments(init_body), "%s init" % name, "init")

    def test_setting_commit_routes_through_the_guarded_helper(self):
        """A setter must not reload GS settings while init is still loading them."""
        store = store_text()
        body = without_comments(block(store, "func commit<T>("))
        self.assertRegex(
            body, r"\brequestGraphicsApplyGuarded\(\)",
            "commit() does not call requestGraphicsApplyGuarded()",
        )
        self.assertNotRegex(
            body, RAW_APPLY,
            "commit() calls requestGraphicsApply() instead of requestGraphicsApplyGuarded()",
        )
        guard = without_comments(block(store, "func requestGraphicsApplyGuarded("))
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
        """The functions each init reaches, two calls deep, need the same ALLOWLIST cover."""
        for store, _, _, init_body in store_inits():
            with self.subTest(store=store):
                callees = self._walk_transitive_callees(init_body, depth=2)
                names = [name for _, name, _ in callees]
                if store == "SettingsStore.swift":
                    self.assertTrue(
                        any("migrateFramePacingOptimalDefaultV1" in name for name in names),
                        "the walk did not reach Self.migrateFramePacingOptimalDefaultV1. "
                        "Reached: %s" % names,
                    )
                for label, name, body in callees:
                    self._assert_no_unallowed_shared(
                        without_comments(body), "`%s` in %s.swift" % (name, label), "callee")

    def test_each_init_assigns_only_its_own_stored_properties(self):
        """A direct assignment to a stored property skips didSet; anything else in init is not a load."""
        for name, text, class_body, init_body in store_inits():
            with self.subTest(store=name):
                stored = self._stored_properties(name, text, class_body)
                self.assertTrue(stored, "%s: found no stored properties" % name)
                body = without_comments(init_body)
                declared = [(m.start(), m.group("name")) for m in LOCAL.finditer(body)]
                declared += [(m.start(), n.strip()) for m in LOCAL_TUPLE.finditer(body)
                             for n in m.group("names").split(",")]
                assignments = list(ASSIGN.finditer(body))
                self.assertTrue(assignments, "%s: found no assignment in init" % name)
                for m in assignments:
                    target = m.group("name")
                    if target in stored:
                        self.assertFalse(
                            m.group("chain").strip(),
                            "%s init writes `%s%s`, which goes through that property's setter"
                            % (name, target, m.group("chain").replace(" ", "")))
                        continue
                    earlier = {n for pos, n in declared if pos < m.start()}
                    self.assertIn(
                        target, earlier,
                        "%s init assigns `%s`, which is neither a stored property of the store "
                        "nor a local declared above it" % (name, target),
                    )

    def test_no_store_method_runs_before_the_closing_load(self):
        """A store method called while init still loads could assign through a setter."""
        funcs = self._store_funcs()
        for name, text, class_body, init_body in store_inits():
            with self.subTest(store=name):
                stored = self._stored_properties(name, text, class_body)
                body, deferred = self._split_init(init_body)
                loads = self._stored_assignments(body, stored)
                self.assertTrue(loads, "%s: found no load in init" % name)
                close = loads[-1].start()
                for pattern in (BARE_CALL, MEMBER_CALL):
                    for m in pattern.finditer(body):
                        callee = m.group("name")
                        if callee not in funcs:
                            continue
                        self.assertGreater(
                            m.start(), close,
                            "%s init calls %s() before its closing load" % (name, callee))
                        self.assertIn(
                            callee, AFTER_CLOSE,
                            "%s init calls %s() after its closing load, and AFTER_CLOSE does not "
                            "list it" % (name, callee))
                    for m in pattern.finditer(deferred):
                        callee = m.group("name")
                        if callee in funcs:
                            self.assertIn(
                                callee, DEFER_CALLS,
                                "%s init calls %s() in its defer, and DEFER_CALLS does not list it"
                                % (name, callee))

    def test_each_init_ends_on_its_closing_load(self):
        """After the last property without a default is set, every assignment fires didSet."""
        for name, text, class_body, init_body in store_inits():
            with self.subTest(store=name):
                stored = self._stored_properties(name, text, class_body)
                body, _ = self._split_init(init_body)
                order = [m.group("name") for m in self._stored_assignments(body, stored)]
                closing = [i for i, prop in enumerate(order) if not stored[prop]]
                self.assertTrue(closing, "%s init assigns no property without a default" % name)
                after = [prop for prop in order[closing[-1] + 1:] if stored[prop]]
                self.assertEqual(
                    after, [],
                    "%s init assigns %s after %s, its last property without a default, so their "
                    "didSets fire" % (name, after, order[closing[-1]]),
                )


if __name__ == "__main__":
    unittest.main()

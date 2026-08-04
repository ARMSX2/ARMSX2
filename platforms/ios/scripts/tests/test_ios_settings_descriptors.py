import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
MODELS = ROOT / "platforms/ios/app/src/main/swift/Models"
STORE = MODELS / "SettingsStore.swift"

DESCRIPTOR = re.compile(r"let _(\w+)Config = Setting<([^>]+)>\(")
COMMIT = re.compile(r"commit\(_(\w+)Config, (\w+)\)")
RESET_FUNCS = ("resetEmulatorDefaults", "resetGraphicsDefaults", "resetAllDefaults")

# init() loads these by hand, and each says why on the line above it. Their
# codecs deliberately do less than the hand-load, so pointing one at load()
# would drop a clamp or a sentinel without breaking the build.
HAND_LOADED = {
    "renderer": "supportedIOSRenderer keeps a desktop renderer out of the Metal path",
    "lastActiveOsdPreset": "-1 means never set, and then the preset decides",
    "osdPerformancePosition": "old INIs hold positions this build no longer offers",
    "jitScriptProtocol": "JITScriptProtocol.normalized maps names older builds wrote",
}

# Migrations run from init() before any property is loaded, and want the value as
# it is on disk right now, sentinel defaults and all. They have to read by hand.
MIGRATION_READS = {
    ("EmuCore/GS", "VsyncQueueSize"),
    ("EmuCore/GS", "SyncToHostRefreshRate"),
    ("SPU2/Output", "OutputLatencyMS"),
    ("SPU2/Output", "BufferMS"),
    ("ARMSX2iOS/UI", "PhoneRumbleStrength"),
}


def normalised(value):
    """`Self.` inside the type and `SettingsStore.` outside it are the same thing."""
    return value.strip().replace("SettingsStore.", "Self.")


def block_at(lines, first):
    """The brace-balanced block starting at line index `first`."""
    depth = 0
    for i in range(first, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > first:
            return lines[first:i + 1]
    return lines[first:]


def call_at(lines, first):
    """The paren-balanced `Setting<T>(...)` literal starting at line index `first`."""
    depth = 0
    for i in range(first, len(lines)):
        depth += lines[i].count("(") - lines[i].count(")")
        if depth == 0 and i >= first:
            return lines[first:i + 1]
    return lines[first:]


def block_named(lines, opener):
    for i, line in enumerate(lines):
        if line.strip().startswith(opener):
            return block_at(lines, i)
    raise AssertionError(f"{opener!r} is gone from SettingsStore.swift; update this test")


class SettingsDescriptorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = STORE.read_text(encoding="utf-8")
        cls.lines = cls.source.split("\n")
        # Every Setting in the store: name -> (section, key, default).
        cls.descriptors = {}
        for i, line in enumerate(cls.lines):
            match = DESCRIPTOR.search(line)
            if not match:
                continue
            blob = " ".join(call_at(cls.lines, i))
            cls.descriptors[match.group(1)] = (
                re.search(r'section: "([^"]*)"', blob).group(1),
                re.search(r'key: "([^"]*)"', blob).group(1),
                re.search(r"default: (.*?),", blob).group(1).strip(),
            )
        # The property each descriptor belongs to, read off its commit call.
        owner = {m.group(1): m.group(2) for m in COMMIT.finditer(cls.source)}
        cls.property_of = {name: owner.get(name, name) for name in cls.descriptors}
        cls.init_body = "\n".join(block_named(cls.lines, "private init()"))
        # The whole store, extensions included, so a read that moved into a
        # helper file cannot slip past the checks below.
        cls.store_source = "\n".join(
            path.read_text(encoding="utf-8") for path in sorted(MODELS.glob("SettingsStore*.swift")))

    def test_the_descriptors_were_actually_found(self):
        """Rename the convention and every check below passes on an empty set."""
        self.assertGreater(len(self.descriptors), 100)

    def test_the_exemption_lists_have_not_gone_stale(self):
        for prop in HAND_LOADED:
            with self.subTest(setting=prop):
                self.assertIn(prop, self.property_of.values(), f"{prop} is no longer a setting")
                assigned = re.findall(rf"^\s*{re.escape(prop)} = (.+)$", self.init_body, re.M)
                self.assertFalse(any(".load(" in rhs for rhs in assigned),
                                 f"{prop} loads through its descriptor now; drop the exemption")
        by_hand = set(re.findall(r'getINI\w+\(\s*"([^"]+)",\s*key: "([^"]+)"', self.store_source))
        self.assertEqual(sorted(MIGRATION_READS - by_hand), [],
                         "these are exempted but nothing reads them by hand any more")

    def test_every_descriptor_is_loaded_in_init(self):
        """A new setting that nobody loads is the whole bug class this guards."""
        for name, prop in self.property_of.items():
            with self.subTest(setting=prop):
                assigned = re.findall(rf"^\s*{re.escape(prop)} = (.+)$", self.init_body, re.M)
                self.assertTrue(assigned, f"{prop} is never assigned in init()")
                if prop in HAND_LOADED:
                    continue
                for rhs in assigned:  # both sides of an #if have to load
                    self.assertIn(f"_{name}Config.load(", rhs,
                                  f"{prop} is assigned without loading through _{name}Config")

    def test_nothing_reads_a_key_a_descriptor_already_owns(self):
        """Reading the INI by hand for an owned key is how the two halves drift."""
        allowed = MIGRATION_READS | {self.descriptors[n][:2] for n in self.descriptors
                                     if self.property_of[n] in HAND_LOADED}
        owned = {(section, key) for section, key, _ in self.descriptors.values()}
        by_hand = set(re.findall(r'getINI\w+\(\s*"([^"]+)",\s*key: "([^"]+)"', self.store_source))
        self.assertEqual(sorted((by_hand & owned) - allowed), [],
                         "a descriptor owns these keys, but the store still reads them by hand")

    def test_every_descriptor_picks_a_named_codec(self):
        """An inline read/write pair is free to disagree, which is the thing this all exists to stop."""
        for i, line in enumerate(self.lines):
            match = DESCRIPTOR.search(line)
            if not match:
                continue
            literal = " ".join(call_at(self.lines, i))
            with self.subTest(setting=match.group(1)):
                self.assertRegex(literal, r"codec: \.\w+",
                                 f"_{match.group(1)}Config does not name one of SettingCodec's presets")

    def test_nothing_on_the_init_path_touches_the_shared_store(self):
        """Reaching SettingsStore.shared from a file init() runs re-enters its own swift_once."""
        for name in ("SettingCodec.swift", "Setting.swift"):
            with self.subTest(file=name):
                self.assertNotIn("SettingsStore.shared", (MODELS / name).read_text(encoding="utf-8"))

    def test_no_two_descriptors_claim_the_same_key(self):
        seen = {}
        for name, (section, key, _) in self.descriptors.items():
            self.assertNotIn((section, key), seen,
                             f"{name} and {seen.get((section, key))} both claim {section}/{key}")
            seen[(section, key)] = name

    def test_every_descriptor_backed_setter_goes_through_commit(self):
        """Anything writing the INI its own way skips suppression and the graphics apply."""
        for name, prop in self.property_of.items():
            with self.subTest(setting=prop):
                declared = next((i for i, l in enumerate(self.lines)
                                 if re.match(rf"^\s*var {re.escape(prop)}\b", l)), None)
                self.assertIsNotNone(declared, f"cannot find the declaration of {prop}")
                observer = "\n".join(block_at(self.lines, declared))
                self.assertIn(f"commit(_{name}Config, {prop})", observer,
                              f"{prop}'s didSet does not commit itself through _{name}Config")

    def test_the_property_starts_on_its_descriptor_default(self):
        """Swift will not let the initializer say _xConfig.defaultValue, so check it here."""
        for name, (_, _, default) in self.descriptors.items():
            prop = self.property_of[name]
            # The declaration wraps onto a second line when the name is long.
            declaration = re.search(rf"^\s*var {re.escape(prop)}(?::[^=]+)? = (.+?) \{{\s*didSet",
                                    self.source, re.M)
            with self.subTest(setting=prop):
                self.assertIsNotNone(declaration, f"cannot find the declaration of {prop}")
                # A leading-dot default is the same enum case spelled shorter.
                started, declared = normalised(declaration.group(1)), normalised(default)
                if started.startswith(".") or declared.startswith("."):
                    started, declared = started.split(".")[-1], declared.split(".")[-1]
                self.assertEqual(started, declared,
                                 f"{prop} starts at {declaration.group(1)} "
                                 f"but _{name}Config defaults to {default}")

    def test_resets_agree_with_the_descriptor_defaults(self):
        """Reset keeps readable literals; this is what stops them drifting."""
        by_property = {self.property_of[n]: (n, d) for n, (_, _, d) in self.descriptors.items()}
        for func in RESET_FUNCS:
            for line in block_named(self.lines, f"func {func}"):
                match = re.match(r"^\s*(\w+) = (.+?)(?:\s*//.*)?$", line)
                if not match or match.group(1) not in by_property:
                    continue
                name, default = by_property[match.group(1)]
                with self.subTest(reset=func, setting=match.group(1)):
                    self.assertEqual(
                        normalised(match.group(2)), normalised(default),
                        f"{func} resets {match.group(1)} to {match.group(2)}, "
                        f"but _{name}Config defaults to {default}")


if __name__ == "__main__":
    unittest.main()

import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
MODELS = ROOT / "platforms/ios/app/src/main/swift/Models"
STORE = MODELS / "SettingsStore.swift"
LIBRARY = MODELS / "ShaderPresetLibrary.swift"

# A value carrying one of these is a container path: it names the UUID iOS handed this
# install, and the next install gets a different one.
CONTAINER_SOURCES = (
    "Bundle.main",
    "documentDirectory",
    "NSHomeDirectory",
    "EmuFolders",
    ".path",
    ".absoluteString",
    "resourceURL",
)

STALE = ("An absolute container path goes stale on the next reinstall: iOS gives the app a "
         "new container UUID every install, and a sideloaded build is reinstalled constantly. "
         "The persisted value has to come from ShaderPresetLibrary.token(for:).")


def stripped(path):
    """The file with comments gone, so a comment can neither satisfy nor trip a check."""
    source = path.read_text(encoding="utf-8")
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def block_at(lines, first):
    """The brace-balanced block starting at line index `first`."""
    depth = 0
    for i in range(first, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > first:
            return "\n".join(lines[first:i + 1])
    return "\n".join(lines[first:])


DECLARATION = re.compile(r"^\s*(?:[\w@]+\s+)*(?:func\s+\w+|init)\s*\(")


def function_containing(source, needle):
    """The body of the func or init whose block holds `needle`, or None."""
    lines = source.split("\n")
    for i, line in enumerate(lines):
        if DECLARATION.match(line):
            body = block_at(lines, i)
            if needle in body:
                return body
    return None


class ShaderPresetPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.store = stripped(STORE)
        cls.library = stripped(LIBRARY)
        # Extensions live in sibling files, so a write that moved into one is still caught.
        cls.store_all = "\n".join(stripped(p) for p in sorted(MODELS.glob("SettingsStore*.swift")))

    def test_the_selection_is_a_setting_backed_token(self):
        self.assertRegex(
            self.store, r'key:\s*"ShaderChainPresetRef"',
            "SettingsStore no longer declares a Setting for ShaderChainPresetRef. Without it "
            "the selection is not persisted through the store's commit path at all, so it "
            "neither reaches the INI nor triggers the coalesced graphics apply.")
        self.assertRegex(
            self.store, r'section:\s*"EmuCore/GS",\s*key:\s*"ShaderChainPresetRef"',
            "ShaderChainPresetRef moved out of EmuCore/GS. It sits beside the core key it "
            "keeps correct, and moving it splits the two halves across sections.")

    def test_nothing_persists_a_container_path_as_the_selection(self):
        """The whole reason this plan exists. A token survives a reinstall; a path does not."""
        # Unanchored on purpose: a violation squeezed onto one line inside an extension is
        # still a violation, and anchoring to the line start walks straight past it.
        assignments = re.findall(r"\bshaderChainPresetRef\s*=\s*([^\n]+)", self.store_all)
        self.assertTrue(assignments, "nothing assigns shaderChainPresetRef any more")
        for rhs in assignments:
            for source in CONTAINER_SOURCES:
                self.assertNotIn(
                    source, rhs,
                    f"shaderChainPresetRef is assigned {rhs.strip()!r}, which is derived from "
                    f"{source}. " + STALE)

    def test_the_token_setting_is_only_ever_committed_with_its_own_property(self):
        commits = re.findall(r"commit\(_shaderChainPresetRefConfig,\s*([^)]+)\)", self.store_all)
        self.assertTrue(commits, "the token setting is never committed, so it never persists")
        for value in commits:
            self.assertEqual(
                value.strip(), "shaderChainPresetRef",
                f"_shaderChainPresetRefConfig is committed with {value.strip()!r} rather than "
                "its own property, which routes around the didSet that resolves the token. "
                + STALE)

    def test_core_still_receives_a_resolved_absolute_path(self):
        writes = re.findall(
            r'setINIString\(\s*"EmuCore/GS",\s*key:\s*"ShaderChainPreset",\s*value:\s*([^)]+)\)',
            self.store_all)
        self.assertTrue(
            writes,
            "nothing writes EmuCore/GS/ShaderChainPreset any more. That is the key the GS "
            "device opens; core reads it as a std::string absolute path and this phase does "
            "not change that.")
        for value in writes:
            self.assertNotIn(
                "shaderChainPresetRef", value,
                f"ShaderChainPreset is written {value.strip()!r}, which hands core the token "
                "instead of a path. librashader would be asked to open a file called "
                "'bundle:presets/...' and the chain would fail on every launch.")

    def test_the_library_encodes_and_decodes_and_refuses_an_escape(self):
        self.assertRegex(
            self.library, r"func\s+token\(for\s+\w+:\s*URL",
            "ShaderPresetLibrary has no encoder taking the URL of a scanned preset, so there "
            "is nothing to persist except a path. " + STALE)
        self.assertRegex(
            self.library, r"func\s+token\(forLegacyPath",
            "ShaderPresetLibrary can no longer read a bare absolute path. A selection made by "
            "an older build, or written into the INI by hand, has nothing to migrate it and "
            "is dropped on the first launch after the upgrade.")
        self.assertRegex(
            self.library, r"func\s+resolve\(",
            "ShaderPresetLibrary has no resolver, so a persisted token cannot be turned back "
            "into the absolute path core needs.")
        self.assertRegex(
            self.library, r'"\.\."',
            "Nothing in ShaderPresetLibrary names the parent-directory component it must "
            "refuse. UIFileSharingEnabled is true, so a user or a dropped file can rewrite "
            "the token in the INI to any string, and a '..' component would walk it straight "
            "out of the root it claims to be inside.")

    def test_an_unresolvable_token_disables_the_chain(self):
        body = function_containing(self.store, "ShaderPresetLibrary.resolve(")
        self.assertIsNotNone(
            body, "nothing in SettingsStore resolves the token, so core's absolute key is "
                  "never re-rooted against the container this launch got.")
        self.assertRegex(
            body, r"shaderChainEnabled\s*=\s*false",
            "A token that resolves to nothing does not switch the chain off. A deleted pack "
            "or a dropped bundled preset would leave core enabled and pointed at a file that "
            "is not there, which fails once per preset load rather than telling the user "
            "their selection is gone.")

    def test_the_migration_still_runs_on_the_init_path(self):
        """A reinstall repairs itself only if this runs before the GS device reads the config."""
        init = function_containing(self.store, "suppressINIWrites = true")
        self.assertIsNotNone(init, "cannot find SettingsStore.init; update this test")
        self.assertIn(
            "migrateShaderChainSelectionV1()", init,
            "init no longer re-roots the selection. Deferring it to a view appearing means "
            "the GS device reads last install's absolute path first, and the user's shader "
            "silently stops working after an update.")


if __name__ == "__main__":
    unittest.main()

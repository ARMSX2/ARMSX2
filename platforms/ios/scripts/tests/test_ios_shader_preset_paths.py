import re
import unittest

from ios_source import SWIFT, block, read, without_comments


MODELS = SWIFT / "Models"
STORE = MODELS / "SettingsStore.swift"
LIBRARY = MODELS / "ShaderPresetLibrary.swift"

# A value carrying one of these is a container path, which changes on every install.
CONTAINER_SOURCES = (
    "Bundle.main",
    "documentDirectory",
    "NSHomeDirectory",
    "EmuFolders",
    ".path",
    ".absoluteString",
    "resourceURL",
)


class ShaderPresetPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.store = without_comments(read(STORE))
        cls.library = without_comments(read(LIBRARY))
        # Extensions live in sibling files, so a write that moved into one is still caught.
        cls.store_all = "\n".join(
            without_comments(read(p)) for p in sorted(MODELS.glob("SettingsStore*.swift")))

    def test_the_selection_is_a_setting_backed_token(self):
        self.assertRegex(
            self.store, r'key:\s*"ShaderChainPresetRef"',
            "SettingsStore does not declare a Setting for ShaderChainPresetRef")
        self.assertRegex(
            self.store, r'section:\s*"EmuCore/GS",\s*key:\s*"ShaderChainPresetRef"',
            "ShaderChainPresetRef is not in EmuCore/GS beside ShaderChainPreset")

    def test_nothing_persists_a_container_path_as_the_selection(self):
        """A token survives a reinstall; a container path does not."""
        # Not anchored to the line start, so an assignment that shares a line is still found.
        assignments = re.findall(r"\bshaderChainPresetRef\s*=\s*([^\n]+)", self.store_all)
        self.assertTrue(assignments, "nothing assigns shaderChainPresetRef any more")
        for rhs in assignments:
            for source in CONTAINER_SOURCES:
                self.assertNotIn(
                    source, rhs,
                    f"shaderChainPresetRef is assigned {rhs.strip()!r}, which is derived from "
                    f"{source} instead of ShaderPresetLibrary.token(for:)")

    def test_the_token_setting_is_only_ever_committed_with_its_own_property(self):
        commits = re.findall(r"commit\(_shaderChainPresetRefConfig,\s*([^)]+)\)", self.store_all)
        self.assertTrue(commits, "the token setting is never committed, so it never persists")
        for value in commits:
            self.assertEqual(
                value.strip(), "shaderChainPresetRef",
                f"_shaderChainPresetRefConfig is committed with {value.strip()!r} rather than "
                "shaderChainPresetRef, which skips the didSet that resolves the token")

    def test_core_still_receives_a_resolved_absolute_path(self):
        writes = re.findall(
            r'setINIString\(\s*"EmuCore/GS",\s*key:\s*"ShaderChainPreset",\s*value:\s*([^)]+)\)',
            self.store_all)
        self.assertTrue(
            writes,
            "nothing writes EmuCore/GS/ShaderChainPreset, the absolute path the GS device opens")
        for value in writes:
            self.assertNotIn(
                "shaderChainPresetRef", value,
                f"ShaderChainPreset is written {value.strip()!r}, which hands core the token "
                "instead of a path")

    def test_the_library_encodes_and_decodes_and_refuses_an_escape(self):
        self.assertRegex(
            self.library, r"func\s+token\(for\s+\w+:\s*URL",
            "ShaderPresetLibrary has no token(for:) taking the URL of a scanned preset")
        self.assertRegex(
            self.library, r"func\s+token\(forLegacyPath",
            "ShaderPresetLibrary has no token(forLegacyPath:), so an absolute path in the INI "
            "is not migrated")
        self.assertRegex(
            self.library, r"func\s+resolve\(",
            "ShaderPresetLibrary has no resolve() to turn a token back into an absolute path")
        self.assertRegex(
            self.library, r'"\.\."',
            "ShaderPresetLibrary does not check a token for a '..' component")

    def test_an_unresolvable_token_disables_the_chain(self):
        body = block(self.store, "func applyShaderChainSelection(")
        self.assertIn(
            "ShaderPresetLibrary.resolve(", body,
            "applyShaderChainSelection() does not call ShaderPresetLibrary.resolve(")
        self.assertRegex(
            body, r"shaderChainEnabled\s*=\s*false",
            "applyShaderChainSelection() does not set shaderChainEnabled = false for a token "
            "that does not resolve")

    def test_the_migration_still_runs_on_the_init_path(self):
        """A reinstall repairs itself only if this runs before the GS device reads the config."""
        init = block(self.store, "private init()")
        self.assertIn(
            "migrateShaderChainSelectionV1()", init,
            "init() does not call migrateShaderChainSelectionV1(), so the GS device reads the "
            "previous install's path")

    def test_saved_presets_follow_the_bundle_after_an_install(self):
        """A preset saved from a built-in one names it by absolute path, and installs move the bundle."""
        repair = block(self.library, "func repairSavedReferences")
        self.assertIn(
            "token(forLegacyPath:", repair,
            "repairSavedReferences() no longer re-roots through token(forLegacyPath:)")
        init = block(self.store, "private init()")
        self.assertIn(
            "ShaderPresetLibrary.repairSavedReferences()", init,
            "init() does not call ShaderPresetLibrary.repairSavedReferences()")


if __name__ == "__main__":
    unittest.main()

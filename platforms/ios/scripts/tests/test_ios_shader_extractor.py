import unittest

from ios_source import CPP, block, without_comments


BRIDGE = CPP / "ARMSX2Bridge.mm"
HEADER = CPP / "ARMSX2Bridge.h"

SHADER_EXTRACTOR = "extractShaderPackArchiveAtURL"
SKIN_EXTRACTOR = "extractControllerSkinArchiveAtURL"
SANITISER = "ARMSX2SanitizedSkinFileName"
IMPORT_PREDICATE = "ARMSX2IsControllerSkinImportName"
SKIN_CAPS = ("kMaxSkinArchiveEntries", "kMaxSkinArchiveTotalEntries")


class ShaderPackExtractorPolicy(unittest.TestCase):
    def setUp(self):
        self.source = BRIDGE.read_text(encoding="utf-8")
        self.header = HEADER.read_text(encoding="utf-8")
        self.code = without_comments(
            block(self.source, f"+ (nonnull NSArray<NSURL *> *){SHADER_EXTRACTOR}:"))

    def test_it_does_not_flatten_entries_through_the_skin_sanitiser(self):
        self.assertNotIn(
            SANITISER, self.code,
            f"{SANITISER} reduces an entry to its lastPathComponent, which breaks the relative "
            "stage paths in a .slangp")

    def test_it_does_not_gate_entries_on_the_controller_skin_allowlist(self):
        self.assertNotIn(
            IMPORT_PREDICATE, self.code,
            f"{IMPORT_PREDICATE} only allows skin files, so it rejects .slang and .slangp "
            "entries")

    def test_it_does_not_reuse_the_controller_skin_entry_caps(self):
        for cap in SKIN_CAPS:
            with self.subTest(cap=cap):
                self.assertNotIn(
                    cap, self.code,
                    f"{cap} is sized for a controller skin, and a shader pack has thousands "
                    "of files")

    def test_the_escape_check_gates_the_write_rather_than_merely_existing(self):
        """realpath of the parent is compared with the root before writeToURL:."""
        resolve = self.code.find("realpath(parentURL.path")
        compare = self.code.find("![resolvedParent isEqualToString:resolvedRoot]")
        prefix = self.code.find("![resolvedParent hasPrefix:guardPrefix]")
        write = self.code.find("writeToURL:")
        self.assertGreater(resolve, 0, "the entry's parent is never canonically resolved")
        self.assertGreater(compare, 0, "nothing compares the resolved parent to the root")
        self.assertGreater(prefix, 0,
                           "nothing checks the resolved parent with hasPrefix:guardPrefix")
        self.assertGreater(write, 0, "the extractor does not write anything")
        self.assertLess(resolve, compare,
                        "the root comparison runs before realpath, so it compares unresolved "
                        "paths")
        self.assertLess(compare, write,
                        "an entry is written before its parent is checked for escaping")

    def test_the_declaration_is_exposed_to_swift(self):
        self.assertIn(
            SHADER_EXTRACTOR, self.header,
            "the extractor is not declared, so no Swift importer can reach it")
        self.assertIn(
            "NS_SWIFT_NAME(extractShaderPackArchive(at:to:error:))", self.header,
            "ARMSX2Bridge.h does not give the extractor "
            "NS_SWIFT_NAME(extractShaderPackArchive(at:to:error:))")

    def test_the_controller_skin_extractor_keeps_its_own_policy(self):
        skin = block(self.source, f"+ (nonnull NSArray<NSURL *> *){SKIN_EXTRACTOR}:")
        self.assertIn(
            SANITISER, without_comments(skin),
            "the skin extractor no longer calls ARMSX2SanitizedSkinFileName on its entry names")


if __name__ == "__main__":
    unittest.main()

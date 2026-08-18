import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
BRIDGE = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm"
HEADER = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.h"

SHADER_EXTRACTOR = "extractShaderPackArchiveAtURL"
SKIN_EXTRACTOR = "extractControllerSkinArchiveAtURL"
SANITISER = "ARMSX2SanitizedSkinFileName"
IMPORT_PREDICATE = "ARMSX2IsControllerSkinImportName"
SKIN_CAPS = ("kMaxSkinArchiveEntries", "kMaxSkinArchiveTotalEntries")
CANONICAL = re.compile(r"URLByResolvingSymlinksInPath|stringByResolvingSymlinksInPath|realpath")


def method_body(source, selector):
    match = re.search(
        r"(?ms)^\+ \(nonnull NSArray<NSURL \*> \*\)" + selector + r":.*?\n\}\n", source)
    return match.group(0) if match else None


def without_comments(body):
    """What the compiler sees, so prose naming a forbidden symbol cannot trip a check."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", body, flags=re.S))


class ShaderPackExtractorPolicy(unittest.TestCase):
    def setUp(self):
        self.source = BRIDGE.read_text(encoding="utf-8")
        self.header = HEADER.read_text(encoding="utf-8")
        body = method_body(self.source, SHADER_EXTRACTOR)
        self.assertIsNotNone(
            body, f"{SHADER_EXTRACTOR}: is missing; the shader pack import has no extractor")
        self.code = without_comments(body)

    def test_it_does_not_flatten_entries_through_the_skin_sanitiser(self):
        self.assertNotIn(
            SANITISER, self.code,
            f"{SANITISER} reduces an entry to its lastPathComponent. A .slangp names its "
            "stages by relative path, so a flattened pack has no working preset in it.")

    def test_it_does_not_gate_entries_on_the_controller_skin_allowlist(self):
        self.assertNotIn(
            IMPORT_PREDICATE, self.code,
            f"{IMPORT_PREDICATE} allows skin images and manifests. Neither .slang nor "
            ".slangp is in it, so every entry would be rejected and the import would "
            "silently produce an empty pack.")

    def test_it_does_not_reuse_the_controller_skin_entry_caps(self):
        for cap in SKIN_CAPS:
            with self.subTest(cap=cap):
                self.assertNotIn(
                    cap, self.code,
                    f"{cap} is sized for a controller skin. The stock RetroArch pack is "
                    "thousands of files, and truncating it silently is worse than refusing "
                    "the archive.")

    def test_it_resolves_paths_canonically_before_writing(self):
        self.assertRegex(
            self.code, CANONICAL,
            "A tree-preserving extractor cannot rely on flattening to make escape "
            "impossible, so every entry's destination must be canonically resolved and "
            "checked against the destination. Comparing raw strings is a zip-slip hole.")

    def test_the_declaration_is_exposed_to_swift(self):
        self.assertIn(
            SHADER_EXTRACTOR, self.header,
            "the extractor is not declared, so no Swift importer can reach it")
        self.assertIn(
            "NS_SWIFT_NAME(extractShaderPackArchive(at:to:error:))", self.header,
            "without the NS_SWIFT_NAME the Swift call site gets the raw selector, and the "
            "error channel that distinguishes an escaping archive from an empty one is "
            "awkward enough to be dropped")

    def test_the_controller_skin_extractor_keeps_its_own_policy(self):
        skin = method_body(self.source, SKIN_EXTRACTOR)
        self.assertIsNotNone(skin, f"{SKIN_EXTRACTOR}: disappeared")
        self.assertIn(
            SANITISER, without_comments(skin),
            "the skin extractor stopped sanitising its own names. If the two extractors are "
            "being unified, shader packs get flattened again and every preset breaks.")


if __name__ == "__main__":
    unittest.main()

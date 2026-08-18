#!/usr/bin/env python3
"""The catalogue downloader's order of operations, asserted against the real source.

Everything this file checks is about ORDER, because order is the whole safety argument.
The manifest is remote and therefore hostile: it states a size and a hash, and the only
reason to trust the bytes is that both were checked before anything was written. A check
that runs after the write is not a check, it is a log line.

Each assertion below is written against character offsets in the real file rather than
against the presence of a symbol. A fence that asserts a function exists passes for a
function that does nothing, which is how three of this phase's other fences turned out to
be decoration.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SWIFT = ROOT / "platforms/ios/app/src/main/swift"
INSTALLER = SWIFT / "Models/ShaderCatalogInstaller.swift"
CATALOG = SWIFT / "Models/ShaderCatalog.swift"
BROWSER = SWIFT / "Views/Settings/ShaderCatalogBrowserView.swift"
ROOT_VIEW = SWIFT / "Views/RootView.swift"
PER_GAME = SWIFT / "Models/PerGameShaderSelection.swift"


def source(path):
    return path.read_text(encoding="utf-8")


def at(text, needle, what):
    """Offset of the one occurrence, or a failure naming what went missing."""
    found = text.find(needle)
    if found < 0:
        raise AssertionError("%s: could not find %r" % (what, needle))
    return found


class Downloader(unittest.TestCase):
    def setUp(self):
        for path in (INSTALLER, CATALOG, BROWSER, ROOT_VIEW, PER_GAME):
            self.assertTrue(path.is_file(), "missing %s" % path)
        self.installer = source(INSTALLER)
        self.catalog = source(CATALOG)

    def test_size_is_refused_before_the_transfer(self):
        """The manifest states the size, so spending the bytes first buys nothing."""
        cap = at(self.installer, "Self.maxDownloadBytes", "size cap")
        download = at(self.installer, "URLSession.shared.download", "the download call")
        self.assertLess(
            cap, download,
            "the size cap is applied after the transfer. The manifest carries zip.bytes, so "
            "the refusal costs nothing if it happens first.")

    def test_both_the_size_and_the_hash_are_checked_before_anything_is_written(self):
        received = at(self.installer, "received == entry.zip.bytes", "received-size check")
        digest = at(self.installer, "digest == entry.zip.sha256", "sha256 check")
        install = at(self.installer, "importer.install(archiveAt:", "the install call")
        self.assertLess(received, install, "the received byte count is compared after the write")
        self.assertLess(digest, install, "the sha256 is compared after the write")

    def test_the_hash_is_streamed_rather_than_read_whole(self):
        """A catalogue zip is small today. The cap that bounds it is 32 MB."""
        self.assertIn("read(upToCount:", self.installer,
                      "the sha256 reads the file whole; stream it")
        self.assertNotIn("Data(contentsOf: staged", self.installer)

    def test_the_manifest_path_is_validated_before_it_becomes_a_url(self):
        """`..` and `/` both survive percent-encoding for .urlPathAllowed."""
        guard = at(self.catalog, "SkinAssetPath.isSafeRelative", "the path guard")
        encode = at(self.catalog, "addingPercentEncoding", "the percent-encode")
        build = at(self.catalog, "URL(string: \"\\(root)/\\(encoded)\")", "the URL construction")
        self.assertLess(guard, encode, "the path is encoded before it is validated")
        self.assertLess(guard, build, "the URL is built before the path is validated")

    def test_the_entry_type_does_not_decode_the_files_array(self):
        """96% of the manifest's bytes. Declaring the key would put 8 MB in the cache."""
        keys = re.search(r"enum CodingKeys[^}]*}", self.catalog, re.S)
        self.assertIsNotNone(keys, "ShaderCatalogEntry has no CodingKeys")
        self.assertNotIn("files", keys.group(0),
                         "the entry decodes the manifest's files array; the zip's own sha256 "
                         "already covers every file inside it")

    def test_the_cache_is_in_caches_and_not_in_documents(self):
        directory = re.search(r"static var cacheDirectory[^}]*}", self.catalog, re.S)
        self.assertIsNotNone(directory, "no cacheDirectory")
        self.assertIn(".cachesDirectory", directory.group(0))
        self.assertNotIn(".documentDirectory", directory.group(0),
                         "the catalogue cache is re-fetchable and must not be backed up, and "
                         "Documents is user-visible through Files")

    def test_the_staging_sweep_runs_at_launch(self):
        """`defer` does not run when iOS kills a backgrounded app mid-download."""
        self.assertIn("static func sweepStagedDownloads", self.installer)
        self.assertIn("ShaderCatalogInstaller.sweepStagedDownloads()", source(ROOT_VIEW),
                      "nothing calls the sweep, so a killed download leaks its staging file "
                      "for the life of the container")

    def test_the_base_url_is_https_and_there_is_one_of_it(self):
        bases = re.findall(r'defaultBase\s*=\s*"([^"]+)"', self.catalog)
        self.assertEqual(len(bases), 1, "expected exactly one base URL constant, got %r" % bases)
        self.assertTrue(bases[0].startswith("https://"), "the base URL is not https")

    def test_the_override_accepts_only_https_and_file(self):
        """It exists so a simulator can read a local emit. It must not open http back up."""
        resolver = re.search(r"func resolvedBase\(\)[^}]*}[^}]*}", self.catalog, re.S)
        self.assertIsNotNone(resolver, "no resolvedBase")
        body = resolver.group(0)
        self.assertIn('url.scheme == "https"', body)
        self.assertIn('url.scheme == "file"', body)

    def test_the_installed_name_comes_from_the_return_and_not_the_shared_property(self):
        """Two rows can install at once against one importer.

        `installedName` is a single property. If the caller reads it instead of the return
        value, install A can read install B's folder name -- which writes A's marker into B's
        pack, and makes cancelling A delete B.
        """
        self.assertIn("let landed = await importer.install(archiveAt:", self.installer,
                      "the installer does not bind the return value of install(archiveAt:)")
        self.assertNotIn("importer.installedName", self.installer,
                         "the installer reads the shared installedName property; two "
                         "concurrent installs overwrite each other's answer")

    def test_per_game_on_without_a_resolvable_preset_is_written_off(self):
        """Enabled true with no preset key falls through to the GLOBAL preset.

        That is the one outcome PerGameShaderSelection's own doc comment forbids, and it is
        invisible: another CRT shader looks like the CRT shader you picked.
        """
        text = source(PER_GAME)
        write = re.search(r"static func write\(chain[^}]*\}[^}]*\}[^}]*\}", text, re.S)
        self.assertIsNotNone(write, "no write(chain:)")
        body = write.group(0)
        self.assertNotIn("setBool(keys.enabled, chain == 1", body,
                         "write() sets the enabled key straight from the picker, so On with a "
                         "preset that no longer resolves leaves the game on the global chain")
        self.assertIn("setBool(keys.enabled, resolved != nil", body)


if __name__ == "__main__":
    unittest.main()

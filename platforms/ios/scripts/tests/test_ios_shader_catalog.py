#!/usr/bin/env python3
"""The catalogue installer checks size, sha256 and path before it writes anything."""

import re
import unittest

from ios_source import SWIFT, at, block, read

INSTALLER = SWIFT / "Models/ShaderCatalogInstaller.swift"
CATALOG = SWIFT / "Models/ShaderCatalog.swift"
BROWSER = SWIFT / "Views/Settings/ShaderCatalogBrowserView.swift"
ROOT_VIEW = SWIFT / "Views/RootView.swift"
PER_GAME = SWIFT / "Models/PerGameShaderSelection.swift"


class Downloader(unittest.TestCase):
    def setUp(self):
        for path in (INSTALLER, CATALOG, BROWSER, ROOT_VIEW, PER_GAME):
            self.assertTrue(path.is_file(), "missing %s" % path)
        self.installer = read(INSTALLER)
        self.catalog = read(CATALOG)

    def test_size_is_refused_before_the_transfer(self):
        """The manifest carries zip.bytes, so the size cap can refuse before the transfer."""
        cap = at(self.installer, "Self.maxDownloadBytes", "size cap")
        download = at(self.installer, "URLSession.shared.download", "the download call")
        self.assertLess(
            cap, download,
            "the size cap is applied after URLSession.shared.download")

    def test_both_the_size_and_the_hash_are_checked_before_anything_is_written(self):
        received = at(self.installer, "received == entry.zip.bytes", "received-size check")
        digest = at(self.installer, "digest == entry.zip.sha256", "sha256 check")
        install = at(self.installer, "importer.install(archiveAt:", "the install call")
        self.assertLess(received, install, "the received byte count is compared after the write")
        self.assertLess(digest, install, "the sha256 is compared after the write")

    def test_the_hash_is_streamed_rather_than_read_whole(self):
        """A catalogue zip can be as large as maxDownloadBytes."""
        self.assertIn("read(upToCount:", self.installer,
                      "the sha256 reads the file whole; stream it")

    def test_the_manifest_path_is_validated_before_it_becomes_a_url(self):
        """`..` and `/` both survive percent-encoding for .urlPathAllowed."""
        guard = at(self.catalog, "SkinAssetPath.isSafeRelative", "the path guard")
        encode = at(self.catalog, "addingPercentEncoding", "the percent-encode")
        build = at(self.catalog, "URL(string: \"\\(root)/\\(encoded)\")", "the URL construction")
        self.assertLess(guard, encode, "the path is encoded before it is validated")
        self.assertLess(guard, build, "the URL is built before the path is validated")

    def test_the_entry_type_does_not_decode_the_files_array(self):
        """The files array is most of the manifest, and the zip's sha256 covers those files."""
        self.assertNotIn("files", block(self.catalog, "enum CodingKeys"),
                         "the entry's CodingKeys in ShaderCatalog.swift include files")

    def test_the_cache_is_in_caches_and_not_in_documents(self):
        directory = block(self.catalog, "static var cacheDirectory")
        self.assertIn(".cachesDirectory", directory)
        self.assertNotIn(".documentDirectory", directory,
                         "cacheDirectory is under Documents, which is backed up and shown in Files")

    def test_the_staging_sweep_runs_at_launch(self):
        """`defer` does not run when iOS kills a backgrounded app mid-download."""
        self.assertIn("static func sweepStagedDownloads", self.installer)
        self.assertIn("ShaderCatalogInstaller.sweepStagedDownloads()", read(ROOT_VIEW),
                      "RootView does not call ShaderCatalogInstaller.sweepStagedDownloads()")

    def test_the_base_url_is_https_and_there_is_one_of_it(self):
        bases = re.findall(r'defaultBase\s*=\s*"([^"]+)"', self.catalog)
        self.assertEqual(len(bases), 1, "expected exactly one base URL constant, got %r" % bases)
        self.assertTrue(bases[0].startswith("https://"), "the base URL is not https")

    def test_the_override_accepts_only_https_and_file(self):
        """The file scheme is for reading a local emit in the simulator."""
        body = block(self.catalog, "func resolvedBase()")
        self.assertIn('url.scheme == "https"', body)
        self.assertIn('url.scheme == "file"', body)

    def test_the_installed_name_comes_from_the_return_and_not_the_shared_property(self):
        """Two rows can install at once against one importer, whose installedName is shared."""
        self.assertIn("let landed = await importer.install(archiveAt:", self.installer,
                      "the installer does not bind the return value of install(archiveAt:)")
        self.assertNotIn("importer.installedName", self.installer,
                         "the installer reads importer.installedName, which a concurrent "
                         "install overwrites")

    def test_per_game_on_without_a_resolvable_preset_is_written_off(self):
        """Per-game On with a preset that does not resolve is saved as Off."""
        body = block(read(PER_GAME), "static func write(chain:")
        self.assertNotIn("setBool(keys.enabled, chain == 1", body,
                         "write() sets the enabled key from the picker, so an unresolved preset "
                         "leaves the game on the global preset")
        self.assertIn("setBool(keys.enabled, resolved != nil", body)


class BrowserReachability(unittest.TestCase):
    """Where the Download Shaders row sits, and whether both hosts can push it."""

    SECTION = SWIFT / "Views/Settings/ShaderChainSection.swift"
    PAGE = SWIFT / "Views/Settings/ShaderSettingsView.swift"
    IN_GAME = SWIFT / "Views/GameScreenView.swift"

    def test_the_download_row_sits_above_install(self):
        text = read(self.SECTION)
        download = at(text, 'localized("Download Shaders")', "the Download row")
        install = at(text, 'localized("Install Shader Pack")', "the Install row")
        self.assertLess(download, install,
                        "Download Shaders sits below Install Shader Pack in ShaderChainSection")

    def test_the_settings_page_does_not_carry_a_second_copy(self):
        self.assertNotIn(
            "ShaderCatalogBrowserView", read(self.PAGE),
            "ShaderSettingsView opens ShaderCatalogBrowserView as well as ShaderChainSection, "
            "so the row appears twice")

    def test_the_in_game_host_can_push_a_destination(self):
        """The row is a NavigationLink and the pause panel has no stack of its own."""
        text = read(self.IN_GAME)
        mount = at(text, "ShaderChainSection(", "the in-game mount")
        # A window, not a whole-file search: GameScreenView carries several NavigationStacks
        # and any one of them would satisfy a backwards search from here.
        window = text[max(0, mount - 200):mount]
        self.assertIn(
            "NavigationStack", window,
            "GameScreenView mounts ShaderChainSection outside a NavigationStack, so its "
            "NavigationLinks do nothing")

    def test_a_downloaded_preset_can_be_picked_from_its_row(self):
        """Use on a downloaded row picks its preset instead of stopping at Installed."""
        section = read(self.SECTION)
        browser = read(BROWSER)
        self.assertRegex(section, r"ShaderCatalogBrowserView\([^)]*onSelect:",
                         "the section opens the download list without onSelect, so Use selects "
                         "nothing")
        self.assertRegex(section, r"enabled = true\s+presetRef = token",
                         "select() does not set enabled = true before presetRef = token")
        self.assertLess(at(browser, "installer.presetToken(for: entry)", "the Use row"),
                        at(browser, "onSelect(token)", "the Use action"),
                        "the Use action runs before the row has found the installed preset")
        self.assertIn('firstIndex(of: "/")', read(INSTALLER),
                      "presetToken only looks for <pack>/<id>.slangp, which misses a pack whose "
                      "shared top folder the extractor dropped")

    def test_a_preset_picked_in_any_folder_closes_the_browser(self):
        """A dismiss taken from an outer folder is ignored once an inner folder is pushed on top."""
        self.assertRegex(
            read(self.SECTION), r"select\(token\)\s+browseRequest = nil",
            "the Shaders section no longer closes the preset sheet on a pick")
        self.assertNotIn(
            "dismiss()", read(SWIFT / "Views/Settings/ShaderPresetBrowserView.swift"),
            "ShaderPresetBrowserView calls dismiss(), which does nothing from an inner folder")


if __name__ == "__main__":
    unittest.main()

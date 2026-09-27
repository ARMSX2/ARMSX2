"""The in-game per-game live preview keeps every committed write to the per-game INI."""

import unittest

from ios_source import CPP, SWIFT, at, block, read, without_comments


PANEL = SWIFT / "Views/PerGameSettingsPanel.swift"
BRIDGE = CPP / "ARMSX2Bridge.mm"


class LivePreviewKeepsCommittedWrites(unittest.TestCase):
    """The bridge used to keep the INI from panel open and write it back after every preview
    and on Discard, so a Save, a Stick Inversion change or a cheat toggle made in between
    was lost."""

    @classmethod
    def setUpClass(cls):
        cls.panel = without_comments(read(PANEL))
        cls.bridge = without_comments(read(BRIDGE))

    def test_each_preview_write_refreshes_the_baseline_first(self):
        loop = block(self.panel, "private func runLivePreviewLoop(")
        refresh = at(loop, "refreshPerGameLivePreviewBaseline(token:", "the baseline refresh")
        write = at(loop, "writeCurrentSettings(normalizesEditableValues: false)", "the preview write")
        self.assertLess(refresh, write, "a preview write is not preceded by the baseline refresh")
        self.assertEqual(self.panel.count("writeCurrentSettings(normalizesEditableValues: false)"), 1,
                         "a second preview write site has no baseline refresh")

    def test_finish_only_retries_a_failed_restore(self):
        finish = block(self.bridge, "+ (void)finishPerGameLivePreviewForToken:")
        restore = finish.find("ARMSX2RestorePerGameLivePreviewSettings(")
        self.assertGreaterEqual(restore, 0, "finish no longer retries a restore that failed")
        self.assertIn('restorePending"] boolValue])', finish[:restore],
                      "finish writes the stored bytes back even after a clean apply")

    def test_a_save_becomes_the_baseline(self):
        """After a failed restore the refresh skips, so a later Save was overwritten."""
        save = block(self.panel, "private func save(showsConfirmation")
        self.assertIn("afterSave: true", save, "a Save leaves the preview baseline behind")

    def test_the_refresh_never_takes_preview_values(self):
        refresh = block(self.bridge, "+ (void)refreshPerGameLivePreviewBaselineForToken:")
        self.assertLess(refresh.index("dataWithContentsOfFile:"), refresh.index("dispatch_async("),
                        "the baseline is read on the queue, after the next preview write")
        self.assertIn('restorePending"] boolValue]', refresh,
                      "the refresh can take a file whose restore failed as the baseline")


if __name__ == "__main__":
    unittest.main()
